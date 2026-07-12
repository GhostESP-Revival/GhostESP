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

/* Adaptive dwell: when kick frames are seen, camp on the current channel for
   this long (refreshed on every new kick frame) so the true per-channel rate is
   measured instead of being diluted ~10-25x by channel hopping. */
#define AIRSPACE_DWELL_MS 2500U

/* EWMA baseline tuning. The window rolls every AIRSPACE_RATE_WINDOW_MS, so the
   warmup is AIRSPACE_BASELINE_WARMUP windows ~= 4s before the baseline is used. */
#define AIRSPACE_BASELINE_WARMUP 8U
#define AIRSPACE_INSIGHT_INTERVAL_MS 1800U

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
    uint16_t last_seq;   /* last 802.11 sequence number seen from this source */
    bool seq_valid;      /* last_seq holds a real prior value */
    uint32_t probe_resp_ssids; /* 32-bit set: distinct SSIDs this source has
                                  probe-responded for (Karma/Mana detection) */
} airspace_device_t;

/* Beacon-derived AP table (Step 2), shared by beacon-flood and evil-twin
   detection. One entry per named SSID; a second BSSID for the same SSID that
   drops encryption is flagged as a rogue AP / evil twin. */
#define AIRSPACE_MAX_APS 32
typedef struct {
    bool used;
    uint8_t bssid[6];
    uint8_t ssid_len;
    char ssid[33];
    uint8_t channel;
    int8_t rssi;
    bool has_enc;        /* Privacy bit set in beacon capability info */
    uint32_t last_seen_ms;
} airspace_ap_t;

static const char *TAG = "AirspaceMonitor";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static airspace_device_t *s_devices = NULL;
static airspace_ap_t *s_aps = NULL;
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

/* Deauth/disassoc fingerprint evidence, used to tell a spoofed flood (attack
   tool) from a real AP legitimately kicking clients, and to name the tool.
   s_window_* accumulate over the current rate window; s_last_* latch the last
   completed window so the classifier in get_snapshot can read a stable value. */
static uint32_t s_window_kick_total = 0;   /* kick frames this window */
static uint32_t s_window_kick_bcast = 0;   /* kick frames aimed at broadcast DA */
static uint32_t s_window_kick_spoof = 0;   /* kick frames with static/zero seq */
static uint16_t s_window_kick_reason = 0;  /* most recent kick reason code */
static uint32_t s_last_kick_total = 0;
static uint32_t s_last_kick_bcast = 0;
static uint32_t s_last_kick_spoof = 0;
static uint16_t s_last_kick_reason = 0;

/* Beacon-flood + evil-twin evidence (Step 2). Same window/latch split as the
   kick counters so the classifier reads a stable last-window value. */
static uint32_t s_window_beacons = 0;      /* beacon frames this window */
static uint32_t s_window_la_beacons = 0;   /* beacons from locally-admin BSSIDs */
static bool s_window_evil = false;         /* evil twin seen this window */
static char s_window_evil_ssid[33] = {0};
static uint32_t s_last_beacons = 0;
static uint32_t s_last_la_beacons = 0;
static bool s_last_evil = false;
static char s_last_evil_ssid[33] = {0};
static uint16_t s_last_bssids = 0;         /* distinct beaconing APs last window */

/* Auth-flood evidence (Step 3). */
static uint32_t s_window_auth = 0;         /* auth frames this window */
static uint32_t s_last_auth = 0;
static uint16_t s_last_auth_src = 0;       /* distinct auth source MACs last window */

/* All of the larger / newer mutable state lives in a single heap block (PSRAM
   preferred, like s_devices) so it stays out of BSS. Only this pointer is
   static. NULL whenever the monitor is stopped. */
