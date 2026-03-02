/**
 * @file ap_scan.c
 * @brief WiFi Access Point scanning implementation
 * 
 * This module handles standard WiFi AP scanning operations including:
 * - Starting and stopping AP scans
 * - Managing scan results storage
 * - Selecting APs for further operations
 * - Printing scan results with vendor OUI lookup
 */

#include "scans/wifi/ap_scan.h"
#include "scans/wifi/wifi_channels.h"
#include "core/scan_saver.h"
#include "core/ouis.h"
#include "core/glog.h"
#include "core/utils.h"
#include "managers/ap_manager.h"
#include "managers/rgb_manager.h"
#include "managers/settings_manager.h"
#include "managers/status_display_manager.h"
#include "managers/views/terminal_screen.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Maximum WiFi channel based on target
#if !defined(MAX_WIFI_CHANNEL)
#if defined(CONFIG_IDF_TARGET_ESP32C5)
#define MAX_WIFI_CHANNEL 165
#else
#define MAX_WIFI_CHANNEL 13
#endif
#endif

// Module tag for logging
static const char *TAG = "APScan";

// Scan result storage (exported for legacy compatibility)
wifi_ap_record_t *scanned_aps = NULL;
uint16_t ap_count = 0;

// Selection storage (module-local)
static wifi_ap_record_t selected_ap;
static wifi_ap_record_t *selected_aps = NULL;
static int selected_ap_count = 0;

// External dependencies
extern RGBManager_t rgb_manager;
extern TaskHandle_t rgb_effect_task_handle;

// Forward declarations
static void sanitize_ssid_and_check_hidden(const uint8_t* input_ssid, char* output_buffer, size_t buffer_size);
static void print_ap_entry_formatted(uint16_t idx, const wifi_ap_record_t *rec, bool include_security);

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Sanitize SSID string and handle hidden networks
 * 
 * @param input_ssid Raw SSID bytes from WiFi record
 * @param output_buffer Buffer to store sanitized SSID string
 * @param buffer_size Size of output buffer
 */
static void sanitize_ssid_and_check_hidden(const uint8_t* input_ssid, char* output_buffer, size_t buffer_size) {
    char temp_ssid[33];
    memcpy(temp_ssid, input_ssid, 32);
    temp_ssid[32] = '\0';

    if (strlen(temp_ssid) == 0) {
        snprintf(output_buffer, buffer_size, "(Hidden)");
    } else {
        int len = strlen(temp_ssid);
        int out_idx = 0;
        for (int k = 0; k < len && out_idx < buffer_size - 1; k++) {
            char c = temp_ssid[k];
            output_buffer[out_idx++] = (c >= 32 && c <= 126) ? c : '.';
        }
        output_buffer[out_idx] = '\0';
    }
}

/**
 * @brief Print a single AP entry in formatted style
 * 
 * @param idx Index number to display
 * @param rec WiFi AP record to print
 * @param include_security Whether to include security details
 */
static void print_ap_entry_formatted(uint16_t idx, const wifi_ap_record_t *rec, bool include_security) {
    char sanitized_ssid[33];
    sanitize_ssid_and_check_hidden(rec->ssid, sanitized_ssid, sizeof(sanitized_ssid));

    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             rec->bssid[0], rec->bssid[1], rec->bssid[2],
             rec->bssid[3], rec->bssid[4], rec->bssid[5]);

    char vendor[64] = {0};
    bool has_vendor = ouis_lookup_vendor(mac_str, vendor, sizeof(vendor));

    glog("[%u] SSID: %s,\n"
         "     BSSID: %s,\n"
         "     RSSI: %d,\n"
         "     Channel: %d,\n",
         idx, sanitized_ssid, mac_str,
         rec->rssi, rec->primary);

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    if (include_security) {
        int ch = rec->primary;
        const char *band_str = wifi_channels_get_band_name(ch);
        glog("     Band: %s,\n", band_str);

        const char *auth_str = "Unknown";
        const char *pmf_str = NULL;

        switch (rec->authmode) {
            case WIFI_AUTH_OPEN:
                auth_str = "Open";
                break;
            case WIFI_AUTH_WEP:
                auth_str = "WEP";
                break;
            case WIFI_AUTH_WPA_PSK:
                auth_str = "WPA";
                break;
            case WIFI_AUTH_WPA2_PSK:
                auth_str = "WPA2";
                break;
            case WIFI_AUTH_WPA_WPA2_PSK:
                auth_str = "WPA/WPA2";
                break;
            case WIFI_AUTH_WPA2_ENTERPRISE:
                auth_str = "WPA2-Enterprise";
                break;
            case WIFI_AUTH_WPA3_PSK:
                auth_str = "WPA3";
                pmf_str = "Required";
                break;
            case WIFI_AUTH_WPA2_WPA3_PSK:
                auth_str = "WPA2/WPA3";
                pmf_str = "Required (WPA3)";
                break;
            case WIFI_AUTH_WAPI_PSK:
                auth_str = "WAPI";
                break;
            case WIFI_AUTH_WPA3_ENTERPRISE:
                auth_str = "WPA3-Enterprise";
                pmf_str = "Required";
                break;
            default:
                auth_str = "Unknown";
                break;
        }

        if (pmf_str) {
            glog("     Security: %s\n     PMF: %s\n", auth_str, pmf_str);
        } else {
            glog("     Security: %s\n", auth_str);
        }
    }
