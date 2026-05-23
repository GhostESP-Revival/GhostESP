#include "core/callbacks.h"
#include "esp_wifi.h"
#include "managers/gps_manager.h"
#include "managers/rgb_manager.h"
#include "managers/views/terminal_screen.h"
#include "managers/wifi_manager.h"
#include "managers/status_display_manager.h"
#include "core/utils.h"
#include "vendor/GPS/gps_logger.h"
#include "vendor/pcap.h"
#include "core/glog.h"
#include "core/esp_comm_manager.h"
#include "scans/wifi/wifi_channels.h"
#include <ctype.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include "esp_rom_sys.h"  // Contains esp_rom_printf
#include <esp_timer.h>  // For esp_timer_get_time
#include "freertos/task.h"
#include "freertos/queue.h"
#include "gui/toast.h"

// prototypes for static inline helpers
static inline bool is_packet_valid(const wifi_promiscuous_pkt_t *pkt, wifi_promiscuous_pkt_type_t type);
static inline bool is_on_target_channel(const wifi_promiscuous_pkt_t *pkt, uint8_t target_channel);

#define STORE_STR_ATTR
#define STORE_DATA_ATTR
#define WPS_OUI 0x0050f204
#define TAG "WIFI_MONITOR"
#define WPS_CONF_METHODS_PBC 0x0080
#define WPS_CONF_METHODS_PIN_DISPLAY 0x0004
#define WPS_CONF_METHODS_PIN_KEYPAD 0x0008
#define WIFI_PKT_DEAUTH 0x0C     // Deauth subtype
#define WIFI_PKT_BEACON 0x08     // Beacon subtype
#define WIFI_PKT_PROBE_REQ 0x04  // Probe Request subtype
#define WIFI_PKT_PROBE_RESP 0x05 // Probe Response subtype
#define WIFI_PKT_EAPOL 0x80
#define ESP_WIFI_VENDOR_METADATA_LEN 8 // Channel(1) + RSSI(1) + Rate(1) + Timestamp(4) + Noise(1)
#define MIN_SSIDS_FOR_DETECTION 2 // Minimum SSIDs needed to flag as PineAP
#define MAX_PINEAP_NETWORKS 20
#define MAX_SSIDS_PER_BSSID 10
#if defined(CONFIG_IDF_TARGET_ESP32C5)
#define MAX_WIFI_CHANNEL 165
#else
#define MAX_WIFI_CHANNEL 13
#endif
#define CHANNEL_HOP_INTERVAL_MS 100
#define WARDRIVE_STREAM_VERSION 2
#define WARDRIVE_STREAM_VERSION_LEGACY 1
#define WARDRIVE_STREAM_FLAG_GPS_PRESENT 0x01
#define WARDRIVE_STREAM_FLAG_GPS_FIX 0x02
#define WARDRIVE_STREAM_FLAG_GPS_DATE_VALID 0x04
#define WARDRIVE_STREAM_FLAG_GPS_TIME_VALID 0x08
#define GPS_STREAM_VERSION 1
#define GPS_STREAM_FLAG_PRESENT 0x01
#define GPS_STREAM_FLAG_FIX 0x02
#define GPS_STREAM_FLAG_DATE_VALID 0x04
#define GPS_STREAM_FLAG_TIME_VALID 0x08
#define WARDRIVE_HELPER_DEDUPE_SIZE 128
#define WARDRIVE_HELPER_REFRESH_MS 25000
#define PEER_GPS_STREAM_INTERVAL_MS 1000
#define PEER_GPS_INIT_RETRY_MS 5000
#define RECENT_SSID_COUNT 5
#define LOG_DELAY_MS 5000
#define PROBE_DEDUPE_TIMEOUT_MS 1000
#define MIN_RSSI_THRESHOLD -90  // Drop packets weaker than -90 dBm
#define MIN_PACKET_LENGTH 24    // Minimum 802.11 header size
#define MAX_IE_LEN 255
static const uint8_t pineapple_ouis[][3] = {
    {0x00, 0x13, 0x37},
};
static const size_t pineapple_oui_count = sizeof(pineapple_ouis) / sizeof(pineapple_ouis[0]);
static pineap_network_t *pineap_networks = NULL;
static int pineap_network_count = 0;
static bool pineap_detection_active = false;
static uint8_t current_channel = 1;
static esp_timer_handle_t channel_hop_timer = NULL;
static bool wardriving_hopping_active = false;
static uint8_t wardrive_channel = 1;
static esp_timer_handle_t wardrive_hop_timer = NULL;
static esp_timer_handle_t wardrive_heartbeat_timer = NULL;
static int64_t wardrive_start_us = 0;
static uint32_t wardrive_wifi_frames_seen = 0;
static uint32_t wardrive_ble_advs_seen = 0;
static uint32_t wardrive_log_attempts = 0;
static uint32_t wardrive_log_ok = 0;
static uint32_t wardrive_gps_rejected = 0;
static uint32_t wardrive_helper_rx_observations = 0;
static uint32_t wardrive_helper_merged_ok = 0;
static uint32_t wardrive_helper_tx_new = 0;
static uint32_t wardrive_helper_tx_ssid_promo = 0;
static uint32_t wardrive_helper_tx_rssi = 0;
static uint32_t wardrive_helper_tx_refresh = 0;
static uint32_t wardrive_helper_tx_suppressed = 0;
static uint32_t wardrive_helper_stream_send_ok = 0;
static uint32_t wardrive_helper_stream_send_fail = 0;
static uint32_t peer_gps_stream_tx_ok = 0;
static uint32_t peer_gps_stream_tx_fail = 0;
static uint32_t peer_gps_stream_rx_packets = 0;
static uint32_t peer_gps_stream_rx_fix_packets = 0;
static TaskHandle_t peer_gps_stream_task_handle = NULL;
static StackType_t *peer_gps_stream_stack = NULL;
static StaticTask_t *peer_gps_stream_tcb = NULL;

#ifndef CONFIG_IDF_TARGET_ESP32S2
#define BLE_WD_SEEN_SIZE 64
static uint32_t ble_wd_seen_hashes[BLE_WD_SEEN_SIZE];
static uint16_t ble_wd_seen_idx = 0;
static uint32_t ble_wd_unique_count = 0;

static uint32_t ble_wd_hash_mac(const uint8_t *addr) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < 6; i++) {
        hash ^= addr[i];
        hash *= 16777619u;
    }
    return hash ? hash : 1u;
}

uint32_t ble_wardriving_get_unique_device_count(void) {
    return ble_wd_unique_count;
}

void ble_wardriving_reset_unique_device_count(void) {
    memset(ble_wd_seen_hashes, 0, sizeof(ble_wd_seen_hashes));
    ble_wd_seen_idx = 0;
    ble_wd_unique_count = 0;
}
#endif

static void wardrive_heartbeat_cb(void *arg);
static void start_wardrive_heartbeat(void);
static void stop_wardrive_heartbeat(void);

static uint8_t wardrive_channels[WIFI_CHANNELS_MAX];
static uint8_t wardrive_channel_count = 0;
static uint8_t wardrive_channel_idx = 0;

typedef enum {
    WARDRIVE_ROLE_PRIMARY = 0,
    WARDRIVE_ROLE_HELPER = 1,
} wardrive_role_t;

typedef enum {
    WD_AUTH_OPEN = 0,
    WD_AUTH_WEP = 1,
    WD_AUTH_WPA = 2,
    WD_AUTH_WPA2 = 3,
    WD_AUTH_WPA3 = 4,
    WD_AUTH_OWE = 5,
} wd_auth_t;

typedef struct {
    uint32_t hash;
    int8_t best_rssi;
    uint32_t last_sent_ms;
    bool ssid_empty;
    bool used;
} wardrive_helper_dedupe_t;

static wardrive_role_t wardrive_role = WARDRIVE_ROLE_PRIMARY;
static bool wardrive_peer_assist_active = false;
static wardrive_helper_dedupe_t wardrive_helper_dedupe[WARDRIVE_HELPER_DEDUPE_SIZE];
static uint8_t wardrive_helper_dedupe_idx = 0;
static uint8_t wardrive_forced_helper_channels[WIFI_CHANNELS_MAX] = {0};
static uint8_t wardrive_forced_helper_channel_count = 0;

static void wardrive_stream_rx_cb(uint8_t channel, const uint8_t *data, size_t length, void *user_data);
static void gps_stream_rx_cb(uint8_t channel, const uint8_t *data, size_t length, void *user_data);
static uint8_t wardrive_select_auth_code(const char *encryption_type);
static const char *wardrive_auth_code_to_string(uint8_t auth_code);
static uint32_t wardrive_hash_bssid(const uint8_t *bssid);
static bool wardrive_helper_should_send(const uint8_t *bssid, int8_t rssi, const char *ssid);
static bool wardrive_send_helper_observation(const uint8_t *bssid,
                                             uint8_t channel,
                                             int8_t rssi,
                                             uint8_t auth_code,
                                             const char *ssid);
static inline void wardrive_put_i32le(uint8_t *dst, int32_t value);
static inline void wardrive_put_i16le(uint8_t *dst, int16_t value);
static inline int32_t wardrive_get_i32le(const uint8_t *src);
static inline int16_t wardrive_get_i16le(const uint8_t *src);
static bool wardrive_is_valid_date(const gps_date_t *date);
static bool wardrive_is_valid_time(const gps_time_t *tim);
static inline uint32_t now_ms_u32(void);
static bool wardrive_send_peer_gps_stream(void);
static void peer_gps_stream_task(void *arg);
static uint32_t wardrive_get_hop_interval_ms(void);
static void wardrive_apply_hop_interval(void);
static uint8_t wardrive_build_full_channel_list(uint8_t *full_channels);

static uint8_t wardrive_select_auth_code(const char *encryption_type) {
    if (!encryption_type) {
        return WD_AUTH_OPEN;
    }
    if (strcmp(encryption_type, "WEP") == 0) {
        return WD_AUTH_WEP;
    }
    if (strcmp(encryption_type, "WPA") == 0) {
        return WD_AUTH_WPA;
    }
    if (strcmp(encryption_type, "WPA2") == 0) {
        return WD_AUTH_WPA2;
    }
    if (strcmp(encryption_type, "WPA3") == 0) {
        return WD_AUTH_WPA3;
    }
    if (strcmp(encryption_type, "OWE") == 0) {
        return WD_AUTH_OWE;
    }
    return WD_AUTH_OPEN;
}

static const char *wardrive_auth_code_to_string(uint8_t auth_code) {
    switch (auth_code) {
        case WD_AUTH_WEP:
            return "WEP";
        case WD_AUTH_WPA:
            return "WPA";
        case WD_AUTH_WPA2:
            return "WPA2";
        case WD_AUTH_WPA3:
            return "WPA3";
        case WD_AUTH_OWE:
            return "OWE";
        case WD_AUTH_OPEN:
        default:
            return "OPEN";
    }
}

static uint32_t wardrive_hash_bssid(const uint8_t *bssid) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < 6; i++) {
        hash ^= bssid[i];
        hash *= 16777619u;
    }
    return hash ? hash : 1u;
}

static inline void wardrive_put_i32le(uint8_t *dst, int32_t value) {
    dst[0] = (uint8_t)(value & 0xFF);
    dst[1] = (uint8_t)((value >> 8) & 0xFF);
    dst[2] = (uint8_t)((value >> 16) & 0xFF);
    dst[3] = (uint8_t)((value >> 24) & 0xFF);
}

static inline void wardrive_put_i16le(uint8_t *dst, int16_t value) {
    dst[0] = (uint8_t)(value & 0xFF);
    dst[1] = (uint8_t)((value >> 8) & 0xFF);
}

static inline int32_t wardrive_get_i32le(const uint8_t *src) {
    return (int32_t)((uint32_t)src[0] |
                     ((uint32_t)src[1] << 8) |
                     ((uint32_t)src[2] << 16) |
                     ((uint32_t)src[3] << 24));
}