typedef struct {
    /* Adaptive dwell (shared callback <-> hop timer, guarded by s_lock). */
    uint32_t dwell_until_ms;
    bool dwelling;

    /* EWMA baseline (guarded by s_lock, updated on every window roll). */
    float baseline_pps;
    float baseline_kick;
    float baseline_bssids;   /* learned normal count of distinct beaconing APs */
    uint32_t baseline_samples;

    /* Per-window hash bitmaps for distinct-count estimates (guarded by s_lock,
       cleared on every window roll). 512 bits each: a beacon BSSID or an auth
       source MAC hashes to one bit; popcount ~= distinct count for this window.
       Saturation at high counts is fine — that itself signals a flood. */
    uint8_t bssid_seen[64];
    uint8_t auth_src_seen[64];

    /* pps history ring for the sparkline (guarded by s_lock). */
    uint16_t pps_hist[AIRSPACE_PPS_HISTORY];
    uint8_t pps_head;
    uint8_t pps_count;

    /* Unified insight engine. Only touched from airspace_monitor_get_snapshot
       (single caller, the view's LVGL timer), so it needs no extra locking. */
    uint32_t insight_last_ms;
    uint32_t insight_prev_pps;
    uint32_t insight_prev_kick;
    uint8_t insight_prev_tx;
    airspace_threat_level_t insight_level;
    char insight_text[64];
    char advice_text[64];
} airspace_state_t;

