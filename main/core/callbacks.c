#include "core/callbacks.h"
#include "esp_wifi.h"
#include "managers/gps_manager.h"
#include "managers/rgb_manager.h"
#include "managers/views/terminal_screen.h"
#include "managers/wifi_manager.h"
#include "core/utils.h"
#include "vendor/GPS/gps_logger.h"
#include "vendor/pcap.h"
#include "core/glog.h"
#include <ctype.h>
#include <esp_log.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_rom_sys.h"  // Contains esp_rom_printf
#include <esp_timer.h>  // For esp_timer_get_time
#include "freertos/task.h"

// prototypes for static inline helpers
static inline bool is_packet_valid(const wifi_promiscuous_pkt_t *pkt, wifi_promiscuous_pkt_type_t type);
static inline bool is_on_target_channel(const wifi_promiscuous_pkt_t *pkt, uint8_t target_channel);

#define STORE_STR_ATTR __attribute__((section(".rodata.str")))
#define STORE_DATA_ATTR __attribute__((section(".rodata.data")))
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
#define CHANNEL_HOP_INTERVAL_MS 200
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
static pineap_network_t pineap_networks[MAX_PINEAP_NETWORKS];
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

static void wardrive_heartbeat_cb(void *arg);
static void start_wardrive_heartbeat(void);
static void stop_wardrive_heartbeat(void);

#if defined(CONFIG_IDF_TARGET_ESP32C5)
#define WARDRIVE_C5_MAX_CHANNEL_PROBE 196
static uint8_t wardrive_c5_channels[WARDRIVE_C5_MAX_CHANNEL_PROBE];
static size_t wardrive_c5_channel_count = 0;
static size_t wardrive_c5_channel_idx = 0;
static bool wardrive_c5_channels_ready = false;

static void wardrive_build_channel_list_c5(void) {
    if (wardrive_c5_channels_ready) {
        return;
    }

    wardrive_c5_channel_count = 0;
    wardrive_c5_channel_idx = 0;

    uint8_t cur_primary = 1;
    wifi_second_chan_t cur_second = WIFI_SECOND_CHAN_NONE;
    (void)esp_wifi_get_channel(&cur_primary, &cur_second);

    for (uint16_t ch = 1; ch <= WARDRIVE_C5_MAX_CHANNEL_PROBE; ch++) {
        if (wardrive_c5_channel_count >= (sizeof(wardrive_c5_channels) / sizeof(wardrive_c5_channels[0]))) {
            break;
        }
        if (esp_wifi_set_channel((uint8_t)ch, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
            wardrive_c5_channels[wardrive_c5_channel_count++] = (uint8_t)ch;
        }
    }

    if (wardrive_c5_channel_count == 0) {
        wardrive_c5_channels[0] = 1;
        wardrive_c5_channel_count = 1;
    }

    (void)esp_wifi_set_channel(cur_primary, cur_second);
    wardrive_c5_channels_ready = true;
}

static uint8_t wardrive_next_channel_c5(void) {
    wardrive_build_channel_list_c5();
    if (wardrive_c5_channel_count == 0) {
        return 1;
    }
    wardrive_c5_channel_idx = (wardrive_c5_channel_idx + 1) % wardrive_c5_channel_count;
    return wardrive_c5_channels[wardrive_c5_channel_idx];
}
#endif
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

static inline bool mac_equal(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 6) == 0;
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
    uint8_t *buffer;
    pcap_capture_type_t cap_type;
} pcap_q_item_t;

#define EAPOL_Q_LEN 64
static QueueHandle_t s_pcap_q = NULL;
static TaskHandle_t s_pcap_writer_task = NULL;

