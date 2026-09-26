/**
 * @file wifi_channels.c
 * @brief WiFi channel utility functions implementation
 */

#include "scans/wifi/wifi_channels.h"
#include "scans/wifi/ap_scan.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WiFiChannels";

#if defined(CONFIG_SOC_WIFI_SUPPORT_5G)
static const uint8_t WIFI_5G_CHANNELS[] = {
    36, 40, 44, 48, 52, 56, 60, 64,
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165, 169, 173, 177
};
#endif

static bool country_code_is(const wifi_country_t *country, const char *cc2) {
    if (!country || !cc2) {
        return false;
    }
    return country->cc[0] == cc2[0] && country->cc[1] == cc2[1];
}

static bool is_valid_5ghz_channel(uint8_t channel) {
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    for (size_t i = 0; i < sizeof(WIFI_5G_CHANNELS) / sizeof(WIFI_5G_CHANNELS[0]); i++) {
        if (WIFI_5G_CHANNELS[i] == channel) {
            return true;
        }
    }
#endif
    return false;
}

static void set_world_safe_fallback(wifi_country_t *country) {
    if (!country) return;
    memset(country, 0, sizeof(*country));
    country->cc[0] = '0';
    country->cc[1] = '1';
    country->cc[2] = ' ';
    country->schan = 1;
    country->nchan = 11;
#if defined(CONFIG_SOC_WIFI_SUPPORT_5G)
    // ESP-IDF's world-safe domain permits UNII-1 and DFS UNII-2
    // (36-64), but not the higher UNII-3/4 channels.
    country->wifi_5g_channel_mask = 0;
#endif
}

static void get_effective_country(wifi_country_t *country) {
    if (!country) return;
    if (esp_wifi_get_country(country) != ESP_OK ||
        country->schan == 0 || country->nchan == 0) {
        set_world_safe_fallback(country);
    }
}

#if defined(CONFIG_SOC_WIFI_SUPPORT_5G)
static uint32_t fallback_5ghz_mask(const wifi_country_t *country) {
    if (!country) return 0;

    if (country_code_is(country, "01")) {
        // ESP-IDF world-safe regulatory domain: 36-64.
        uint32_t mask = 0;
        for (uint8_t ch = 36; ch <= 64; ch += 4) {
            mask |= CHANNEL_TO_BIT(ch);
        }
        return mask;
    }
    if (country_code_is(country, "US") || country_code_is(country, "CA")) {
        uint32_t mask = 0;
        for (uint8_t ch = 36; ch <= 144; ch += 4) mask |= CHANNEL_TO_BIT(ch);
        for (uint8_t ch = 149; ch <= 165; ch += 4) mask |= CHANNEL_TO_BIT(ch);
        return mask;
    }
    if (country_code_is(country, "JP")) {
        uint32_t mask = 0;
        for (uint8_t ch = 36; ch <= 64; ch += 4) mask |= CHANNEL_TO_BIT(ch);
        for (uint8_t ch = 100; ch <= 140; ch += 4) mask |= CHANNEL_TO_BIT(ch);
        return mask;
    }
    if (country_code_is(country, "CN")) {
        uint32_t mask = 0;
        for (uint8_t ch = 36; ch <= 64; ch += 4) mask |= CHANNEL_TO_BIT(ch);
        for (uint8_t ch = 149; ch <= 165; ch += 4) mask |= CHANNEL_TO_BIT(ch);
        return mask;
    }
    if (country_code_is(country, "AU")) {
        uint32_t mask = 0;
        for (uint8_t ch = 36; ch <= 64; ch += 4) mask |= CHANNEL_TO_BIT(ch);
        for (uint8_t ch = 100; ch <= 140; ch += 4) mask |= CHANNEL_TO_BIT(ch);
        for (uint8_t ch = 149; ch <= 165; ch += 4) mask |= CHANNEL_TO_BIT(ch);
        return mask;
    }
    if (country_code_is(country, "NZ") || country_code_is(country, "EU") ||
        country_code_is(country, "GB") || country_code_is(country, "DE") ||
        country_code_is(country, "FR")) {
        uint32_t mask = 0;
        for (uint8_t ch = 36; ch <= 165; ch += 4) mask |= CHANNEL_TO_BIT(ch);
        return mask;
    }

    // Unknown country: keep the conservative UNII-1 fallback.
    uint32_t mask = 0;
    for (uint8_t ch = 36; ch <= 48; ch += 4) mask |= CHANNEL_TO_BIT(ch);
    return mask;
}

