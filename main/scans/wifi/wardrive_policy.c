#include "scans/wifi/wardrive_policy.h"
#include <string.h>

bool wd_channel_valid(uint8_t ch) {
    return (ch >= 1 && ch <= 14) ||
           (ch >= 36 && ch <= 64 && ch % 4 == 0) ||
           (ch >= 100 && ch <= 144 && ch % 4 == 0) ||
           (ch >= 149 && ch <= 165 && (ch - 149) % 4 == 0);
}

bool wd_plan_contains(const uint8_t *plan, size_t count, uint8_t ch) {
    for (size_t i = 0; i < count; ++i) if (plan[i] == ch) return true;
    return false;
}

size_t wd_plan_helper(const uint8_t *primary, size_t pn,
                      const uint8_t *helper, size_t hn, uint8_t *out) {
    bool p5 = false, h5 = false;
    for (size_t i = 0; i < pn; ++i) p5 |= primary[i] > 14;
    for (size_t i = 0; i < hn; ++i) h5 |= helper[i] > 14;
    size_t n = 0, shared = 0;
    for (size_t i = 0; i < hn && n < WD_PLAN_MAX; ++i) {
        uint8_t ch = helper[i];
        if (!wd_channel_valid(ch) || wd_plan_contains(out, n, ch)) continue;
        bool exclusive = !wd_plan_contains(primary, pn, ch);
        bool assigned;
        if (p5 != h5) {
            // Mixed radios: the dual-band radio owns 5 GHz; preserve any
            // channels only one device can receive regardless of the band.
            assigned = exclusive || (h5 ? ch > 14 : ch <= 14);
        } else {
            assigned = exclusive || ((shared++ & 1U) != 0);
        }
        if (assigned) out[n++] = ch;
    }
    return n;
}

size_t wd_plan_remaining(const uint8_t *local, size_t ln,
                         const uint8_t *peer, size_t pn, uint8_t *out) {
    size_t n = 0;
    for (size_t i = 0; i < ln && n < WD_PLAN_MAX; ++i) {
        if (wd_channel_valid(local[i]) && !wd_plan_contains(peer, pn, local[i]) &&
            !wd_plan_contains(out, n, local[i])) out[n++] = local[i];
    }
    return n;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

static uint8_t rsn_auth(const uint8_t *p, size_t n) {
    if (n < 8 || le16(p) != 1) return 3;
    size_t pos = 6;
    size_t count = le16(p + pos); pos += 2;
    if (count > (n - pos) / 4) return 3;
    pos += count * 4;
    if (n - pos < 2) return 3;
    count = le16(p + pos); pos += 2;
    if (count > (n - pos) / 4) return 3;
    uint8_t auth = 3;
    for (size_t i = 0; i < count; ++i, pos += 4) {
        if (p[pos] != 0 || p[pos + 1] != 0x0f || p[pos + 2] != 0xac) continue;
        if (p[pos + 3] == 8 || p[pos + 3] == 9) auth = 4; // SAE, FT-SAE
        if (p[pos + 3] == 18) auth = 5; // OWE
    }
    return auth;
}

bool wd_parse_ap(const uint8_t *f, size_t len, uint8_t ch, int8_t rssi, wd_ap_t *out) {
    if (!f || !out || len < 36 || (f[0] != 0x80 && f[0] != 0x50)) return false;
    if (f[16] & 1) return false;
    static const uint8_t zero[6] = {0};
    if (memcmp(f + 16, zero, 6) == 0) return false;
    wd_ap_t ap = {.channel = ch, .rssi = rssi, .auth = (f[34] & 0x10) ? 1 : 0};
    memcpy(ap.bssid, f + 16, 6);
    bool ssid_seen = false, rsn_seen = false, ds_seen = false;
    size_t pos = 36;
    while (pos < len) {
        if (len - pos < 2) return false;
        uint8_t id = f[pos], n = f[pos + 1]; pos += 2;
        if (n > len - pos) return false;
        const uint8_t *p = f + pos;
        if (id == 0 && !ssid_seen) {
            if (n > 32) return false;
            ssid_seen = true;
            for (uint8_t i = 0; i < n; ++i) ap.ssid[i] = (p[i] < 0x20 || p[i] == 0x7f) ? '?' : (char)p[i];
        } else if (id == 3 && n == 1 && wd_channel_valid(p[0])) {
            ap.channel = p[0]; ds_seen = true;
        } else if (id == 61 && n >= 1 && !ds_seen && wd_channel_valid(p[0])) {
            ap.channel = p[0]; // HT operation primary channel (5 GHz often has no DS IE).
        } else if (id == 48) {
            ap.auth = rsn_auth(p, n); rsn_seen = true;
        } else if (id == 221 && n >= 4 && !rsn_seen &&
                   p[0] == 0 && p[1] == 0x50 && p[2] == 0xf2 && p[3] == 1) {
            ap.auth = 2;
        }
        pos += n;
    }
    if (!ssid_seen || !wd_channel_valid(ap.channel)) return false;
    *out = ap;
    return true;
}
