#include "scans/wifi/airspace_monitor.h"
#include "core/callbacks.h"
#include "core/utils.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "scans/wifi/wifi_channels.h"
#include "scans/wifi/hop_profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AIRSPACE_MAX_DEVICES 32
#define AIRSPACE_DEVICE_TTL_MS 30000U
#define AIRSPACE_HOP_INTERVAL_MS 500U
#define AIRSPACE_RATE_WINDOW_MS 500U

/* Evidence-triggered dwell has a hard deadline and requires a successful hop
   before rearming. Ordinary disconnects do not hold the scanner. */
#define AIRSPACE_DWELL_MS 2500U

/* EWMA baseline tuning. The window rolls every AIRSPACE_RATE_WINDOW_MS, so the
   warmup needs ~4 seconds of clean observation on each channel. */
#define AIRSPACE_BASELINE_WARMUP 8U
#define AIRSPACE_MIN_SAMPLE_MS 250U
#define AIRSPACE_KARMA_PERIOD_MS 4000U
#define AIRSPACE_CRYPTO_BIT 0x80000000U
#define AIRSPACE_TWIN_KICK_BIT 0x40000000U
#define AIRSPACE_CSA_COUNT_MASK 0x3fffffffU
#define AIRSPACE_CONFIRM_WINDOWS 2U
#define AIRSPACE_LOG_INTERVAL_MS 5000U
#define AIRSPACE_ROW_CONFIRM_WINDOWS 2U

typedef struct {
    uint8_t macs[AIRSPACE_MAX_SUSPECTS][6];
    uint8_t challenger[6];
    uint32_t challenger_window;
    uint8_t challenger_streak;
} airspace_rows_t;

#define AIRSPACE_SEC_PRIVACY     0x01U
#define AIRSPACE_SEC_WPA         0x02U
#define AIRSPACE_SEC_RSN         0x04U
#define AIRSPACE_SEC_PSK         0x08U
#define AIRSPACE_SEC_EAP         0x10U
#define AIRSPACE_SEC_SAE         0x20U
#define AIRSPACE_SEC_OWE         0x40U
#define AIRSPACE_SEC_PMF_REQUIRED 0x80U

typedef struct {
    bool used;
    uint8_t mac[6], channel;
    uint32_t last_seen_ms, total, deauth_total, disassoc_total;
    int8_t rssi;
    uint8_t probe_epoch;
    uint16_t sample_ms;
    uint16_t last_deauth_count, last_disassoc_count, win_deauth, win_disassoc;
    uint32_t probe_resp_ssids;
} airspace_device_t;

/* One entry per BSSID, including every observed mesh node. Sharing an SSID
   or differing in WPA/RSN/PMF capabilities alone is not attack evidence. */
#define AIRSPACE_MAX_APS 32
typedef struct {
    bool used;
    uint8_t bssid[6];
    uint8_t ssid_len;
    char ssid[32]; /* length-delimited; terminator lives only in display copies */
    uint8_t channel;
    int8_t rssi;
    uint8_t security;    /* Compact WPA/RSN/AKM/PMF fingerprint */
    uint8_t csa_count;
    uint32_t last_seen_ms;
} airspace_ap_t;

typedef struct {
    uint8_t bssid[2][6];
    uint8_t security[2], channel[2];
    int8_t rssi[2];
} airspace_twin_pair_t;

enum {
    AIRSPACE_PATTERN_NONE = 0, AIRSPACE_PATTERN_EVIL, AIRSPACE_PATTERN_KARMA,
    AIRSPACE_PATTERN_KICK, AIRSPACE_PATTERN_BEACON, AIRSPACE_PATTERN_AUTH,
    AIRSPACE_PATTERN_CSA, AIRSPACE_PATTERN_CRYPTO, AIRSPACE_PATTERN_COUNT
};

/* One fixed-size record for every supported hop-plan slot. Integer EWMAs:
   kick mean/deviation use sixteenths of a frame/second. */
typedef struct {
    uint16_t pps, kick_q4;
    uint8_t noise_q4, bssids, samples, epoch;
} airspace_channel_t;

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

/* Window evidence is latched by the sampling timer, independently of the UI.
   Broadcast/concentration/rate are evidence, never a tool fingerprint. */
static uint32_t s_window_kick_total = 0;   /* kick frames this window */
static uint32_t s_window_kick_bcast = 0;   /* kick frames aimed at broadcast DA */
static uint32_t s_window_csa = 0; /* excess CSA frames; high bit = security change */
static uint16_t s_window_kick_reason = 0;  /* most recent kick reason code */
static uint32_t s_last_kick_total = 0;
static uint32_t s_last_kick_bcast = 0;
static uint32_t s_last_csa = 0;
static uint16_t s_last_kick_reason = 0;

/* Beacon-flood + evil-twin evidence (Step 2). Same window/latch split as the
   kick counters so the classifier reads a stable last-window value. */
static uint32_t s_window_beacons = 0;      /* beacon frames this window */
static uint32_t s_window_la_beacons = 0;   /* beacons from locally-admin BSSIDs */
static uint8_t s_window_evil = 0;          /* candidate SSID length; zero = none */
static char s_window_evil_ssid[33] = {0};
static uint32_t s_last_beacons = 0;         /* normalized frames per second */
static uint32_t s_last_la_beacons = 0;      /* normalized frames per second */
static uint8_t s_last_evil = 0;
static char s_last_evil_ssid[33] = {0};
static airspace_twin_pair_t s_window_twin, s_last_twin;
static uint16_t s_last_bssids = 0;         /* distinct beaconing APs last window */

/* Auth-flood evidence (Step 3). */
static uint32_t s_window_auth = 0;         /* auth frames this window */
static uint32_t s_last_auth = 0;            /* normalized frames per second */
static uint16_t s_last_auth_src = 0;       /* distinct auth source MACs last window */

/* All of the larger / newer mutable state lives in a single heap block (PSRAM
   preferred, like s_devices) so it stays out of BSS. Only this pointer is
   static. NULL whenever the monitor is stopped. */
typedef struct {
    airspace_channel_t channel[WIFI_CHANNELS_MAX];
    uint8_t bssid_seen[64], auth_src_seen[64];
    uint16_t pps_hist[AIRSPACE_PPS_HISTORY];
    airspace_rows_t rows;
    uint32_t dwell_until_ms, window_serial, last_window_ms, log_last_ms;
    uint32_t confirm_twin_key, event_first_ms, event_last_ms;
    uint16_t event_peak, rx_failed, rx_invalid, security_invalid, profile_variations;
    uint16_t win_auth_fail, last_auth_fail, callback_max_us;
    uint16_t detector_epoch[AIRSPACE_PATTERN_COUNT - 1];
    uint8_t detector_history[AIRSPACE_PATTERN_COUNT - 1];
    uint8_t detector_channel[AIRSPACE_PATTERN_COUNT - 1];
    uint8_t pps_head, pps_count, sample_idx, baseline_update_flags;
    uint8_t log_last_pattern, log_last_level, log_ready, evidence_flags;
    uint8_t event_pattern, event_channel, pressure, strong_kick;
    bool dwelling, sampling;
} airspace_state_t;

_Static_assert(sizeof(airspace_device_t) == 40, "device budget exceeded");
_Static_assert(sizeof(airspace_ap_t) == 48, "AP budget exceeded");
_Static_assert(sizeof(airspace_channel_t) == 8, "channel budget exceeded");
_Static_assert(AIRSPACE_MAX_DEVICES * sizeof(airspace_device_t) +
               AIRSPACE_MAX_APS * sizeof(airspace_ap_t) + WIFI_CHANNELS_MAX +
               sizeof(airspace_state_t) <= 3704, "Airspace heap budget exceeded");

static airspace_state_t *s_state = NULL;
static void sat_inc(uint16_t *n);
static uint32_t device_rate(const airspace_device_t *dev, bool disassoc);

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

/* Beacon and probe-response IEs normally start with SSID, but valid frames do
   not have to preserve that order. Walk the bounded TLV list instead. */
static uint8_t parse_mgmt_ssid(const uint8_t *frame, int len, const uint8_t **ssid_out) {
    if (len < 38) return 0;
    int offset = 36;
    while (offset + 2 <= len) {
        uint8_t id = frame[offset];
        uint8_t ie_len = frame[offset + 1];
        offset += 2;
        if (offset + ie_len > len) return 0;
        if (id == 0) {
            if (ie_len == 0 || ie_len > 32) return 0;
            bool named = false;
            for (uint8_t i = 0; i < ie_len; i++) named |= frame[offset + i] != 0;
            if (!named) return 0; /* Zero-filled hidden SSID. */
            *ssid_out = frame + offset;
            return ie_len;
        }
        offset += ie_len;
    }
    return 0;
}