static bool country_allows_5ghz(const wifi_country_t *country, uint8_t channel) {
    if (!country || !is_valid_5ghz_channel(channel)) return false;
    uint32_t mask = country->wifi_5g_channel_mask;
    if (mask == 0) {
        mask = fallback_5ghz_mask(country);
    }
    return (mask & CHANNEL_TO_BIT(channel)) != 0;
}

static bool country_allows_non_dfs_upper_band(const wifi_country_t *country) {
    if (!country) return false;
    return country_code_is(country, "US") || country_code_is(country, "CA") ||
           country_code_is(country, "CN") || country_code_is(country, "AU") ||
           country_code_is(country, "NZ") || country_code_is(country, "EU") ||
           country_code_is(country, "GB") || country_code_is(country, "DE") ||
           country_code_is(country, "FR");
}
#endif

bool wifi_channels_is_5ghz(uint8_t channel) {
    return channel > 14;
}

bool wifi_channels_is_dfs(uint8_t channel) {
    if (channel >= 52 && channel <= 144) {
        return true;
    }
#if defined(CONFIG_SOC_WIFI_SUPPORT_5G)
    if (channel >= 149) {
        wifi_country_t country;
        get_effective_country(&country);
        return !country_allows_non_dfs_upper_band(&country);
    }
#endif
    return false;
}

bool wifi_channels_requires_passive_scan(uint8_t channel) {
    if (wifi_channels_is_dfs(channel)) {
        return true;
    }
#if defined(CONFIG_SOC_WIFI_SUPPORT_5G)
    if (channel >= 149) {
        wifi_country_t country;
        get_effective_country(&country);
        return !country_allows_non_dfs_upper_band(&country);
    }
#endif
    if (channel >= 12 && channel <= 14) {
        wifi_country_t country;
        get_effective_country(&country);
        return country.policy == WIFI_COUNTRY_POLICY_AUTO;
    }
    return false;
}

bool wifi_channels_is_target_channel(uint8_t channel) {
    if (channel >= 1 && channel <= 14) {
        return true;
    }
    return is_valid_5ghz_channel(channel);
}

bool wifi_channels_is_country_channel(uint8_t channel) {
    if (!wifi_channels_is_target_channel(channel)) {
        return false;
    }

    wifi_country_t country;
    get_effective_country(&country);
    if (channel <= 14) {
        return channel >= country.schan &&
               channel < (uint16_t)country.schan + country.nchan;
    }
#if defined(CONFIG_SOC_WIFI_SUPPORT_5G)
    return country_allows_5ghz(&country, channel);
#else
    return false;
#endif
}

bool wifi_channels_is_monitor_channel(uint8_t channel) {
    return wifi_channels_is_country_channel(channel);
}

bool wifi_channels_is_tx_channel(uint8_t channel) {
    if (!wifi_channels_is_monitor_channel(channel) || wifi_channels_is_dfs(channel)) {
        return false;
    }
#if defined(CONFIG_SOC_WIFI_SUPPORT_5G)
    if (channel >= 149) {
        wifi_country_t country;
        get_effective_country(&country);
        return country_allows_non_dfs_upper_band(&country);
    }
#endif
    return true;
}

bool wifi_channels_is_safe_monitor_channel(uint8_t channel) {
    return wifi_channels_is_monitor_channel(channel);
}

const char* wifi_channels_get_band_name(uint8_t channel) {
    return wifi_channels_is_5ghz(channel) ? "5GHz" : "2.4GHz";
}

static void append_channel_unique(uint8_t *channels, uint8_t *count,
                                  uint8_t max_count, uint8_t channel) {
    if (!channels || !count || *count >= max_count) return;
    for (uint8_t i = 0; i < *count; i++) {
        if (channels[i] == channel) return;
    }
    channels[(*count)++] = channel;
}