#else
    (void)include_security;
#endif

    if (has_vendor) {
        glog("     Vendor: %s\n", vendor);
    }
}

// ============================================================================
// Scan Operations
// ============================================================================

void ap_scan_start(void) {
    log_heap_status(TAG, "scan_start_pre");
    status_display_show_status("WiFi Scanning...");

    // Free any previous selections or scan buffers before starting a fresh scan
    if (selected_aps != NULL) {
        free(selected_aps);
        selected_aps = NULL;
        selected_ap_count = 0;
    }
    if (scanned_aps != NULL) {
        free(scanned_aps);
        scanned_aps = NULL;
        ap_count = 0;
    }

    ap_manager_stop_services();

    wifi_mode_t current_mode;
    esp_err_t err = esp_wifi_get_mode(&current_mode);
    if (err == ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "Wi-Fi not initialized, reinitializing driver...");
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reinit Wi-Fi: %s", esp_err_to_name(err));
            TERMINAL_VIEW_ADD_TEXT("WiFi init failed: %s\n", esp_err_to_name(err));
            return;
        }
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set STA mode: %s", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("WiFi mode set failed: %s\n", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Wi-Fi: %s", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("WiFi start failed: %s\n", esp_err_to_name(err));
        return;
    }

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
#ifdef CONFIG_IDF_TARGET_ESP32C5
        // Target ~5s total sweep on C5
        .scan_time = {.active.min = 250, .active.max = 300, .passive = 300}
#else
        .scan_time = {.active.min = 450, .active.max = 500, .passive = 500}
#endif
    };

    rgb_manager_set_color(&rgb_manager, -1, 50, 255, 50, false);

    printf("WiFi Scan started\n");
#ifdef CONFIG_IDF_TARGET_ESP32C5
    printf("Please wait ~5 Seconds...\n");
    TERMINAL_VIEW_ADD_TEXT("Please wait ~5 Seconds...\n");
#else
    printf("Please wait 5 Seconds...\n");
    TERMINAL_VIEW_ADD_TEXT("Please wait 5 Seconds...\n");
#endif

    err = esp_wifi_scan_start(&scan_config, true);

    if (err != ESP_OK) {
        printf("WiFi scan failed to start: %s", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("WiFi scan failed to start\n");
        log_heap_status(TAG, "scan_start_failed");
        return;
    }

    ap_scan_stop();
    log_heap_status(TAG, "scan_start_post");
    esp_wifi_stop();
    ap_manager_start_services();

    // Restore saved static color if no RGB effect is running
    if (rgb_effect_task_handle == NULL) {
        RGBMode mode = settings_get_rgb_mode(&G_Settings);
        if (mode != RGB_MODE_RAINBOW && mode != RGB_MODE_STEALTH &&
            mode != RGB_MODE_KNIGHT_RIDER && mode != RGB_MODE_NORMAL) {
            rgb_manager_apply_static_from_settings();
        }
    }
}