static inline int16_t wardrive_get_i16le(const uint8_t *src) {
    return (int16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static bool wardrive_is_valid_date(const gps_date_t *date) {
    if (!date) {
        return false;
    }
    if (date->year > 99 || date->month < 1 || date->month > 12 || date->day < 1 || date->day > 31) {
        return false;
    }
    return true;
}

static bool wardrive_is_valid_time(const gps_time_t *tim) {
    if (!tim) {
        return false;
    }
    return tim->hour <= 23 && tim->minute <= 59 && tim->second <= 59;
}

static bool wardrive_helper_should_send(const uint8_t *bssid, int8_t rssi, const char *ssid) {
    uint32_t hash = wardrive_hash_bssid(bssid);
    uint32_t now_ms = now_ms_u32();
    bool ssid_empty = (!ssid || ssid[0] == '\0');

    for (int i = 0; i < WARDRIVE_HELPER_DEDUPE_SIZE; i++) {
        wardrive_helper_dedupe_t *entry = &wardrive_helper_dedupe[i];
        if (entry->used && entry->hash == hash) {
            if (entry->ssid_empty && !ssid_empty) {
                entry->ssid_empty = false;
                if (rssi > entry->best_rssi) {
                    entry->best_rssi = rssi;
                }
                entry->last_sent_ms = now_ms;
                wardrive_helper_tx_ssid_promo++;
                return true;
            }
            if (rssi > entry->best_rssi + 3) {
                entry->best_rssi = rssi;
                if (!ssid_empty) {
                    entry->ssid_empty = false;
                }
                entry->last_sent_ms = now_ms;
                wardrive_helper_tx_rssi++;
                return true;
            }
            if ((uint32_t)(now_ms - entry->last_sent_ms) >= WARDRIVE_HELPER_REFRESH_MS) {
                if (rssi > entry->best_rssi) {
                    entry->best_rssi = rssi;
                }
                if (!ssid_empty) {
                    entry->ssid_empty = false;
                }
                entry->last_sent_ms = now_ms;
                wardrive_helper_tx_refresh++;
                return true;
            }
            wardrive_helper_tx_suppressed++;
            return false;
        }
    }

    wardrive_helper_dedupe_t *slot = &wardrive_helper_dedupe[wardrive_helper_dedupe_idx];
    slot->used = true;
    slot->hash = hash;
    slot->best_rssi = rssi;
    slot->last_sent_ms = now_ms;
    slot->ssid_empty = ssid_empty;
    wardrive_helper_dedupe_idx = (uint8_t)((wardrive_helper_dedupe_idx + 1) % WARDRIVE_HELPER_DEDUPE_SIZE);
    wardrive_helper_tx_new++;
    return true;
}

static bool wardrive_send_helper_observation(const uint8_t *bssid,
                                             uint8_t channel,
                                             int8_t rssi,
                                             uint8_t auth_code,
                                             const char *ssid) {
    if (!esp_comm_manager_is_connected()) {
        return false;
    }

    uint8_t ssid_len = 0;
    if (ssid) {
        size_t raw_len = strlen(ssid);
        /* Keep helper stream payload <= 59 bytes so it fits one comm packet. */
        if (raw_len > 23) {
            raw_len = 23;
        }
        ssid_len = (uint8_t)raw_len;
    }

    uint8_t payload[1 + 1 + 1 + 1 + 6 + 1 + 32 + 1 + 4 + 4 + 2 + 1 + 1 + 1 + 1 + 2 + 7] = {0};
    size_t pos = 0;
    payload[pos++] = WARDRIVE_STREAM_VERSION;
    payload[pos++] = channel;
    payload[pos++] = (uint8_t)rssi;
    payload[pos++] = auth_code;
    memcpy(payload + pos, bssid, 6);
    pos += 6;
    payload[pos++] = ssid_len;
    if (ssid_len > 0) {
        memcpy(payload + pos, ssid, ssid_len);
        pos += ssid_len;
    }

    uint8_t gps_flags = 0;
    int32_t lat_e7 = 0;
    int32_t lon_e7 = 0;
    int16_t alt_dm = 0;
    uint8_t sats_in_use = 0;
    uint8_t sats_in_view = 0;
    uint8_t fix = (uint8_t)GPS_FIX_INVALID;
    uint8_t fix_mode = (uint8_t)GPS_MODE_INVALID;
    uint16_t hdop_x10 = 0;
    uint8_t gps_day = 0;
    uint8_t gps_month = 0;
    uint16_t gps_year = 0;
    uint8_t gps_hour = 0;
    uint8_t gps_minute = 0;
    uint8_t gps_second = 0;

    gps_t local_gps = {0};
    if (gps_manager_has_recent_update() && gps_manager_get_local_gps_snapshot(&local_gps)) {
        gps_flags |= WARDRIVE_STREAM_FLAG_GPS_PRESENT;
        sats_in_use = local_gps.sats_in_use;
        sats_in_view = local_gps.sats_in_view;
        fix = (uint8_t)local_gps.fix;
        fix_mode = (uint8_t)local_gps.fix_mode;

        if (local_gps.dop_h >= 0.0f && local_gps.dop_h <= 6553.5f) {
            hdop_x10 = (uint16_t)(local_gps.dop_h * 10.0f);
        }

        if (wardrive_is_valid_date(&local_gps.date)) {
            gps_flags |= WARDRIVE_STREAM_FLAG_GPS_DATE_VALID;
        }
        if (wardrive_is_valid_time(&local_gps.tim)) {
            gps_flags |= WARDRIVE_STREAM_FLAG_GPS_TIME_VALID;
        }
        gps_day = local_gps.date.day;
        gps_month = local_gps.date.month;
        gps_year = local_gps.date.year;
        gps_hour = local_gps.tim.hour;
        gps_minute = local_gps.tim.minute;
        gps_second = local_gps.tim.second;

        if (local_gps.valid && local_gps.fix >= GPS_FIX_GPS && local_gps.fix_mode >= GPS_MODE_2D &&
            local_gps.latitude >= -90.0f && local_gps.latitude <= 90.0f &&
            local_gps.longitude >= -180.0f && local_gps.longitude <= 180.0f) {
            gps_flags |= WARDRIVE_STREAM_FLAG_GPS_FIX;
            lat_e7 = (int32_t)(local_gps.latitude * 10000000.0f);
            lon_e7 = (int32_t)(local_gps.longitude * 10000000.0f);
            float alt = local_gps.altitude * 10.0f;
            if (alt > 32767.0f) {
                alt = 32767.0f;
            }
            if (alt < -32768.0f) {
                alt = -32768.0f;
            }
            alt_dm = (int16_t)alt;
        }
    }

    payload[pos++] = gps_flags;
    if (gps_flags & WARDRIVE_STREAM_FLAG_GPS_PRESENT) {
        wardrive_put_i32le(payload + pos, lat_e7);
        pos += 4;
        wardrive_put_i32le(payload + pos, lon_e7);
        pos += 4;
        wardrive_put_i16le(payload + pos, alt_dm);
        pos += 2;
        payload[pos++] = sats_in_use;
        payload[pos++] = sats_in_view;
        payload[pos++] = fix;
        payload[pos++] = fix_mode;
        payload[pos++] = (uint8_t)(hdop_x10 & 0xFF);
        payload[pos++] = (uint8_t)((hdop_x10 >> 8) & 0xFF);
        payload[pos++] = gps_day;
        payload[pos++] = gps_month;
        wardrive_put_i16le(payload + pos, (int16_t)gps_year);
        pos += 2;
        payload[pos++] = gps_hour;
        payload[pos++] = gps_minute;
        payload[pos++] = gps_second;
    }

    return esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_WARDRIVE, payload, pos);
}

static bool wardrive_send_peer_gps_stream(void) {
    if (!esp_comm_manager_is_connected()) {
        return false;
    }

    gps_t gps_local = {0};
    if (!gps_manager_get_local_gps_snapshot(&gps_local)) {
        return false;
    }

    uint8_t payload[1 + 1 + 4 + 4 + 2 + 1 + 1 + 1 + 1 + 2 + 2 + 2 + 7] = {0};
    size_t pos = 0;
    payload[pos++] = GPS_STREAM_VERSION;

    uint8_t flags = GPS_STREAM_FLAG_PRESENT;
    bool has_fix = gps_local.valid &&
                   gps_local.fix >= GPS_FIX_GPS &&
                   gps_local.fix_mode >= GPS_MODE_2D &&
                   gps_local.latitude >= -90.0f && gps_local.latitude <= 90.0f &&
                   gps_local.longitude >= -180.0f && gps_local.longitude <= 180.0f;
    if (has_fix) {
        flags |= GPS_STREAM_FLAG_FIX;
    }
    if (wardrive_is_valid_date(&gps_local.date)) {
        flags |= GPS_STREAM_FLAG_DATE_VALID;
    }
    if (wardrive_is_valid_time(&gps_local.tim)) {
        flags |= GPS_STREAM_FLAG_TIME_VALID;
    }
    payload[pos++] = flags;

    int32_t lat_e7 = has_fix ? (int32_t)(gps_local.latitude * 10000000.0f) : 0;
    int32_t lon_e7 = has_fix ? (int32_t)(gps_local.longitude * 10000000.0f) : 0;
    float alt_dm_f = gps_local.altitude * 10.0f;
    if (alt_dm_f > 32767.0f) {
        alt_dm_f = 32767.0f;
    }
    if (alt_dm_f < -32768.0f) {
        alt_dm_f = -32768.0f;
    }
    int16_t alt_dm = (int16_t)alt_dm_f;

    uint16_t hdop_x10 = 0;
    if (isfinite(gps_local.dop_h) && gps_local.dop_h >= 0.0f && gps_local.dop_h <= 6553.5f) {
        hdop_x10 = (uint16_t)(gps_local.dop_h * 10.0f);
    }

    float speed_x100_f = gps_local.speed * 100.0f;
    if (!isfinite(speed_x100_f)) {
        speed_x100_f = 0.0f;
    }
    if (speed_x100_f > 32767.0f) {
        speed_x100_f = 32767.0f;
    }
    if (speed_x100_f < -32768.0f) {
        speed_x100_f = -32768.0f;
    }
    int16_t speed_x100 = (int16_t)speed_x100_f;

    float course_x100_f = gps_local.cog * 100.0f;
    if (!isfinite(course_x100_f)) {
        course_x100_f = 0.0f;
    }
    if (course_x100_f > 32767.0f) {
        course_x100_f = 32767.0f;
    }
    if (course_x100_f < -32768.0f) {
        course_x100_f = -32768.0f;
    }
    int16_t course_x100 = (int16_t)course_x100_f;

    wardrive_put_i32le(payload + pos, lat_e7);
    pos += 4;
    wardrive_put_i32le(payload + pos, lon_e7);
    pos += 4;
    wardrive_put_i16le(payload + pos, alt_dm);
    pos += 2;
    payload[pos++] = gps_local.sats_in_use;
    payload[pos++] = gps_local.sats_in_view;
    payload[pos++] = (uint8_t)gps_local.fix;
    payload[pos++] = (uint8_t)gps_local.fix_mode;
    wardrive_put_i16le(payload + pos, (int16_t)hdop_x10);
    pos += 2;
    wardrive_put_i16le(payload + pos, speed_x100);
    pos += 2;
    wardrive_put_i16le(payload + pos, course_x100);
    pos += 2;
    payload[pos++] = gps_local.date.day;
    payload[pos++] = gps_local.date.month;
    wardrive_put_i16le(payload + pos, (int16_t)gps_local.date.year);
    pos += 2;
    payload[pos++] = gps_local.tim.hour;
    payload[pos++] = gps_local.tim.minute;
    payload[pos++] = gps_local.tim.second;

    return esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_GPS, payload, pos);
}

static void peer_gps_stream_task(void *arg) {
    (void)arg;
    int64_t last_init_try_ms = 0;
    gps_t gps_local = {0};

    while (1) {
        if (esp_comm_manager_is_connected()) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
            if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething2") == 0 &&
                !g_gpsManager.isinitilized) {
                int64_t now_ms = esp_timer_get_time() / 1000;
                if ((now_ms - last_init_try_ms) >= PEER_GPS_INIT_RETRY_MS) {
                    gps_manager_init(&g_gpsManager);
                    last_init_try_ms = now_ms;
                }
            }
#endif

            if (gps_manager_get_local_gps_snapshot(&gps_local)) {
                if (wardrive_send_peer_gps_stream()) {
                    peer_gps_stream_tx_ok++;
                } else {
                    peer_gps_stream_tx_fail++;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(PEER_GPS_STREAM_INTERVAL_MS));
    }
}

static uint32_t wardrive_get_hop_interval_ms(void) {
    uint32_t interval_ms = CHANNEL_HOP_INTERVAL_MS;
    if (interval_ms < 40) {
        interval_ms = 40;
    }
    if (interval_ms > 1000) {
        interval_ms = 1000;
    }
    return interval_ms;
}

static void wardrive_apply_hop_interval(void) {
    if (!wardriving_hopping_active || wardrive_hop_timer == NULL) {
        return;
    }

    uint32_t interval_ms = wardrive_get_hop_interval_ms();
    (void)esp_timer_stop(wardrive_hop_timer);
    esp_err_t err = esp_timer_start_periodic(wardrive_hop_timer, (uint64_t)interval_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wardrive: failed to apply hop interval (%s)", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Wardrive: hop interval now %lu ms", (unsigned long)interval_ms);
}

static uint8_t wardrive_build_full_channel_list(uint8_t *full_channels) {
    if (!full_channels) {
        return 0;
    }

    uint8_t full_count = wifi_channels_build_country_list(full_channels, WIFI_CHANNELS_MAX);
    if (full_count == 0) {
        uint8_t fallback[] = {1,2,3,4,5,6,7,8,9,10,11,12,13,
            36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,144,149,153,157,161,165};
        uint8_t count = sizeof(fallback);
        if (count > WIFI_CHANNELS_MAX) count = WIFI_CHANNELS_MAX;
        memcpy(full_channels, fallback, count);
        full_count = count;
    }

    return full_count;
}

static void wardrive_build_channel_list(void) {
    uint8_t full_channels[WIFI_CHANNELS_MAX] = {0};
    uint8_t channels_24[WIFI_CHANNELS_MAX] = {0};
    uint8_t channels_5[WIFI_CHANNELS_MAX] = {0};
    uint8_t channels_24_count = 0;
    uint8_t channels_5_count = 0;
    uint8_t full_count = wardrive_build_full_channel_list(full_channels);

    for (uint8_t i = 0; i < full_count; i++) {
        if (full_channels[i] <= 14) {
            channels_24[channels_24_count++] = full_channels[i];
        } else {
            channels_5[channels_5_count++] = full_channels[i];
        }
    }

    wardrive_channel_count = 0;
    if (wardrive_role == WARDRIVE_ROLE_PRIMARY && !wardrive_peer_assist_active) {
        memcpy(wardrive_channels, full_channels, full_count);
        wardrive_channel_count = full_count;
    } else if (wardrive_role == WARDRIVE_ROLE_PRIMARY) {
        if (channels_5_count > 0 && channels_24_count > 0) {
            memcpy(wardrive_channels, channels_5, channels_5_count);
            wardrive_channel_count = channels_5_count;
        } else {
            for (uint8_t i = 0; i < full_count && wardrive_channel_count < WIFI_CHANNELS_MAX; i += 2) {
                wardrive_channels[wardrive_channel_count++] = full_channels[i];
            }
        }
    } else {
        if (wardrive_forced_helper_channel_count > 0) {
            memcpy(wardrive_channels, wardrive_forced_helper_channels, wardrive_forced_helper_channel_count);
            wardrive_channel_count = wardrive_forced_helper_channel_count;
        } else if (channels_24_count > 0) {
            memcpy(wardrive_channels, channels_24, channels_24_count);
            wardrive_channel_count = channels_24_count;
        } else {
            for (uint8_t i = 1; i < full_count && wardrive_channel_count < WIFI_CHANNELS_MAX; i += 2) {
                wardrive_channels[wardrive_channel_count++] = full_channels[i];
            }
        }
    }

    if (wardrive_channel_count == 0) {
        memcpy(wardrive_channels, full_channels, full_count);
        wardrive_channel_count = full_count;
    }

    ESP_LOGI(TAG,
             "Wardrive: role=%s channels=%d/%d assist=%s bands(2.4=%d,5=%d) forced_helper=%d",
             wardrive_role == WARDRIVE_ROLE_PRIMARY ? "primary" : "helper",
             wardrive_channel_count,
             full_count,
             wardrive_peer_assist_active ? "on" : "off",
             channels_24_count,
             channels_5_count,
             wardrive_forced_helper_channel_count);
}

static uint8_t wardrive_parse_channel_csv(const char *csv, uint8_t *out, uint8_t out_max) {
    if (!csv || !out || out_max == 0) {
        return 0;
    }

    char buf[192];
    strncpy(buf, csv, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    uint8_t count = 0;
    char *tok = strtok(buf, ",");
    while (tok && count < out_max) {
        while (*tok && isspace((unsigned char)*tok)) tok++;
        char *end = tok + strlen(tok);
        while (end > tok && isspace((unsigned char)end[-1])) {
            end--;
        }
        *end = '\0';

        if (*tok != '\0') {
            char *num_end = NULL;
            long ch = strtol(tok, &num_end, 10);
            if (num_end != tok && *num_end == '\0' && ch >= 1 && ch <= MAX_WIFI_CHANNEL) {
                bool exists = false;
                for (uint8_t i = 0; i < count; i++) {
                    if (out[i] == (uint8_t)ch) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    out[count++] = (uint8_t)ch;
                }
            }
        }

        tok = strtok(NULL, ",");
    }

    return count;
}

static uint32_t hash_ssid(const char *ssid);
static bool ssid_hash_exists(pineap_network_t *network, uint32_t hash);
static int build_recent_ssids_string(const pineap_network_t *network, char *out, size_t out_size);
static void log_pineap_details(pineap_network_t *network,
                               const char *title,
                               const char *ssids_str,
                               int ssid_count);
static bool is_pineapple_oui(const uint8_t *bssid);
static void trim_trailing(char *str);
static bool compare_bssid(const uint8_t *bssid1, const uint8_t *bssid2);
static bool is_beacon_packet(const wifi_promiscuous_pkt_t *pkt);
static pineap_network_t *find_or_create_network(const uint8_t *bssid);
#ifndef CONFIG_IDF_TARGET_ESP32S2
#endif

// handshake pairing and limited beacon emission for eapol capture
typedef struct {
    uint8_t ap[6];
    uint8_t sta[6];
    uint64_t replay;
    uint8_t ap_msg;   // 0=unknown, 1..4=M1..M4
    uint8_t sta_msg;  // 0=unknown, 1..4=M1..M4
} hs_entry_t;

#define HS_TABLE_MAX 16
static hs_entry_t hs_table[HS_TABLE_MAX];
static uint8_t hs_count_local = 0;
static uint8_t hs_insert_idx_local = 0;
static uint32_t hs_found_count = 0;
static bool s_pcap_enabled = true;

static inline bool mac_equal(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 6) == 0;
}

