/**
 * @file wifi_channels.c
 * @brief WiFi channel utility functions implementation
 */

#include "scans/wifi/wifi_channels.h"
#include "scans/wifi/ap_scan.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include <string.h>

static const char *TAG = "WiFiChannels";

static bool country_code_is(const wifi_country_t *country, const char *cc2) {
    if (!country || !cc2) {
        return false;
    }
    return country->cc[0] == cc2[0] && country->cc[1] == cc2[1];
}

bool wifi_channels_is_5ghz(uint8_t channel) {
    return channel > 14;
}

bool wifi_channels_is_dfs(uint8_t channel) {
    return channel >= 52 && channel <= 144;
}

bool wifi_channels_is_safe_monitor_channel(uint8_t channel) {
    if (channel >= 1 && channel <= 14) {
        return true;
    }
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    if (channel >= 36 && channel <= 48) {
        return true;
    }
    if (channel >= 149 && channel <= 165) {
        return true;
    }
#endif
    return false;
}

const char* wifi_channels_get_band_name(uint8_t channel) {
    return wifi_channels_is_5ghz(channel) ? "5GHz" : "2.4GHz";
}

uint8_t wifi_channels_build_country_list(uint8_t *channels, uint8_t max_count) {
    uint8_t count = 0;
    
    if (channels == NULL || max_count == 0) {
        return 0;
    }
    
    // Get current WiFi country configuration
    wifi_country_t country;
    esp_err_t ret = esp_wifi_get_country(&country);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi country not set, using default channels");
        // 2.4GHz: channels 1, 6, 11 (common worldwide)
        if (count < max_count) channels[count++] = 1;
        if (count < max_count) channels[count++] = 6;
        if (count < max_count) channels[count++] = 11;
        
        #if defined(CONFIG_IDF_TARGET_ESP32C5)
        // 5GHz: common UNII-1 channels
        if (count < max_count) channels[count++] = 36;
        if (count < max_count) channels[count++] = 40;
        if (count < max_count) channels[count++] = 44;
        if (count < max_count) channels[count++] = 48;
        #endif
        
        ESP_LOGI(TAG, "Using %d default channels", count);
        return count;
    }
    
    // Build channel list based on country regulations
    // 2.4GHz band: channels 1-14 (varies by country)
    uint8_t max_24ghz_channel = country.nchan;
    if (max_24ghz_channel > 14) max_24ghz_channel = 14;
    
    // Add 2.4GHz channels (prioritize 1, 6, 11 for non-overlapping)
    for (uint8_t ch = 1; ch <= max_24ghz_channel && count < max_count; ch++) {
        if (ch == 1 || ch == 6 || ch == 11) {
            channels[count++] = ch;
        }
    }
    
    // Add overlapping 2.4GHz channels if needed
    for (uint8_t ch = 2; ch <= max_24ghz_channel && count < max_count; ch++) {
        if (ch != 1 && ch != 6 && ch != 11) {
            channels[count++] = ch;
        }
    }
    
    #if defined(CONFIG_IDF_TARGET_ESP32C5)
    // 5GHz band support for ESP32-C5 (dual-band)
    if (country_code_is(&country, "US") || country_code_is(&country, "CA")) {
        // North America: all bands allowed
        uint8_t us_5ghz[] = {36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165};
        for (int i = 0; i < sizeof(us_5ghz) && count < max_count; i++) {
            channels[count++] = us_5ghz[i];
        }
    } else if (country_code_is(&country, "JP")) {
        // Japan: all bands with restrictions
        uint8_t jp_5ghz[] = {36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140};
        for (int i = 0; i < sizeof(jp_5ghz) && count < max_count; i++) {
            channels[count++] = jp_5ghz[i];
        }
    } else if (country_code_is(&country, "CN")) {
        // China: limited 5GHz
        uint8_t cn_5ghz[] = {36, 40, 44, 48, 52, 56, 60, 64, 149, 153, 157, 161, 165};
        for (int i = 0; i < sizeof(cn_5ghz) && count < max_count; i++) {
            channels[count++] = cn_5ghz[i];
        }
    } else if (country_code_is(&country, "EU") || country_code_is(&country, "GB") ||
               country_code_is(&country, "DE") || country_code_is(&country, "FR")) {
        // Europe: UNII-1 and UNII-2
        uint8_t eu_5ghz[] = {36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140};
        for (int i = 0; i < sizeof(eu_5ghz) && count < max_count; i++) {
            channels[count++] = eu_5ghz[i];
        }
    } else if (country_code_is(&country, "AU") || country_code_is(&country, "NZ")) {
        // Australia/New Zealand: UNII-1, UNII-2, UNII-3 (no DFS mid-band)
        uint8_t au_5ghz[] = {36, 40, 44, 48, 52, 56, 60, 64, 149, 153, 157, 161, 165};
        for (int i = 0; i < sizeof(au_5ghz) && count < max_count; i++) {
            channels[count++] = au_5ghz[i];
        }
    } else {
        // Default: UNII-1 only (most permissive worldwide)
        uint8_t default_5ghz[] = {36, 40, 44, 48};
        for (int i = 0; i < sizeof(default_5ghz) && count < max_count; i++) {
            channels[count++] = default_5ghz[i];
        }
    }
    #endif
    
    ESP_LOGI(TAG, "Country %.2s: using %d channels", country.cc, count);
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
        
        if (!found) {
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