void ap_scan_stop(void) {
    esp_err_t err;

    log_heap_status(TAG, "scan_stop_pre");
    err = esp_wifi_scan_stop();
    if (err == ESP_ERR_WIFI_NOT_STARTED) {
        return;
    } else if (err != ESP_OK) {
        printf("Failed to stop WiFi scan: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("Failed to stop WiFi scan\n");
        return;
    }

    rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);

    uint16_t initial_ap_count = 0;
    err = esp_wifi_scan_get_ap_num(&initial_ap_count);
    if (err != ESP_OK) {
        printf("Failed to get AP count: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("Failed to get AP count: %s\n", esp_err_to_name(err));
        return;
    }

    printf("Found %u access points\n", initial_ap_count);
    TERMINAL_VIEW_ADD_TEXT("Found %u access points\n", initial_ap_count);

    // Truncate to avoid excessive memory usage
    if (initial_ap_count > AP_SCAN_MAX_RESULTS) {
        printf("Too many APs (%u). Truncating list to first %d\n", initial_ap_count, AP_SCAN_MAX_RESULTS);
        TERMINAL_VIEW_ADD_TEXT("Showing first %d APs (truncated)\n", AP_SCAN_MAX_RESULTS);
        initial_ap_count = AP_SCAN_MAX_RESULTS;
    }

    if (initial_ap_count > 0) {
        if (scanned_aps != NULL) {
            free(scanned_aps);
            scanned_aps = NULL;
        }

        if (selected_aps != NULL) {
            free(selected_aps);
            selected_aps = NULL;
            selected_ap_count = 0;
        }

        scanned_aps = calloc(initial_ap_count, sizeof(wifi_ap_record_t));
        if (scanned_aps == NULL) {
            printf("Failed to allocate memory for AP info\n");
            ap_count = 0;
            return;
        }

        uint16_t actual_ap_count = initial_ap_count;
        err = esp_wifi_scan_get_ap_records(&actual_ap_count, scanned_aps);
        if (err != ESP_OK) {
            printf("Failed to get AP records: %s\n", esp_err_to_name(err));
            free(scanned_aps);
            scanned_aps = NULL;
            ap_count = 0;
            return;
        }

        ap_count = actual_ap_count;
    } else {
        printf("No access points found\n");
        ap_count = 0;
    }
}

void ap_scan_get_results(uint16_t *count, wifi_ap_record_t **aps) {
    if (count != NULL) {
        *count = ap_count;
    }
    if (aps != NULL) {
        *aps = scanned_aps;
    }
}

void ap_scan_clear_results(void) {
    if (scanned_aps != NULL) {
        free(scanned_aps);
        scanned_aps = NULL;
        ap_count = 0;
    }
    if (selected_aps != NULL) {
        free(selected_aps);
        selected_aps = NULL;
        selected_ap_count = 0;
    }
}

void ap_scan_print_results(void) {
    if (scanned_aps == NULL) {
        glog("AP information not available\n");
        return;
    }

    scan_file_t sf = SCAN_FILE_INIT;
    bool saving = (scan_file_open(&sf, "ap_scan", "txt") == ESP_OK);

    uint16_t limit = ap_count;

    if (saving) {
        scan_file_printf(&sf, "--- AP Scan Results (%u APs) ---\n", limit);
    }

    for (uint16_t i = 0; i < limit; i++) {
        char sanitized_ssid[33];
        sanitize_ssid_and_check_hidden(scanned_aps[i].ssid, sanitized_ssid, sizeof(sanitized_ssid));

        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 scanned_aps[i].bssid[0], scanned_aps[i].bssid[1], scanned_aps[i].bssid[2],
                 scanned_aps[i].bssid[3], scanned_aps[i].bssid[4], scanned_aps[i].bssid[5]);
        char vendor[64] = {0};
        bool has_vendor = ouis_lookup_vendor(mac_str, vendor, sizeof(vendor));

        glog("[%u] SSID: %s,\n"
             "     BSSID: %02X:%02X:%02X:%02X:%02X:%02X,\n"
             "     RSSI: %d,\n"
             "     Channel: %d,\n",
             i, sanitized_ssid,
             scanned_aps[i].bssid[0], scanned_aps[i].bssid[1],
             scanned_aps[i].bssid[2], scanned_aps[i].bssid[3],
             scanned_aps[i].bssid[4], scanned_aps[i].bssid[5],
             scanned_aps[i].rssi,
             scanned_aps[i].primary);

        if (saving) {
            scan_file_printf(&sf, "[%u] SSID: %s, BSSID: %s, RSSI: %d, CH: %d",
                             i, sanitized_ssid, mac_str,
                             scanned_aps[i].rssi, scanned_aps[i].primary);
        }

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        {
            int ch = scanned_aps[i].primary;
            const char *band_str = wifi_channels_get_band_name(ch);
            glog("     Band: %s,\n", band_str);

            const char *auth_str = "Unknown";
            const char *pmf_str = NULL;

            switch (scanned_aps[i].authmode) {
                case WIFI_AUTH_OPEN:
                    auth_str = "Open";
                    break;
                case WIFI_AUTH_WEP:
                    auth_str = "WEP";
                    break;
                case WIFI_AUTH_WPA_PSK:
                    auth_str = "WPA";
                    break;
                case WIFI_AUTH_WPA2_PSK:
                    auth_str = "WPA2";
                    break;
                case WIFI_AUTH_WPA_WPA2_PSK:
                    auth_str = "WPA/WPA2";
                    break;
                case WIFI_AUTH_WPA2_ENTERPRISE:
                    auth_str = "WPA2-Enterprise";
                    break;
                case WIFI_AUTH_WPA3_PSK:
                    auth_str = "WPA3";
                    pmf_str = "Required";
                    break;
                case WIFI_AUTH_WPA2_WPA3_PSK:
                    auth_str = "WPA2/WPA3";
                    pmf_str = "Required (WPA3)";
                    break;
                case WIFI_AUTH_WAPI_PSK:
                    auth_str = "WAPI";
                    break;
                case WIFI_AUTH_WPA3_ENTERPRISE:
                    auth_str = "WPA3-Enterprise";
                    pmf_str = "Required";
                    break;
                default:
                    auth_str = "Unknown";
                    break;
            }

            if (pmf_str) {
                glog("     Security: %s\n     PMF: %s\n", auth_str, pmf_str);
            } else {
                glog("     Security: %s\n", auth_str);
            }
            if (saving) {
                scan_file_printf(&sf, ", Band: %s, Security: %s", band_str, auth_str);
                if (pmf_str) scan_file_printf(&sf, ", PMF: %s", pmf_str);
            }
        }
#endif
        if (has_vendor) {
            glog("     Vendor: %s\n", vendor);
            if (saving) scan_file_printf(&sf, ", Vendor: %s", vendor);
        }
        if (saving) scan_file_printf(&sf, "\n");
    }

    if (saving) scan_file_close(&sf);
}