/* Shared WPA/RSN body layout: version, group cipher, pairwise list, AKM list,
   optional capabilities. Reject incomplete lists instead of guessing open. */
static bool parse_security_suites(const uint8_t *ie, size_t len, bool rsn, uint8_t *security) {
    if (len < 18 || ie[0] != 1 || ie[1] != 0) return false;
    size_t pos = 6;
    uint16_t count = (uint16_t)(ie[pos] | (ie[pos + 1] << 8));
    pos += 2;
    if (count == 0 || count > (len - pos) / 4U) return false;
    pos += (size_t)count * 4U;
    if (pos + 2 > len) return false;
    count = (uint16_t)(ie[pos] | (ie[pos + 1] << 8));
    pos += 2;
    if (count == 0 || count > (len - pos) / 4U) return false;
    for (uint16_t i = 0; i < count; i++, pos += 4) {
        bool known_oui = ie[pos] == 0 && ie[pos + 1] == (rsn ? 0x0f : 0x50) &&
                         ie[pos + 2] == (rsn ? 0xac : 0xf2);
        if (!known_oui) continue;
        uint8_t suite = ie[pos + 3];
        if (suite == 1 || (rsn && (suite == 3 || suite == 5 || suite == 11 || suite == 12))) {
            *security |= AIRSPACE_SEC_EAP;
        } else if (suite == 2 || (rsn && (suite == 4 || suite == 6))) {
            *security |= AIRSPACE_SEC_PSK;
        } else if (rsn && (suite == 8 || suite == 9)) {
            *security |= AIRSPACE_SEC_SAE;
        } else if (rsn && suite == 18) {
            *security |= AIRSPACE_SEC_OWE;
        }
    }
    if (pos == len) return true;
    if (pos + 2 > len) return false;
    uint16_t caps = (uint16_t)(ie[pos] | (ie[pos + 1] << 8));
    if (rsn && (caps & 0x0040U)) *security |= AIRSPACE_SEC_PMF_REQUIRED;
    pos += 2;
    if (pos == len) return true;
    if (!rsn || pos + 2 > len) return false;
    count = (uint16_t)(ie[pos] | (ie[pos + 1] << 8)); /* optional PMKID list */
    pos += 2;
    if (count > (len - pos) / 16U) return false;
    pos += (size_t)count * 16U;
    return pos == len || pos + 4 == len; /* optional group-management cipher */
}

static bool parse_mgmt_security(const uint8_t *frame, int len, uint16_t capability,
                                uint8_t *security_out) {
    uint8_t security = (capability & 0x0010U) ? AIRSPACE_SEC_PRIVACY : 0;
    int offset = 36;

    while (offset + 2 <= len) {
        uint8_t id = frame[offset];
        uint8_t ie_len = frame[offset + 1];
        offset += 2;
        if (offset + ie_len > len) return false;

        const uint8_t *ie = frame + offset;
        if (id == 221 && ie_len >= 4 && ie[0] == 0x00 && ie[1] == 0x50 &&
            ie[2] == 0xF2 && ie[3] == 0x01) {
            if ((security & AIRSPACE_SEC_WPA) ||
                !parse_security_suites(ie + 4, ie_len - 4U, false, &security)) return false;
            security |= AIRSPACE_SEC_WPA;
        } else if (id == 48) {
            if ((security & AIRSPACE_SEC_RSN) ||
                !parse_security_suites(ie, ie_len, true, &security)) return false;
            security |= AIRSPACE_SEC_RSN;
        }
        offset += ie_len;
    }
    if (offset != len) return false;
    if ((security & (AIRSPACE_SEC_WPA | AIRSPACE_SEC_RSN)) &&
        !(security & AIRSPACE_SEC_PRIVACY)) return false;
    *security_out = security;
    return true;
}

/* A capability mismatch is common across mesh nodes/bands. Only clear loss of
   encryption or incompatible credential families is a twin candidate. Treat
   WPA2/WPA3 personal transition/FT variants as the same credential family. */
static bool security_conflict(uint8_t a, uint8_t b) {
    if ((a & AIRSPACE_SEC_PRIVACY) != (b & AIRSPACE_SEC_PRIVACY)) return true;
    if (!(a & AIRSPACE_SEC_PRIVACY)) return false;
    const uint8_t modern = AIRSPACE_SEC_WPA | AIRSPACE_SEC_RSN;
    if (!!(a & modern) != !!(b & modern)) return true;
    uint8_t family_a = a & (AIRSPACE_SEC_PSK | AIRSPACE_SEC_SAE | AIRSPACE_SEC_EAP | AIRSPACE_SEC_OWE);
    uint8_t family_b = b & (AIRSPACE_SEC_PSK | AIRSPACE_SEC_SAE | AIRSPACE_SEC_EAP | AIRSPACE_SEC_OWE);
    if (family_a & (AIRSPACE_SEC_PSK | AIRSPACE_SEC_SAE)) family_a |= AIRSPACE_SEC_PSK | AIRSPACE_SEC_SAE;
    if (family_b & (AIRSPACE_SEC_PSK | AIRSPACE_SEC_SAE)) family_b |= AIRSPACE_SEC_PSK | AIRSPACE_SEC_SAE;
    /* Unknown AKMs must not be guessed to be an incompatible/open network. */
    return family_a && family_b && !(family_a & family_b);
}

static bool is_broadcast_mac(const uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0xff) return false;
    }
    return true;
}

/* Called under s_lock. Keep all mesh BSSIDs; compare only valid named beacons. */
static airspace_ap_t *airspace_ap_track_locked(const uint8_t *bssid, const uint8_t *ssid,
                                     uint8_t ssid_len, uint8_t channel,
                                     int8_t rssi, uint8_t security, uint32_t now) {
    if (s_aps == NULL || ssid_len == 0 || ssid_len > 32) {
        return NULL;
    }

    int free_slot = -1;
    int oldest = 0;
    uint32_t oldest_seen = UINT32_MAX;
    int existing = -1;

    for (int i = 0; i < AIRSPACE_MAX_APS; i++) {
        airspace_ap_t *ap = &s_aps[i];
        if (!ap->used) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if ((uint32_t)(now - ap->last_seen_ms) > AIRSPACE_DEVICE_TTL_MS) {
            memset(ap, 0, sizeof(*ap));
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (memcmp(ap->bssid, bssid, 6) == 0) {
            existing = i;
        } else if (ap->ssid_len == ssid_len && memcmp(ap->ssid, ssid, ssid_len) == 0) {
            if (ap->security != security) {
                if (security_conflict(ap->security, security)) {
                    if (!s_window_evil) {
                        /* Canonical order makes pair identity independent of RX order. */
                        unsigned peer = memcmp(ap->bssid, bssid, 6) < 0 ? 0 : 1;
                        memcpy(s_window_twin.bssid[peer], ap->bssid, 6);
                        memcpy(s_window_twin.bssid[1 - peer], bssid, 6);
                        s_window_twin.security[peer] = ap->security;
                        s_window_twin.security[1 - peer] = security;
                        s_window_twin.channel[peer] = ap->channel;
                        s_window_twin.channel[1 - peer] = channel;
                        s_window_twin.rssi[peer] = ap->rssi;
                        s_window_twin.rssi[1 - peer] = rssi;
                        memcpy(s_window_evil_ssid, ssid, ssid_len);
                        s_window_evil_ssid[ssid_len] = '\0';
                    }
                    s_window_evil = ssid_len;
                } else if (s_state != NULL) sat_inc(&s_state->profile_variations);
            }
        }
        bool pinned = (s_window_evil &&
                       (!memcmp(ap->bssid, s_window_twin.bssid[0], 6) ||
                        !memcmp(ap->bssid, s_window_twin.bssid[1], 6))) ||
                      (s_last_evil &&
                       (!memcmp(ap->bssid, s_last_twin.bssid[0], 6) ||
                        !memcmp(ap->bssid, s_last_twin.bssid[1], 6)));
        if (!pinned && ap->last_seen_ms < oldest_seen) {
            oldest_seen = ap->last_seen_ms;
            oldest = i;
        }
    }

    if (existing < 0 && free_slot < 0 && s_state) s_state->pressure |= 2U;
    int slot = existing >= 0 ? existing : (free_slot >= 0 ? free_slot : oldest);
    airspace_ap_t *ap = &s_aps[slot];
    if (existing >= 0 && ap->security != security && !s_window_evil) {
        /* A factual configuration change is useful context, not an attack
           verdict. Retain the two profiles in the existing evidence record. */
        s_window_csa |= AIRSPACE_CRYPTO_BIT;
        memcpy(s_window_evil_ssid, ssid, ssid_len);
        s_window_evil_ssid[ssid_len] = 0;
        for (unsigned i = 0; i < 2; i++) {
            memcpy(s_window_twin.bssid[i], bssid, 6);
            s_window_twin.channel[i] = channel;
            s_window_twin.rssi[i] = rssi;
        }
        s_window_twin.security[0] = ap->security;
        s_window_twin.security[1] = security;
    }
    if (existing < 0) memset(ap, 0, sizeof(*ap));
    ap->used = true;
    memcpy(ap->bssid, bssid, 6);
    memcpy(ap->ssid, ssid, ssid_len);
    ap->ssid_len = ssid_len;
    ap->channel = channel;
    ap->rssi = rssi;
    ap->security = security;
    ap->last_seen_ms = now;
    return ap;
}

static int find_device_slot_locked(const uint8_t mac[6], uint32_t now, bool candidate) {
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
        bool protected = dev->win_deauth || dev->win_disassoc ||
                         ((uint32_t)(now - dev->last_seen_ms) <= 3000U &&
                          (dev->last_deauth_count || dev->last_disassoc_count)) ||
                         popcount_bytes((const uint8_t *)&dev->probe_resp_ssids, 4) >= 2U;
        if (dev->used && !protected && dev->last_seen_ms < oldest_seen) {
            oldest_seen = dev->last_seen_ms;
            oldest_slot = i;
        }
    }

    if (free_slot < 0) {
        if (s_state) s_state->pressure |= 1U;
        if (oldest_seen == UINT32_MAX && !candidate) return -1;
    }
    int slot = free_slot >= 0 ? free_slot : oldest_slot;
    memset(&s_devices[slot], 0, sizeof(s_devices[slot]));
    s_devices[slot].used = true;
    memcpy(s_devices[slot].mac, mac, 6);
    return slot;
}