uint32_t wifi_callbacks_get_handshake_count(void) {
    return hs_found_count;
}

void wifi_callbacks_reset_handshake_tracking(void) {
    memset(hs_table, 0, sizeof(hs_table));
    hs_count_local = 0;
    hs_insert_idx_local = 0;
    hs_found_count = 0;
}

void wifi_callbacks_set_pcap_enabled(bool enabled) {
    s_pcap_enabled = enabled;
}

static const char *msg_name(uint8_t m) {
    switch (m) { case 1: return "M1"; case 2: return "M2"; case 3: return "M3"; case 4: return "M4"; default: return "M?"; }
}

static void process_eapol_candidate_pair(const uint8_t *ap,
                                         const uint8_t *sta,
                                         uint64_t replay,
                                         bool from_ap,
                                         uint8_t msg_type) {
    for (uint8_t i = 0; i < hs_count_local; i++) {
        hs_entry_t *e = &hs_table[i];
        if (mac_equal(e->ap, ap) && mac_equal(e->sta, sta) && e->replay == replay) {
            if (from_ap) e->ap_msg = msg_type; else e->sta_msg = msg_type;
            if (e->ap_msg && e->sta_msg) {
                hs_found_count++;
                char ap_str[18];
                snprintf(ap_str, sizeof(ap_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                         e->ap[0], e->ap[1], e->ap[2], e->ap[3], e->ap[4], e->ap[5]);
                glog("Handshake found!\nAP=%s\nPair=%s/%s\n",
                     ap_str, msg_name(e->ap_msg), msg_name(e->sta_msg));
                // reset to avoid duplicate notifications for same replay
                e->ap_msg = 0;
                e->sta_msg = 0;
            }
            return;
        }
    }
    uint8_t idx;
    if (hs_count_local < HS_TABLE_MAX) {
        idx = hs_count_local++;
    } else {
        idx = hs_insert_idx_local;
        hs_insert_idx_local = (hs_insert_idx_local + 1) % HS_TABLE_MAX;
    }
    hs_entry_t *ne = &hs_table[idx];
    memcpy(ne->ap, ap, 6);
    memcpy(ne->sta, sta, 6);
    ne->replay = replay;
    ne->ap_msg = from_ap ? msg_type : 0;
    ne->sta_msg = from_ap ? 0 : msg_type;
}

typedef struct {
    uint8_t bssid[6];
    uint8_t emitted;
    bool saw_nonempty_ssid;
} beacon_limiter_t;

#define BEACON_LIMIT_MAX 64
#define BEACON_MAX_PER_BSSID 3
static beacon_limiter_t beacon_limits[BEACON_LIMIT_MAX];
static uint8_t beacon_limit_count = 0;
static uint8_t beacon_limit_insert = 0;

// probe request dedupe to keep files small
#define PROBE_DEDUPE_MAX 64
typedef struct {
    uint8_t src[6];
    uint32_t ssid_hash;
    uint64_t last_ms;
} probe_dedupe_t;
static probe_dedupe_t probe_dedupe_tbl[PROBE_DEDUPE_MAX];
static uint8_t probe_dedupe_count = 0;
static uint8_t probe_dedupe_insert = 0;

static bool probe_should_emit(const uint8_t *src, uint32_t ssid_hash, uint64_t now_ms) {
    for (uint8_t i = 0; i < probe_dedupe_count; i++) {
        probe_dedupe_t *e = &probe_dedupe_tbl[i];
        if (memcmp(e->src, src, 6) == 0 && e->ssid_hash == ssid_hash) {
            if (now_ms - e->last_ms < PROBE_DEDUPE_TIMEOUT_MS) {
                return false;
            }
            e->last_ms = now_ms;
            return true;
        }
    }
    uint8_t idx;
    if (probe_dedupe_count < PROBE_DEDUPE_MAX) {
        idx = probe_dedupe_count++;
    } else {
        idx = probe_dedupe_insert;
        probe_dedupe_insert = (probe_dedupe_insert + 1) % PROBE_DEDUPE_MAX;
    }
    probe_dedupe_t *ne = &probe_dedupe_tbl[idx];
    memcpy(ne->src, src, 6);
    ne->ssid_hash = ssid_hash;
    ne->last_ms = now_ms;
    return true;
}

static bool beacon_should_emit_limited(const uint8_t *bssid, bool ssid_has_text) {
    for (uint8_t i = 0; i < beacon_limit_count; i++) {
        if (mac_equal(beacon_limits[i].bssid, bssid)) {
            if (beacon_limits[i].emitted >= BEACON_MAX_PER_BSSID) {
                if (!beacon_limits[i].saw_nonempty_ssid && ssid_has_text) {
                    beacon_limits[i].saw_nonempty_ssid = true;
                    return true;
                }
                return false;
            }
            beacon_limits[i].emitted++;
            if (ssid_has_text) beacon_limits[i].saw_nonempty_ssid = true;
            return true;
        }
    }
    uint8_t idx;
    if (beacon_limit_count < BEACON_LIMIT_MAX) {
        idx = beacon_limit_count++;
    } else {
        idx = beacon_limit_insert;
        beacon_limit_insert = (beacon_limit_insert + 1) % BEACON_LIMIT_MAX;
    }
    memcpy(beacon_limits[idx].bssid, bssid, 6);
    beacon_limits[idx].emitted = 1;
    beacon_limits[idx].saw_nonempty_ssid = ssid_has_text;
    return true;
}

// queued writer to avoid heavy work in promiscuous callback
typedef struct {
    uint16_t length;
    uint8_t data[768];
    bool in_use;
} pcap_pool_slot_t;

typedef struct {
    uint8_t slot_idx;
    pcap_capture_type_t cap_type;
} pcap_q_item_t;

#define EAPOL_Q_LEN 64
#if defined(CONFIG_IDF_TARGET_ESP32S2)
#define PCAP_POOL_SLOTS_DEFAULT 10
#define PCAP_POOL_SLOTS_MIN 4
#else
#define PCAP_POOL_SLOTS_DEFAULT 16
#define PCAP_POOL_SLOTS_MIN 8
#endif
static QueueHandle_t s_pcap_q = NULL;
static TaskHandle_t s_pcap_writer_task = NULL;
static pcap_pool_slot_t *s_pcap_pool = NULL;
static size_t s_pcap_pool_slots = 0;
static portMUX_TYPE s_pcap_pool_lock = portMUX_INITIALIZER_UNLOCKED;

static bool pcap_pool_init(void) {
    if (s_pcap_pool != NULL && s_pcap_pool_slots > 0) {
        return true;
    }

    size_t slots = PCAP_POOL_SLOTS_DEFAULT;
    while (slots >= PCAP_POOL_SLOTS_MIN) {
        pcap_pool_slot_t *pool = (pcap_pool_slot_t *)heap_caps_calloc(slots, sizeof(pcap_pool_slot_t), MALLOC_CAP_8BIT);
        if (pool != NULL) {
            s_pcap_pool = pool;
            s_pcap_pool_slots = slots;
            ESP_LOGI(TAG, "PCAP pool allocated: %lu slots (%lu bytes)",
                     (unsigned long)s_pcap_pool_slots,
                     (unsigned long)(s_pcap_pool_slots * sizeof(pcap_pool_slot_t)));
            return true;
        }
        if (slots == PCAP_POOL_SLOTS_MIN) {
            break;
        }
        slots = (slots > 2) ? (slots - 2) : PCAP_POOL_SLOTS_MIN;
        if (slots < PCAP_POOL_SLOTS_MIN) {
            slots = PCAP_POOL_SLOTS_MIN;
        }
    }

    ESP_LOGE(TAG, "PCAP pool allocation failed");
    return false;
}

static int pcap_pool_acquire_slot(void) {
    if (s_pcap_pool == NULL || s_pcap_pool_slots == 0) {
        return -1;
    }

    int slot = -1;
    taskENTER_CRITICAL(&s_pcap_pool_lock);
    for (size_t i = 0; i < s_pcap_pool_slots; i++) {
        if (!s_pcap_pool[i].in_use) {
            s_pcap_pool[i].in_use = true;
            slot = (int)i;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_pcap_pool_lock);
    return slot;
}

static void pcap_pool_release_slot(uint8_t slot_idx) {
    if (s_pcap_pool == NULL || s_pcap_pool_slots == 0 || slot_idx >= s_pcap_pool_slots) {
        return;
    }

    taskENTER_CRITICAL(&s_pcap_pool_lock);
    s_pcap_pool[slot_idx].in_use = false;
    s_pcap_pool[slot_idx].length = 0;
    taskEXIT_CRITICAL(&s_pcap_pool_lock);
}

static void pcap_writer_task(void *arg) {
    (void)arg;
    pcap_q_item_t item;
    uint32_t processed = 0;
    for (;;) {
        if (xQueueReceive(s_pcap_q, &item, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (s_pcap_pool != NULL && item.slot_idx < s_pcap_pool_slots) {
                pcap_pool_slot_t *slot = &s_pcap_pool[item.slot_idx];
                if (slot->length > 0) {
                    pcap_write_packet_to_buffer(slot->data, slot->length, item.cap_type);
                }
                pcap_pool_release_slot(item.slot_idx);
            }
            processed++;
            if ((processed & 0xFF) == 0) { // log occasionally to avoid spam
                UBaseType_t hwm_words = uxTaskGetStackHighWaterMark(NULL);
                glog("PCAP writer HWM (bytes): %lu\n", (unsigned long)hwm_words);
            }
            if ((processed & 0x1F) == 0 && pcap_auto_flush_enabled()) {
                pcap_flush_buffer_to_file();
            }
        } else {
            // periodic flush even if idle
            if (pcap_auto_flush_enabled()) {
                pcap_flush_buffer_to_file();
            }
        }
    }
}

static inline void ensure_pcap_queue_started(void) {
    if (s_pcap_q != NULL) {
        return;
    }

    if (!pcap_pool_init()) {
        return;
    }

    s_pcap_q = xQueueCreate(EAPOL_Q_LEN, sizeof(pcap_q_item_t));
    if (s_pcap_q != NULL && s_pcap_writer_task == NULL) {
        xTaskCreate(pcap_writer_task, "pcap_wr", 3072, NULL, 5, &s_pcap_writer_task);
    }
}

static inline void enqueue_pcap_write_typed(const uint8_t *payload, uint16_t len, pcap_capture_type_t cap_type) {
    if (!payload || len == 0) return;
    ensure_pcap_queue_started();
    if (!s_pcap_q) return;

    if (s_pcap_pool == NULL || s_pcap_pool_slots == 0) {
        return;
    }

    if (len > sizeof(s_pcap_pool[0].data)) {
        return;
    }

    int slot = pcap_pool_acquire_slot();
    if (slot < 0) {
        return;
    }

    pcap_pool_slot_t *pool_slot = &s_pcap_pool[slot];
    pool_slot->length = len;
    memcpy(pool_slot->data, payload, len);

    pcap_q_item_t item = {0};
    item.cap_type = cap_type;
    item.slot_idx = (uint8_t)slot;

    if (xQueueSend(s_pcap_q, &item, 0) != pdTRUE) {
        pcap_pool_release_slot((uint8_t)slot);
    }
}

static inline void enqueue_pcap_write(const uint8_t *payload, uint16_t len) {
    if (!s_pcap_enabled) return;
    enqueue_pcap_write_typed(payload, len, PCAP_CAPTURE_WIFI);
}

// cleanup function to free pcap queue and task when not capturing
void cleanup_pcap_queue(void) {
    if (s_pcap_writer_task != NULL) {
        vTaskDelete(s_pcap_writer_task);
        s_pcap_writer_task = NULL;
    }
    if (s_pcap_q != NULL) {
        // drain any remaining items and release pool slots
        pcap_q_item_t item;
        while (xQueueReceive(s_pcap_q, &item, 0) == pdTRUE) {
            pcap_pool_release_slot(item.slot_idx);
        }
        vQueueDelete(s_pcap_q);
        s_pcap_q = NULL;
    }

    if (s_pcap_pool != NULL) {
        pcap_pool_slot_t *pool_to_free = NULL;
        taskENTER_CRITICAL(&s_pcap_pool_lock);
        pool_to_free = s_pcap_pool;
        s_pcap_pool = NULL;
        s_pcap_pool_slots = 0;
        taskEXIT_CRITICAL(&s_pcap_pool_lock);
        heap_caps_free(pool_to_free);
    }
}

static const char *suspicious_names[] STORE_DATA_ATTR = {
    "HC-03", "HC-05", "HC-06",  "HC-08",    "BT-HC05", "JDY-31",
    "AT-09", "HM-10", "CC41-A", "MLT-BT05", "SPP-CA",  "FFD0"};

wps_network_t detected_wps_networks[MAX_WPS_NETWORKS];
int detected_network_count = 0;
esp_timer_handle_t stop_timer;
int should_store_wps = 1;
gps_t *gps = NULL;
static bool gps_time_synced = false;

static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static bool gps_build_utc_timeval(const gps_t *g, struct timeval *out) {
    if (!g || !out) {
        return false;
    }
    if (!gps_is_valid_year(g->date.year)) {
        return false;
    }
    if (g->date.month < 1 || g->date.month > 12 || g->date.day < 1 || g->date.day > 31) {
        return false;
    }
    if (g->tim.hour > 23 || g->tim.minute > 59 || g->tim.second > 59) {
        return false;
    }

    const int year = (int)gps_get_absolute_year(g->date.year);
    const int64_t days = days_from_civil(year, (unsigned)g->date.month, (unsigned)g->date.day);
    const int64_t sec = days * 86400LL + (int64_t)g->tim.hour * 3600LL + (int64_t)g->tim.minute * 60LL + (int64_t)g->tim.second;
    if (sec < 946684800LL) {
        return false;
    }

    out->tv_sec = (time_t)sec;
    out->tv_usec = 0;
    return true;
}

static void gps_try_sync_time_from_fix(const gps_t *g) {
    if (gps_time_synced || !g) {
        return;
    }

    struct timeval tv;
    if (!gps_build_utc_timeval(g, &tv)) {
        return;
    }

    struct timeval now;
    if (gettimeofday(&now, NULL) != 0) {
        return;
    }

    if (now.tv_sec >= 1600000000) {
        gps_time_synced = true;
        return;
    }

    if (tv.tv_sec >= 1600000000) {
        settimeofday(&tv, NULL);
        gps_time_synced = true;
    }
}

typedef struct {
    uint8_t bssid[6];
    time_t detection_time;
    time_t last_update_time;
} blacklisted_ap_t;

static blacklisted_ap_t *blacklist = NULL;
static int blacklist_count = 0;

typedef struct {
    uint8_t network_index;
    uint32_t due_ms;
} pineap_log_event_t;

#define PINEAP_LOG_QUEUE_LEN 8
static QueueHandle_t s_pineap_log_queue = NULL;
static TaskHandle_t s_pineap_log_task = NULL;

static inline uint32_t now_ms_u32(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool allocate_pineap_tables(void) {
    if (pineap_networks != NULL && blacklist != NULL) {
        return true;
    }

    pineap_network_t *new_networks = calloc(MAX_PINEAP_NETWORKS, sizeof(*new_networks));
    blacklisted_ap_t *new_blacklist = calloc(MAX_PINEAP_NETWORKS, sizeof(*new_blacklist));
    if (new_networks == NULL || new_blacklist == NULL) {
        free(new_networks);
        free(new_blacklist);
        return false;
    }

    pineap_networks = new_networks;
    blacklist = new_blacklist;
    return true;
}

static void free_pineap_tables(void) {
    free(pineap_networks);
    pineap_networks = NULL;
    free(blacklist);
    blacklist = NULL;
    pineap_network_count = 0;
    blacklist_count = 0;
}

static bool is_blacklisted(const uint8_t *bssid) {
    if (blacklist == NULL) {
        return false;
    }
    for (int i = 0; i < blacklist_count; i++) {
        if (memcmp(blacklist[i].bssid, bssid, 6) == 0) {
            return true;
        }
    }
    return false;
}

static bool should_update_blacklisted(const uint8_t *bssid) {
    if (blacklist == NULL) {
        return false;
    }
    for (int i = 0; i < blacklist_count; i++) {
        if (memcmp(blacklist[i].bssid, bssid, 6) == 0) {
            time_t current_time = time(NULL);
            // Allow updates every 30 seconds
            if (current_time - blacklist[i].last_update_time >= 30) {
                blacklist[i].last_update_time = current_time;
                return true;
            }
            return false;
        }
    }
    return false;
}

static void add_to_blacklist(const uint8_t *bssid) {
    if (blacklist == NULL) {
        return;
    }
    time_t current_time = time(NULL);

    // First check if BSSID exists
    for (int i = 0; i < blacklist_count; i++) {
        if (memcmp(blacklist[i].bssid, bssid, 6) == 0) {
            blacklist[i].last_update_time = current_time;
            return;
        }
    }

    // If not found and we have space, add new entry
    if (blacklist_count < MAX_PINEAP_NETWORKS) {
        memcpy(blacklist[blacklist_count].bssid, bssid, 6);
        blacklist[blacklist_count].detection_time = current_time;
        blacklist[blacklist_count].last_update_time = current_time;
        blacklist_count++;
    }
}

static void channel_hop_timer_callback(void *arg) {
    if (!pineap_detection_active)
        return;

    wardrive_channel_idx = (wardrive_channel_idx + 1) % wardrive_channel_count;
    current_channel = wardrive_channels[wardrive_channel_idx];
    esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE);
}

static esp_err_t start_channel_hopping(void) {
    esp_timer_create_args_t timer_args = {.callback = channel_hop_timer_callback,
                                          .name = "channel_hop"};

    if (channel_hop_timer == NULL) {
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &channel_hop_timer));
    }

    return esp_timer_start_periodic(channel_hop_timer, CHANNEL_HOP_INTERVAL_MS * 1000);
}

static void stop_channel_hopping(void) {
    if (channel_hop_timer) {
        esp_timer_stop(channel_hop_timer);
        esp_timer_delete(channel_hop_timer);
        channel_hop_timer = NULL;
    }
}

static pineap_network_t *find_or_create_network(const uint8_t *bssid) {
    if (pineap_networks == NULL) {
        return NULL;
    }

    for (int i = 0; i < pineap_network_count; i++) {
        if (compare_bssid(pineap_networks[i].bssid, bssid)) {
            return &pineap_networks[i];
        }
    }

    if (pineap_network_count < MAX_PINEAP_NETWORKS) {
        pineap_network_t *network = &pineap_networks[pineap_network_count++];
        memcpy(network->bssid, bssid, 6);
        network->ssid_count = 0;
        network->is_pineap = false;
        network->has_pineapple_oui = is_pineapple_oui(bssid);
        network->oui_logged = false;
        network->first_seen = time(NULL);
        network->log_due_ms = 0;
        network->log_pending = false;
        return network;
    }

    return NULL;
}

static uint32_t hash_ssid(const char *ssid) {
    uint32_t hash = 5381;
    int c;
    while ((c = *ssid++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

static bool ssid_hash_exists(pineap_network_t *network, uint32_t hash) {
    for (int i = 0; i < network->ssid_count; i++) {
        if (network->ssid_hashes[i] == hash) {
            return true;
        }
    }
    return false;
}

static void wardrive_send_probe_request(void) {
    // Broadcast probe request frame
    uint8_t probe_req[] = {
        0x40, 0x00,                         // Frame Control: Probe Request
        0x00, 0x00,                         // Duration
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // Destination: broadcast
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Source: filled below
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // BSSID: broadcast
        0x00, 0x00,                         // Sequence Control
        // SSID IE (wildcard - empty means "any SSID")
        0x00, 0x00,
        // Supported Rates IE
        0x01, 0x08, 0x02, 0x04, 0x0b, 0x16, 0x0c, 0x12, 0x18, 0x24,
        // Extended Supported Rates IE
        0x32, 0x04, 0x30, 0x48, 0x60, 0x6c,
        // DS Parameter Set (current channel)
        0x03, 0x01, 0x01  // Channel placeholder
    };
    
    // Get our MAC address
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    memcpy(&probe_req[10], mac, 6);
    
    // Set current channel in DS Parameter Set
    probe_req[sizeof(probe_req) - 1] = wardrive_channel;
    
    esp_wifi_80211_tx(WIFI_IF_STA, probe_req, sizeof(probe_req), false);
}

static int hop_count = 0;

static void wardrive_hop_timer_callback(void *arg) {
    if (!wardriving_hopping_active)
        return;

    if (wardrive_role == WARDRIVE_ROLE_PRIMARY && wardrive_peer_assist_active && !esp_comm_manager_is_connected()) {
        wardrive_peer_assist_active = false;
        gps_manager_set_peer_gps_preferred(false);
        gps_manager_clear_peer_fix();
        wardrive_build_channel_list();
        wardrive_channel_idx = 0;
        if (wardrive_channel_count > 0) {
            wardrive_channel = wardrive_channels[0];
            (void)esp_wifi_set_channel(wardrive_channel, WIFI_SECOND_CHAN_NONE);
        }
        glog("Wardrive: peer helper link lost, continuing local scan only\n");
        wardrive_apply_hop_interval();
    }

    if (wardrive_channel_count == 0) return;
    wardrive_channel_idx = (wardrive_channel_idx + 1) % wardrive_channel_count;
    wardrive_channel = wardrive_channels[wardrive_channel_idx];
    esp_wifi_set_channel(wardrive_channel, WIFI_SECOND_CHAN_NONE);
    
    // Send probe request to trigger AP responses
    wardrive_send_probe_request();
    
    hop_count++;
    if (hop_count % 200 == 0) {
        ESP_LOGI(TAG, "Wardrive hopped to channel %d (hop #%d)", wardrive_channel, hop_count);
    }
}

static esp_err_t start_wardrive_channel_hopping(void) {
    esp_timer_create_args_t timer_args = {.callback = wardrive_hop_timer_callback,
                                          .name = "wardrive_hop"};

    if (wardrive_hop_timer == NULL) {
        esp_err_t err = esp_timer_create(&timer_args, &wardrive_hop_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create wardrive hop timer: %s", esp_err_to_name(err));
            return err;
        }
    }

    wardrive_build_channel_list();
    wardrive_channel_idx = 0;
    wardrive_channel = wardrive_channels[0];
    wardriving_hopping_active = true;
    hop_count = 0;
    
    esp_err_t err = esp_wifi_set_channel(wardrive_channel, WIFI_SECOND_CHAN_NONE);
    ESP_LOGI(TAG, "Wardrive starting on channel %d (set_channel: %s)", wardrive_channel, esp_err_to_name(err));
    
    uint32_t interval_ms = wardrive_get_hop_interval_ms();
    err = esp_timer_start_periodic(wardrive_hop_timer, (uint64_t)interval_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start wardrive hop timer: %s", esp_err_to_name(err));
        toast_show("Wardrive hop failed", TOAST_ERROR);
        return err;
    }
    ESP_LOGI(TAG,
             "Wardrive channel hopping started (%d channels, %lums interval, role=%s)",
             wardrive_channel_count,
             (unsigned long)interval_ms,
             wardrive_role == WARDRIVE_ROLE_PRIMARY ? "primary" : "helper");
    return ESP_OK;
}

static void stop_wardrive_channel_hopping(void) {
    wardriving_hopping_active = false;
    if (wardrive_hop_timer) {
        esp_timer_stop(wardrive_hop_timer);
        esp_timer_delete(wardrive_hop_timer);
        wardrive_hop_timer = NULL;
    }
}

#define WARDRIVE_HEARTBEAT_INTERVAL_MS 10000

static void wardrive_heartbeat_cb(void *arg) {
    (void)arg;

    if (!wardriving_hopping_active) {
        return;
    }

    gps_t gps_snapshot = {0};
    bool using_peer_gps = false;
    bool have_active_gps = gps_manager_get_active_gps_snapshot(&gps_snapshot, &using_peer_gps);
    bool peer_preferred = gps_manager_is_peer_gps_preferred();
    gps_t *gps_local = NULL;
    const char *fix_status = "No GPS";
    char fix_status_buf[24] = {0};
    uint8_t sats = 0;

    if (have_active_gps) {
        gps_local = &gps_snapshot;
    } else if (!peer_preferred) {
        static gps_t local_snapshot = {0};
        if (gps_manager_get_local_gps_snapshot(&local_snapshot)) {
            gps_local = &local_snapshot;
        }
    }

    if (gps_local != NULL) {
        sats = gps_local->sats_in_use;
        if (!gps_local->valid || gps_local->fix < GPS_FIX_GPS || gps_local->fix_mode < GPS_MODE_2D) {
            fix_status = using_peer_gps ? "Peer No Fix" : "No Fix";
        } else if (gps_local->fix_mode == GPS_MODE_2D) {
            fix_status = using_peer_gps ? "Peer 2D" : "2D";
        } else if (gps_local->fix_mode == GPS_MODE_3D) {
            fix_status = using_peer_gps ? "Peer 3D" : "3D";
        } else {
            fix_status = using_peer_gps ? "Peer Fix" : "Fix";
        }

        if (!wardrive_is_valid_date(&gps_local->date)) {
            snprintf(fix_status_buf, sizeof(fix_status_buf), "%s/NoDate", fix_status);
            fix_status = fix_status_buf;
        }
    } else if (peer_preferred) {
        fix_status = "Peer Stale";
    }

    uint32_t up_s = 0;
    if (wardrive_start_us != 0) {
        up_s = (uint32_t)((esp_timer_get_time() - wardrive_start_us) / 1000000LL);
    }

    uint32_t up_m = up_s / 60;
    uint32_t up_rem_s = up_s % 60;

    size_t pending = csv_get_pending_bytes();
    size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t heap_largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    if (wardrive_role == WARDRIVE_ROLE_HELPER) {
        glog("Wardrive: ap=%lu logged=%lu/%lu gpsrej=%lu helper=%lu/%lu tx(n/p/r/t/s)=%lu/%lu/%lu/%lu/%lu send(ok/fail)=%lu/%lu peergps(rx/fix tx_ok/fail)=%lu/%lu %lu/%lu ch=%u up=%lum%02lus gps=%s/%u pending=%uB heap=%u/%uB\n",
             (unsigned long)wardrive_wifi_frames_seen,
             (unsigned long)wardrive_log_ok,
             (unsigned long)wardrive_log_attempts,
             (unsigned long)wardrive_gps_rejected,
             (unsigned long)wardrive_helper_merged_ok,
             (unsigned long)wardrive_helper_rx_observations,
             (unsigned long)wardrive_helper_tx_new,
             (unsigned long)wardrive_helper_tx_ssid_promo,
             (unsigned long)wardrive_helper_tx_rssi,
             (unsigned long)wardrive_helper_tx_refresh,
             (unsigned long)wardrive_helper_tx_suppressed,
             (unsigned long)wardrive_helper_stream_send_ok,
             (unsigned long)wardrive_helper_stream_send_fail,
             (unsigned long)peer_gps_stream_rx_packets,
             (unsigned long)peer_gps_stream_rx_fix_packets,
             (unsigned long)peer_gps_stream_tx_ok,
             (unsigned long)peer_gps_stream_tx_fail,
             (unsigned)wardrive_channel,
             (unsigned long)up_m,
             (unsigned long)up_rem_s,
             fix_status,
             (unsigned)sats,
             (unsigned)pending,
             (unsigned)heap_free,
             (unsigned)heap_largest);
    } else {
        glog("Wardrive: ap=%lu logged=%lu/%lu gpsrej=%lu helper=%lu/%lu peergps(rx/fix tx_ok/fail)=%lu/%lu %lu/%lu ch=%u up=%lum%02lus gps=%s/%u pending=%uB heap=%u/%uB\n",
             (unsigned long)wardrive_wifi_frames_seen,
             (unsigned long)wardrive_log_ok,
             (unsigned long)wardrive_log_attempts,
             (unsigned long)wardrive_gps_rejected,
             (unsigned long)wardrive_helper_merged_ok,
             (unsigned long)wardrive_helper_rx_observations,
             (unsigned long)peer_gps_stream_rx_packets,
             (unsigned long)peer_gps_stream_rx_fix_packets,
             (unsigned long)peer_gps_stream_tx_ok,
             (unsigned long)peer_gps_stream_tx_fail,
             (unsigned)wardrive_channel,
             (unsigned long)up_m,
             (unsigned long)up_rem_s,
             fix_status,
             (unsigned)sats,
             (unsigned)pending,
             (unsigned)heap_free,
             (unsigned)heap_largest);
    }
}

static void start_wardrive_heartbeat(void) {
    wardrive_start_us = esp_timer_get_time();
    wardrive_wifi_frames_seen = 0;
    wardrive_ble_advs_seen = 0;
    wardrive_log_attempts = 0;
    wardrive_log_ok = 0;
    wardrive_gps_rejected = 0;
    wardrive_helper_rx_observations = 0;
    wardrive_helper_merged_ok = 0;
    wardrive_helper_tx_new = 0;
    wardrive_helper_tx_ssid_promo = 0;
    wardrive_helper_tx_rssi = 0;
    wardrive_helper_tx_refresh = 0;
    wardrive_helper_tx_suppressed = 0;
    wardrive_helper_stream_send_ok = 0;
    wardrive_helper_stream_send_fail = 0;
    peer_gps_stream_tx_ok = 0;
    peer_gps_stream_tx_fail = 0;
    peer_gps_stream_rx_packets = 0;
    peer_gps_stream_rx_fix_packets = 0;
    memset(wardrive_helper_dedupe, 0, sizeof(wardrive_helper_dedupe));
    wardrive_helper_dedupe_idx = 0;
#ifndef CONFIG_IDF_TARGET_ESP32S2
    memset(ble_wd_seen_hashes, 0, sizeof(ble_wd_seen_hashes));
    ble_wd_seen_idx = 0;
    ble_wd_unique_count = 0;
#endif

    if (!wardrive_heartbeat_timer) {
        const esp_timer_create_args_t timer_args = {
            .callback = &wardrive_heartbeat_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "wardrive_hb"
        };
        esp_err_t err = esp_timer_create(&timer_args, &wardrive_heartbeat_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create wardrive heartbeat timer: %s", esp_err_to_name(err));
            return;
        }
    }

    (void)esp_timer_stop(wardrive_heartbeat_timer);
    esp_err_t start_err = esp_timer_start_periodic(wardrive_heartbeat_timer,
                                                    (uint64_t)WARDRIVE_HEARTBEAT_INTERVAL_MS * 1000ULL);
    if (start_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start wardrive heartbeat timer: %s", esp_err_to_name(start_err));
    }
}

static void stop_wardrive_heartbeat(void) {
    if (wardrive_heartbeat_timer) {
        esp_timer_stop(wardrive_heartbeat_timer);
        esp_timer_delete(wardrive_heartbeat_timer);
        wardrive_heartbeat_timer = NULL;
    }
}

static void pineap_log_worker_task(void *arg) {
    (void)arg;
    pineap_log_event_t ev;

    for (;;) {
        if (xQueueReceive(s_pineap_log_queue, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        uint32_t now = now_ms_u32();
        if (ev.due_ms > now) {
            vTaskDelay(pdMS_TO_TICKS(ev.due_ms - now));
        }

        if (!pineap_detection_active || pineap_networks == NULL) {
            continue;
        }

        if (ev.network_index >= (uint8_t)pineap_network_count) {
            continue;
        }

        pineap_network_t *network = &pineap_networks[ev.network_index];
        if (!network->log_pending || network->log_due_ms != ev.due_ms) {
            continue;
        }

        char mac_str[18];
        format_mac_address(network->bssid, mac_str, sizeof(mac_str), false);

        char ssids_str[256] = {0};
        int valid_ssid_count = build_recent_ssids_string(network, ssids_str, sizeof(ssids_str));

        if (valid_ssid_count >= MIN_SSIDS_FOR_DETECTION) {
            pulse_once(&rgb_manager, 255, 0, 255);

            for (int i = 0; i < pineap_network_count; i++) {
                if (i != (network - pineap_networks) &&
                    strcasecmp(network->recent_ssids[0], pineap_networks[i].recent_ssids[0]) == 0) {
                    char other_mac_str[18];
                    format_mac_address(pineap_networks[i].bssid, other_mac_str, sizeof(other_mac_str), false);
                    glog("Evil Twin Detected:\nSame SSID '%.100s'\nfrom BSSID %s and\n%s\n",
                         network->recent_ssids[0], mac_str, other_mac_str);
                }
            }

            log_pineap_details(network, "Pineapple detected!", ssids_str, valid_ssid_count);
        }

        network->log_pending = false;
        network->log_due_ms = 0;
    }
}

static void start_pineap_log_worker(void) {
    if (s_pineap_log_queue == NULL) {
        s_pineap_log_queue = xQueueCreate(PINEAP_LOG_QUEUE_LEN, sizeof(pineap_log_event_t));
    }
    if (s_pineap_log_queue != NULL && s_pineap_log_task == NULL) {
        if (xTaskCreate(pineap_log_worker_task, "pineap_logw", 1792, NULL, 1, &s_pineap_log_task) != pdPASS) {
            s_pineap_log_task = NULL;
        }
    }
}

static void stop_pineap_log_worker(void) {
    if (s_pineap_log_task != NULL) {
        vTaskDelete(s_pineap_log_task);
        s_pineap_log_task = NULL;
    }
    if (s_pineap_log_queue != NULL) {
        vQueueDelete(s_pineap_log_queue);
        s_pineap_log_queue = NULL;
    }
}

void start_pineap_detection(void) {
    if (!allocate_pineap_tables()) {
        glog("PineAP: failed to allocate detection tables\n");
        return;
    }

    pineap_detection_active = true;
    pineap_network_count = 0;
    blacklist_count = 0;
    memset(pineap_networks, 0, MAX_PINEAP_NETWORKS * sizeof(*pineap_networks));
    memset(blacklist, 0, MAX_PINEAP_NETWORKS * sizeof(*blacklist));
    start_pineap_log_worker();
    wardrive_build_channel_list();
    wardrive_channel_idx = 0;
    current_channel = wardrive_channels[0];
    start_channel_hopping();
}

void stop_pineap_detection(void) {
    pineap_detection_active = false;
    stop_channel_hopping();
    stop_pineap_log_worker();
    free_pineap_tables();
}

static void wardrive_stream_rx_cb(uint8_t channel, const uint8_t *data, size_t length, void *user_data) {
    (void)channel;
    (void)user_data;

    if (!data || length < (1 + 1 + 1 + 1 + 6 + 1)) {
        return;
    }
    if (!wardriving_hopping_active || wardrive_role != WARDRIVE_ROLE_PRIMARY) {
        return;
    }

    size_t pos = 0;
    uint8_t version = data[pos++];
    if (version != WARDRIVE_STREAM_VERSION && version != WARDRIVE_STREAM_VERSION_LEGACY) {
        return;
    }

    uint8_t channel_num = data[pos++];
    int8_t rssi = (int8_t)data[pos++];
    uint8_t auth_code = data[pos++];
    const uint8_t *bssid = data + pos;
    pos += 6;
    uint8_t ssid_len = data[pos++];
    if (ssid_len > 32 || (pos + ssid_len) > length) {
        return;
    }

    char ssid[33] = {0};
    if (ssid_len > 0) {
        memcpy(ssid, data + pos, ssid_len);
        ssid[ssid_len] = '\0';
    }
    pos += ssid_len;

    gps_peer_fix_t peer_fix = {0};
    bool peer_fix_present = false;
    bool peer_fix_has_coords = false;
    if (version >= WARDRIVE_STREAM_VERSION) {
        if (pos >= length) {
            return;
        }

        uint8_t gps_flags = data[pos++];
        if (gps_flags & WARDRIVE_STREAM_FLAG_GPS_PRESENT) {
            if ((length - pos) < (4 + 4 + 2 + 1 + 1 + 1 + 1 + 2)) {
                return;
            }

            int32_t lat_e7 = wardrive_get_i32le(data + pos);
            pos += 4;
            int32_t lon_e7 = wardrive_get_i32le(data + pos);
            pos += 4;
            int16_t alt_dm = wardrive_get_i16le(data + pos);
            pos += 2;
            uint8_t sats_in_use = data[pos++];
            uint8_t sats_in_view = data[pos++];
            uint8_t fix = data[pos++];
            uint8_t fix_mode = data[pos++];
            uint16_t hdop_x10 = (uint16_t)data[pos] | ((uint16_t)data[pos + 1] << 8);
            pos += 2;
            uint8_t day = 0;
            uint8_t month = 0;
            uint16_t year = 0;
            uint8_t hour = 0;
            uint8_t minute = 0;
            uint8_t second = 0;
            if ((length - pos) >= 7) {
                day = data[pos++];
                month = data[pos++];
                year = (uint16_t)data[pos] | ((uint16_t)data[pos + 1] << 8);
                pos += 2;
                hour = data[pos++];
                minute = data[pos++];
                second = data[pos++];
            }

            peer_fix_present = true;
            peer_fix.valid = (gps_flags & WARDRIVE_STREAM_FLAG_GPS_FIX) != 0;
            peer_fix.fix = (gps_fix_t)fix;
            peer_fix.fix_mode = (gps_fix_mode_t)fix_mode;
            peer_fix.date_valid = (gps_flags & WARDRIVE_STREAM_FLAG_GPS_DATE_VALID) != 0;
            peer_fix.time_valid = (gps_flags & WARDRIVE_STREAM_FLAG_GPS_TIME_VALID) != 0;
            peer_fix.date.day = day;
            peer_fix.date.month = month;
            peer_fix.date.year = year;
            peer_fix.tim.hour = hour;
            peer_fix.tim.minute = minute;
            peer_fix.tim.second = second;
            peer_fix.tim.thousand = 0;
            peer_fix.sats_in_use = sats_in_use;
            peer_fix.sats_in_view = sats_in_view;
            peer_fix.latitude = (float)lat_e7 / 10000000.0f;
            peer_fix.longitude = (float)lon_e7 / 10000000.0f;
            peer_fix.altitude = (float)alt_dm / 10.0f;
            peer_fix.speed = 0.0f;
            peer_fix.course = 0.0f;
            peer_fix.hdop = (float)hdop_x10 / 10.0f;

            peer_fix_has_coords =
                peer_fix.valid &&
                peer_fix.latitude >= -90.0f && peer_fix.latitude <= 90.0f &&
                peer_fix.longitude >= -180.0f && peer_fix.longitude <= 180.0f;

            gps_manager_update_peer_fix(&peer_fix);
        }
    }

    wardrive_helper_rx_observations++;

    wardriving_data_t wardriving_data = {0};
    wardriving_data.ble_data.is_ble_device = false;
    if (ssid_len == 0) {
        strncpy(wardriving_data.ssid, "<hidden>", sizeof(wardriving_data.ssid) - 1);
    } else {
        strncpy(wardriving_data.ssid, ssid, sizeof(wardriving_data.ssid) - 1);
    }
    snprintf(wardriving_data.bssid,
             sizeof(wardriving_data.bssid),
             "%02x:%02x:%02x:%02x:%02x:%02x",
             bssid[0],
             bssid[1],
             bssid[2],
             bssid[3],
             bssid[4],
             bssid[5]);
    wardriving_data.rssi = rssi;
    wardriving_data.channel = channel_num;
    strncpy(wardriving_data.encryption_type,
            wardrive_auth_code_to_string(auth_code),
            sizeof(wardriving_data.encryption_type) - 1);
    if (peer_fix_present && peer_fix_has_coords) {
        wardriving_data.latitude = peer_fix.latitude;
        wardriving_data.longitude = peer_fix.longitude;
        wardriving_data.altitude = peer_fix.altitude;
        wardriving_data.accuracy = peer_fix.hdop * 5.0f;
    }

    wardrive_log_attempts++;
    esp_err_t err = gps_manager_log_wardriving_data(&wardriving_data);
    if (err == ESP_ERR_INVALID_STATE) {
        wardrive_gps_rejected++;
    } else if (err == ESP_OK) {
        wardrive_log_ok++;
        wardrive_helper_merged_ok++;
    }
}

static void gps_stream_rx_cb(uint8_t channel, const uint8_t *data, size_t length, void *user_data) {
    (void)channel;
    (void)user_data;

    if (!data || length < (1 + 1 + 4 + 4 + 2 + 1 + 1 + 1 + 1 + 2 + 2 + 2)) {
        return;
    }

    size_t pos = 0;
    uint8_t version = data[pos++];
    if (version != GPS_STREAM_VERSION) {
        return;
    }

    uint8_t flags = data[pos++];
    int32_t lat_e7 = wardrive_get_i32le(data + pos);
    pos += 4;
    int32_t lon_e7 = wardrive_get_i32le(data + pos);
    pos += 4;
    int16_t alt_dm = wardrive_get_i16le(data + pos);
    pos += 2;
    uint8_t sats_in_use = data[pos++];
    uint8_t sats_in_view = data[pos++];
    uint8_t fix = data[pos++];
    uint8_t fix_mode = data[pos++];
    int16_t hdop_x10 = wardrive_get_i16le(data + pos);
    pos += 2;
    int16_t speed_x100 = wardrive_get_i16le(data + pos);
    pos += 2;
    int16_t course_x100 = wardrive_get_i16le(data + pos);
    pos += 2;
    uint8_t day = 0;
    uint8_t month = 0;
    uint16_t year = 0;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
    if ((length - pos) >= 7) {
        day = data[pos++];
        month = data[pos++];
        year = (uint16_t)data[pos] | ((uint16_t)data[pos + 1] << 8);
        pos += 2;
        hour = data[pos++];
        minute = data[pos++];
        second = data[pos++];
    }

    gps_peer_fix_t peer_fix = {0};
    peer_fix.valid = (flags & GPS_STREAM_FLAG_FIX) != 0;
    peer_fix.fix = (gps_fix_t)fix;
    peer_fix.fix_mode = (gps_fix_mode_t)fix_mode;
    peer_fix.date_valid = (flags & GPS_STREAM_FLAG_DATE_VALID) != 0;
    peer_fix.time_valid = (flags & GPS_STREAM_FLAG_TIME_VALID) != 0;
    peer_fix.date.day = day;
    peer_fix.date.month = month;
    peer_fix.date.year = year;
    peer_fix.tim.hour = hour;
    peer_fix.tim.minute = minute;
    peer_fix.tim.second = second;
    peer_fix.tim.thousand = 0;
    peer_fix.sats_in_use = sats_in_use;
    peer_fix.sats_in_view = sats_in_view;
    peer_fix.latitude = (float)lat_e7 / 10000000.0f;
    peer_fix.longitude = (float)lon_e7 / 10000000.0f;
    peer_fix.altitude = (float)alt_dm / 10.0f;
    peer_fix.hdop = (float)hdop_x10 / 10.0f;
    peer_fix.speed = (float)speed_x100 / 100.0f;
    peer_fix.course = (float)course_x100 / 100.0f;

    gps_manager_update_peer_fix(&peer_fix);
    peer_gps_stream_rx_packets++;
    if (peer_fix.valid) {
        peer_gps_stream_rx_fix_packets++;
    }
}

void wardriving_register_stream_handler(void) {
    bool ok = esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_WARDRIVE,
                                                       wardrive_stream_rx_cb,
                                                       NULL);
    bool gps_ok = esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_GPS,
                                                           gps_stream_rx_cb,
                                                           NULL);
    ESP_LOGI(TAG, "Wardrive stream handler: %s", ok ? "OK" : "FAIL");
    ESP_LOGI(TAG, "Peer GPS stream handler: %s", gps_ok ? "OK" : "FAIL");

    if (peer_gps_stream_task_handle == NULL) {
        peer_gps_stream_stack = heap_caps_malloc(3072 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
        peer_gps_stream_tcb = malloc(sizeof(StaticTask_t));
        if (peer_gps_stream_stack && peer_gps_stream_tcb) {
            peer_gps_stream_task_handle = xTaskCreateStatic(
                peer_gps_stream_task,
                "peer_gps_stream",
                3072,
                NULL,
                3,
                peer_gps_stream_stack,
                peer_gps_stream_tcb);
            ESP_LOGI(TAG, "Peer GPS stream task stack allocated from PSRAM: %d bytes", 
                     (int)(3072 * sizeof(StackType_t)));
        }
        if (!peer_gps_stream_task_handle) {
            if (peer_gps_stream_stack) {
                free(peer_gps_stream_stack);
                peer_gps_stream_stack = NULL;
            }
            if (peer_gps_stream_tcb) {
                free(peer_gps_stream_tcb);
                peer_gps_stream_tcb = NULL;
            }
            ESP_LOGW(TAG, "Peer GPS stream task create failed");
        }
    }
}

bool wardriving_get_helper_channel_plan_csv(char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return false;
    }

    uint8_t full_channels[WIFI_CHANNELS_MAX] = {0};
    uint8_t channels_24[WIFI_CHANNELS_MAX] = {0};
    uint8_t full_count = wardrive_build_full_channel_list(full_channels);
    uint8_t channels_24_count = 0;

    for (uint8_t i = 0; i < full_count; i++) {
        if (full_channels[i] <= 14 && channels_24_count < WIFI_CHANNELS_MAX) {
            channels_24[channels_24_count++] = full_channels[i];
        }
    }

    uint8_t plan[WIFI_CHANNELS_MAX] = {0};
    uint8_t plan_count = 0;
    if (channels_24_count > 0) {
        memcpy(plan, channels_24, channels_24_count);
        plan_count = channels_24_count;
    } else {
        for (uint8_t i = 1; i < full_count && plan_count < WIFI_CHANNELS_MAX; i += 2) {
            plan[plan_count++] = full_channels[i];
        }
    }

    if (plan_count == 0) {
        out[0] = '\0';
        return false;
    }

    size_t pos = 0;
    for (uint8_t i = 0; i < plan_count; i++) {
        int wrote = snprintf(out + pos, out_len - pos, (i == 0) ? "%u" : ",%u", (unsigned)plan[i]);
        if (wrote <= 0 || (size_t)wrote >= (out_len - pos)) {
            out[0] = '\0';
            return false;
        }
        pos += (size_t)wrote;
    }

    return true;
}

bool wardriving_set_helper_channels_from_csv(const char *csv) {
    if (!csv || csv[0] == '\0') {
        wardrive_forced_helper_channel_count = 0;
        return true;
    }

    uint8_t parsed[WIFI_CHANNELS_MAX] = {0};
    uint8_t count = wardrive_parse_channel_csv(csv, parsed, WIFI_CHANNELS_MAX);
    if (count == 0) {
        wardrive_forced_helper_channel_count = 0;
        return false;
    }

    memcpy(wardrive_forced_helper_channels, parsed, count);
    wardrive_forced_helper_channel_count = count;

    if (wardrive_role == WARDRIVE_ROLE_HELPER) {
        wardrive_build_channel_list();
        wardrive_channel_idx = 0;
        if (wardrive_channel_count > 0) {
            wardrive_channel = wardrive_channels[0];
            (void)esp_wifi_set_channel(wardrive_channel, WIFI_SECOND_CHAN_NONE);
        }
        wardrive_apply_hop_interval();
    }

    return true;
}

void start_wardriving(void) {
    wardrive_role = WARDRIVE_ROLE_PRIMARY;
    wardrive_forced_helper_channel_count = 0;
    gps_manager_set_peer_gps_preferred(false);
    gps_manager_clear_peer_fix();
    start_wardrive_channel_hopping();
    start_wardrive_heartbeat();
}

void start_wardriving_helper(void) {
    wardrive_role = WARDRIVE_ROLE_HELPER;
    wardrive_peer_assist_active = false;
    gps_manager_set_peer_gps_preferred(false);
    gps_manager_clear_peer_fix();
    start_wardrive_channel_hopping();
    start_wardrive_heartbeat();
}

void stop_wardriving(void) {
    stop_wardrive_heartbeat();
    stop_wardrive_channel_hopping();
    wardrive_role = WARDRIVE_ROLE_PRIMARY;
    wardrive_peer_assist_active = false;
    wardrive_forced_helper_channel_count = 0;
    gps_manager_set_peer_gps_preferred(false);
    gps_manager_clear_peer_fix();
}

void wardriving_set_peer_assist(bool enabled) {
    bool changed = (wardrive_peer_assist_active != enabled);
    wardrive_peer_assist_active = enabled;
    gps_manager_set_peer_gps_preferred(enabled);
    if (!enabled) {
        gps_manager_clear_peer_fix();
    }
    if (changed && wardrive_role == WARDRIVE_ROLE_PRIMARY) {
        wardrive_build_channel_list();
        wardrive_channel_idx = 0;
        if (wardrive_channel_count > 0) {
            wardrive_channel = wardrive_channels[0];
            (void)esp_wifi_set_channel(wardrive_channel, WIFI_SECOND_CHAN_NONE);
        }
        wardrive_apply_hop_interval();
    }
}

bool wardriving_is_helper_mode(void) {
    return wardrive_role == WARDRIVE_ROLE_HELPER;
}

uint32_t wardriving_get_ap_count(void) {
    return wardrive_wifi_frames_seen;
}

#define IRAM_PRINTF(fmt, ...) do { \
    static const char flash_fmt[] STORE_STR_ATTR = fmt; \
    esp_rom_printf(flash_fmt, ##__VA_ARGS__); \
} while(0)

// Helper function to check if SSID is valid and unique
static bool is_valid_unique_ssid(const char *new_ssid, pineap_network_t *network) {
    // Check if SSID is empty or just whitespace
    if (strlen(new_ssid) == 0)
        return false;

    bool all_whitespace = true;
    for (const char *p = new_ssid; *p; p++) {
        if (!isspace((unsigned char)*p)) {
            all_whitespace = false;
            break;
        }
    }
    if (all_whitespace)
        return false;

    // Check if this SSID is already in our recent list
    for (int i = 0; i < network->ssid_count && i < RECENT_SSID_COUNT; i++) {
        if (strcasecmp(network->recent_ssids[i], new_ssid) == 0) {
            return false; // SSID already exists (case insensitive)
        }
    }

    return true;
}

static int build_recent_ssids_string(const pineap_network_t *network, char *out, size_t out_size) {
    if (!network || !out || out_size == 0) {
        return 0;
    }

    out[0] = '\0';
    size_t len = 0;
    int count = 0;

    for (int i = 0; i < network->ssid_count && i < RECENT_SSID_COUNT; i++) {
        const char *ssid = network->recent_ssids[i];
        if (!ssid || ssid[0] == '\0')
            continue;

        if (len < out_size - 1 && count > 0) {
            if (len <= out_size - 3) {
                out[len++] = ',';
                out[len++] = ' ';
            } else {
                break;
            }
        }

        size_t ssid_len = strnlen(ssid, 32);
        size_t avail = out_size - len - 1;
        if (avail == 0)
            break;
        size_t to_copy = ssid_len < avail ? ssid_len : avail;
        memcpy(out + len, ssid, to_copy);
        len += to_copy;
        out[len] = '\0';

        count++;
    }

    return count;
}

static void log_pineap_details(pineap_network_t *network,
                               const char *title,
                               const char *ssids_str,
                               int ssid_count) {
    if (!network)
        return;

    char mac_str[18];
    format_mac_address(network->bssid, mac_str, sizeof(mac_str), false);
    const char *heading = title && title[0] ? title : "Pineapple detected!";

    IRAM_PRINTF("\n%s\nBSSID: %s\n", heading, mac_str);
    IRAM_PRINTF("Channel: %d\n", network->last_channel);
    IRAM_PRINTF("RSSI: %d\n", network->last_rssi);
    IRAM_PRINTF("SSIDs (%d): %s\n", ssid_count, ssids_str ? ssids_str : "");

    glog("\n%s\n", heading);
    glog("BSSID: %s\n", mac_str);
    glog("Channel: %d\n", network->last_channel);
    glog("RSSI: %d\n", network->last_rssi);
    glog("SSIDs (%d): %s\n", ssid_count, ssids_str ? ssids_str : "");

}

static void log_oui_match_notice(pineap_network_t *network) {
    if (!network || !network->has_pineapple_oui || network->oui_logged)
        return;

    char ssids_str[256] = {0};
    int valid_ssids = build_recent_ssids_string(network, ssids_str, sizeof(ssids_str));
    log_pineap_details(network, "Pineapple OUI match!", ssids_str, valid_ssids);
    network->oui_logged = true;
}

void wifi_pineap_detector_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!pineap_detection_active || type != WIFI_PKT_MGMT)
        return;

    const wifi_promiscuous_pkt_t *ppkt = (wifi_promiscuous_pkt_t *)buf;
    const wifi_ieee80211_packet_t *ipkt = (wifi_ieee80211_packet_t *)ppkt->payload;
    wifi_ieee80211_mac_hdr_t hdr_copy;
    memcpy(&hdr_copy, &ipkt->hdr, sizeof(wifi_ieee80211_mac_hdr_t));  // Copy to avoid unaligned pointer
    const wifi_ieee80211_mac_hdr_t *hdr = &hdr_copy;

    // Only process beacon frames
    if (!is_beacon_packet(ppkt))
        return;

    // Early filtering
    if (!is_packet_valid(ppkt, type)) {
        return;
    }

    // Channel filtering
    if (!is_on_target_channel(ppkt, current_channel)) {
        return;
    }

    // Find or create network
    pineap_network_t *network = find_or_create_network(hdr->addr3);
    if (!network)
        return;

    // Update channel and RSSI
    network->last_channel = ppkt->rx_ctrl.channel;
    network->last_rssi = ppkt->rx_ctrl.rssi;

    log_oui_match_notice(network);

    // Extract SSID from beacon
    const uint8_t *payload = ppkt->payload;
    int len = ppkt->rx_ctrl.sig_len;

    // Skip fixed parameters (24 bytes header + 12 bytes fixed params)
    int index = 36;
    if (index + 2 > len)
        return;

    // Look specifically for SSID element (ID = 0)
    if (payload[index] != 0)
        return;

    uint8_t ie_len = payload[index + 1];
    if (ie_len > 32 || index + 2 + ie_len > len)
        return;

    // Get SSID
    char ssid[33] = {0};
    memcpy(ssid, &payload[index + 2], ie_len);
    ssid[ie_len] = '\0';
    trim_trailing(ssid);

    // Only proceed if this is a valid and unique SSID
    if (!is_valid_unique_ssid(ssid, network))
        return;

    uint32_t ssid_hash = hash_ssid(ssid);

    // If this is a new SSID hash for this BSSID, add it
    if (!ssid_hash_exists(network, ssid_hash) && network->ssid_count < MAX_SSIDS_PER_BSSID) {
        network->ssid_hashes[network->ssid_count++] = ssid_hash;

        // Add to recent SSIDs circular buffer
        strncpy(network->recent_ssids[network->recent_ssid_index], ssid, 32);
        network->recent_ssid_index = (network->recent_ssid_index + 1) % RECENT_SSID_COUNT;

        // If we detect multiple SSIDs from same BSSID, mark as potential Pineap
        if (network->ssid_count >= MIN_SSIDS_FOR_DETECTION &&
            (!is_blacklisted(hdr->addr3) || should_update_blacklisted(hdr->addr3))) {

            network->is_pineap = true;
            add_to_blacklist(hdr->addr3);

            if (!network->log_pending && s_pineap_log_queue != NULL) {
                pineap_log_event_t ev = {
                    .network_index = (uint8_t)(network - pineap_networks),
                    .due_ms = now_ms_u32() + 5000
                };
                network->log_due_ms = ev.due_ms;
                network->log_pending = true;
                if (xQueueSend(s_pineap_log_queue, &ev, 0) != pdTRUE) {
                    network->log_pending = false;
                    network->log_due_ms = 0;
                }
            }

            // Write to PCAP if capture is active
            if (pcap_is_capturing()) {
                enqueue_pcap_write(ppkt->payload, ppkt->rx_ctrl.sig_len);
            }
        }
    }
}

static void trim_trailing(char *str) {
    int i = strlen(str) - 1;
    while (i >= 0 && (str[i] == ' ' || str[i] == '\t' || str[i] == '\n' || str[i] == '\r')) {
        str[i] = '\0';
        i--;
    }
}

void gps_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id,
                       void *event_data) {
    switch (event_id) {
    case GPS_UPDATE:
        gps = (gps_t *)event_data;
        gps_manager_update_local_snapshot(gps);
        gps_manager_note_update();
        gps_try_sync_time_from_fix(gps);
        
        // Add status display messages for GPS fix status
        if (gps->valid && gps->fix >= GPS_FIX_GPS && gps->fix_mode >= GPS_MODE_2D && gps->sats_in_use > 0) {
            if (gps->fix_mode == GPS_MODE_3D) {
                status_display_show_status("GPS 3D Lock");
            } else {
                status_display_show_status("GPS 2D Lock");
            }
        } else if (gps->valid && gps->fix == GPS_FIX_INVALID) {
            status_display_show_status("GPS No Fix");
        }
        break;
    default:
        break;
    }
}

static bool compare_bssid(const uint8_t *bssid1, const uint8_t *bssid2) {
    for (int i = 0; i < 6; i++) {
        if (bssid1[i] != bssid2[i]) {
            return false;
        }
    }
    return true;
}

static bool is_pineapple_oui(const uint8_t *bssid) {
    if (!bssid)
        return false;

    if (bssid[1] == 0x13 && bssid[2] == 0x37)
        return true;

    for (size_t i = 0; i < pineapple_oui_count; i++) {
        if (memcmp(bssid, pineapple_ouis[i], 3) == 0) {
            return true;
        }
    }
    return false;
}

bool is_network_duplicate(const char *ssid, const uint8_t *bssid) {
    for (int i = 0; i < detected_network_count; i++) {
        if (strcmp(detected_wps_networks[i].ssid, ssid) == 0 &&
            compare_bssid(detected_wps_networks[i].bssid, bssid)) {
            return true;
        }
    }
    return false;
}

void get_frame_type_and_subtype(const wifi_promiscuous_pkt_t *pkt, uint8_t *frame_type,
                                uint8_t *frame_subtype) {
    if (pkt->rx_ctrl.sig_len < 24) {
        *frame_type = 0xFF;
        *frame_subtype = 0xFF;
        return;
    }

    const uint8_t *frame_ctrl = pkt->payload;

    *frame_type = (frame_ctrl[0] & 0x0C) >> 2;
    *frame_subtype = (frame_ctrl[0] & 0xF0) >> 4;
}

bool is_beacon_packet(const wifi_promiscuous_pkt_t *pkt) {
    uint8_t frame_type, frame_subtype;
    get_frame_type_and_subtype(pkt, &frame_type, &frame_subtype);
    return (frame_type == WIFI_PKT_MGMT && frame_subtype == WIFI_PKT_BEACON);
}

bool is_deauth_packet(const wifi_promiscuous_pkt_t *pkt) {
    uint8_t frame_type, frame_subtype;
    get_frame_type_and_subtype(pkt, &frame_type, &frame_subtype);
    return (frame_type == WIFI_PKT_MGMT && frame_subtype == WIFI_PKT_DEAUTH);
}

bool is_probe_request(const wifi_promiscuous_pkt_t *pkt) {
    uint8_t frame_type, frame_subtype;
    get_frame_type_and_subtype(pkt, &frame_type, &frame_subtype);
    return (frame_type == WIFI_PKT_MGMT && frame_subtype == WIFI_PKT_PROBE_REQ);
}

bool is_probe_response(const wifi_promiscuous_pkt_t *pkt) {
    uint8_t frame_type, frame_subtype;
    get_frame_type_and_subtype(pkt, &frame_type, &frame_subtype);
    return (frame_type == WIFI_PKT_MGMT && frame_subtype == WIFI_PKT_PROBE_RESP);
}

bool is_eapol_response(const wifi_promiscuous_pkt_t *pkt) {
    if (pkt->rx_ctrl.sig_len < 34) {
        return false;
    }

    const uint8_t *frame = pkt->payload;

    if ((frame[30] == 0x88 && frame[31] == 0x8E) || (frame[32] == 0x88 && frame[33] == 0x8E)) {
        return true;
    }

    return false;
}

bool is_pwn_response(const wifi_promiscuous_pkt_t *pkt) {
    const uint8_t *frame = pkt->payload;

    if (frame[0] == 0x80) {
        return true;
    }

    return false;
}

void wifi_raw_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    
    // Early filtering - raw captures everything but still filter junk
    if (type == WIFI_PKT_MISC || pkt->rx_ctrl.sig_len < MIN_PACKET_LENGTH) {
        return;
    }
    
    if (pkt->rx_ctrl.sig_len > 0) {
        enqueue_pcap_write(pkt->payload, pkt->rx_ctrl.sig_len);
    }
}

