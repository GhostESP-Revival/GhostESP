#include "scans/wifi/airspace_monitor.h"
#include "core/callbacks.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "scans/wifi/wifi_channels.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AIRSPACE_MAX_DEVICES 32
#define AIRSPACE_DEVICE_TTL_MS 30000U
#define AIRSPACE_HOP_INTERVAL_MS 100U
#define AIRSPACE_RATE_WINDOW_MS 500U

#if defined(CONFIG_IDF_TARGET_ESP32C5)
#define AIRSPACE_MAX_WIFI_CHANNEL 165
#else
#define AIRSPACE_MAX_WIFI_CHANNEL 13
#endif

typedef struct {
    bool used;
    uint8_t mac[6];
    uint32_t last_seen_ms;
    uint32_t total;
    uint32_t deauth_total;
    uint32_t disassoc_total;
    uint32_t last_deauth_rate;
    uint32_t last_disassoc_rate;
    uint32_t win_deauth;
    uint32_t win_disassoc;
    int8_t rssi;
    uint8_t channel;
} airspace_device_t;

static const char *TAG = "AirspaceMonitor";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static airspace_device_t *s_devices = NULL;
static uint8_t *s_channels = NULL;
static esp_timer_handle_t s_hop_timer = NULL;
static bool s_active = false;
static uint8_t s_channel_count = 0;
static uint8_t s_channel_idx = 0;
static uint8_t s_current_channel = 1;
static int64_t s_start_us = 0;
static uint32_t s_hop_success = 0;
static uint32_t s_hop_fail = 0;

static uint32_t s_total_packets = 0;
static uint32_t s_mgmt_packets = 0;
static uint32_t s_data_packets = 0;
static uint32_t s_ctrl_packets = 0;
static uint32_t s_beacon_packets = 0;
static uint32_t s_probe_packets = 0;
static uint32_t s_auth_packets = 0;
static uint32_t s_assoc_packets = 0;
static uint32_t s_deauth_packets = 0;
static uint32_t s_disassoc_packets = 0;