static void sat_inc(uint16_t *n) { if (*n != UINT16_MAX) ++*n; }
static uint8_t pattern_bit(uint8_t pattern) { return (uint8_t)(1U << (pattern - 1U)); }
static uint32_t device_rate(const airspace_device_t *dev, bool disassoc);

static uint32_t twin_key(void) {
    return fnv1a((const uint8_t *)s_last_twin.bssid, 12) ^
           fnv1a(s_last_twin.security, 2) ^
           fnv1a((const uint8_t *)s_last_evil_ssid, s_last_evil);
}

static uint8_t evidence_count(uint8_t history) {
    return (uint8_t)popcount_bytes(&history, 1);
}

static bool confirmed(uint8_t pattern) {
    if (!s_state || !s_state->last_window_ms) return false;
    unsigned i = pattern - 1U;
    uint16_t epoch = (uint16_t)(now_ms() / AIRSPACE_MIN_SAMPLE_MS);
    return s_state->detector_channel[i] == s_channels[s_state->sample_idx] &&
           (uint16_t)(epoch - s_state->detector_epoch[i]) <= 32U &&
           (s_state->detector_history[i] & 3U) == 3U;
}

static uint8_t selected_pattern(bool require_confirmation) {
    if (!s_state) return AIRSPACE_PATTERN_NONE;
    if ((s_state->strong_kick & 1U) && (s_state->evidence_flags & pattern_bit(AIRSPACE_PATTERN_KICK)))
        return AIRSPACE_PATTERN_KICK;
    for (uint8_t p = 1; p < AIRSPACE_PATTERN_COUNT; p++) {
        if (!(s_state->evidence_flags & pattern_bit(p))) continue;
        if (!require_confirmation || confirmed(p) || p == AIRSPACE_PATTERN_CRYPTO) return p;
    }
    return AIRSPACE_PATTERN_NONE;
}

static void reset_window_locked(uint32_t now) {
    s_window_packets = s_window_deauth = s_window_disassoc = 0;
    s_window_kick_total = s_window_kick_bcast = s_window_csa = 0;
    s_window_kick_reason = 0;
    s_window_beacons = s_window_la_beacons = s_window_auth = 0;
    s_window_evil = false;
    s_window_evil_ssid[0] = 0;
    if (s_state) {
        memset(s_state->bssid_seen, 0, sizeof(s_state->bssid_seen));
        memset(s_state->auth_src_seen, 0, sizeof(s_state->auth_src_seen));
        s_state->win_auth_fail = 0;
    }
    if (s_devices) for (int i = 0; i < AIRSPACE_MAX_DEVICES; i++)
        s_devices[i].win_deauth = s_devices[i].win_disassoc = 0;
    if (s_aps) for (int i = 0; i < AIRSPACE_MAX_APS; i++) s_aps[i].csa_count = 0;
    s_window_start_ms = now;
}

/* A window belongs to exactly one channel. Hop boundaries close it before
   switching radios; no snapshots or screen refreshes are needed to advance
   baselines, evidence histories, expiry or dwell decisions. */