void wardriving_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) {
        return;
    }

    const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    int len = pkt->rx_ctrl.sig_len;
    if (len < 36) {
        return;
    }

    const uint8_t *payload = pkt->payload;
    const wifi_ieee80211_packet_t *ipkt = (wifi_ieee80211_packet_t *)payload;
    wifi_ieee80211_mac_hdr_t hdr_copy;
    memcpy(&hdr_copy, &ipkt->hdr, sizeof(wifi_ieee80211_mac_hdr_t));
    const wifi_ieee80211_mac_hdr_t *hdr = &hdr_copy;

    uint8_t frame_type = hdr->frame_ctrl & 0xFC;
    if (frame_type != 0x80 && frame_type != 0x50) {
        return;
    }

    wardrive_wifi_frames_seen++;

    int index = 36;
    char ssid[33] = {0};
    bool ssid_ie_seen = false;
    uint8_t bssid[6];
    memcpy(bssid, hdr->addr3, 6);
    if ((bssid[0] & 0x01) != 0) {
        return;
    }
    static const uint8_t zero_bssid[6] = {0};
    static const uint8_t ff_bssid[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    if (memcmp(bssid, zero_bssid, 6) == 0 || memcmp(bssid, ff_bssid, 6) == 0) {
        return;
    }

    int rssi = pkt->rx_ctrl.rssi;
    int channel = pkt->rx_ctrl.channel;
    bool ssid_malformed = false;

    char encryption_type[8] = "OPEN";
    bool found_wpa = false;
    bool found_rsn = false;

    uint16_t capability_info = 0;
    bool privacy_required = false;
    if (len >= 36) {
        capability_info = payload[34] | (payload[35] << 8);
        privacy_required = (capability_info & (1 << 4)) != 0;
    }

    while (index + 1 < len) {
        uint8_t id = payload[index];
        uint8_t ie_len = payload[index + 1];

        /* sanity checks: ensure IE length fits within bounds */
        if (index + 2 + ie_len > len) {
            return;
        }

        if (id == 0 && ie_len <= 32) {
            ssid_ie_seen = true;
            memcpy(ssid, &payload[index + 2], ie_len);
            ssid[ie_len] = '\0';
            trim_trailing(ssid);
            // Sanitize: detect and mark malformed SSIDs
            for (char *p = ssid; *p; p++) {
                if ((uint8_t)*p < 0x20 || (uint8_t)*p > 0x7E) {
                    *p = '?';
                    ssid_malformed = true;
                }
            }
        }

        // DS Parameter Set IE - authoritative AP channel from the beacon itself
        // (rx_ctrl.channel can be unreliable on some targets)
        if (id == 3 && ie_len == 1) {
            channel = payload[index + 2];
        }

        if (id == 48) {
            size_t copy_len = sizeof(encryption_type) - 1;
            strncpy(encryption_type, "WPA2", copy_len);
            encryption_type[copy_len] = '\0';
            found_rsn = true;

            if (ie_len >= 8) {
                const uint8_t *rsn = &payload[index + 2];
                size_t pos = 0;
                pos += 2; // version
                if (pos + 4 > ie_len) goto rsn_done;
                pos += 4; // group cipher
                if (pos + 2 > ie_len) goto rsn_done;
                uint16_t pairwise_count = rsn[pos] | (rsn[pos + 1] << 8);
                pos += 2;
                size_t pairwise_len = pairwise_count * 4;
                if (pos + pairwise_len > ie_len) goto rsn_done;
                pos += pairwise_len;
                if (pos + 2 > ie_len) goto rsn_done;
                uint16_t akm_count = rsn[pos] | (rsn[pos + 1] << 8);
                pos += 2;
                for (uint16_t i = 0; i < akm_count; i++) {
                    if (pos + 4 > ie_len) break;
                    uint32_t akm = rsn[pos] | (rsn[pos + 1] << 8) | (rsn[pos + 2] << 16) |
                                    (rsn[pos + 3] << 24);
                    if (akm == 0x000FAC08) {
                        strncpy(encryption_type, "WPA3", copy_len);
                        encryption_type[copy_len] = '\0';
                        break;
                    } else if (akm == 0x000FAC09) {
                        strncpy(encryption_type, "OWE", copy_len);
                        encryption_type[copy_len] = '\0';
                        break;
                    }
                    pos += 4;
                }
            }
rsn_done:
            
        } else if (id == 221 && ie_len >= 4) {
            uint32_t oui =
                (payload[index + 2] << 16) | (payload[index + 3] << 8) | payload[index + 4];
            uint8_t oui_type = payload[index + 5];
            if (oui == 0x0050f2 && oui_type == 0x01) {
                size_t copy_len = sizeof(encryption_type) - 1;
                strncpy(encryption_type, "WPA", copy_len);
                encryption_type[copy_len] = '\0';
                found_wpa = true;
            }
        }

        index += (2 + ie_len);
        
        // Early exit: stop parsing once we have SSID, channel from DS param, and security
        if (ssid[0] != '\0' && found_rsn) {
            break;
        }
    }

    if (!ssid_ie_seen) {
        return;
    }

    if (!found_rsn && !found_wpa) {
        size_t copy_len = sizeof(encryption_type) - 1;
        if (privacy_required && ssid_ie_seen) {
            strncpy(encryption_type, "WEP", copy_len);
        } else {
            strncpy(encryption_type, "OPEN", copy_len);
        }
        encryption_type[copy_len] = '\0';
    }

    if (ssid_malformed) {
        return;
    }

    if (wardrive_role == WARDRIVE_ROLE_HELPER) {
        const char *tx_ssid = (ssid[0] == '\0') ? "" : ssid;
        if (wardrive_helper_should_send(bssid, (int8_t)rssi, tx_ssid)) {
            bool send_ok = wardrive_send_helper_observation(bssid,
                                                            (uint8_t)channel,
                                                            (int8_t)rssi,
                                                            wardrive_select_auth_code(encryption_type),
                                                            tx_ssid);
            if (send_ok) {
                wardrive_helper_stream_send_ok++;
            } else {
                wardrive_helper_stream_send_fail++;
            }
        }
        return;
    }

    wardriving_data_t wardriving_data = {0};
    wardriving_data.ble_data.is_ble_device = false;  // ensure Wi-Fi entry
    strncpy(wardriving_data.ssid, ssid, sizeof(wardriving_data.ssid) - 1);
    wardriving_data.ssid[sizeof(wardriving_data.ssid) - 1] = '\0'; // Null-terminate
    snprintf(wardriving_data.bssid, sizeof(wardriving_data.bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    wardriving_data.rssi = rssi;
    wardriving_data.channel = channel;
    strncpy(wardriving_data.encryption_type, encryption_type,
            sizeof(wardriving_data.encryption_type) - 1);
    wardriving_data.encryption_type[sizeof(wardriving_data.encryption_type) - 1] = '\0';

    // Log hidden networks with placeholder for WiGLE compatibility
    if (ssid[0] == '\0') {
        strncpy(wardriving_data.ssid, "<hidden>", sizeof(wardriving_data.ssid) - 1);
        wardriving_data.ssid[sizeof(wardriving_data.ssid) - 1] = '\0';
    }

    wardrive_log_attempts++;
    esp_err_t err = gps_manager_log_wardriving_data(&wardriving_data);
    if (err == ESP_ERR_INVALID_STATE) {
        wardrive_gps_rejected++;
    } else if (err == ESP_OK) {
        wardrive_log_ok++;
    }
}

void wifi_probe_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    // Early filtering for management frames only
    if (type != WIFI_PKT_MGMT) return;
    
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    
    // Additional early filtering
    if (!is_packet_valid(pkt, type)) return;
    
    if (pkt->rx_ctrl.sig_len > 0) {
        enqueue_pcap_write(pkt->payload, pkt->rx_ctrl.sig_len);
    }
}

void wifi_beacon_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    // Early filtering for management frames only
    if (type != WIFI_PKT_MGMT) return;
    
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    
    // Additional early filtering
    if (!is_packet_valid(pkt, type)) return;
    
    // Beacon-specific filtering - only capture beacon frames
    uint8_t frame_subtype = (pkt->payload[0] & 0xF0) >> 4;
    if (frame_subtype != WIFI_PKT_BEACON) return;
    
    if (pkt->rx_ctrl.sig_len > 0) {
        enqueue_pcap_write(pkt->payload, pkt->rx_ctrl.sig_len);
    }
}

