#include "scans/wifi/hop_profile.h"
#include "scans/wifi/wifi_channels.h"
#include "managers/settings_manager.h"
#include "esp_log.h"
#include <ctype.h>
#include <string.h>

static const char *TAG = "HopProfile";

static uint8_t clamp_mode(uint8_t mode) {
    if (mode >= HOP_MODE_COUNT) return HOP_MODE_DEFAULT;
    return mode;
}

void hop_profile_get_mode(hop_mode_t *mode) {
    if (!mode) return;
    *mode = (hop_mode_t)clamp_mode(settings_get_hop_mode(&G_Settings));
}

void hop_profile_set_mode(hop_mode_t mode) {
    settings_set_hop_mode(&G_Settings, clamp_mode((uint8_t)mode));
}

static void normalize_custom_string(char *text) {
    // Collapse runs of spaces/commas into a single comma so the stored list
    // is always a clean "a,b,c".
    char *dst = text;
    bool last_sep = true;
    for (const char *src = text; *src; src++) {
        if (isdigit((unsigned char)*src)) {
            *dst++ = *src;
            last_sep = false;
        } else if (!last_sep) {
            *dst++ = ',';
            last_sep = true;
        }
    }
    if (dst > text && last_sep) dst--; // strip a trailing separator
    *dst = '\0';
}

bool hop_profile_set_custom_from_string(const char *text) {
    if (!text || !*text) return false;

    // Validate the input parses and every channel is safe to monitor.
    uint8_t parsed[WIFI_CHANNELS_MAX] = {0};
    uint8_t parsed_count = wifi_channels_parse_list(text, parsed, WIFI_CHANNELS_MAX);
    if (parsed_count == 0) {
        return false;
    }
    for (uint8_t i = 0; i < parsed_count; i++) {
        if (!wifi_channels_is_safe_monitor_channel(parsed[i])) {
            ESP_LOGW(TAG, "Rejecting unsafe monitor channel %u",
                     (unsigned)parsed[i]);
            return false;
        }
    }

    char buf[129];
    if (strlen(text) >= sizeof(buf)) return false;
    strcpy(buf, text);
    normalize_custom_string(buf);
    settings_set_hop_custom_channels(&G_Settings, buf);
    return true;
}

const char *hop_profile_get_custom_str(void) {
    return settings_get_hop_custom_channels(&G_Settings);
}

void hop_profile_resolve(uint8_t *out, size_t out_cap, size_t *out_count) {
    if (!out || !out_count || out_cap == 0) return;
    *out_count = 0;

    hop_mode_t mode = HOP_MODE_DEFAULT;
    hop_profile_get_mode(&mode);

    const uint8_t *candidates = NULL;
    size_t candidate_count = 0;

    switch (mode) {
        case HOP_MODE_ALL: {
            // "All" = every channel the current WiFi country config allows
            // (includes target-appropriate 5GHz via the country tables).
            uint8_t *country = (uint8_t *)out;
            candidate_count = wifi_channels_build_country_list(country,
                                                               out_cap);
            if (candidate_count > 0) candidates = country;
            break;
        }
        case HOP_MODE_BASIC: {
            static const uint8_t basic[] = {1, 6, 11};
            candidates = basic;
            candidate_count = sizeof(basic);
            break;
        }
        case HOP_MODE_CUSTOM: {
            static uint8_t custom[WIFI_CHANNELS_MAX];
            candidate_count = wifi_channels_parse_list(
                settings_get_hop_custom_channels(&G_Settings), custom,
                WIFI_CHANNELS_MAX);
            if (candidate_count > 0) candidates = custom;
            break;
        }
        case HOP_MODE_DEFAULT:
        default:
            return; // Caller falls back to its feature-specific list.
    }

    if (candidate_count == 0) return;

    for (size_t i = 0; i < candidate_count && *out_count < out_cap; i++) {
        uint8_t ch = candidates[i];
        if (!wifi_channels_is_safe_monitor_channel(ch)) continue;

        bool seen = false;
        for (size_t j = 0; j < *out_count; j++) {
            if (out[j] == ch) {
                seen = true;
                break;
            }
        }
        if (!seen) out[(*out_count)++] = ch;
    }
}