static void finish_window_locked(uint32_t now, bool force) {
    if (!s_state || !s_state->sampling) return;
    uint32_t elapsed = now - s_window_start_ms;
    if (!force && elapsed < AIRSPACE_RATE_WINDOW_MS) return;
    if (elapsed < AIRSPACE_MIN_SAMPLE_MS) {
        if (force) reset_window_locked(now);
        return;
    }
    airspace_state_t *st = s_state;
    airspace_channel_t *ch = &st->channel[s_channel_idx];
    uint8_t epoch = (uint8_t)(now / 8000U);
    if ((uint8_t)(epoch - ch->epoch) > 15U) memset(ch, 0, sizeof(*ch));
    ch->epoch = epoch;
    st->sample_idx = s_channel_idx;
    st->last_window_ms = elapsed;
    st->window_serial++;
    s_last_pps = (s_window_packets * 1000U) / elapsed;
    s_last_deauth_pps = (s_window_deauth * 1000U) / elapsed;
    s_last_disassoc_pps = (s_window_disassoc * 1000U) / elapsed;
    s_last_kick_total = s_window_kick_total;
    s_last_kick_bcast = s_window_kick_bcast;
    s_last_kick_reason = s_window_kick_reason;
    s_last_csa = s_window_csa;
    s_last_beacons = (s_window_beacons * 1000U) / elapsed;
    s_last_la_beacons = (s_window_la_beacons * 1000U) / elapsed;
    s_last_auth = (s_window_auth * 1000U) / elapsed;
    s_last_evil = s_window_evil;
    memcpy(s_last_evil_ssid, s_window_evil_ssid, sizeof(s_last_evil_ssid));
    s_last_twin = s_window_twin;
    s_last_bssids = (uint16_t)popcount_bytes(st->bssid_seen, sizeof(st->bssid_seen));
    s_last_auth_src = (uint16_t)popcount_bytes(st->auth_src_seen, sizeof(st->auth_src_seen));
    st->last_auth_fail = st->win_auth_fail;
    uint32_t top_kick = 0, karma = 0;
    uint8_t probe_epoch = (uint8_t)(now / AIRSPACE_KARMA_PERIOD_MS);
    for (int i = 0; i < AIRSPACE_MAX_DEVICES; i++) {
        airspace_device_t *d = &s_devices[i];
        if (!d->used) continue;
        if (d->probe_epoch != probe_epoch) { d->probe_resp_ssids = 0; d->probe_epoch = probe_epoch; }
        if (d->channel != s_current_channel) continue;
        d->sample_ms = (uint16_t)(elapsed <= UINT16_MAX ? elapsed : 1000U);
        d->last_deauth_count = elapsed <= UINT16_MAX ? d->win_deauth :
            (uint16_t)((uint32_t)d->win_deauth * 1000U / elapsed);
        d->last_disassoc_count = elapsed <= UINT16_MAX ? d->win_disassoc :
            (uint16_t)((uint32_t)d->win_disassoc * 1000U / elapsed);
        uint32_t rate = ((uint32_t)d->win_deauth + d->win_disassoc) * 1000U / elapsed;
        if (rate > top_kick) top_kick = rate;
        uint32_t bins = popcount_bytes((const uint8_t *)&d->probe_resp_ssids, 4);
        if (bins > karma) karma = bins;
    }
    uint32_t kick = s_last_deauth_pps + s_last_disassoc_pps;
    uint32_t base = ch->kick_q4 / 16U, noise = ch->noise_q4 / 16U;
    uint32_t margin = 4U + noise * 4U;
    bool ready = ch->samples >= AIRSPACE_BASELINE_WARMUP;
    bool broadcast = s_last_kick_total >= 6U && s_last_kick_bcast * 2U >= s_last_kick_total;
    bool concentrated = kick >= 10U && top_kick * 4U >= kick * 3U;
    bool kick_anom = kick >= 10U && (ready ? kick >= base + margin : kick >= 30U);
    bool beacon = s_last_beacons >= 40U && s_last_la_beacons * 2U >= s_last_beacons &&
                  (ready ? s_last_bssids >= (uint32_t)ch->bssids * 3U + 15U :
                           s_last_bssids >= 80U && s_last_beacons >= 200U);
    uint32_t failures = (uint32_t)st->last_auth_fail * 1000U / elapsed;
    bool auth = s_last_auth >= 80U && s_last_auth_src >= 12U &&
                (ready || st->last_auth_fail >= 20U) &&
                (s_last_auth * 2U >= s_last_pps || st->last_auth_fail >= 20U) &&
                (s_last_auth >= (uint32_t)ch->pps / 4U + 30U || st->last_auth_fail >= 20U);
    auth |= failures >= 40U && s_last_auth_src >= 12U &&
            failures >= (uint32_t)ch->pps / 4U + 20U;
    st->strong_kick = kick_anom &&
                     ((broadcast && kick >= 30U) ||
                      (concentrated && kick >= (ready ? base + margin * 3U : 100U)));
    uint8_t flags = (s_last_evil ? pattern_bit(AIRSPACE_PATTERN_EVIL) : 0) |
                    (karma >= 6U ? pattern_bit(AIRSPACE_PATTERN_KARMA) : 0) |
                    (kick_anom ? pattern_bit(AIRSPACE_PATTERN_KICK) : 0) |
                    (beacon ? pattern_bit(AIRSPACE_PATTERN_BEACON) : 0) |
                    (auth ? pattern_bit(AIRSPACE_PATTERN_AUTH) : 0) |
                    ((s_last_csa & AIRSPACE_CSA_COUNT_MASK) ? pattern_bit(AIRSPACE_PATTERN_CSA) : 0) |
                    ((s_last_csa & AIRSPACE_CRYPTO_BIT) ? pattern_bit(AIRSPACE_PATTERN_CRYPTO) : 0);
    if (s_last_evil) {
        uint32_t key = twin_key();
        if (key != st->confirm_twin_key) st->detector_history[AIRSPACE_PATTERN_EVIL - 1] = 0;
        st->confirm_twin_key = key;
    }
    uint16_t tick = (uint16_t)(now / AIRSPACE_MIN_SAMPLE_MS);
    for (unsigned i = 0; i < AIRSPACE_PATTERN_COUNT - 1; i++) {
        bool present = flags & (1U << i);
        if ((uint16_t)(tick - st->detector_epoch[i]) > 32U) st->detector_history[i] = 0;
        if (st->detector_channel[i] != s_current_channel) {
            if (!present) continue;
            st->detector_history[i] = 0;
            st->detector_channel[i] = s_current_channel;
        }
        st->detector_history[i] = (uint8_t)(((st->detector_history[i] << 1) | present) & 15U);
        st->detector_epoch[i] = tick;
    }
    st->evidence_flags = flags;
    bool hostile = (kick_anom && (broadcast || concentrated)) || beacon || auth ||
                   s_last_evil || (flags & pattern_bit(AIRSPACE_PATTERN_CSA));
    st->baseline_update_flags = (hostile ? 4U : 0U) | (st->dwelling ? 8U : 0U);
    if (!hostile) {
        /* Fast startup, slow mature mean and mean absolute deviation. Never
           learn a hostile window after warmup either. */
        unsigned divisor = ready ? 16U : 4U;
        uint32_t pps = s_last_pps > UINT16_MAX ? UINT16_MAX : s_last_pps;
        uint32_t qkick = kick > 4095U ? UINT16_MAX : kick * 16U;
        uint32_t delta = qkick > ch->kick_q4 ? qkick - ch->kick_q4 : ch->kick_q4 - qkick;
        if (delta > UINT8_MAX) delta = UINT8_MAX;
        ch->pps = (uint16_t)((int32_t)ch->pps + ((int32_t)pps - ch->pps) / (int)divisor);
        ch->kick_q4 = (uint16_t)((int32_t)ch->kick_q4 + ((int32_t)qkick - ch->kick_q4) / (int)divisor);
        ch->noise_q4 = (uint8_t)((int)ch->noise_q4 + ((int)delta - ch->noise_q4) / (int)divisor);
        uint32_t bssids = s_last_bssids > UINT8_MAX ? UINT8_MAX : s_last_bssids;
        ch->bssids = (uint8_t)((int)ch->bssids + ((int)bssids - ch->bssids) / (int)divisor);
        if (ch->samples < UINT8_MAX) ch->samples++;
        st->baseline_update_flags |= 3U;
    }
    st->pps_hist[st->pps_head] = s_last_pps > UINT16_MAX ? UINT16_MAX : (uint16_t)s_last_pps;
    st->pps_head = (uint8_t)((st->pps_head + 1) % AIRSPACE_PPS_HISTORY);
    if (st->pps_count < AIRSPACE_PPS_HISTORY) st->pps_count++;
    uint8_t pattern = selected_pattern(true);
    if (pattern) {
        if (st->event_pattern != pattern || st->event_channel != s_current_channel ||
            (uint32_t)(now - st->event_last_ms) > 8000U) {
            st->event_first_ms = now; st->event_peak = 0;
        }
        st->event_pattern = pattern;
        st->event_channel = s_current_channel;
        st->event_last_ms = now;
        uint32_t peak = pattern == AIRSPACE_PATTERN_KICK ? kick :
                        pattern == AIRSPACE_PATTERN_AUTH ? s_last_auth :
                        pattern == AIRSPACE_PATTERN_CSA ? (s_last_csa & AIRSPACE_CSA_COUNT_MASK) * 1000U / elapsed :
                        s_last_beacons;
        if (peak > st->event_peak) st->event_peak = peak > UINT16_MAX ? UINT16_MAX : (uint16_t)peak;
    }
    /* An isolated disconnect cannot trigger a hold. Holds never extend:
       a successful hop is required before another one can be armed. */
    uint8_t follow_up = flags & (pattern_bit(AIRSPACE_PATTERN_EVIL) | pattern_bit(AIRSPACE_PATTERN_KARMA) |
                                pattern_bit(AIRSPACE_PATTERN_BEACON) | pattern_bit(AIRSPACE_PATTERN_AUTH) |
                                pattern_bit(AIRSPACE_PATTERN_CSA));
    if (!st->dwell_until_ms && s_channel_count > 1 &&
        (follow_up || (kick_anom && (broadcast || concentrated)))) {
        st->dwell_until_ms = now + AIRSPACE_DWELL_MS;
        st->dwelling = true;
    }
    st->pressure = (uint8_t)((st->pressure & 3U) << 4);
    reset_window_locked(now);
}

static void roll_window_locked(uint32_t now) { finish_window_locked(now, false); }