void wifi_deauth_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    // Early filtering for management frames only
    if (type != WIFI_PKT_MGMT) return;
    
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    
    // Additional early filtering
    if (!is_packet_valid(pkt, type)) return;
    
    // Deauth-specific filtering - only capture deauth/disassoc frames
    uint8_t frame_subtype = (pkt->payload[0] & 0xF0) >> 4;
    if (frame_subtype != WIFI_PKT_DEAUTH && frame_subtype != 0x0A) return; // 0x0A = disassoc
    
    if (pkt->rx_ctrl.sig_len > 0) {
        enqueue_pcap_write(pkt->payload, pkt->rx_ctrl.sig_len);
    }
}

void wifi_pwn_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT)
        return;
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    if (!is_packet_valid(pkt, type)) return;
    if (pkt->rx_ctrl.sig_len > 0) {
        enqueue_pcap_write(pkt->payload, pkt->rx_ctrl.sig_len);
    }
}

void wifi_eapol_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;

    if (type == WIFI_PKT_MISC) return;
    if (pkt->rx_ctrl.sig_len < 24) return;

    if (type == WIFI_PKT_DATA) {
        const uint8_t *frame = pkt->payload;
        int len = pkt->rx_ctrl.sig_len;

        uint16_t fc = frame[0] | (frame[1] << 8);
        uint8_t dsub = (fc >> 4) & 0xF;
        bool qos = (dsub & 0x8) != 0;
        bool to_ds = (fc >> 8) & 0x1;
        bool from_ds = (fc >> 9) & 0x1;

        size_t hdr_len = 24;
        if (to_ds && from_ds) hdr_len = 30;
        if (qos) hdr_len += 2;

        // always write data frames to pcap first, then check for EAPOL
        enqueue_pcap_write(pkt->payload, pkt->rx_ctrl.sig_len);

        // check if this is an EAPOL frame for handshake tracking
        if (len < (int)(hdr_len + 8)) return;
        const uint8_t *llc = frame + hdr_len;
        if (llc[0] != 0xAA || llc[1] != 0xAA || llc[2] != 0x03) return;
        uint16_t ethertype = (llc[6] << 8) | llc[7];
        if (ethertype != 0x888E) return;

        const uint8_t *eapol = llc + 8;
        if (len < (int)(hdr_len + 8 + 17)) return;

        uint8_t key_desc_type = eapol[4];
        if (key_desc_type != 2) return;

        uint16_t key_info = (eapol[5] << 8) | eapol[6];
        bool has_mic = (key_info & 0x0100) != 0;
        bool is_pairwise = (key_info & 0x0008) != 0;
        bool is_install = (key_info & 0x0040) != 0;
        bool is_ack = (key_info & 0x0080) != 0;
        bool is_secure = (key_info & 0x0200) != 0;

        const uint8_t *addr1 = frame + 4;
        const uint8_t *addr2 = frame + 10;
        const uint8_t *ap_mac = is_ack ? addr2 : addr1;
        const uint8_t *sta_mac = is_ack ? addr1 : addr2;

        uint64_t replay = 0;
        for (int i = 0; i < 8; i++) replay = (replay << 8) | eapol[9 + i];

        if (is_pairwise) {
            uint8_t msg = 0;
            if (!has_mic && is_ack && !is_install) {
                msg = 1;  // M1: AP->STA, no MIC, no Install
            } else if (has_mic) {
                if (is_ack && is_install) msg = 3;             // M3
                else if (!is_ack && !is_install && !is_secure) msg = 2; // M2
                else if (!is_ack && !is_install && is_secure) msg = 4;  // M4
            }
            if (msg > 0) {
                process_eapol_candidate_pair(ap_mac, sta_mac, replay, is_ack, msg);
            }
        }
        return;
    }

    if (type == WIFI_PKT_MGMT) {
        const uint8_t *frame = pkt->payload;
        if (pkt->rx_ctrl.sig_len < 24) return;
        uint8_t subtype = (frame[0] & 0xF0) >> 4;

        // assoc/reassoc frames
        if (subtype == 0x0 || subtype == 0x1 || subtype == 0x2 || subtype == 0x3) {
            enqueue_pcap_write(pkt->payload, pkt->rx_ctrl.sig_len);
            return;
        }

        // authentication frames (useful for context/sae)
        if (subtype == 0x0B) {
            enqueue_pcap_write(pkt->payload, pkt->rx_ctrl.sig_len);
            return;
        }

        // probe request frames (capture undirected and directed) with de-duplication
        if (subtype == WIFI_PKT_PROBE_REQ) {
            const uint8_t *src = frame + 10; // addr2
            // parse SSID element
            char ssid[33] = {0};
            bool ssid_found = false;
            int index = 24;
            if (pkt->rx_ctrl.sig_len > index) {
                const uint8_t *body = frame + index;
                int body_len = pkt->rx_ctrl.sig_len - index;
                for (int i = 0; i < body_len - 1; i += 2 + body[i+1]) {
                    uint8_t tag_num = body[i];
                    uint8_t tag_len = body[i+1];
                    if (tag_num == 0 && tag_len < sizeof(ssid) && i + 2 + tag_len <= body_len) {
                        memcpy(ssid, &body[i+2], tag_len);
                        ssid[tag_len] = '\0';
                        if (tag_len == 0) strcpy(ssid, "Broadcast");
                        ssid_found = true;
                        break;
                    }
                }
                if (!ssid_found) strcpy(ssid, "Broadcast");
            }
            uint32_t h = hash_ssid(ssid);
            uint64_t now_ms = esp_timer_get_time() / 1000ULL;
            if (probe_should_emit(src, h, now_ms)) {
                enqueue_pcap_write(pkt->payload, pkt->rx_ctrl.sig_len);
            }
            return;
        }

        // limited beacons and probe responses
        if (subtype == WIFI_PKT_BEACON || subtype == WIFI_PKT_PROBE_RESP) {
            if (pkt->rx_ctrl.sig_len >= 38) {
                const uint8_t *bssid = frame + 16;
                uint8_t ssid_len = frame[37];
                bool ssid_nonempty = ssid_len > 0;
                if (beacon_should_emit_limited(bssid, ssid_nonempty)) {
                    enqueue_pcap_write(pkt->payload, pkt->rx_ctrl.sig_len);
                }
            }
            return;
        }
    }
}