// ============================================================================
// Selection Operations
// ============================================================================

esp_err_t ap_scan_select(int index) {
    if (ap_count == 0) {
        printf("No access points found\n");
        TERMINAL_VIEW_ADD_TEXT("No access points found\n");
        return ESP_ERR_NOT_FOUND;
    }

    if (scanned_aps == NULL) {
        printf("No AP info available (scanned_aps is NULL)\n");
        TERMINAL_VIEW_ADD_TEXT("No AP info available (scanned_aps is NULL)\n");
        return ESP_ERR_NOT_FOUND;
    }

    if (index < 0 || index >= ap_count) {
        printf("Invalid index: %d. Index should be between 0 and %d\n", index, ap_count - 1);
        TERMINAL_VIEW_ADD_TEXT("Invalid index: %d. Index should be between 0 and %d\n", index, ap_count - 1);
        return ESP_ERR_INVALID_ARG;
    }

    selected_ap = scanned_aps[index];

    if (selected_aps != NULL) {
        free(selected_aps);
        selected_aps = NULL;
    }

    selected_aps = malloc(sizeof(wifi_ap_record_t));
    if (selected_aps != NULL) {
        selected_aps[0] = selected_ap;
        selected_ap_count = 1;
    } else {
        selected_ap_count = 0;
    }

    char sanitized_ssid[33];
    sanitize_ssid_and_check_hidden(selected_ap.ssid, sanitized_ssid, sizeof(sanitized_ssid));

    printf("Selected Access Point: SSID: %s, BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n",
           sanitized_ssid, selected_ap.bssid[0], selected_ap.bssid[1], selected_ap.bssid[2],
           selected_ap.bssid[3], selected_ap.bssid[4], selected_ap.bssid[5]);

    TERMINAL_VIEW_ADD_TEXT(
        "Selected Access Point: SSID: %s, BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n", sanitized_ssid,
        selected_ap.bssid[0], selected_ap.bssid[1], selected_ap.bssid[2], selected_ap.bssid[3],
        selected_ap.bssid[4], selected_ap.bssid[5]);

    printf("Selected Access Point Successfully\n");
    TERMINAL_VIEW_ADD_TEXT("Selected Access Point Successfully\n");

    return ESP_OK;
}