static void build_channel_list(void) {
    if (s_channels == NULL) {
        s_channel_count = 0;
        return;
    }

    uint8_t candidates[WIFI_CHANNELS_MAX] = {0};

    // User hop profile takes priority; otherwise build the country-appropriate
    // list (with the legacy fallback if the country query comes up empty).
    size_t profile_count = 0;
    hop_profile_resolve_monitor(candidates, WIFI_CHANNELS_MAX, &profile_count);
    uint8_t candidate_count;
    if (profile_count > 0) {
        candidate_count = (uint8_t)profile_count;
    } else {
        candidate_count = wifi_channels_build_country_list(candidates, WIFI_CHANNELS_MAX);
        if (candidate_count == 0) {
            candidates[0] = 1;
            candidate_count = 1;
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
        if (!wifi_channels_is_monitor_channel(ch)) {
            ESP_LOGD(TAG, "Skipping country/unsupported monitor channel %u", (unsigned)ch);
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
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    if (!s_active || !s_state || !s_channel_count) { portEXIT_CRITICAL(&s_lock); return; }
    roll_window_locked(now);
    if (s_state->dwell_until_ms && (int32_t)(s_state->dwell_until_ms - now) > 0) {
        s_state->dwelling = true;
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    finish_window_locked(now, true);
    s_state->sampling = false;
    s_state->dwelling = false;
    uint8_t previous = s_channel_idx;
    uint8_t count = s_channel_count;
    int64_t session = s_start_us;
    portEXIT_CRITICAL(&s_lock);
    for (uint8_t attempt = 1; attempt <= count; attempt++) {
        portENTER_CRITICAL(&s_lock);
        if (!s_active || !s_state || !s_channels || s_start_us != session) {
            portEXIT_CRITICAL(&s_lock);
            return;
        }
        uint8_t idx = (uint8_t)((previous + attempt) % count);
        uint8_t channel = s_channels[idx];
        portEXIT_CRITICAL(&s_lock);
        esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        portENTER_CRITICAL(&s_lock);
        if (!s_active || !s_state || s_start_us != session) {
            portEXIT_CRITICAL(&s_lock);
            return;
        }
        if (err == ESP_OK) {
            s_channel_idx = idx;
            s_current_channel = channel;
            s_hop_success++;
            s_window_start_ms = now_ms();
            s_state->sampling = true;
            s_state->dwell_until_ms = 0;
            portEXIT_CRITICAL(&s_lock);
            return;
        }
        s_hop_fail++;
        portEXIT_CRITICAL(&s_lock);
    }
    portENTER_CRITICAL(&s_lock);
    if (s_active && s_state && s_start_us == session) {
        s_window_start_ms = now_ms();
        s_state->sampling = true;
    }
    /* Keep the expired deadline: a failed hop cannot rearm another hold. */
    portEXIT_CRITICAL(&s_lock);
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
        s_state->sampling = true;
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
    s_window_csa = 0;
    s_window_kick_reason = 0;
    s_last_kick_total = 0;
    s_last_kick_bcast = 0;
    s_last_csa = 0;
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
    memset(&s_window_twin, 0, sizeof(s_window_twin));
    memset(&s_last_twin, 0, sizeof(s_last_twin));
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

    ESP_LOGI(TAG, "started channels=%u window=%ums hop=%ums dwell=%ums warmup=%u confirm=%u ttl=%lums row_confirm=%u row_margin=200/25%%",
             (unsigned)s_channel_count, AIRSPACE_RATE_WINDOW_MS, AIRSPACE_HOP_INTERVAL_MS,
             AIRSPACE_DWELL_MS, AIRSPACE_BASELINE_WARMUP, AIRSPACE_CONFIRM_WINDOWS,
             (unsigned long)AIRSPACE_DEVICE_TTL_MS, AIRSPACE_ROW_CONFIRM_WINDOWS);
    return ESP_OK;
}

void airspace_monitor_stop(void) {
    bool was_active = s_active;
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
    if (was_active) {
        ESP_LOGI(TAG, "stopped packets=%lu kicks=%lu hops=%lu/%lu",
                 (unsigned long)s_total_packets,
                 (unsigned long)(s_deauth_packets + s_disassoc_packets),
                 (unsigned long)s_hop_success, (unsigned long)s_hop_fail);
    }
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

static uint32_t device_rate(const airspace_device_t *dev, bool disassoc) {
    if (!dev->sample_ms || (uint32_t)(now_ms() - dev->last_seen_ms) > 3000U) return 0;
    return (uint32_t)(disassoc ? dev->last_disassoc_count : dev->last_deauth_count) * 1000U / dev->sample_ms;
}

static uint32_t suspect_score(const airspace_device_t *dev) {
    uint32_t rate = device_rate(dev, false) + device_rate(dev, true);
    uint32_t total = dev->deauth_total + dev->disassoc_total;
    return rate * 200U + (total > 199U ? 199U : total);
}

static void fill_suspect(const airspace_device_t *dev, airspace_suspect_t *out) {
    memcpy(out->mac, dev->mac, 6);
    out->channel = dev->channel;
    out->rssi = dev->rssi;
    out->deauth_rate = device_rate(dev, false);
    out->disassoc_rate = device_rate(dev, true);
    out->deauth_total = dev->deauth_total;
    out->disassoc_total = dev->disassoc_total;
    out->total = dev->total;
    out->score = suspect_score(dev);
}

static bool contains_suspect(const airspace_monitor_snapshot_t *snap, const uint8_t mac[6]) {
    for (uint8_t i = 0; i < snap->suspect_count; i++)
        if (memcmp(snap->suspects[i].mac, mac, 6) == 0) return true;
    return false;
}

/* Presentation order only. The live top scorer is latched into offender_*
   before this runs, so row retention never delays or lowers an alert. */
static bool stabilize_suspects_locked(airspace_monitor_snapshot_t *snap, uint32_t now) {
    if (!s_state || !s_devices) return false;
    airspace_rows_t *rows = &s_state->rows;
    airspace_suspect_t ranked[AIRSPACE_MAX_SUSPECTS];
    uint8_t ranked_count = snap->suspect_count;
    memcpy(ranked, snap->suspects, sizeof(ranked));
    snap->suspect_count = 0;

    // Keep fresh incumbents in their existing order, with current metrics.
    for (uint8_t row = 0; row < AIRSPACE_MAX_SUSPECTS; row++) {
        for (uint8_t i = 0; i < AIRSPACE_MAX_DEVICES; i++) {
            const airspace_device_t *dev = &s_devices[i];
            if (!dev->used || (uint32_t)(now - dev->last_seen_ms) > AIRSPACE_DEVICE_TTL_MS ||
                !suspect_score(dev) || memcmp(rows->macs[row], dev->mac, 6) != 0) continue;
            fill_suspect(dev, &snap->suspects[snap->suspect_count++]);
            break;
        }
    }
    for (uint8_t i = 0; i < ranked_count && snap->suspect_count < AIRSPACE_MAX_SUSPECTS; i++) {
        if (!contains_suspect(snap, ranked[i].mac))
            snap->suspects[snap->suspect_count++] = ranked[i];
    }

    const airspace_suspect_t *challenger = NULL;
    for (uint8_t i = 0; i < ranked_count; i++) {
        if (!contains_suspect(snap, ranked[i].mac)) { challenger = &ranked[i]; break; }
    }
    uint8_t weakest = 0;
    for (uint8_t i = 1; i < snap->suspect_count; i++)
        if (snap->suspects[i].score <= snap->suspects[weakest].score) weakest = i;
    uint32_t incumbent_score = snap->suspect_count ? snap->suspects[weakest].score : 0;
    bool promote = challenger && challenger->score > incumbent_score &&
                   challenger->score - incumbent_score >= 200U &&
                   (uint64_t)challenger->score * 4U >= (uint64_t)incumbent_score * 5U;
    if (!promote) {
        rows->challenger_streak = 0;
    } else {
        uint32_t window = s_state->window_serial;
        if (!rows->challenger_streak || memcmp(rows->challenger, challenger->mac, 6) != 0) {
            memcpy(rows->challenger, challenger->mac, 6);
            rows->challenger_streak = 1;
        } else if (window != rows->challenger_window) {
            rows->challenger_streak = window - rows->challenger_window == 1U
                                          ? rows->challenger_streak + 1 : 1;
        }
        rows->challenger_window = window;
        if (rows->challenger_streak >= AIRSPACE_ROW_CONFIRM_WINDOWS) {
            snap->suspects[weakest] = *challenger;
            rows->challenger_streak = 0;
        }
    }
    bool changed = false;
    for (uint8_t i = 0; i < AIRSPACE_MAX_SUSPECTS; i++) {
        uint8_t empty[6] = {0};
        const uint8_t *mac = i < snap->suspect_count ? snap->suspects[i].mac : empty;
        if (memcmp(rows->macs[i], mac, 6) != 0) changed = true;
        memcpy(rows->macs[i], mac, 6);
    }
    return changed;
}

typedef struct {
    uint32_t window, elapsed_ms, baseline_samples, base_bssids;
    uint32_t kick_total, kick_bcast, csa;
    uint32_t beacons, la_beacons, bssids, auth, auth_src, karma_ssids;
    uint16_t kick_reason;
    uint8_t baseline_updates, flags;
    bool evil, rows_changed, correlated;
    uint32_t event_first, event_last;
    uint16_t event_peak, auth_fail, callback_max_us;
    uint8_t event_pattern, event_channel;
    uint32_t histories;
    uint32_t rx_failed, rx_invalid, security_invalid, profile_variations;
    airspace_twin_pair_t twin;
    char twin_ssid[33];
} airspace_diagnostics_t;

static void log_mac(const uint8_t mac[6], char out[18]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    privacy_redact_mac(out);
}

static void log_snapshot(const airspace_monitor_snapshot_t *snap,
                         const airspace_diagnostics_t *d, uint8_t pattern, uint32_t now) {
    if (!s_state || !snap->active || esp_log_level_get(TAG) < ESP_LOG_INFO) return;
    airspace_state_t *st = s_state;
    bool decision_changed = st->log_ready &&
        (st->log_last_pattern != pattern || st->log_last_level != snap->threat_level);
    bool periodic = !st->log_ready || (uint32_t)(now - st->log_last_ms) >= AIRSPACE_LOG_INTERVAL_MS;
    // State changes are useful immediately; row churn alone is capped at 1 Hz.
    if (!periodic && !decision_changed &&
        !(d->rows_changed && (uint32_t)(now - st->log_last_ms) >= 1000U)) return;
    st->log_ready = 1;
    st->log_last_ms = now;
    st->log_last_pattern = pattern;
    st->log_last_level = (uint8_t)snap->threat_level;
    static const char *const patterns[] = {"none", "evil", "karma", "kick", "beacon", "auth", "csa", "security-change"};
    ESP_LOGI(TAG, "sample win=%lu dt=%lums ch_now=%u ch_obs=%u mode=%s pps=%lu base_pps=%lu kick/s=%lu base_kick=%lu devices=%u/%u hops=%lu/%lu",
             (unsigned long)d->window, (unsigned long)d->elapsed_ms, snap->current_channel,
             snap->sample_channel,
             snap->dwelling ? "dwell" : "hop", (unsigned long)snap->packets_per_sec,
             (unsigned long)snap->baseline_pps,
             (unsigned long)(snap->deauth_per_sec + snap->disassoc_per_sec),
             (unsigned long)snap->baseline_kick, snap->unique_devices, AIRSPACE_MAX_DEVICES,
             (unsigned long)snap->hop_success, (unsigned long)snap->hop_fail);
    ESP_LOGI(TAG, "frames total=%lu mgmt=%lu data=%lu ctrl=%lu probes=%lu auth=%lu assoc=%lu",
             (unsigned long)snap->total_packets, (unsigned long)snap->mgmt_packets,
             (unsigned long)snap->data_packets, (unsigned long)snap->ctrl_packets,
             (unsigned long)snap->probe_packets, (unsigned long)snap->auth_packets,
             (unsigned long)snap->assoc_packets);
    ESP_LOGI(TAG, "evidence kick_n=%lu bcast=%lu excess_csa=%lu reason=%u bcn/s=%lu la/s=%lu bssid_est=%lu base_bssid=%lu auth/s=%lu auth_src_est=%lu karma_bins=%lu evil=%u",
             (unsigned long)d->kick_total, (unsigned long)d->kick_bcast,
             (unsigned long)d->csa, d->kick_reason, (unsigned long)d->beacons,
             (unsigned long)d->la_beacons, (unsigned long)d->bssids, (unsigned long)d->base_bssids,
             (unsigned long)d->auth, (unsigned long)d->auth_src, (unsigned long)d->karma_ssids, d->evil);
    ESP_LOGI(TAG, "validation rx_failed=%lu rx_invalid=%lu security_invalid=%lu compatible_profiles=%lu",
             (unsigned long)d->rx_failed, (unsigned long)d->rx_invalid,
             (unsigned long)d->security_invalid, (unsigned long)d->profile_variations);
    ESP_LOGI(TAG, "detectors evil=%x karma=%x kick=%x beacon=%x auth=%x csa=%x change=%x coverage=0x%02x auth_fail=%u callback_max_us=%u correlated=%u",
             (unsigned)(d->histories & 15U), (unsigned)((d->histories >> 4) & 15U),
             (unsigned)((d->histories >> 8) & 15U), (unsigned)((d->histories >> 12) & 15U),
             (unsigned)((d->histories >> 16) & 15U), (unsigned)((d->histories >> 20) & 15U),
             (unsigned)((d->histories >> 24) & 15U), snap->coverage_flags, d->auth_fail, d->callback_max_us, d->correlated);
    if (d->evil || (d->flags & pattern_bit(AIRSPACE_PATTERN_CRYPTO))) {
        char ssid[33], a[18], b[18];
        privacy_redact_ssid_copy(d->twin_ssid, ssid, sizeof(ssid));
        log_mac(d->twin.bssid[0], a);
        log_mac(d->twin.bssid[1], b);
        ESP_LOGI(TAG, "twin ssid=\"%s\" a=%s sec=0x%02x ch=%u rssi=%d b=%s sec=0x%02x ch=%u rssi=%d",
                 ssid, a, d->twin.security[0], d->twin.channel[0], d->twin.rssi[0],
                 b, d->twin.security[1], d->twin.channel[1], d->twin.rssi[1]);
    }
    ESP_LOGI(TAG, "decision level=%s candidate=%s confirm=%u/%u ready=%u samples=%lu base_update=0x%02x flags=0x%02x reason=\"%s\" insight_level=%s insight=\"%s\"",
             airspace_monitor_threat_label(snap->threat_level), patterns[pattern],
             pattern ? (((d->histories >> ((pattern - 1U) * 4U)) & 3U) == 3U ? 2U : 1U) : 0U, AIRSPACE_CONFIRM_WINDOWS, snap->baseline_ready,
             (unsigned long)d->baseline_samples, d->baseline_updates, d->flags,
             snap->reason, airspace_monitor_threat_label(snap->insight_level), snap->insight);
    char mac[18];
    if (snap->has_offender) {
        log_mac(snap->offender_mac, mac);
        ESP_LOGI(TAG, "lead mac=%s ch=%u rssi=%d kick/s=%lu total=%lu", mac,
                 snap->offender_channel, snap->offender_rssi,
                 (unsigned long)(snap->offender_deauth_rate + snap->offender_disassoc_rate),
                 (unsigned long)snap->offender_total);
    }
    for (uint8_t i = 0; i < snap->suspect_count; i++) {
        const airspace_suspect_t *row = &snap->suspects[i];
        log_mac(row->mac, mac);
        ESP_LOGI(TAG, "row=%u mac=%s ch=%u rssi=%d score=%lu kick/s=%lu kick_total=%lu", i + 1, mac,
                 row->channel, row->rssi, (unsigned long)row->score,
                 (unsigned long)(row->deauth_rate + row->disassoc_rate),
                 (unsigned long)(row->deauth_total + row->disassoc_total));
    }
}

void airspace_monitor_get_snapshot(airspace_monitor_snapshot_t *out) {
    if (!out) return;
    airspace_monitor_snapshot_t snap = {0};
    airspace_diagnostics_t d = {0};
    uint32_t now = now_ms();
    uint8_t candidate = 0, active = 0;
    bool strong = false;
    uint8_t karma_mac[6] = {0};
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
    if (s_start_us) snap.uptime_s = (uint32_t)((esp_timer_get_time() - s_start_us) / 1000000LL);
    d.kick_total = s_last_kick_total;
    d.kick_bcast = s_last_kick_bcast;
    d.csa = s_last_csa & AIRSPACE_CSA_COUNT_MASK;
    d.kick_reason = s_last_kick_reason;
    d.beacons = s_last_beacons;
    d.la_beacons = s_last_la_beacons;
    d.bssids = s_last_bssids;
    d.auth = s_last_auth;
    d.auth_src = s_last_auth_src;
    d.evil = s_last_evil;
    d.twin = s_last_twin;
    memcpy(d.twin_ssid, s_last_evil_ssid, sizeof(d.twin_ssid));
    if (s_state) {
        airspace_channel_t *ch = &s_state->channel[s_state->sample_idx];
        snap.sample_channel = s_channels[s_state->sample_idx];
        snap.baseline_pps = ch->pps;
        snap.baseline_kick = ch->kick_q4 / 16U;
        snap.baseline_ready = ch->samples >= AIRSPACE_BASELINE_WARMUP;
        snap.coverage_flags = (s_state->pressure & 3U) | (s_state->pressure >> 4) |
                              (snap.baseline_ready ? 0 : 4U);
        snap.dwelling = s_state->dwelling;
        d.base_bssids = ch->bssids;
        d.window = s_state->window_serial;
        d.elapsed_ms = s_state->last_window_ms;
        d.baseline_samples = ch->samples;
        d.baseline_updates = s_state->baseline_update_flags;
        d.rx_failed = s_state->rx_failed;
        d.rx_invalid = s_state->rx_invalid;
        d.security_invalid = s_state->security_invalid;
        d.profile_variations = s_state->profile_variations;
        d.auth_fail = s_state->last_auth_fail;
        d.callback_max_us = s_state->callback_max_us;
        d.flags = s_state->evidence_flags;
        for (unsigned i = 0; i < AIRSPACE_PATTERN_COUNT - 1; i++)
            d.histories |= (uint32_t)s_state->detector_history[i] << (i * 4U);
        candidate = selected_pattern(false);
        active = selected_pattern(true);
        strong = (s_state->strong_kick & 1U) != 0;
        d.correlated = (s_last_csa & AIRSPACE_TWIN_KICK_BIT) &&
                       confirmed(AIRSPACE_PATTERN_EVIL) && confirmed(AIRSPACE_PATTERN_KICK);
        if (active || candidate) snap.evidence_strength =
            evidence_count(s_state->detector_history[(active ? active : candidate) - 1]);
        snap.pps_history_count = s_state->pps_count;
        for (uint8_t i = 0; i < snap.pps_history_count; i++) {
            uint8_t idx = (uint8_t)((s_state->pps_head + AIRSPACE_PPS_HISTORY - snap.pps_history_count + i) % AIRSPACE_PPS_HISTORY);
            snap.pps_history[i] = s_state->pps_hist[idx];
            if (snap.pps_history[i] > snap.pps_history_max) snap.pps_history_max = snap.pps_history[i];
        }
        d.event_first = s_state->event_first_ms;
        d.event_last = s_state->event_last_ms;
        d.event_peak = s_state->event_peak;
        d.event_pattern = s_state->event_pattern;
        d.event_channel = s_state->event_channel;
    }
    if (s_devices) for (int i = 0; i < AIRSPACE_MAX_DEVICES; i++) {
        airspace_device_t *dev = &s_devices[i];
        if (!dev->used || (uint32_t)(now - dev->last_seen_ms) > AIRSPACE_DEVICE_TTL_MS) continue;
        snap.unique_devices++;
        uint32_t bins = popcount_bytes((const uint8_t *)&dev->probe_resp_ssids, 4);
        if (dev->channel == snap.sample_channel && bins > d.karma_ssids) {
            d.karma_ssids = bins;
            memcpy(karma_mac, dev->mac, 6);
        }
        uint32_t score = suspect_score(dev);
        if (!score) continue;
        airspace_suspect_t row = {0};
        fill_suspect(dev, &row);
        for (uint8_t pos = 0; pos < AIRSPACE_MAX_SUSPECTS; pos++) {
            if (pos < snap.suspect_count && snap.suspects[pos].score >= score) continue;
            uint8_t limit = snap.suspect_count < AIRSPACE_MAX_SUSPECTS ? snap.suspect_count : AIRSPACE_MAX_SUSPECTS - 1;
            for (uint8_t move = limit; move > pos; move--) snap.suspects[move] = snap.suspects[move - 1];
            snap.suspects[pos] = row;
            if (snap.suspect_count < AIRSPACE_MAX_SUSPECTS) snap.suspect_count++;
            break;
        }
    }
    if (snap.suspect_count) {
        airspace_suspect_t *dev = &snap.suspects[0];
        snap.has_offender = true;
        memcpy(snap.offender_mac, dev->mac, 6);
        snap.offender_channel = dev->channel;
        snap.offender_rssi = dev->rssi;
        snap.offender_deauth_rate = dev->deauth_rate;
        snap.offender_disassoc_rate = dev->disassoc_rate;
        snap.offender_total = dev->total;
    }
    d.rows_changed = stabilize_suspects_locked(&snap, now);
    portEXIT_CRITICAL(&s_lock);

    uint32_t kick = snap.deauth_per_sec + snap.disassoc_per_sec;
    char ssid[33];
    privacy_redact_ssid_copy(d.twin_ssid, ssid, sizeof(ssid));
    snap.threat_level = AIRSPACE_THREAT_QUIET;
    if (active == AIRSPACE_PATTERN_KICK) {
        snap.threat_level = strong ? AIRSPACE_THREAT_ATTACK_LIKELY : AIRSPACE_THREAT_SUSPICIOUS;
        snprintf(snap.reason, sizeof(snap.reason), "%s disconnects %lu/s ch%u r%u",
                 strong ? "Flood-like" : "Unusual", (unsigned long)kick,
                 snap.sample_channel, d.kick_reason);
    } else if (active == AIRSPACE_PATTERN_EVIL) {
        snap.threat_level = d.correlated ? AIRSPACE_THREAT_ATTACK_LIKELY : AIRSPACE_THREAT_SUSPICIOUS;
        snprintf(snap.reason, sizeof(snap.reason), "Possible twin: %s (%s)", ssid,
                 d.correlated ? "disconnects" : "security");
    } else if (active == AIRSPACE_PATTERN_KARMA) {
        char mac[18];
        log_mac(karma_mac, mac);
        snap.threat_level = AIRSPACE_THREAT_SUSPICIOUS;
        snprintf(snap.reason, sizeof(snap.reason), "Unusual SSID replies: %s (%lu bins)", mac, (unsigned long)d.karma_ssids);
    } else if (active == AIRSPACE_PATTERN_BEACON) {
        snap.threat_level = AIRSPACE_THREAT_SUSPICIOUS;
        snprintf(snap.reason, sizeof(snap.reason), "New AP burst: ~%lu ch%u", (unsigned long)d.bssids, snap.sample_channel);
    } else if (active == AIRSPACE_PATTERN_AUTH) {
        snap.threat_level = AIRSPACE_THREAT_SUSPICIOUS;
        if (d.auth)
            snprintf(snap.reason, sizeof(snap.reason), "Auth burst %lu/s; %u failures ch%u",
                     (unsigned long)d.auth, d.auth_fail, snap.sample_channel);
        else
            snprintf(snap.reason, sizeof(snap.reason), "Auth/assoc failures: %u ch%u", d.auth_fail, snap.sample_channel);
    } else if (active == AIRSPACE_PATTERN_CSA) {
        snap.threat_level = AIRSPACE_THREAT_SUSPICIOUS;
        snprintf(snap.reason, sizeof(snap.reason), "Excess channel-switch announcements ch%u", snap.sample_channel);
    } else if (active == AIRSPACE_PATTERN_CRYPTO) {
        snap.threat_level = AIRSPACE_THREAT_BUSY;
        snprintf(snap.reason, sizeof(snap.reason), "Security changed: %s", ssid);
    } else if (snap.packets_per_sec >= 400 || snap.unique_devices >= 24) {
        snap.threat_level = AIRSPACE_THREAT_BUSY;
        snprintf(snap.reason, sizeof(snap.reason), "Busy channel; no confirmed threat");
    } else {
        snprintf(snap.reason, sizeof(snap.reason), "%s", candidate ? "Collecting evidence" : "No active threat pattern");
    }
    /* One decision drives status and insight. Keep event details in existing
       labels/snapshot storage rather than allocating an event list. */
    snap.insight_level = snap.threat_level;
    if (active) {
        snprintf(snap.insight, sizeof(snap.insight), "Observed ch%u | evidence %u/4", snap.sample_channel, snap.evidence_strength);
        if (active == AIRSPACE_PATTERN_EVIL || active == AIRSPACE_PATTERN_CRYPTO)
            snprintf(snap.advice, sizeof(snap.advice), "Security 0x%02x -> 0x%02x | last %lus",
                     d.twin.security[0], d.twin.security[1],
                     (unsigned long)((now - d.event_last) / 1000U));
        else
            snprintf(snap.advice, sizeof(snap.advice), "Seen %lus | last %lus | peak %u/s",
                     (unsigned long)((d.event_last - d.event_first) / 1000U),
                     (unsigned long)((now - d.event_last) / 1000U), d.event_peak);
    } else if (d.event_pattern && (uint32_t)(now - d.event_last) <= 30000U) {
        snprintf(snap.insight, sizeof(snap.insight), "Previous event ch%u, %lus ago", d.event_channel, (unsigned long)((now - d.event_last) / 1000U));
        snprintf(snap.advice, sizeof(snap.advice), "No current confirmation; keep observing");
    } else {
        snprintf(snap.insight, sizeof(snap.insight), "%s ch%u | observed %lums",
                 snap.baseline_ready ? "Baseline ready" : "Learning", snap.sample_channel, (unsigned long)d.elapsed_ms);
        snprintf(snap.advice, sizeof(snap.advice), "%s",
                 snap.coverage_flags & 3U ? "Tracking full; background detail limited" :
                 candidate ? "Candidate only; awaiting another observation" : "Hopping observes one channel at a time");
    }
    log_snapshot(&snap, &d, candidate, now);
    *out = snap;
}


static void record_csa_locked(airspace_ap_t *ap, const uint8_t *frame,
                              int offset, int len, uint16_t interval_tu, uint32_t now) {
    if (!ap || !interval_tu) return;
    while (offset + 2 <= len) {
        uint8_t id = frame[offset], n = frame[offset + 1];
        offset += 2;
        if (offset + n > len) return;
        if ((id == 37 && n == 3) || (id == 60 && n == 4)) {
            if (ap->csa_count != UINT8_MAX) ap->csa_count++;
            uint32_t period_us = (uint32_t)interval_tu * 1024U;
            uint32_t expected = ((now - s_window_start_ms) * 1000U + period_us - 1U) / period_us;
            if (ap->csa_count >= 8U && ap->csa_count > expected * 2U + 2U)
                s_window_csa++; /* excess announcements, not all legitimate CSAs */
            return;
        }
        offset += n;
    }
}

void wifi_airspace_monitor_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!s_active || !buf || s_devices == NULL || type == WIFI_PKT_MISC) {
        return;
    }

    uint32_t started_us = (uint32_t)esp_timer_get_time();
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    /* ALL includes failed-FCS receptions and aggregate blobs. Never interpret
       those as management frames or security evidence, even if the caller
       changes the hardware filter later. Lengths below exclude the FCS. */
    if (pkt->rx_ctrl.rx_state != 0) {
        portENTER_CRITICAL(&s_lock);
        if (s_state != NULL) sat_inc(&s_state->rx_failed);
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    int len = pkt->rx_ctrl.sig_len > 4 ? pkt->rx_ctrl.sig_len - 4 : 0;
#if CONFIG_SOC_WIFI_HE_SUPPORT
    if (pkt->rx_ctrl.dump_len > 0 && pkt->rx_ctrl.dump_len <= pkt->rx_ctrl.sig_len) {
        len = pkt->rx_ctrl.dump_len;
    }
#endif
    if (len < 2) {
        portENTER_CRITICAL(&s_lock);
        if (s_state != NULL) sat_inc(&s_state->rx_invalid);
        portEXIT_CRITICAL(&s_lock);
        return;
    }

    const uint8_t *frame = pkt->payload;
    uint8_t frame_type = (frame[0] & 0x0C) >> 2;
    uint8_t subtype = (frame[0] & 0xF0) >> 4;
    int expected_type = frame_type == 0 ? WIFI_PKT_MGMT :
                        frame_type == 1 ? WIFI_PKT_CTRL : WIFI_PKT_DATA;
    if ((frame[0] & 0x03U) || frame_type > 2 || type != expected_type ||
        len < (frame_type == 1 ? 10 : 24) ||
        (frame_type == 0 && (frame[1] & 0x03U)) ||
        (frame_type == 0 && (subtype == 10 || subtype == 12) && len < 26)) {
        portENTER_CRITICAL(&s_lock);
        if (s_state != NULL) sat_inc(&s_state->rx_invalid);
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    bool kick_frame = (frame_type == 0 && (subtype == 0x0C || subtype == 0x0A));
    bool has_tx = len >= 16;
    const uint8_t *tx_mac = has_tx ? frame + 10 : NULL;
    uint32_t now = now_ms();

    portENTER_CRITICAL(&s_lock);
    if (!s_state || !s_state->sampling || pkt->rx_ctrl.channel != s_current_channel) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    /* The existing hop timer owns window processing. Keep RX work bounded
       even during floods; screen refreshes may also close a completed window. */
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

    if (kick_frame) {
        s_window_kick_total++;
        if ((s_window_evil &&
             (!memcmp(frame + 16, s_window_twin.bssid[0], 6) ||
              !memcmp(frame + 16, s_window_twin.bssid[1], 6))) ||
            (s_last_evil &&
             (!memcmp(frame + 16, s_last_twin.bssid[0], 6) ||
              !memcmp(frame + 16, s_last_twin.bssid[1], 6))))
            s_window_csa |= AIRSPACE_TWIN_KICK_BIT;
        bool protected_frame = (frame[1] & 0x40U) != 0;
        if (len >= 26 && !protected_frame) {
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
        uint16_t capability = (uint16_t)(frame[34] | (frame[35] << 8));
        uint8_t security = 0;
        bool valid_security = parse_mgmt_security(frame, len, capability, &security);
        if (!valid_security && s_state != NULL) sat_inc(&s_state->security_invalid);
        const uint8_t *ssid = NULL;
        uint8_t ssid_len = parse_mgmt_ssid(frame, len, &ssid);
        if (ssid_len > 0 && valid_security && !is_zero_or_broadcast(bssid) &&
            memcmp(bssid, frame + 10, 6) == 0) {
            airspace_ap_t *ap = airspace_ap_track_locked(bssid, ssid, ssid_len,
                                                        pkt->rx_ctrl.channel, pkt->rx_ctrl.rssi,
                                                        security, now);
            record_csa_locked(ap, frame, 36, len, (uint16_t)(frame[32] | (frame[33] << 8)), now);
        }
    }

    /* Broadcast spectrum-management action / CSA. Ignore protected bodies and
       per-client unicast announcements, which can legitimately be numerous. */
    if (frame_type == 0 && subtype == 13 && len >= 31 && !(frame[1] & 0x40U) &&
        frame[24] == 0 && frame[25] == 4 && is_broadcast_mac(frame + 4)) {
        for (unsigned i = 0; i < AIRSPACE_MAX_APS; i++) {
            airspace_ap_t *ap = &s_aps[i];
            if (ap->used && !memcmp(ap->bssid, frame + 16, 6) &&
                !memcmp(ap->bssid, frame + 10, 6)) {
                record_csa_locked(ap, frame, 26, len, 100, now);
                break;
            }
        }
    }

    /* Count client requests separately from AP replies. Authentication and
       association failures are context; a failure alone is not an attack. */
    if (frame_type == 0 && !(frame[1] & 0x40U) && len >= 30) {
        if (subtype == 11) {
            uint16_t status = (uint16_t)(frame[28] | (frame[29] << 8));
            if (memcmp(frame + 10, frame + 16, 6)) {
                s_window_auth++;
                bitmap_set9(s_state->auth_src_seen, frame + 10, 6);
            } else if (status != 0 && status != 76) {
                sat_inc(&s_state->win_auth_fail);
                if (!is_zero_or_broadcast(frame + 4)) bitmap_set9(s_state->auth_src_seen, frame + 4, 6);
            }
        } else if ((subtype == 1 || subtype == 3) && (frame[26] || frame[27])) {
            sat_inc(&s_state->win_auth_fail);
            if (!is_zero_or_broadcast(frame + 4)) bitmap_set9(s_state->auth_src_seen, frame + 4, 6);
        }
    }

    if (tx_mac && !is_zero_or_broadcast(tx_mac)) {
        int slot = find_device_slot_locked(tx_mac, now, kick_frame || (frame_type == 0 && subtype == 5));
        if (slot < 0) {
            goto finish;
        }
        airspace_device_t *dev = &s_devices[slot];
        dev->last_seen_ms = now;
        dev->total++;
        dev->rssi = pkt->rx_ctrl.rssi;
        dev->channel = pkt->rx_ctrl.channel;
        if (frame_type == 0 && subtype == 0x0C) {
            dev->deauth_total++;
            sat_inc(&dev->win_deauth);
        } else if (frame_type == 0 && subtype == 0x0A) {
            dev->disassoc_total++;
            sat_inc(&dev->win_disassoc);
        } else if (frame_type == 0 && subtype == 0x05) {
            /* Probe response: record which SSID this source answered for. A
               Karma/Mana AP answers for many distinct SSIDs it doesn't own. */
            const uint8_t *ssid = NULL;
            uint8_t ssid_len = parse_mgmt_ssid(frame, len, &ssid);
            bool advertised = false;
            for (unsigned i = 0; i < AIRSPACE_MAX_APS && ssid_len; i++) {
                const airspace_ap_t *ap = &s_aps[i];
                if (ap->used && ap->ssid_len == ssid_len && !memcmp(ap->bssid, tx_mac, 6) &&
                    !memcmp(ap->ssid, ssid, ssid_len)) { advertised = true; break; }
            }
            uint8_t epoch = (uint8_t)(now / AIRSPACE_KARMA_PERIOD_MS);
            if (dev->probe_epoch != epoch) { dev->probe_epoch = epoch; dev->probe_resp_ssids = 0; }
            if (ssid_len > 0 && !advertised) {
                dev->probe_resp_ssids |= (1u << (fnv1a(ssid, ssid_len) & 31u));
            }
        }

    }
finish:
    if (s_state) {
        uint32_t cost = (uint32_t)esp_timer_get_time() - started_us;
        if (cost > s_state->callback_max_us) s_state->callback_max_us = cost > UINT16_MAX ? UINT16_MAX : (uint16_t)cost;
    }
    portEXIT_CRITICAL(&s_lock);
}