uint8_t wifi_channels_build_country_list(uint8_t *channels, uint8_t max_count) {
    if (channels == NULL || max_count == 0) {
        return 0;
    }

    uint8_t count = 0;
    wifi_country_t country;
    get_effective_country(&country);

    // ESP-IDF defines schan as the first channel and nchan as the number of
    // allowed 2.4 GHz channels, not nchan as a maximum channel number.
    uint16_t first_2g = country.schan;
    uint16_t end_2g = (uint16_t)country.schan + country.nchan;
    if (end_2g > 15) end_2g = 15;

    // Keep the historical non-overlapping-first ordering.
    for (uint8_t ch = 1; ch <= 14 && count < max_count; ch++) {
        if ((ch == 1 || ch == 6 || ch == 11) &&
            ch >= first_2g && ch < end_2g) {
            append_channel_unique(channels, &count, max_count, ch);
        }
    }
    for (uint8_t ch = 1; ch <= 14 && count < max_count; ch++) {
        if (ch != 1 && ch != 6 && ch != 11 &&
            ch >= first_2g && ch < end_2g) {
            append_channel_unique(channels, &count, max_count, ch);
        }
    }

#if defined(CONFIG_SOC_WIFI_SUPPORT_5G)
    for (size_t i = 0; i < sizeof(WIFI_5G_CHANNELS) / sizeof(WIFI_5G_CHANNELS[0]); i++) {
        if (count >= max_count) break;
        if (country_allows_5ghz(&country, WIFI_5G_CHANNELS[i])) {
            append_channel_unique(channels, &count, max_count, WIFI_5G_CHANNELS[i]);
        }
    }
#endif

    ESP_LOGI(TAG, "Country %.2s: using %u channels", country.cc,
             (unsigned)count);
    return count;
}

uint8_t wifi_channels_build_from_ap_results(uint8_t *channels, uint8_t max_count) {
    uint8_t count = 0;
    
    if (channels == NULL || max_count == 0) {
        return 0;
    }
    
    // Get AP scan results
    uint16_t ap_count = 0;
    wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&ap_count, &aps);
    
    if (ap_count == 0 || aps == NULL) {
        ESP_LOGW(TAG, "No AP results available for channel list");
        return 0;
    }
    
    // Build unique channel list from AP results
    for (uint16_t i = 0; i < ap_count && count < max_count; i++) {
        uint8_t ch = aps[i].primary;
        
        // Check if channel already in list
        bool found = false;
        for (uint8_t j = 0; j < count; j++) {
            if (channels[j] == ch) {
                found = true;
                break;
            }
        }
        
        if (!found && wifi_channels_is_monitor_channel(ch)) {
            channels[count++] = ch;
        }
    }
    
    // Sort channels (simple bubble sort for small arrays)
    for (uint8_t i = 0; i < count - 1; i++) {
        for (uint8_t j = i + 1; j < count; j++) {
            if (channels[i] > channels[j]) {
                uint8_t tmp = channels[i];
                channels[i] = channels[j];
                channels[j] = tmp;
            }
        }
    }
    
    ESP_LOGI(TAG, "Built channel list from %d APs: %d unique channels", ap_count, count);
    return count;
}

uint8_t wifi_channels_parse_list(const char *text, uint8_t *channels, uint8_t max_count) {
    if (text == NULL || channels == NULL || max_count == 0) {
        return 0;
    }

    uint8_t count = 0;
    const char *cursor = text;

    while (*cursor) {
        while (*cursor && !isdigit((unsigned char)*cursor)) {
            cursor++;
        }
        if (!*cursor) {
            break;
        }

        char *end = NULL;
        long channel = strtol(cursor, &end, 10);
        if (end == cursor || channel < 1 || channel > 177) {
            return 0;
        }

        bool seen = false;
        for (uint8_t i = 0; i < count; i++) {
            if (channels[i] == (uint8_t)channel) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            if (count >= max_count) {
                return 0;
            }
            channels[count++] = (uint8_t)channel;
        }

        cursor = end;
    }

    return count;
}