static airspace_state_t *s_state = NULL;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool allocate_buffers(void) {
    if (s_devices != NULL && s_aps != NULL && s_channels != NULL && s_state != NULL) {
        return true;
    }

    if (s_devices == NULL) {
        s_devices = heap_caps_calloc(AIRSPACE_MAX_DEVICES, sizeof(*s_devices), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_devices) {
            s_devices = heap_caps_calloc(AIRSPACE_MAX_DEVICES, sizeof(*s_devices), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }
    if (s_aps == NULL) {
        s_aps = heap_caps_calloc(AIRSPACE_MAX_APS, sizeof(*s_aps), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_aps) {
            s_aps = heap_caps_calloc(AIRSPACE_MAX_APS, sizeof(*s_aps), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }
    if (s_channels == NULL) {
        s_channels = heap_caps_calloc(WIFI_CHANNELS_MAX, sizeof(*s_channels), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_channels) {
            s_channels = heap_caps_calloc(WIFI_CHANNELS_MAX, sizeof(*s_channels), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }
    if (s_state == NULL) {
        s_state = heap_caps_calloc(1, sizeof(*s_state), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_state) {
            s_state = heap_caps_calloc(1, sizeof(*s_state), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }

    if (s_devices == NULL || s_aps == NULL || s_channels == NULL || s_state == NULL) {
        free(s_devices);
        free(s_aps);
        free(s_channels);
        free(s_state);
        s_devices = NULL;
        s_aps = NULL;
        s_channels = NULL;
        s_state = NULL;
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

static uint32_t popcount_bytes(const uint8_t *b, size_t n) {
    uint32_t c = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t v = b[i];
        while (v) { v &= (uint8_t)(v - 1); c++; }
    }
    return c;
}

static uint32_t fnv1a(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/* 9-bit bucket for a 512-bit (64-byte) hash bitmap. */
static void bitmap_set9(uint8_t *bm, const uint8_t *key, size_t keylen) {
    uint16_t bit = (uint16_t)(fnv1a(key, keylen) & 511u);
    bm[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
}

/* Parse the SSID element shared by beacon (0x08) and probe-response (0x05)
   frame bodies: fixed 12-byte params then the SSID IE (tag 0) at offset 36.
   Returns the SSID length (0 if absent/hidden/invalid) and sets *ssid_out. */
static uint8_t parse_mgmt_ssid(const uint8_t *frame, int len, const uint8_t **ssid_out) {
    if (len < 38 || frame[36] != 0x00) return 0;
    uint8_t sl = frame[37];
    if (sl == 0 || sl > 32 || len < (int)(38 + sl)) return 0;
    *ssid_out = frame + 38;
    return sl;
}

static bool is_broadcast_mac(const uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0xff) return false;
    }
    return true;
}

/* Track a named beacon in the AP table and flag evil twins. Called under
   s_lock from the RX callback. A second BSSID advertising a known SSID that
   drops encryption is the high-precision rogue-AP signal; legitimate
   multi-BSSID networks (mesh, band-steering) keep the same security and are
   ignored. */
static void airspace_ap_track_locked(const uint8_t *bssid, const uint8_t *ssid,
                                     uint8_t ssid_len, uint8_t channel,
                                     int8_t rssi, bool has_enc, uint32_t now) {
    if (s_aps == NULL || ssid_len == 0 || ssid_len > 32) {
        return;
    }

    int free_slot = -1;
    int oldest = 0;
    uint32_t oldest_seen = UINT32_MAX;

    for (int i = 0; i < AIRSPACE_MAX_APS; i++) {
        airspace_ap_t *ap = &s_aps[i];
        if (!ap->used) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (ap->ssid_len == ssid_len && memcmp(ap->ssid, ssid, ssid_len) == 0) {
            if (memcmp(ap->bssid, bssid, 6) != 0) {
                /* Same SSID from a second BSSID with a different security mode
                   (one encrypted, one open) is the evil-twin signal, whichever
                   was seen first. Consistent security = legit multi-BSSID net. */
                if (ap->has_enc != has_enc) {
                    s_window_evil = true;
                    memcpy(s_window_evil_ssid, ssid, ssid_len);
                    s_window_evil_ssid[ssid_len] = '\0';
                }
            } else {
                ap->channel = channel;
                ap->rssi = rssi;
                ap->has_enc = has_enc;
                ap->last_seen_ms = now;
            }
            return;
        }
        if (ap->last_seen_ms < oldest_seen) {
            oldest_seen = ap->last_seen_ms;
            oldest = i;
        }
    }

    int slot = (free_slot >= 0) ? free_slot : oldest;
    airspace_ap_t *ap = &s_aps[slot];
    memset(ap, 0, sizeof(*ap));
    ap->used = true;
    memcpy(ap->bssid, bssid, 6);
    memcpy(ap->ssid, ssid, ssid_len);
    ap->ssid_len = ssid_len;
    ap->channel = channel;
    ap->rssi = rssi;
    ap->has_enc = has_enc;
    ap->last_seen_ms = now;
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

    s_last_kick_total = s_window_kick_total;
    s_last_kick_bcast = s_window_kick_bcast;
    s_last_kick_spoof = s_window_kick_spoof;
    s_last_kick_reason = s_window_kick_reason;

    s_last_beacons = s_window_beacons;
    s_last_la_beacons = s_window_la_beacons;
    s_last_evil = s_window_evil;
    memcpy(s_last_evil_ssid, s_window_evil_ssid, sizeof(s_last_evil_ssid));
    s_last_auth = s_window_auth;

    if (s_devices != NULL) {
        for (int i = 0; i < AIRSPACE_MAX_DEVICES; i++) {
            if (!s_devices[i].used) continue;
            s_devices[i].last_deauth_rate = (s_devices[i].win_deauth * 1000U) / elapsed;
            s_devices[i].last_disassoc_rate = (s_devices[i].win_disassoc * 1000U) / elapsed;
            s_devices[i].win_deauth = 0;
            s_devices[i].win_disassoc = 0;
        }
    }

    if (s_state != NULL) {
        /* Push this window's pps onto the sparkline ring (clamped to u16). */
        uint16_t sample = s_last_pps > UINT16_MAX ? UINT16_MAX : (uint16_t)s_last_pps;
        s_state->pps_hist[s_state->pps_head] = sample;
        s_state->pps_head = (uint8_t)((s_state->pps_head + 1) % AIRSPACE_PPS_HISTORY);
        if (s_state->pps_count < AIRSPACE_PPS_HISTORY) {
            s_state->pps_count++;
        }

        /* Update the EWMA baseline, but freeze it while an attack/dwell is in
           progress so a flood does not poison "normal". Warm up fast, then
           settle to a slow trailing average. */
        uint32_t kick_now = s_last_deauth_pps + s_last_disassoc_pps;
        bool calm = (kick_now <= (uint32_t)(s_state->baseline_kick + 3.0f)) && !s_state->dwelling;

        /* Distinct-count estimates from this window's hash bitmaps. */
        uint32_t bssids_est = popcount_bytes(s_state->bssid_seen, sizeof(s_state->bssid_seen));
        s_last_bssids = (uint16_t)bssids_est;
        s_last_auth_src = (uint16_t)popcount_bytes(s_state->auth_src_seen,
                                                   sizeof(s_state->auth_src_seen));
        /* A locally-administered-heavy beacon window is itself a suspected
           flood, so don't let it raise the "normal distinct APs" baseline. */
        bool beacon_calm = (s_window_la_beacons * 2U) < s_window_beacons ||
                           s_window_beacons < 10U;

        if (s_state->baseline_samples < AIRSPACE_BASELINE_WARMUP) {
            s_state->baseline_pps += ((float)s_last_pps - s_state->baseline_pps) * 0.25f;
            s_state->baseline_kick += ((float)kick_now - s_state->baseline_kick) * 0.25f;
            s_state->baseline_bssids += ((float)bssids_est - s_state->baseline_bssids) * 0.25f;
            s_state->baseline_samples++;
        } else {
            if (calm) {
                s_state->baseline_pps += ((float)s_last_pps - s_state->baseline_pps) * 0.10f;
                s_state->baseline_kick += ((float)kick_now - s_state->baseline_kick) * 0.10f;
            }
            if (beacon_calm) {
                s_state->baseline_bssids +=
                    ((float)bssids_est - s_state->baseline_bssids) * 0.10f;
            }
        }

        memset(s_state->bssid_seen, 0, sizeof(s_state->bssid_seen));
        memset(s_state->auth_src_seen, 0, sizeof(s_state->auth_src_seen));
    }

    s_window_packets = 0;
    s_window_deauth = 0;
    s_window_disassoc = 0;
    s_window_kick_total = 0;
    s_window_kick_bcast = 0;
    s_window_kick_spoof = 0;
    s_window_beacons = 0;
    s_window_la_beacons = 0;
    s_window_evil = false;
    s_window_evil_ssid[0] = '\0';
    s_window_auth = 0;
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

    /* Adaptive dwell: if a kick frame was seen recently, stay parked on the
       current channel so the true per-channel rate is measured instead of being
       diluted by hopping. The window is refreshed on every new kick frame. */
    uint32_t now = now_ms();
    bool hold = false;
    portENTER_CRITICAL(&s_lock);
    if (s_active && s_state != NULL) {
        if (s_state->dwell_until_ms != 0 && (int32_t)(s_state->dwell_until_ms - now) > 0) {
            s_state->dwelling = true;
            hold = true;
        } else {
            s_state->dwelling = false;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (hold) {
        return;
    }

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
    if (s_aps != NULL) {
        memset(s_aps, 0, AIRSPACE_MAX_APS * sizeof(*s_aps));
    }
    if (s_state != NULL) {
        memset(s_state, 0, sizeof(*s_state));
        s_state->insight_level = AIRSPACE_THREAT_QUIET;
        snprintf(s_state->insight_text, sizeof(s_state->insight_text), "Learning normal activity");
        snprintf(s_state->advice_text, sizeof(s_state->advice_text), "Hold steady; building a baseline");
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
    s_window_kick_total = 0;
    s_window_kick_bcast = 0;
    s_window_kick_spoof = 0;
    s_window_kick_reason = 0;
    s_last_kick_total = 0;
    s_last_kick_bcast = 0;
    s_last_kick_spoof = 0;
    s_last_kick_reason = 0;
    s_window_beacons = 0;
    s_window_la_beacons = 0;
    s_window_evil = false;
    s_window_evil_ssid[0] = '\0';
    s_window_auth = 0;
    s_last_beacons = 0;
    s_last_la_beacons = 0;
    s_last_evil = false;
    s_last_evil_ssid[0] = '\0';
    s_last_bssids = 0;
    s_last_auth = 0;
    s_last_auth_src = 0;
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
    airspace_ap_t *aps = s_aps;
    uint8_t *channels = s_channels;
    airspace_state_t *state = s_state;
    s_devices = NULL;
    s_aps = NULL;
    s_channels = NULL;
    s_state = NULL;
    s_channel_count = 0;
    s_channel_idx = 0;
    portEXIT_CRITICAL(&s_lock);

    free(devices);
    free(aps);
    free(channels);
    free(state);
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

/* Unified trend/insight engine. Lives here (not in the view) so the on-screen
   status and the "insight" line can never disagree. Runs on a slow cadence and
   keeps its prior-sample state in the heap block, not BSS. Single caller. */
static void fill_insight(airspace_monitor_snapshot_t *snap) {
    if (s_state == NULL) {
        snprintf(snap->insight, sizeof(snap->insight), "Learning normal activity");
        snprintf(snap->advice, sizeof(snap->advice), "Hold steady; building a baseline");
        snap->insight_level = AIRSPACE_THREAT_QUIET;
        return;
    }

    airspace_state_t *st = s_state;
    uint32_t now = now_ms();
    uint32_t kick = snap->deauth_per_sec + snap->disassoc_per_sec;

    if (st->insight_last_ms == 0 ||
        (uint32_t)(now - st->insight_last_ms) >= AIRSPACE_INSIGHT_INTERVAL_MS) {
        if (!snap->baseline_ready) {
            snprintf(st->insight_text, sizeof(st->insight_text), "Learning normal activity");
            snprintf(st->advice_text, sizeof(st->advice_text), "Hold steady; building a baseline");
            st->insight_level = AIRSPACE_THREAT_QUIET;
        } else if (kick >= st->insight_prev_kick + 3 && kick >= 3) {
            snprintf(st->insight_text, sizeof(st->insight_text), "Kick spike: clients may be forced off");
            snprintf(st->advice_text, sizeof(st->advice_text), "Watch suspect MAC and channel now");
            st->insight_level = AIRSPACE_THREAT_ATTACK_LIKELY;
        } else if (st->insight_prev_kick >= 3 && kick == 0) {
            snprintf(st->insight_text, sizeof(st->insight_text), "Kick traffic stopped");
            snprintf(st->advice_text, sizeof(st->advice_text), "Keep monitoring; attack may resume");
            st->insight_level = AIRSPACE_THREAT_BUSY;
        } else if (snap->packets_per_sec >= (st->insight_prev_pps * 2U + 20U) && snap->packets_per_sec >= 30U) {
            snprintf(st->insight_text, sizeof(st->insight_text), "Traffic surge: many frames appeared");
            snprintf(st->advice_text, sizeof(st->advice_text), "Look for a rising suspect device");
            st->insight_level = AIRSPACE_THREAT_SUSPICIOUS;
        } else if (st->insight_prev_pps >= 30U && (snap->packets_per_sec * 2U + 10U) < st->insight_prev_pps) {
            snprintf(st->insight_text, sizeof(st->insight_text), "Traffic dropped: air is quieter");
            snprintf(st->advice_text, sizeof(st->advice_text), "Likely normal if no kick frames");
            st->insight_level = AIRSPACE_THREAT_BUSY;
        } else if (snap->unique_devices >= st->insight_prev_tx + 5 && snap->unique_devices >= 8) {
            snprintf(st->insight_text, sizeof(st->insight_text), "New devices appeared quickly");
            snprintf(st->advice_text, sizeof(st->advice_text), "Busy area or clients reconnecting");
            st->insight_level = AIRSPACE_THREAT_BUSY;
        } else if (st->insight_prev_tx >= snap->unique_devices + 5 && st->insight_prev_tx >= 8) {
            snprintf(st->insight_text, sizeof(st->insight_text), "Several devices went quiet");
            snprintf(st->advice_text, sizeof(st->advice_text), "Could be channel hop or clients left");
            st->insight_level = AIRSPACE_THREAT_BUSY;
        } else if (snap->packets_per_sec < 3 && kick == 0) {
            snprintf(st->insight_text, sizeof(st->insight_text), "Very quiet airspace");
            snprintf(st->advice_text, sizeof(st->advice_text), "No action needed");
            st->insight_level = AIRSPACE_THREAT_QUIET;
        } else if (kick == 0) {
            snprintf(st->insight_text, sizeof(st->insight_text), "Normal chatter, no kick pattern");
            snprintf(st->advice_text, sizeof(st->advice_text), "Healthy unless suspect appears");
            st->insight_level = AIRSPACE_THREAT_QUIET;
        } else {
            snprintf(st->insight_text, sizeof(st->insight_text), "Some kick frames seen; watching trend");
            snprintf(st->advice_text, sizeof(st->advice_text), "A few kick frames can be normal");
            st->insight_level = AIRSPACE_THREAT_SUSPICIOUS;
        }

        st->insight_prev_pps = snap->packets_per_sec;
        st->insight_prev_kick = kick;
        st->insight_prev_tx = snap->unique_devices;
        st->insight_last_ms = now;
    }

    memcpy(snap->insight, st->insight_text, sizeof(snap->insight));
    memcpy(snap->advice, st->advice_text, sizeof(snap->advice));
    snap->insight[sizeof(snap->insight) - 1] = '\0';
    snap->advice[sizeof(snap->advice) - 1] = '\0';
    snap->insight_level = st->insight_level;
}

void airspace_monitor_get_snapshot(airspace_monitor_snapshot_t *out) {
    if (!out) return;

    airspace_monitor_snapshot_t snap = {0};
    uint32_t now = now_ms();
    uint32_t kick_total_w = 0, kick_bcast_w = 0, kick_spoof_w = 0;
    uint16_t kick_reason_w = 0;
    uint32_t beacons_w = 0, la_beacons_w = 0;
    bool evil_w = false;
    char evil_ssid_w[33] = {0};
    uint32_t bssids_w = 0, base_bssids = 0, auth_w = 0, auth_src_w = 0;
    bool karma_w = false;
    uint8_t karma_mac[6] = {0};
    uint32_t karma_ssids = 0;

    portENTER_CRITICAL(&s_lock);
    roll_window_locked(now);

    kick_total_w = s_last_kick_total;
    kick_bcast_w = s_last_kick_bcast;
    kick_spoof_w = s_last_kick_spoof;
    kick_reason_w = s_last_kick_reason;
    beacons_w = s_last_beacons;
    la_beacons_w = s_last_la_beacons;
    evil_w = s_last_evil;
    memcpy(evil_ssid_w, s_last_evil_ssid, sizeof(evil_ssid_w));
    bssids_w = s_last_bssids;
    auth_w = s_last_auth;
    auth_src_w = s_last_auth_src;
    if (s_state != NULL) {
        base_bssids = (uint32_t)(s_state->baseline_bssids + 0.5f);
    }

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

            /* Karma/Mana: a source that has probe-responded for many distinct
               SSIDs is answering for networks it doesn't own. */
            uint32_t pr = popcount_bytes((const uint8_t *)&dev->probe_resp_ssids,
                                         sizeof(dev->probe_resp_ssids));
            if (pr > karma_ssids) {
                karma_ssids = pr;
                memcpy(karma_mac, dev->mac, 6);
            }

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

    if (s_state != NULL) {
        snap.baseline_ready = s_state->baseline_samples >= AIRSPACE_BASELINE_WARMUP;
        snap.baseline_pps = (uint32_t)(s_state->baseline_pps + 0.5f);
        snap.baseline_kick = (uint32_t)(s_state->baseline_kick + 0.5f);
        snap.dwelling = s_state->dwelling;

        uint8_t count = s_state->pps_count;
        snap.pps_history_count = count;
        uint16_t maxv = 0;
        for (uint8_t i = 0; i < count; i++) {
            uint8_t idx = (uint8_t)((s_state->pps_head + AIRSPACE_PPS_HISTORY - count + i) % AIRSPACE_PPS_HISTORY);
            uint16_t v = s_state->pps_hist[idx];
            snap.pps_history[i] = v;
            if (v > maxv) maxv = v;
        }
        snap.pps_history_max = maxv;
    }
    portEXIT_CRITICAL(&s_lock);

    uint32_t offender_kick_rate = snap.offender_deauth_rate + snap.offender_disassoc_rate;
    uint32_t global_kick_rate = snap.deauth_per_sec + snap.disassoc_per_sec;

    /* Baseline-relative anomalies. In a normally-quiet environment baseline_kick
       is ~0, so even a few sustained kick frames stand out; in a busy one the
       baseline rises and absorbs harmless churn. This adapts the fixed ceilings
       below to the local environment instead of using one global yardstick. */
    bool kick_anom = snap.baseline_ready && global_kick_rate >= 3 &&
                     global_kick_rate >= snap.baseline_kick + 4;
    bool kick_anom_strong = snap.baseline_ready && global_kick_rate >= 6 &&
                            global_kick_rate >= snap.baseline_kick + 10;
    bool pps_anom = snap.baseline_ready &&
                    snap.packets_per_sec >= snap.baseline_pps * 3 + 50;

    /* Beacon flood: a spike in the number of DISTINCT beaconing APs that is
       dominated by locally-administered (spoofed) BSSIDs. Comparing the
       distinct-AP count to the learned baseline (not a fixed frame count) means
       a dense-but-stable room doesn't trip it — only a surge of *new* spoofed
       APs does. Before the baseline warms up, fall back to a conservative
       absolute floor. la_heavy confirms the surge is spoofed, not just a busy
       channel. beacons_w/la_beacons_w/bssids_w are per ~500ms window. */
    bool la_heavy = beacons_w >= 20 && la_beacons_w * 2U >= beacons_w;
    bool bssid_spike = snap.baseline_ready ? (bssids_w >= base_bssids * 3U + 15U)
                                           : (bssids_w >= 40U);
    bool beacon_flood = la_heavy && bssid_spike;

    /* Karma/Mana: one source probe-responded for many distinct SSIDs. */
    karma_w = karma_ssids >= 6U;

    /* Auth flood: auth frames are rare in normal traffic, so require both a
       high rate and many distinct source MACs (spoofed clients). */
    bool auth_flood = auth_w >= 40U && auth_src_w >= 12U;

    if (offender_kick_rate >= 10 || global_kick_rate >= 30 || kick_anom_strong) {
        snap.threat_level = AIRSPACE_THREAT_ATTACK_LIKELY;
        /* Fingerprint the source from last window's evidence. A spoofed flood
           is mostly broadcast-targeted with a static/zero sequence number; an
           ESP-based tool shows both. A real AP kicking clients does neither. */
        const char *tool = "";
        if (kick_total_w >= 6) {
            bool bcast_heavy = kick_bcast_w * 2U >= kick_total_w;
            bool spoof_heavy = kick_spoof_w * 2U >= kick_total_w;
            if (bcast_heavy && spoof_heavy) tool = " [Marauder/deauther]";
            else if (spoof_heavy)          tool = " [spoofed]";
        }
        if (snap.dwelling) {
            snprintf(snap.reason, sizeof(snap.reason), "Deauth flood %lu/s ch%u r%u%s",
                     (unsigned long)global_kick_rate, (unsigned)snap.current_channel,
                     (unsigned)kick_reason_w, tool);
        } else {
            snprintf(snap.reason, sizeof(snap.reason), "Deauth flood %lu/s r%u%s",
                     (unsigned long)global_kick_rate, (unsigned)kick_reason_w, tool);
        }
    } else if (evil_w) {
        snap.threat_level = AIRSPACE_THREAT_SUSPICIOUS;
        snprintf(snap.reason, sizeof(snap.reason), "Evil twin: %s (sec mismatch)", evil_ssid_w);
    } else if (karma_w) {
        snap.threat_level = AIRSPACE_THREAT_SUSPICIOUS;
        snprintf(snap.reason, sizeof(snap.reason),
                 "Karma AP: %02X:%02X:%02X:%02X:%02X:%02X (%lu SSIDs)",
                 karma_mac[0], karma_mac[1], karma_mac[2], karma_mac[3],
                 karma_mac[4], karma_mac[5], (unsigned long)karma_ssids);
    } else if (offender_kick_rate >= 5 || global_kick_rate >= 15 || kick_anom) {
        snap.threat_level = AIRSPACE_THREAT_SUSPICIOUS;
        snprintf(snap.reason, sizeof(snap.reason), "Elevated kick traffic: %lu/s (base %lu)",
                 (unsigned long)global_kick_rate, (unsigned long)snap.baseline_kick);
    } else if (beacon_flood) {
        snap.threat_level = AIRSPACE_THREAT_SUSPICIOUS;
        snprintf(snap.reason, sizeof(snap.reason), "Beacon flood: %lu spoofed APs/win",
                 (unsigned long)bssids_w);
    } else if (auth_flood) {
        snap.threat_level = AIRSPACE_THREAT_SUSPICIOUS;
        snprintf(snap.reason, sizeof(snap.reason), "Auth flood: ~%lu/s from %lu srcs",
                 (unsigned long)(auth_w * 2U), (unsigned long)auth_src_w);
    } else if (snap.packets_per_sec >= 400 || snap.unique_devices >= 24 || pps_anom) {
        snap.threat_level = AIRSPACE_THREAT_BUSY;
        snprintf(snap.reason, sizeof(snap.reason), "High airtime activity");
    } else {
        snap.threat_level = AIRSPACE_THREAT_QUIET;
        snprintf(snap.reason, sizeof(snap.reason), "No active threat pattern");
    }

    fill_insight(&snap);

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
    bool kick_frame = (frame_type == 0 && (subtype == 0x0C || subtype == 0x0A));
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
            if (s_state != NULL) s_state->dwell_until_ms = now + AIRSPACE_DWELL_MS;
        } else if (subtype == 0x0A) {
            s_disassoc_packets++;
            s_window_disassoc++;
            if (s_state != NULL) s_state->dwell_until_ms = now + AIRSPACE_DWELL_MS;
        }
    } else if (frame_type == 1) {
        s_ctrl_packets++;
    } else if (frame_type == 2) {
        s_data_packets++;
    }

    if (kick_frame) {
        s_window_kick_total++;
        if (len >= 26) {
            s_window_kick_reason = (uint16_t)(frame[24] | (frame[25] << 8));
        }
        /* Addr1 (destination) at frame+4; a broadcast target is the classic
           mass-deauth signature. */
        if (len >= 10 && is_broadcast_mac(frame + 4)) {
            s_window_kick_bcast++;
        }
    }

    if (frame_type == 0 && subtype == 0x08 && len >= 38) {
        s_window_beacons++;
        const uint8_t *bssid = frame + 16;   /* Addr3 */
        if ((bssid[0] & 0x02) != 0) {
            s_window_la_beacons++;           /* locally-administered => likely spoofed */
        }
        if (s_state != NULL) {
            bitmap_set9(s_state->bssid_seen, bssid, 6);  /* distinct-AP estimate */
        }
        /* Capability info at frame[34..35] (Privacy bit => encrypted). */
        bool has_enc = ((frame[34] | (frame[35] << 8)) & 0x0010) != 0;
        const uint8_t *ssid = NULL;
        uint8_t ssid_len = parse_mgmt_ssid(frame, len, &ssid);
        if (ssid_len > 0) {
            airspace_ap_track_locked(bssid, ssid, ssid_len,
                                     pkt->rx_ctrl.channel, pkt->rx_ctrl.rssi,
                                     has_enc, now);
        }
    }

    /* Auth frames are rare in normal traffic; a burst from many distinct
       source MACs is an auth/handshake-farming flood (Step 3). */
    if (frame_type == 0 && subtype == 0x0B) {
        s_window_auth++;
        if (s_state != NULL && has_tx) {
            bitmap_set9(s_state->auth_src_seen, frame + 10, 6);
        }
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
        } else if (frame_type == 0 && subtype == 0x05) {
            /* Probe response: record which SSID this source answered for. A
               Karma/Mana AP answers for many distinct SSIDs it doesn't own. */
            const uint8_t *ssid = NULL;
            uint8_t ssid_len = parse_mgmt_ssid(frame, len, &ssid);
            if (ssid_len > 0) {
                dev->probe_resp_ssids |= (1u << (fnv1a(ssid, ssid_len) & 31u));
            }
        }

        /* Sequence-number spoof detection. Real APs increment the 802.11 seq
           counter on every frame; ESP-based deauth tools repeat or zero it.
           Retransmits legitimately reuse a seq, so skip retries. */
        bool is_retry = (frame[1] & 0x08) != 0;
        if (len >= 24 && !is_retry) {
            uint16_t seq = (uint16_t)(((frame[22] | (frame[23] << 8)) >> 4) & 0x0FFF);
            if (kick_frame && dev->seq_valid && (seq == dev->last_seq || seq == 0)) {
                s_window_kick_spoof++;
            }
            dev->last_seq = seq;
            dev->seq_valid = true;
        }
    }
    portEXIT_CRITICAL(&s_lock);
}