void wifi_wps_detection_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) {
        return;
    }

    const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const wifi_ieee80211_packet_t *ipkt = (wifi_ieee80211_packet_t *)pkt->payload;
    wifi_ieee80211_mac_hdr_t hdr_copy;
    memcpy(&hdr_copy, &ipkt->hdr, sizeof(wifi_ieee80211_mac_hdr_t));
    const wifi_ieee80211_mac_hdr_t *hdr = &hdr_copy;

    const uint8_t *payload = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;

    uint8_t frame_type = hdr->frame_ctrl & 0xFC;
    if (frame_type != 0x80 && frame_type != 0x50) {
        return;
    }

    int index = 36;
    char ssid[33] = {0};
    uint8_t bssid[6];
    memcpy(bssid, hdr->addr3, 6);

    while (index + 1 < len) {
        uint8_t id = payload[index];
        uint8_t ie_len = payload[index + 1];

        /* sanity checks: ensure IE length fits within bounds */
        if (index + 2 + ie_len > len) {
            break;
        }

        if (id == 0 && ie_len <= 32) {
            memcpy(ssid, &payload[index + 2], ie_len);
            ssid[ie_len] = '\0';
            trim_trailing(ssid);
        }

        if (is_network_duplicate(ssid, bssid)) {
            return;
        }

        if (id == 221 && ie_len >= 4) {
            uint32_t oui =
                (payload[index + 2] << 16) | (payload[index + 3] << 8) | payload[index + 4];
            uint8_t oui_type = payload[index + 5];

            if (oui == 0x0050f2 && oui_type == 0x04) {
                int attr_index = index + 6;
                int wps_ie_end = index + 2 + ie_len;

                while (attr_index + 4 <= wps_ie_end) {
                    uint16_t attr_id = (payload[attr_index] << 8) | payload[attr_index + 1];
                    uint16_t attr_len = (payload[attr_index + 2] << 8) | payload[attr_index + 3];

                    /* sanity: attr_len must be reasonable and fit inside the WPS IE */
                    if (attr_len > MAX_IE_LEN || attr_len > (wps_ie_end - (attr_index + 4))) {
                        break;
                    }

                    if (attr_id == 0x1008 && attr_len == 2) {
                        uint16_t config_methods =
                            (payload[attr_index + 4] << 8) | payload[attr_index + 5];

                        IRAM_PRINTF("Configuration Methods found: 0x%04x\n", config_methods);

                        if (config_methods & WPS_CONF_METHODS_PBC) {
                            glog("WPS Push Button detected:\n%s\n", ssid);
                        } else if (config_methods &
                                   (WPS_CONF_METHODS_PIN_DISPLAY | WPS_CONF_METHODS_PIN_KEYPAD)) {
                            glog("WPS PIN detected:\n%s\n", ssid);
                        }

                        if (should_store_wps == 1) {
                            if (detected_network_count < MAX_WPS_NETWORKS) {
                                wps_network_t new_network;
                                strncpy(new_network.ssid, ssid, sizeof(new_network.ssid) - 1);
                                new_network.ssid[sizeof(new_network.ssid) - 1] = '\0';
                                memcpy(new_network.bssid, bssid, sizeof(new_network.bssid));
                                new_network.wps_enabled = true;
                                new_network.wps_mode = config_methods & (WPS_CONF_METHODS_PIN_DISPLAY |
                                                                         WPS_CONF_METHODS_PIN_KEYPAD)
                                                           ? WPS_MODE_PIN
                                                           : WPS_MODE_PBC;
                                detected_wps_networks[detected_network_count++] = new_network;
                            }
                        } else {
                            enqueue_pcap_write(pkt->payload, pkt->rx_ctrl.sig_len);
                        }

                        if (detected_network_count >= MAX_WPS_NETWORKS) {
                            glog("Maximum number of WPS networks detected\nStopping "
                                 "monitor mode.\n");
                            wifi_manager_stop_monitor_mode();
                        }
                    }

                    attr_index += (4 + attr_len);
                }
            }
        }

        index += (2 + ie_len);
    }
}