static uint32_t s_window_start_ms = 0;
static uint32_t s_window_packets = 0;
static uint32_t s_window_deauth = 0;
static uint32_t s_window_disassoc = 0;
static uint32_t s_last_pps = 0;
static uint32_t s_last_deauth_pps = 0;
static uint32_t s_last_disassoc_pps = 0;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool allocate_buffers(void) {
    if (s_devices != NULL && s_channels != NULL) {
        return true;
    }

    if (s_devices == NULL) {
        s_devices = heap_caps_calloc(AIRSPACE_MAX_DEVICES, sizeof(*s_devices), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_channels == NULL) {
        s_channels = heap_caps_calloc(WIFI_CHANNELS_MAX, sizeof(*s_channels), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    if (s_devices == NULL || s_channels == NULL) {
        free(s_devices);
        free(s_channels);
        s_devices = NULL;
        s_channels = NULL;
        ESP_LOGE(TAG, "Airspace monitor heap allocation failed");
        return false;
    }

    return true;
}

static bool is_zero_or_broadcast(const uint8_t mac[6]) {
    bool all_zero = true;
    bool all_ff = true;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0x00) all_zero = false;
        if (mac[i] != 0xff) all_ff = false;
    }
    return all_zero || all_ff || ((mac[0] & 0x01) != 0);
}

static int find_device_slot_locked(const uint8_t mac[6], uint32_t now) {
    if (s_devices == NULL) {
        return -1;
    }

    int free_slot = -1;
    int oldest_slot = 0;
    uint32_t oldest_seen = UINT32_MAX;

    for (int i = 0; i < AIRSPACE_MAX_DEVICES; i++) {
        airspace_device_t *dev = &s_devices[i];
        if (dev->used && memcmp(dev->mac, mac, 6) == 0) {
            return i;
        }

        bool stale = dev->used && ((uint32_t)(now - dev->last_seen_ms) > AIRSPACE_DEVICE_TTL_MS);
        if ((!dev->used || stale) && free_slot < 0) {
            free_slot = i;
        }
        if (dev->used && dev->last_seen_ms < oldest_seen) {
            oldest_seen = dev->last_seen_ms;
            oldest_slot = i;
        }
    }

    int slot = free_slot >= 0 ? free_slot : oldest_slot;
    memset(&s_devices[slot], 0, sizeof(s_devices[slot]));
    s_devices[slot].used = true;
    memcpy(s_devices[slot].mac, mac, 6);
    return slot;
}

static void roll_window_locked(uint32_t now) {
    if (s_window_start_ms == 0) {
        s_window_start_ms = now;
        return;
    }

    uint32_t elapsed = now - s_window_start_ms;
    if (elapsed < AIRSPACE_RATE_WINDOW_MS) {
        return;
    }

    if (elapsed == 0) elapsed = 1;
    s_last_pps = (s_window_packets * 1000U) / elapsed;
    s_last_deauth_pps = (s_window_deauth * 1000U) / elapsed;
    s_last_disassoc_pps = (s_window_disassoc * 1000U) / elapsed;

    if (s_devices != NULL) {
        for (int i = 0; i < AIRSPACE_MAX_DEVICES; i++) {
            if (!s_devices[i].used) continue;
            s_devices[i].last_deauth_rate = (s_devices[i].win_deauth * 1000U) / elapsed;
            s_devices[i].last_disassoc_rate = (s_devices[i].win_disassoc * 1000U) / elapsed;
            s_devices[i].win_deauth = 0;
            s_devices[i].win_disassoc = 0;
        }
    }

    s_window_packets = 0;
    s_window_deauth = 0;
    s_window_disassoc = 0;
    s_window_start_ms = now;
}

static void build_channel_list(void) {
    if (s_channels == NULL) {
        s_channel_count = 0;
        return;
    }

    uint8_t candidates[WIFI_CHANNELS_MAX] = {0};
    uint8_t candidate_count = wifi_channels_build_country_list(candidates, WIFI_CHANNELS_MAX);
    if (candidate_count == 0) {
        static const uint8_t fallback[] = {
            1,2,3,4,5,6,7,8,9,10,11,12,13,
            36,40,44,48,149,153,157,161,165
        };
        candidate_count = 0;
        for (size_t i = 0; i < sizeof(fallback) && candidate_count < WIFI_CHANNELS_MAX; i++) {
            if (fallback[i] <= AIRSPACE_MAX_WIFI_CHANNEL) {
                candidates[candidate_count++] = fallback[i];
            }
        }
    }

    uint8_t accepted_count = 0;
    for (uint8_t i = 0; i < candidate_count && accepted_count < WIFI_CHANNELS_MAX; i++) {
        uint8_t ch = candidates[i];
        bool duplicate = false;
        for (uint8_t j = 0; j < accepted_count; j++) {
            if (s_channels[j] == ch) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        if (!wifi_channels_is_safe_monitor_channel(ch)) {
            ESP_LOGD(TAG, "Skipping DFS/unsafe monitor channel %u", (unsigned)ch);
            continue;
        }

        esp_err_t err = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        if (err == ESP_OK) {
            s_channels[accepted_count++] = ch;
        } else {
            ESP_LOGD(TAG, "Skipping unsupported channel %u: %s", (unsigned)ch, esp_err_to_name(err));
        }
    }

    if (accepted_count == 0) {
        s_channels[0] = 1;
        accepted_count = 1;
        (void)esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    }

    s_channel_count = accepted_count;
    s_channel_idx = 0;
    s_current_channel = s_channels[0];
    ESP_LOGI(TAG, "Airspace channel plan: %u/%u accepted", (unsigned)accepted_count, (unsigned)candidate_count);
}

static void hop_timer_cb(void *arg) {
    (void)arg;

    uint8_t attempts_made = 0;
    for (uint8_t attempt = 0; attempt < WIFI_CHANNELS_MAX; attempt++) {
        portENTER_CRITICAL(&s_lock);
        if (!s_active || s_channels == NULL || s_channel_count == 0 || attempt >= s_channel_count) {
            portEXIT_CRITICAL(&s_lock);
            break;
        }
        s_channel_idx = (uint8_t)((s_channel_idx + 1) % s_channel_count);
        uint8_t channel = s_channels[s_channel_idx];
        portEXIT_CRITICAL(&s_lock);
        attempts_made++;

        if (!wifi_channels_is_safe_monitor_channel(channel)) {
            portENTER_CRITICAL(&s_lock);
            s_hop_fail++;
            portEXIT_CRITICAL(&s_lock);
            continue;
        }

        esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        if (err != ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            s_hop_fail++;
            portEXIT_CRITICAL(&s_lock);
            continue;
        }

        uint8_t actual_channel = channel;
        wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
        if (esp_wifi_get_channel(&actual_channel, &second) != ESP_OK) {
            actual_channel = channel;
        }

        portENTER_CRITICAL(&s_lock);
        if (s_active) {
            s_current_channel = actual_channel;
            s_hop_success++;
        }
        portEXIT_CRITICAL(&s_lock);
        return;
    }

    if (attempts_made > 0) {
        esp_err_t recover_err = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
        portENTER_CRITICAL(&s_lock);
        if (recover_err == ESP_OK) {
            s_current_channel = 1;
            s_channel_idx = 0;
            s_hop_success++;
        } else {
            s_hop_fail++;
        }
        portEXIT_CRITICAL(&s_lock);
    }
}

void airspace_monitor_reset(void) {
    uint32_t reset_ms = now_ms();
    int64_t reset_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_lock);
    if (s_devices != NULL) {
        memset(s_devices, 0, AIRSPACE_MAX_DEVICES * sizeof(*s_devices));
    }
    s_total_packets = 0;
    s_mgmt_packets = 0;
    s_data_packets = 0;
    s_ctrl_packets = 0;
    s_beacon_packets = 0;
    s_probe_packets = 0;
    s_auth_packets = 0;
    s_assoc_packets = 0;
    s_deauth_packets = 0;
    s_disassoc_packets = 0;
    s_window_start_ms = reset_ms;
    s_window_packets = 0;
    s_window_deauth = 0;
    s_window_disassoc = 0;
    s_last_pps = 0;
    s_last_deauth_pps = 0;
    s_last_disassoc_pps = 0;
    s_hop_success = 0;
    s_hop_fail = 0;
    s_start_us = reset_us;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t airspace_monitor_start(void) {
    if (s_active) {
        return ESP_OK;
    }

    if (!allocate_buffers()) {
        return ESP_ERR_NO_MEM;
    }

    build_channel_list();
    airspace_monitor_reset();
    s_active = true;
    (void)esp_wifi_set_channel(s_current_channel, WIFI_SECOND_CHAN_NONE);

    if (s_hop_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = hop_timer_cb,
            .name = "airspace_hop"
        };
        esp_err_t err = esp_timer_create(&args, &s_hop_timer);
        if (err != ESP_OK) {
            s_active = false;
            return err;
        }
    }

    esp_err_t err = esp_timer_start_periodic(s_hop_timer, AIRSPACE_HOP_INTERVAL_MS * 1000ULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        s_active = false;
        return err;
    }

    ESP_LOGI(TAG, "Airspace monitor started with %u channels", (unsigned)s_channel_count);
    return ESP_OK;
}

void airspace_monitor_stop(void) {
    s_active = false;
    if (s_hop_timer != NULL) {
        (void)esp_timer_stop(s_hop_timer);
        esp_timer_delete(s_hop_timer);
        s_hop_timer = NULL;
    }

    portENTER_CRITICAL(&s_lock);
    airspace_device_t *devices = s_devices;
    uint8_t *channels = s_channels;
    s_devices = NULL;
    s_channels = NULL;
    s_channel_count = 0;
    s_channel_idx = 0;
    portEXIT_CRITICAL(&s_lock);

    free(devices);
    free(channels);
}

bool airspace_monitor_is_active(void) {
    return s_active;
}

const char *airspace_monitor_threat_label(airspace_threat_level_t level) {
    switch (level) {
        case AIRSPACE_THREAT_ATTACK_LIKELY: return "Attack Likely";
        case AIRSPACE_THREAT_SUSPICIOUS: return "Suspicious";
        case AIRSPACE_THREAT_BUSY: return "Busy";
        case AIRSPACE_THREAT_QUIET:
        default: return "Quiet";
    }
}

void airspace_monitor_get_snapshot(airspace_monitor_snapshot_t *out) {
    if (!out) return;

    airspace_monitor_snapshot_t snap = {0};
    uint32_t now = now_ms();

    portENTER_CRITICAL(&s_lock);
    roll_window_locked(now);

    snap.active = s_active;
    snap.current_channel = s_current_channel;
    snap.channel_count = s_channel_count;
    snap.hop_success = s_hop_success;
    snap.hop_fail = s_hop_fail;
    snap.total_packets = s_total_packets;
    snap.packets_per_sec = s_last_pps;
    snap.deauth_per_sec = s_last_deauth_pps;
    snap.disassoc_per_sec = s_last_disassoc_pps;
    snap.mgmt_packets = s_mgmt_packets;
    snap.data_packets = s_data_packets;
    snap.ctrl_packets = s_ctrl_packets;
    snap.beacon_packets = s_beacon_packets;
    snap.probe_packets = s_probe_packets;
    snap.auth_packets = s_auth_packets;
    snap.assoc_packets = s_assoc_packets;
    snap.deauth_packets = s_deauth_packets;
    snap.disassoc_packets = s_disassoc_packets;
    if (s_start_us != 0) {
        snap.uptime_s = (uint32_t)((esp_timer_get_time() - s_start_us) / 1000000LL);
    }

    if (s_devices != NULL) {
        for (int i = 0; i < AIRSPACE_MAX_DEVICES; i++) {
            airspace_device_t *dev = &s_devices[i];
            if (!dev->used) continue;
            if ((uint32_t)(now - dev->last_seen_ms) > AIRSPACE_DEVICE_TTL_MS) continue;
            snap.unique_devices++;

            uint32_t kick_rate = dev->last_deauth_rate + dev->last_disassoc_rate;
            uint32_t kick_total = dev->deauth_total + dev->disassoc_total;
            uint32_t score = (kick_rate * 200U) + (kick_total * 20U) + dev->total;
            if (score == 0) continue;

            airspace_suspect_t candidate = {0};
            memcpy(candidate.mac, dev->mac, 6);
            candidate.channel = dev->channel;
            candidate.rssi = dev->rssi;
            candidate.deauth_rate = dev->last_deauth_rate;
            candidate.disassoc_rate = dev->last_disassoc_rate;
            candidate.deauth_total = dev->deauth_total;
            candidate.disassoc_total = dev->disassoc_total;
            candidate.total = dev->total;
            candidate.score = score;

            for (uint8_t pos = 0; pos < AIRSPACE_MAX_SUSPECTS; pos++) {
                if (pos < snap.suspect_count && snap.suspects[pos].score >= score) {
                    continue;
                }
                uint8_t limit = snap.suspect_count < AIRSPACE_MAX_SUSPECTS ? snap.suspect_count : AIRSPACE_MAX_SUSPECTS - 1;
                for (uint8_t move = limit; move > pos; move--) {
                    snap.suspects[move] = snap.suspects[move - 1];
                }
                snap.suspects[pos] = candidate;
                if (snap.suspect_count < AIRSPACE_MAX_SUSPECTS) {
                    snap.suspect_count++;
                }
                break;
            }
        }
    }

    if (snap.suspect_count > 0) {
        airspace_suspect_t *dev = &snap.suspects[0];
        snap.has_offender = true;
        memcpy(snap.offender_mac, dev->mac, 6);
        snap.offender_channel = dev->channel;
        snap.offender_rssi = dev->rssi;
        snap.offender_deauth_rate = dev->deauth_rate;
        snap.offender_disassoc_rate = dev->disassoc_rate;
        snap.offender_total = dev->total;
    }
    portEXIT_CRITICAL(&s_lock);

    uint32_t offender_kick_rate = snap.offender_deauth_rate + snap.offender_disassoc_rate;
    uint32_t global_kick_rate = snap.deauth_per_sec + snap.disassoc_per_sec;
    if (offender_kick_rate >= 10 || global_kick_rate >= 30) {
        snap.threat_level = AIRSPACE_THREAT_ATTACK_LIKELY;
        snprintf(snap.reason, sizeof(snap.reason), "Deauth/disassoc flood: %lu/s", (unsigned long)global_kick_rate);
    } else if (offender_kick_rate >= 5 || global_kick_rate >= 15) {
        snap.threat_level = AIRSPACE_THREAT_SUSPICIOUS;
        snprintf(snap.reason, sizeof(snap.reason), "Elevated deauth traffic: %lu/s", (unsigned long)global_kick_rate);
    } else if (snap.packets_per_sec >= 400 || snap.unique_devices >= 24) {
        snap.threat_level = AIRSPACE_THREAT_BUSY;
        snprintf(snap.reason, sizeof(snap.reason), "High airtime activity");
    } else {
        snap.threat_level = AIRSPACE_THREAT_QUIET;
        snprintf(snap.reason, sizeof(snap.reason), "No active threat pattern");
    }

    *out = snap;
}

void wifi_airspace_monitor_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!s_active || !buf || s_devices == NULL || type == WIFI_PKT_MISC) {
        return;
    }

    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    int len = pkt->rx_ctrl.sig_len;
    if (len < 2) {
        return;
    }

    const uint8_t *frame = pkt->payload;
    uint8_t frame_type = (frame[0] & 0x0C) >> 2;
    uint8_t subtype = (frame[0] & 0xF0) >> 4;
    bool has_tx = len >= 16;
    const uint8_t *tx_mac = has_tx ? frame + 10 : NULL;
    uint32_t now = now_ms();

    portENTER_CRITICAL(&s_lock);
    roll_window_locked(now);
    s_total_packets++;
    s_window_packets++;

    if (frame_type == 0) {
        s_mgmt_packets++;
        if (subtype == 0x08) s_beacon_packets++;
        else if (subtype == 0x04 || subtype == 0x05) s_probe_packets++;
        else if (subtype == 0x0B) s_auth_packets++;
        else if (subtype <= 0x03) s_assoc_packets++;
        else if (subtype == 0x0C) {
            s_deauth_packets++;
            s_window_deauth++;
        } else if (subtype == 0x0A) {
            s_disassoc_packets++;
            s_window_disassoc++;
        }
    } else if (frame_type == 1) {
        s_ctrl_packets++;
    } else if (frame_type == 2) {
        s_data_packets++;
    }

    if (tx_mac && !is_zero_or_broadcast(tx_mac)) {
        int slot = find_device_slot_locked(tx_mac, now);
        if (slot < 0) {
            portEXIT_CRITICAL(&s_lock);
            return;
        }
        airspace_device_t *dev = &s_devices[slot];
        dev->last_seen_ms = now;
        dev->total++;
        dev->rssi = pkt->rx_ctrl.rssi;
        dev->channel = pkt->rx_ctrl.channel;
        if (frame_type == 0 && subtype == 0x0C) {
            dev->deauth_total++;
            dev->win_deauth++;
        } else if (frame_type == 0 && subtype == 0x0A) {
            dev->disassoc_total++;
            dev->win_disassoc++;
        }
    }
    portEXIT_CRITICAL(&s_lock);
}