esp_err_t ap_scan_select_multiple(int *indices, int count) {
    if (ap_count == 0) {
        printf("No access points found\n");
        TERMINAL_VIEW_ADD_TEXT("No access points found\n");
        return ESP_ERR_NOT_FOUND;
    }

    if (scanned_aps == NULL) {
        printf("No AP info available (scanned_aps is NULL)\n");
        TERMINAL_VIEW_ADD_TEXT("No AP info available (scanned_aps is NULL)\n");
        return ESP_ERR_NOT_FOUND;
    }

    if (count <= 0) {
        printf("Invalid count: %d\n", count);
        TERMINAL_VIEW_ADD_TEXT("Invalid count: %d\n", count);
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < count; i++) {
        if (indices[i] < 0 || indices[i] >= ap_count) {
            printf("Invalid index: %d. Index should be between 0 and %d\n", indices[i], ap_count - 1);
            TERMINAL_VIEW_ADD_TEXT("Invalid index: %d. Index should be between 0 and %d\n", indices[i], ap_count - 1);
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (selected_aps != NULL) {
        free(selected_aps);
        selected_aps = NULL;
    }

    selected_aps = malloc(count * sizeof(wifi_ap_record_t));
    if (selected_aps == NULL) {
        printf("Failed to allocate memory for selected APs\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to allocate memory for selected APs\n");
        selected_ap_count = 0;
        return ESP_ERR_NO_MEM;
    }

    selected_ap_count = count;

    for (int i = 0; i < count; i++) {
        selected_aps[i] = scanned_aps[indices[i]];
    }

    selected_ap = selected_aps[0];

    printf("Selected %d Access Points:\n", count);
    TERMINAL_VIEW_ADD_TEXT("Selected %d Access Points:\n", count);

    for (int i = 0; i < count; i++) {
        char sanitized_ssid[33];
        sanitize_ssid_and_check_hidden(selected_aps[i].ssid, sanitized_ssid, sizeof(sanitized_ssid));

        printf("[%d] SSID: %s, BSSID: %02X:%02X:%02X:%02X:%02X:%02X%s\n",
               i, sanitized_ssid,
               selected_aps[i].bssid[0], selected_aps[i].bssid[1], selected_aps[i].bssid[2],
               selected_aps[i].bssid[3], selected_aps[i].bssid[4], selected_aps[i].bssid[5],
               (i == 0) ? " (Primary)" : "");

        TERMINAL_VIEW_ADD_TEXT("[%d] SSID: %s, BSSID: %02X:%02X:%02X:%02X:%02X:%02X%s\n",
               i, sanitized_ssid,
               selected_aps[i].bssid[0], selected_aps[i].bssid[1], selected_aps[i].bssid[2],
               selected_aps[i].bssid[3], selected_aps[i].bssid[4], selected_aps[i].bssid[5],
               (i == 0) ? " (Primary)" : "");
    }

    printf("Multiple APs selected successfully. Primary AP: %s\n", (char*)selected_ap.ssid);
    TERMINAL_VIEW_ADD_TEXT("Multiple APs selected successfully.\n");

    return ESP_OK;
}

void ap_scan_get_selected(wifi_ap_record_t **aps, int *count) {
    if (aps != NULL) {
        *aps = selected_aps;
    }
    if (count != NULL) {
        *count = selected_ap_count;
    }
}

bool ap_scan_get_selection(wifi_ap_record_t *ap) {
    if (ap == NULL) {
        return false;
    }
    if (selected_ap_count == 0) {
        return false;
    }
    *ap = selected_ap;
    return true;
}

uint16_t ap_scan_get_count(void) {
    return ap_count;
}

bool ap_scan_has_selection(void) {
    return (selected_ap_count > 0);
}

void ap_scan_clear_selection(void) {
    if (selected_aps != NULL) {
        free(selected_aps);
        selected_aps = NULL;
        selected_ap_count = 0;
    }
    memset(&selected_ap, 0, sizeof(selected_ap));
}