#ifndef CONFIG_IDF_TARGET_ESP32S2
#ifndef BLE_HS_ADV_TYPE_APPEARANCE
#define BLE_HS_ADV_TYPE_APPEARANCE 0x19
#endif

struct ble_hs_adv_field;
static int ble_hs_adv_parse_fields_cb(const struct ble_hs_adv_field *field, void *arg);

static const char SKIMMER_TAG[] STORE_STR_ATTR = "SKIMMER_DETECT";

struct ble_adv_parse_arg {
    wardriving_data_t *wd;
};

void ble_wardriving_callback(struct ble_gap_event *event, void *arg) {
    if (!event || event->type != BLE_GAP_EVENT_DISC) {
        return;
    }

    wardrive_ble_advs_seen++;

    uint32_t mac_hash = ble_wd_hash_mac(event->disc.addr.val);
    bool already_seen = false;
    for (int i = 0; i < BLE_WD_SEEN_SIZE; i++) {
        if (ble_wd_seen_hashes[i] == mac_hash) { already_seen = true; break; }
    }
    if (!already_seen) {
        ble_wd_seen_hashes[ble_wd_seen_idx] = mac_hash;
        ble_wd_seen_idx = (ble_wd_seen_idx + 1) % BLE_WD_SEEN_SIZE;
        ble_wd_unique_count++;
    }

    wardriving_data_t wardriving_data = {0};
    wardriving_data.ble_data.is_ble_device = true;

    snprintf(wardriving_data.ble_data.ble_mac, sizeof(wardriving_data.ble_data.ble_mac),
             "%02x:%02x:%02x:%02x:%02x:%02x", event->disc.addr.val[0], event->disc.addr.val[1],
             event->disc.addr.val[2], event->disc.addr.val[3], event->disc.addr.val[4],
             event->disc.addr.val[5]);

    wardriving_data.ble_data.ble_rssi = event->disc.rssi;

    if (event->disc.length_data > 0) {
        parse_ble_device_name(event->disc.data, event->disc.length_data,
                              wardriving_data.ble_data.ble_name,
                              sizeof(wardriving_data.ble_data.ble_name));
        struct ble_adv_parse_arg parse_arg = {.wd = &wardriving_data};
        ble_hs_adv_parse(event->disc.data, event->disc.length_data, ble_hs_adv_parse_fields_cb,
                         &parse_arg);
    }

    // Get GPS data from the global handle, if available
    gps_t gps_local = {0};
    if (gps_manager_get_local_gps_snapshot(&gps_local) && gps_local.valid) {
        wardriving_data.gps_quality.satellites_used = gps_local.sats_in_use;
        wardriving_data.gps_quality.hdop = gps_local.dop_h;
        wardriving_data.gps_quality.speed = gps_local.speed;
        wardriving_data.gps_quality.course = gps_local.cog;
        wardriving_data.gps_quality.fix_quality = gps_local.fix;
        wardriving_data.gps_quality.has_valid_fix = (gps_local.fix >= GPS_FIX_GPS);
    }
    

    // Use GPS manager to log data
    wardrive_log_attempts++;
    esp_err_t err = gps_manager_log_wardriving_data(&wardriving_data);
    if (err == ESP_ERR_INVALID_STATE) {
        wardrive_gps_rejected++;
    } else if (err == ESP_OK) {
        wardrive_log_ok++;
    }
}