static void pcap_writer_task(void *arg) {
    (void)arg;
    pcap_q_item_t item;
    uint32_t processed = 0;
    for (;;) {
        if (xQueueReceive(s_pcap_q, &item, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (item.buffer && item.length > 0) {
                pcap_write_packet_to_buffer(item.buffer, item.length, item.cap_type);
                free(item.buffer);
            }
            processed++;
            if ((processed & 0xFF) == 0) { // log occasionally to avoid spam
                UBaseType_t hwm_words = uxTaskGetStackHighWaterMark(NULL);
                glog("PCAP writer HWM (words): %lu\n", (unsigned long)hwm_words);
            }
            if ((processed & 0x1F) == 0) {
                pcap_flush_buffer_to_file();
            }
        } else {
            // periodic flush even if idle
            pcap_flush_buffer_to_file();
        }
    }
}

static inline void ensure_pcap_queue_started(void) {
    if (s_pcap_q == NULL) {
        s_pcap_q = xQueueCreate(EAPOL_Q_LEN, sizeof(pcap_q_item_t));
        if (s_pcap_q != NULL && s_pcap_writer_task == NULL) {
            xTaskCreate(pcap_writer_task, "pcap_wr", 3072, NULL, 5, &s_pcap_writer_task);
        }
    }
}

static inline void enqueue_pcap_write_typed(const uint8_t *payload, uint16_t len, pcap_capture_type_t cap_type) {
    if (!payload || len == 0) return;
    ensure_pcap_queue_started();
    if (!s_pcap_q) return;
    pcap_q_item_t item = {0};
    item.length = len;
    item.buffer = (uint8_t *)malloc(len);
    item.cap_type = cap_type;
    if (!item.buffer) return;
    memcpy(item.buffer, payload, len);
    if (xQueueSend(s_pcap_q, &item, 0) != pdTRUE) {
        free(item.buffer);
    }
}

static inline void enqueue_pcap_write(const uint8_t *payload, uint16_t len) {
    enqueue_pcap_write_typed(payload, len, PCAP_CAPTURE_WIFI);
}

// cleanup function to free pcap queue and task when not capturing
void cleanup_pcap_queue(void) {
    if (s_pcap_writer_task != NULL) {
        vTaskDelete(s_pcap_writer_task);
        s_pcap_writer_task = NULL;
    }
    if (s_pcap_q != NULL) {
        // drain any remaining items and free their buffers
        pcap_q_item_t item;
        while (xQueueReceive(s_pcap_q, &item, 0) == pdTRUE) {
            if (item.buffer) free(item.buffer);
        }
        vQueueDelete(s_pcap_q);
        s_pcap_q = NULL;
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

static blacklisted_ap_t blacklist[MAX_PINEAP_NETWORKS];
static int blacklist_count = 0;

static bool is_blacklisted(const uint8_t *bssid) {
    for (int i = 0; i < blacklist_count; i++) {
        if (memcmp(blacklist[i].bssid, bssid, 6) == 0) {
            return true;
        }
    }
    return false;
}

static bool should_update_blacklisted(const uint8_t *bssid) {
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

#if defined(CONFIG_IDF_TARGET_ESP32C5)
    for (size_t tries = 0; tries < wardrive_c5_channel_count; tries++) {
        current_channel = wardrive_next_channel_c5();
        if (esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
            break;
        }
    }
#else
    uint8_t start = current_channel;
    do {
        current_channel = (current_channel % MAX_WIFI_CHANNEL) + 1;
        if (esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE) == ESP_OK)
            break;
    } while (current_channel != start);
#endif
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

static void wardrive_hop_timer_callback(void *arg) {
    if (!wardriving_hopping_active)
        return;

#if defined(CONFIG_IDF_TARGET_ESP32C5)
    for (size_t tries = 0; tries < wardrive_c5_channel_count; tries++) {
        wardrive_channel = wardrive_next_channel_c5();
        if (esp_wifi_set_channel(wardrive_channel, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
            break;
        }
    }
#else
    uint8_t start = wardrive_channel;
    do {
        wardrive_channel = (wardrive_channel % MAX_WIFI_CHANNEL) + 1;
        if (esp_wifi_set_channel(wardrive_channel, WIFI_SECOND_CHAN_NONE) == ESP_OK)
            break;
    } while (wardrive_channel != start);
#endif
}

static esp_err_t start_wardrive_channel_hopping(void) {
    esp_timer_create_args_t timer_args = {.callback = wardrive_hop_timer_callback,
                                          .name = "wardrive_hop"};

    if (wardrive_hop_timer == NULL) {
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &wardrive_hop_timer));
    }

    wardriving_hopping_active = true;
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    wardrive_c5_channels_ready = false;
    wardrive_build_channel_list_c5();
    wardrive_c5_channel_idx = 0;
    wardrive_channel = wardrive_c5_channels[0];
#else
    wardrive_channel = 1;
#endif
    return esp_timer_start_periodic(wardrive_hop_timer, CHANNEL_HOP_INTERVAL_MS * 1000);
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

    gps_t *gps_local = NULL;
    const char *fix_status = "No GPS";
    uint8_t sats = 0;

    if (nmea_hdl != NULL) {
        gps_local = &((esp_gps_t *)nmea_hdl)->parent;
    }

    if (gps_local != NULL) {
        sats = gps_local->sats_in_use;
        if (!gps_local->valid || gps_local->fix < GPS_FIX_GPS || gps_local->fix_mode < GPS_MODE_2D) {
            fix_status = "No Fix";
        } else if (gps_local->fix_mode == GPS_MODE_2D) {
            fix_status = "2D";
        } else if (gps_local->fix_mode == GPS_MODE_3D) {
            fix_status = "3D";
        } else {
            fix_status = "Fix";
        }
    }

    uint32_t up_s = 0;
    if (wardrive_start_us != 0) {
        up_s = (uint32_t)((esp_timer_get_time() - wardrive_start_us) / 1000000LL);
    }

    uint32_t up_m = up_s / 60;
    uint32_t up_rem_s = up_s % 60;

    size_t pending = csv_get_pending_bytes();

    glog("Wardrive: ap=%lu logged=%lu/%lu gpsrej=%lu ch=%u up=%lum%02lus gps=%s/%u pending=%uB\n",
         (unsigned long)wardrive_wifi_frames_seen,
         (unsigned long)wardrive_log_ok,
         (unsigned long)wardrive_log_attempts,
         (unsigned long)wardrive_gps_rejected,
         (unsigned)wardrive_channel,
         (unsigned long)up_m,
         (unsigned long)up_rem_s,
         fix_status,
         (unsigned)sats,
         (unsigned)pending);
}

static void start_wardrive_heartbeat(void) {
    wardrive_start_us = esp_timer_get_time();
    wardrive_wifi_frames_seen = 0;
    wardrive_ble_advs_seen = 0;
    wardrive_log_attempts = 0;
    wardrive_log_ok = 0;
    wardrive_gps_rejected = 0;
}

static void stop_wardrive_heartbeat(void) {
    if (wardrive_heartbeat_timer) {
        esp_timer_stop(wardrive_heartbeat_timer);
        esp_timer_delete(wardrive_heartbeat_timer);
        wardrive_heartbeat_timer = NULL;
    }
}

void start_pineap_detection(void) {
    pineap_detection_active = true;
    pineap_network_count = 0;
    memset(pineap_networks, 0, sizeof(pineap_networks));
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    wardrive_c5_channels_ready = false;
    wardrive_build_channel_list_c5();
    wardrive_c5_channel_idx = 0;
    current_channel = wardrive_c5_channels[0];
#else
    current_channel = 1;
#endif
    start_channel_hopping();
}

void stop_pineap_detection(void) {
    pineap_detection_active = false;
    stop_channel_hopping();
}

void start_wardriving(void) {
    start_wardrive_channel_hopping();
    start_wardrive_heartbeat();
}

void stop_wardriving(void) {
    stop_wardrive_heartbeat();
    stop_wardrive_channel_hopping();
}

uint32_t wardriving_get_ap_count(void) {
    return wardrive_wifi_frames_seen;
}

#define IRAM_PRINTF(fmt, ...) do { \
    static const char flash_fmt[] STORE_STR_ATTR = fmt; \
    esp_rom_printf(flash_fmt, ##__VA_ARGS__); \
} while(0)

void log_pineap_detection(void *arg) {
    pineap_log_data_t *log_data = (pineap_log_data_t *)arg;
    pineap_network_t *network = log_data->network;

    vTaskDelay(pdMS_TO_TICKS(5000));

    char mac_str[18];
    format_mac_address(log_data->bssid, mac_str, sizeof(mac_str), false);

    char ssids_str[256] = {0};
    int valid_ssid_count =
        build_recent_ssids_string(network, ssids_str, sizeof(ssids_str));

    // Only log if we have valid SSIDs
    if (valid_ssid_count >= MIN_SSIDS_FOR_DETECTION) {
        // Pulse RGB purple (red + blue) to indicate Pineapple detection
        pulse_once(&rgb_manager, 255, 0, 255);

        // Evil Twin Detection: Check for same SSID from different BSSIDs
        for (int i = 0; i < pineap_network_count; i++) {
            if (i != (network - pineap_networks) && // Skip self
                strcasecmp(network->recent_ssids[0], pineap_networks[i].recent_ssids[0]) == 0) {
                // format the other network's BSSID into a string before logging
                char other_mac_str[18];
                format_mac_address(pineap_networks[i].bssid, other_mac_str, sizeof(other_mac_str), false);

                glog("Evil Twin Detected:\nSame SSID '%.100s'\nfrom BSSID %s and\n%s\n",
                     network->recent_ssids[0], mac_str, other_mac_str);
            }
        }

        log_pineap_details(network, "Pineapple detected!", ssids_str, valid_ssid_count);
    }

    free(log_data);
    network->log_task_handle = NULL; // Clear handle before deletion
    vTaskDelete(NULL);
}

static void start_log_task(pineap_network_t *network, const char *new_ssid, int8_t channel,
                           int8_t rssi) {
    // Check if a task is already running
    if (network->log_task_handle != NULL) {
        TaskHandle_t existing_handle = network->log_task_handle;
        network->log_task_handle = NULL; // Clear it first to avoid race conditions
        vTaskDelete(existing_handle);    // Clean up existing task
    }

    pineap_log_data_t *log_data = malloc(sizeof(pineap_log_data_t));
    if (!log_data)
        return;

    // Copy network data
    memcpy(log_data->bssid, network->bssid, 6);
    memcpy(log_data->recent_ssids, network->recent_ssids, sizeof(network->recent_ssids));
    log_data->ssid_count = network->ssid_count;
    log_data->channel = channel;
    log_data->rssi = rssi;
    log_data->network = network;
    BaseType_t result = xTaskCreate(log_pineap_detection, "pineap_log", 1024, log_data, 1,
                                    &network->log_task_handle);
    if (result != pdPASS) {
        free(log_data);
        network->log_task_handle = NULL;
    }
}

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

            // Create new logging task if previous one has completed
            if (network->log_task_handle == NULL) {
                pineap_log_data_t *log_data = malloc(sizeof(pineap_log_data_t));
                if (!log_data)
                    return;

                memcpy(log_data->bssid, network->bssid, 6);
                log_data->network = network; // Pass network pointer for up-to-date info

                BaseType_t result = xTaskCreate(log_pineap_detection, "pineap_log", 1024, log_data,
                                                1, &network->log_task_handle);
                if (result != pdPASS) {
                    free(log_data);
                    network->log_task_handle = NULL;
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
        gps_try_sync_time_from_fix(gps);
        break;
    default:
        break;
    }
}

bool compare_bssid(const uint8_t *bssid1, const uint8_t *bssid2) {
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

    wardrive_wifi_frames_seen++;

    int index = 36;
    char ssid[33] = {0};
    uint8_t bssid[6];
    memcpy(bssid, hdr->addr3, 6);

    int rssi = pkt->rx_ctrl.rssi;
    int channel = pkt->rx_ctrl.channel;

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

        /* sanity checks: ensure IE length is reasonable and within bounds */
        if (ie_len > MAX_IE_LEN || index + 2 + ie_len > len) {
            break;
        }

        if (id == 0 && ie_len <= 32) {
            memcpy(ssid, &payload[index + 2], ie_len);
            ssid[ie_len] = '\0';
            trim_trailing(ssid);
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
            
        } else if (id == 221) {
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
    }

    if (!found_rsn && !found_wpa) {
        size_t copy_len = sizeof(encryption_type) - 1;
        if (privacy_required) {
            strncpy(encryption_type, "WEP", copy_len);
        } else {
            strncpy(encryption_type, "OPEN", copy_len);
        }
        encryption_type[copy_len] = '\0';
    }

    double latitude = 0;
    double longitude = 0;

    if (gps != NULL) {
        latitude = gps->latitude;
        longitude = gps->longitude;
    }

    wardriving_data_t wardriving_data = {0};
    wardriving_data.ble_data.is_ble_device = false;  // ensure Wi-Fi entry
    strncpy(wardriving_data.ssid, ssid, sizeof(wardriving_data.ssid) - 1);
    wardriving_data.ssid[sizeof(wardriving_data.ssid) - 1] = '\0'; // Null-terminate
    snprintf(wardriving_data.bssid, sizeof(wardriving_data.bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    wardriving_data.rssi = rssi;
    wardriving_data.channel = channel;
    wardriving_data.latitude = latitude;
    wardriving_data.longitude = longitude;
    strncpy(wardriving_data.encryption_type, encryption_type,
            sizeof(wardriving_data.encryption_type) - 1);
    wardriving_data.encryption_type[sizeof(wardriving_data.encryption_type) - 1] = '\0';

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
                if (is_ack && is_install) msg = 3;        // M3
                else if (!is_ack && !is_install) msg = 2; // M2
                else if (!is_ack && is_install) msg = 4;  // M4
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
    const wifi_ieee80211_mac_hdr_t *hdr = &ipkt->hdr;

    const uint8_t *payload = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;

    uint8_t frame_type = hdr->frame_ctrl & 0xFC;
    if (frame_type != 0x80 && frame_type != 0x50) {
        return;
    }

    int index = 36;
    char ssid[33] = {0};
    bool wps_found = false;
    uint8_t bssid[6];
    memcpy(bssid, hdr->addr3, 6);

    while (index + 1 < len) {
        uint8_t id = payload[index];
        uint8_t ie_len = payload[index + 1];

        /* sanity checks: ensure IE length is reasonable and within bounds */
        if (ie_len > MAX_IE_LEN || index + 2 + ie_len > len) {
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
                wps_found = true;

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
                            wps_network_t new_network;
                            strncpy(new_network.ssid, ssid, sizeof(new_network.ssid) - 1);
                            new_network.ssid[sizeof(new_network.ssid) - 1] =
                                '\0'; // Ensure null termination
                            memcpy(new_network.bssid, bssid, sizeof(new_network.bssid));
                            new_network.wps_enabled = true;
                            new_network.wps_mode = config_methods & (WPS_CONF_METHODS_PIN_DISPLAY |
                                                                     WPS_CONF_METHODS_PIN_KEYPAD)
                                                       ? WPS_MODE_PIN
                                                       : WPS_MODE_PBC;

                            detected_wps_networks[detected_network_count++] = new_network;
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
// Forward declare the struct and callback before use
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

    wardriving_data_t wardriving_data = {0};
    wardriving_data.ble_data.is_ble_device = true;

    // Get BLE MAC and RSSI
    snprintf(wardriving_data.ble_data.ble_mac, sizeof(wardriving_data.ble_data.ble_mac),
             "%02x:%02x:%02x:%02x:%02x:%02x", event->disc.addr.val[0], event->disc.addr.val[1],
             event->disc.addr.val[2], event->disc.addr.val[3], event->disc.addr.val[4],
             event->disc.addr.val[5]);

    wardriving_data.ble_data.ble_rssi = event->disc.rssi;

    // Parse BLE name / manufacturer data if available
    if (event->disc.length_data > 0) {
        struct ble_adv_parse_arg parse_arg = {.wd = &wardriving_data};
        ble_hs_adv_parse(event->disc.data, event->disc.length_data, ble_hs_adv_parse_fields_cb,
                         &parse_arg);
    }

    // Get GPS data from the global handle, if available
    if (nmea_hdl != NULL) {
        gps_t *gps_local = &((esp_gps_t *)nmea_hdl)->parent;
        if (gps_local != NULL && gps_local->valid) {
            wardriving_data.gps_quality.satellites_used = gps_local->sats_in_use;
            wardriving_data.gps_quality.hdop = gps_local->dop_h;
            wardriving_data.gps_quality.speed = gps_local->speed;
            wardriving_data.gps_quality.course = gps_local->cog;
            wardriving_data.gps_quality.fix_quality = gps_local->fix;
            wardriving_data.gps_quality.has_valid_fix = (gps_local->fix >= GPS_FIX_GPS);
        }
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
    }

    if (field->type == BLE_HS_ADV_TYPE_MFG_DATA && field->length >= 2) {
        const uint8_t *v = (const uint8_t *)field->value;
        data->ble_data.ble_mfgr_id = (uint16_t)v[0] | ((uint16_t)v[1] << 8);
        data->ble_data.ble_has_mfgr_id = true;
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
                    enhanced_packet[packet_len++] = (uint8_t)name_len;
                    memcpy(enhanced_packet + packet_len, device_name, name_len);
                    packet_len += name_len;

                    // Add reason for flagging
                    const char *reason = suspicious_names[i];
                    uint8_t reason_len = strlen(reason);
                    enhanced_packet[packet_len++] = reason_len;
                    memcpy(enhanced_packet + packet_len, reason, reason_len);
                    packet_len += reason_len;

                    // Add raw advertisement data
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