// Move the callback implementation inside the ESP32S2 guard
static int ble_hs_adv_parse_fields_cb(const struct ble_hs_adv_field *field, void *arg) {
    struct ble_adv_parse_arg *p = (struct ble_adv_parse_arg *)arg;
    wardriving_data_t *data = p ? p->wd : NULL;
    if (data == NULL || field == NULL) {
        return 0;
    }

    if (field->type == BLE_HS_ADV_TYPE_COMP_NAME) {
        size_t name_len = MIN(field->length, sizeof(data->ble_data.ble_name) - 1);
        memcpy(data->ble_data.ble_name, field->value, name_len);
        data->ble_data.ble_name[name_len] = '\0';
    } else if (field->type == BLE_HS_ADV_TYPE_INCOMP_NAME && data->ble_data.ble_name[0] == '\0') {
        size_t name_len = MIN(field->length, sizeof(data->ble_data.ble_name) - 1);
        memcpy(data->ble_data.ble_name, field->value, name_len);
        data->ble_data.ble_name[name_len] = '\0';
    }

    if (field->type == BLE_HS_ADV_TYPE_MFG_DATA && field->length >= 2) {
        const uint8_t *v = (const uint8_t *)field->value;
        data->ble_data.ble_mfgr_id = (uint16_t)v[0] | ((uint16_t)v[1] << 8);
        data->ble_data.ble_has_mfgr_id = true;
    }

    if (field->type == BLE_HS_ADV_TYPE_APPEARANCE && field->length >= 2) {
        const uint8_t *v = (const uint8_t *)field->value;
        data->ble_data.ble_appearance = (uint16_t)v[0] | ((uint16_t)v[1] << 8);
    }

    return 0;
}
#endif

// wrap for esp32s2
#ifndef CONFIG_IDF_TARGET_ESP32S2

static const int suspicious_names_count = sizeof(suspicious_names) / sizeof(suspicious_names[0]);
void ble_skimmer_scan_callback(struct ble_gap_event *event, void *arg) {
    if (!event || event->type != BLE_GAP_EVENT_DISC) {
        return;
    }

    struct ble_hs_adv_fields fields;
    int rc = ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);

    if (rc != 0) {
        ESP_LOGD(SKIMMER_TAG, "Failed to parse advertisement data");
        return;
    }

    // Check device name
    if (fields.name != NULL && fields.name_len > 0) {
        char device_name[32] = {0};
        size_t name_len = MIN(fields.name_len, sizeof(device_name) - 1);
        memcpy(device_name, fields.name, name_len);

        // Check against suspicious names
        for (int i = 0; i < suspicious_names_count; i++) {
            if (strcasecmp(device_name, suspicious_names[i]) == 0) {
                char mac_addr[18];
                snprintf(mac_addr, sizeof(mac_addr), "%02x:%02x:%02x:%02x:%02x:%02x",
                         event->disc.addr.val[0], event->disc.addr.val[1], event->disc.addr.val[2],
                         event->disc.addr.val[3], event->disc.addr.val[4], event->disc.addr.val[5]);

                glog("\nPOTENTIAL SKIMMER DETECTED!\n");
                
                glog("Device Name: %s\n", device_name);

                glog("MAC Address: %s\n", mac_addr);

                glog("RSSI: %d dBm\n", event->disc.rssi);

                glog("Reason:\nMatched known skimmer pattern: %s\n", suspicious_names[i]);

                glog("Please verify before taking action.\n\n");

                // pulse rgb red once when skimmer is detected
                pulse_once(&rgb_manager, 255, 0, 0);

                // Create enhanced PCAP packet with metadata
                if (pcap_is_capturing()) {
                    // Format: [Timestamp][MAC][RSSI][Name][Raw Data]
                    uint8_t enhanced_packet[256] = {0};
                    size_t packet_len = 0;

                    // Add MAC address
                    memcpy(enhanced_packet + packet_len, event->disc.addr.val, 6);
                    packet_len += 6;

                    // Add RSSI
                    enhanced_packet[packet_len++] = (uint8_t)event->disc.rssi;

                    // Add device name length and name
                    if (packet_len + 1 + name_len > sizeof(enhanced_packet)) break;
                    enhanced_packet[packet_len++] = (uint8_t)name_len;
                    memcpy(enhanced_packet + packet_len, device_name, name_len);
                    packet_len += name_len;

                    // Add reason for flagging
                    const char *reason = suspicious_names[i];
                    uint8_t reason_len = strlen(reason);
                    if (packet_len + 1 + reason_len > sizeof(enhanced_packet)) break;
                    enhanced_packet[packet_len++] = reason_len;
                    memcpy(enhanced_packet + packet_len, reason, reason_len);
                    packet_len += reason_len;

                    // Add raw advertisement data
                    if (packet_len + event->disc.length_data > sizeof(enhanced_packet)) break;
                    memcpy(enhanced_packet + packet_len, event->disc.data, event->disc.length_data);
                    packet_len += event->disc.length_data;

                    // Write to PCAP with proper BLE packet format
                    pcap_write_packet_to_buffer(enhanced_packet, packet_len,
                                                PCAP_CAPTURE_BLUETOOTH);

                    // Force flush to ensure suspicious device is captured
                    pcap_flush_buffer_to_file();
                }
                break;
            }
        }
    }
}
#endif

// Packet statistics for monitoring filter effectiveness
static uint32_t total_packets_received = 0;
static uint32_t packets_filtered_out = 0;
static uint32_t packets_processed = 0;

// Early filtering helper - checks basic packet validity
static inline bool is_packet_valid(const wifi_promiscuous_pkt_t *pkt, wifi_promiscuous_pkt_type_t type) {
    total_packets_received++;
    
    // Drop MISC packets immediately
    if (type == WIFI_PKT_MISC) {
        packets_filtered_out++;
        return false;
    }
    
    // Check minimum length
    if (pkt->rx_ctrl.sig_len < MIN_PACKET_LENGTH) {
        packets_filtered_out++;
        return false;
    }
    
    // RSSI threshold filtering
    if (pkt->rx_ctrl.rssi < MIN_RSSI_THRESHOLD) {
        packets_filtered_out++;
        return false;
    }
    
    packets_processed++;
    
    // Log stats less frequently to reduce spam (every ~20000 packets)
    if (total_packets_received % 20000 == 0) {
        char stats_msg[128];
        snprintf(stats_msg, sizeof(stats_msg), "Filter stats: %lu total, %lu filtered, %lu processed (%.1f%% filtered)", 
                (unsigned long)total_packets_received, 
                (unsigned long)packets_filtered_out,
                (unsigned long)packets_processed,
                (float)packets_filtered_out * 100.0f / total_packets_received);
        
        glog("%s\n", stats_msg);
    }
    
    return true;
}

// Channel filtering helper
static inline bool is_on_target_channel(const wifi_promiscuous_pkt_t *pkt, uint8_t target_channel) {
    return (target_channel == 0) || (pkt->rx_ctrl.channel == target_channel);
}

// Flag indicating whether to save probe PCAP data to SD (disable UART fallback if false)
bool g_listen_probes_save_to_sd = false;

static char last_probe_log[128] = {0};
static uint64_t last_probe_log_time_ms = 0;

void wifi_listen_probes_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    // Early filtering for management frames only
    if (type != WIFI_PKT_MGMT) return;

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    
    // Additional early filtering
    if (!is_packet_valid(pkt, type)) return;
    
    // Quick probe request check using frame subtype
    uint8_t frame_subtype = (pkt->payload[0] & 0xF0) >> 4;
    if (frame_subtype != WIFI_PKT_PROBE_REQ) return;

    const wifi_ieee80211_packet_t *ipkt = (wifi_ieee80211_packet_t *)pkt->payload;
    wifi_ieee80211_mac_hdr_t hdr_copy;
    memcpy(&hdr_copy, &ipkt->hdr, sizeof(wifi_ieee80211_mac_hdr_t));  // Copy to avoid unaligned pointer
    const wifi_ieee80211_mac_hdr_t *hdr = &hdr_copy;
    const uint8_t *payload = pkt->payload;

    // Extract source and dest MAC and SSID as before...
    char src_mac_str[18];
    format_mac_address(hdr->addr2, src_mac_str, sizeof(src_mac_str), false);
    char dest_mac_str[18];
    format_mac_address(hdr->addr1, dest_mac_str, sizeof(dest_mac_str), false);
    int index = 24;
    char ssid[33] = {0};
    bool ssid_found = false;
    if (pkt->rx_ctrl.sig_len > index) {
        const uint8_t *body = payload + index;
        int body_len = pkt->rx_ctrl.sig_len - index;
        for (int i = 0; i < body_len - 1; i += 2 + body[i+1]) {
            uint8_t tag_num = body[i];
            uint8_t tag_len = body[i+1];
            if (tag_num == 0 && tag_len < sizeof(ssid) && i + 2 + tag_len <= body_len) {
                memcpy(ssid, &body[i+2], tag_len);
                ssid[tag_len] = '\0';
                if (tag_len == 0) strcpy(ssid, "Broadcast");
                ssid_found = true;
                break;
            }
        }
        if (!ssid_found) strcpy(ssid, "Broadcast");
    }

    // Build log message
    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg), "Probe Req: %s -> %s for %s", src_mac_str, dest_mac_str, ssid);

    // Deduplicate: skip if same message within timeout
    uint64_t now_ms = esp_timer_get_time() / 1000ULL;
    if (strcmp(log_msg, last_probe_log) == 0 && (now_ms - last_probe_log_time_ms) < PROBE_DEDUPE_TIMEOUT_MS) {
        return;
    }
    strcpy(last_probe_log, log_msg);
    last_probe_log_time_ms = now_ms;

    // Optionally save packet to SD if enabled
    if (g_listen_probes_save_to_sd && pkt->rx_ctrl.sig_len > 0) {
        esp_err_t ret = pcap_write_packet_to_buffer(payload, pkt->rx_ctrl.sig_len, PCAP_CAPTURE_WIFI);
        if (ret != ESP_OK) {
            ESP_LOGE("PROBE_LISTEN", "Failed to write packet to buffer");
        }
    }

    // Print to console and display
    glog("%s\n", log_msg);
}
