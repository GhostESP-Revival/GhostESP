/**
 * @file airtag_scan.c
 * @brief Apple AirTag device detection scan implementation
 * 
 * This module handles BLE scanning for Apple AirTag devices including:
 * - Starting and stopping AirTag detection scans
 * - Managing discovered device storage
 * - Listing and selecting discovered AirTags
 * - Spoofing selected AirTag devices
 */

#include "scans/ble/airtag_scan.h"
#include "core/scan_saver.h"
#include "core/glog.h"
#include "core/utils.h"
#include "managers/ble_manager.h"
#include "managers/rgb_manager.h"
#include "managers/status_display_manager.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "nimble/ble.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Maximum number of AirTags to track based on available memory
#ifdef CONFIG_SPIRAM
#define MAX_AIRTAGS 50
#else
#define MAX_AIRTAGS 16
#endif

// RSSI log interval to avoid spamming logs
#define AIRTAG_RSSI_LOG_INTERVAL_MS 5000

// Module tag for logging
static const char *TAG = "AirTagScan";

// Structure to store discovered AirTag information
typedef struct {
    ble_addr_t addr;
    uint8_t payload[BLE_HS_ADV_MAX_SZ]; // Store the full payload
    size_t payload_len;
    int8_t rssi;
    bool selected_for_spoofing;
} AirTagDevice;

// Discovered AirTag storage
static AirTagDevice discovered_airtags[MAX_AIRTAGS];
static int discovered_airtag_count = 0;
static int selected_airtag_index = -1;
static TickType_t airtag_last_rssi_log[MAX_AIRTAGS];
static volatile bool airtag_scan_active = false;
static volatile bool airtag_spoofing_active = false;

// External RGB manager
extern RGBManager_t rgb_manager;

// Forward declarations
static void airtag_scanner_callback(struct ble_gap_event *event, size_t len);

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Check if payload contains Apple AirTag patterns
 * 
 * @param payload BLE advertisement payload
 * @param len Length of payload
 * @return true if AirTag pattern detected
 */
static bool is_airtag_pattern(const uint8_t *payload, size_t len) {
    if (payload == NULL || len < 4) {
        return false;
    }
    
    for (size_t i = 0; i <= len - 4; i++) {
        // Pattern 1: Nearby AirTag (0x1E 0xFF 0x4C 0x00)
        // Pattern 2: Offline Finding (0x4C 0x00 0x12 0x19)
        if ((payload[i] == 0x1E && payload[i + 1] == 0xFF && 
             payload[i + 2] == 0x4C && payload[i + 3] == 0x00) ||
            (payload[i] == 0x4C && payload[i + 1] == 0x00 && 
             payload[i + 2] == 0x12 && payload[i + 3] == 0x19)) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Find AirTag device index by MAC address
 * 
 * @param addr BLE address to search for
 * @return int Index of device if found, -1 otherwise
 */
static int find_airtag_by_addr(const ble_addr_t *addr) {
    for (int i = 0; i < discovered_airtag_count; i++) {
        if (memcmp(discovered_airtags[i].addr.val, addr->val, 6) == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Log AirTag discovery information
 * 
 * @param idx Index of discovered AirTag
 * @param total Total count of discovered AirTags
 * @param mac MAC address string
 * @param rssi RSSI value
 */
static void log_airtag_discovery(int idx, int total, const char *mac, int8_t rssi) {
    glog("[%d] AirTag Found (Total: %d)\n"
         "     MAC: %s,\n"
         "     RSSI: %d dBm (%s),\n",
         idx, total, mac, rssi, rssi_to_proximity(rssi));
}

/**
 * @brief Log AirTag RSSI update
 * 
 * @param idx Index of AirTag
 * @param mac MAC address string
 * @param rssi RSSI value
 */
static void log_airtag_rssi_update(int idx, const char *mac, int8_t rssi) {
    glog("[%d] AirTag RSSI Update: %d dBm (%s)\n"
         "     MAC: %s\n",
         idx, rssi, rssi_to_proximity(rssi), mac);
}

// ============================================================================
// BLE Callback
// ============================================================================

/**
 * @brief BLE callback for AirTag detection during scan
 * 
 * This callback is invoked for each BLE advertisement received during
 * an AirTag scan. It checks if the device is an AirTag and adds
 * it to the discovered devices list.
 * 
 * @param event BLE gap event containing advertisement data
 * @param len Length of event data
 */
static void airtag_scanner_callback(struct ble_gap_event *event, size_t len) {
    (void)len;
    if (!airtag_scan_active || event->type != BLE_GAP_EVENT_DISC) {
        return;
    }
    
    const uint8_t *payload = event->disc.data;
    size_t payload_len = event->disc.length_data;

    if (!is_airtag_pattern(payload, payload_len)) {
        return;
    }

    // Check if this AirTag is already discovered
    int existing_idx = find_airtag_by_addr(&event->disc.addr);
    
    if (existing_idx >= 0) {
        // Update RSSI for existing AirTag
        discovered_airtags[existing_idx].rssi = event->disc.rssi;

        TickType_t now = xTaskGetTickCount();
        TickType_t elapsed = now - airtag_last_rssi_log[existing_idx];
        if (airtag_last_rssi_log[existing_idx] == 0 || 
            elapsed >= pdMS_TO_TICKS(AIRTAG_RSSI_LOG_INTERVAL_MS)) {
            char macAddress[18];
            format_mac_address(discovered_airtags[existing_idx].addr.val, 
                              macAddress, sizeof(macAddress), false);
            log_airtag_rssi_update(existing_idx, macAddress, event->disc.rssi);
            airtag_last_rssi_log[existing_idx] = now;
        }
        return;
    }

    // Add new AirTag to discovered list
    if (discovered_airtag_count >= MAX_AIRTAGS) {
        return;
    }

    AirTagDevice *new_tag = &discovered_airtags[discovered_airtag_count];
    memcpy(new_tag->addr.val, event->disc.addr.val, 6);
    new_tag->addr.type = event->disc.addr.type;
    new_tag->rssi = event->disc.rssi;
    memcpy(new_tag->payload, payload, payload_len);
    new_tag->payload_len = payload_len;
    new_tag->selected_for_spoofing = false;
    airtag_last_rssi_log[discovered_airtag_count] = xTaskGetTickCount();

    char macAddress[18];
    format_mac_address(event->disc.addr.val, macAddress, sizeof(macAddress), false);
    log_airtag_discovery(discovered_airtag_count, discovered_airtag_count + 1,
                         macAddress, event->disc.rssi);
    pulse_once(&rgb_manager, 0, 0, 255);
    
    discovered_airtag_count++;
}

// ============================================================================
// Scan Operations - One function at a time
// ============================================================================

/**
 * @brief Start scanning for Apple AirTag devices
 */
void airtag_scan_start(void) {
    // Initialize BLE first if needed (same pattern as flipper_scan)
    if (!ble_is_initialized()) {
        ble_init();
    }

    // Wait for BLE stack to be ready with proper sync
    if (!ble_wait_for_ready()) {
        ESP_LOGE(TAG, "BLE stack not ready for AirTag scanner");
        return;
    }

    // Now clear data structures and set active flag
    memset(discovered_airtags, 0, sizeof(discovered_airtags));
    memset(airtag_last_rssi_log, 0, sizeof(airtag_last_rssi_log));
    discovered_airtag_count = 0;
    selected_airtag_index = -1;
    airtag_scan_active = true;

    ESP_LOGI(TAG, "AirTag scanner: registering handler and starting BLE scan");
    ble_register_handler(airtag_scanner_callback);
    ble_start_scanning();
}

/**
 * @brief Stop scanning for Apple AirTag devices
 */
void airtag_scan_stop(void) {
    // Save discovered AirTags to file if any were found
    if (airtag_scan_active && discovered_airtag_count > 0) {
        scan_file_t sf = SCAN_FILE_INIT;
        if (scan_file_open(&sf, "airtag_scan", "txt") == ESP_OK) {
            scan_file_printf(&sf, "--- Discovered AirTags (%d) ---\n", discovered_airtag_count);
            for (int i = 0; i < discovered_airtag_count; i++) {
                char mac[18];
                format_mac_address(discovered_airtags[i].addr.val, mac, sizeof(mac), false);
                scan_file_printf(&sf, "[%d] MAC: %s, RSSI: %d dBm\n",
                                 i, mac, discovered_airtags[i].rssi);
            }
            scan_file_close(&sf);
        }
    }

    airtag_scan_active = false;
    ble_unregister_handler(airtag_scanner_callback);
}

/**
 * @brief Get the count of discovered AirTag devices
 */
int airtag_scan_get_count(void) {
    return discovered_airtag_count;
}

/**
 * @brief Print the list of discovered AirTag devices
 */
void airtag_scan_print_results(void) {
    glog("--- Discovered AirTags (%d) ---\n", discovered_airtag_count);
    if (discovered_airtag_count == 0) {
        glog("No AirTags discovered yet.\n");
        return;
    }

    scan_file_t sf = SCAN_FILE_INIT;
    bool saving = (scan_file_open(&sf, "airtag_scan", "txt") == ESP_OK);
    if (saving) scan_file_printf(&sf, "--- Discovered AirTags (%d) ---\n", discovered_airtag_count);

    for (int i = 0; i < discovered_airtag_count; i++) {
        char macAddress[18];
        format_mac_address(discovered_airtags[i].addr.val, macAddress, sizeof(macAddress), false);

        glog("[%d] MAC: %s,\n"
             "     RSSI: %d dBm (%s)%s\n",
             i, macAddress, discovered_airtags[i].rssi,
             rssi_to_proximity(discovered_airtags[i].rssi),
             (i == selected_airtag_index) ? " (Selected)" : "");
        if (saving) {
            scan_file_printf(&sf, "[%d] MAC: %s, RSSI: %d dBm\n",
                             i, macAddress, discovered_airtags[i].rssi);
        }
    }
    glog("-------------------------------\n");
    if (saving) scan_file_close(&sf);
}

/**
 * @brief Check if an AirTag scan is currently active
 */
bool airtag_scan_is_active(void) {
    return airtag_scan_active;
}

/**
 * @brief Select an AirTag for spoofing
 */
void airtag_scan_select(int index) {
    if (index < 0 || index >= discovered_airtag_count) {
        glog("Error: Invalid AirTag index %d. Use 'listairtags' to see valid indices.\n", index);
        selected_airtag_index = -1;
        return;
    }

    selected_airtag_index = index;
    char macAddress[18];
    format_mac_address(discovered_airtags[index].addr.val, macAddress, sizeof(macAddress), false);

    glog("Selected AirTag [%d]: MAC %s\n", index, macAddress);
}

/**
 * @brief Start spoofing the selected AirTag
 */
void airtag_scan_start_spoofing(void) {
    if (selected_airtag_index < 0 || selected_airtag_index >= discovered_airtag_count) {
        glog("Error: No AirTag selected for spoofing. Use 'selectairtag <index>'.\n");
        return;
    }

    // Stop current activities (scanning, advertising) before starting new advertisement
    if (ble_is_initialized()) {
        ble_stop();
    }

    // Reinitialize BLE for advertising
    if (!ble_is_initialized()) {
        ble_init();
    }

    if (!ble_wait_for_ready()) {
        ESP_LOGE(TAG, "BLE stack not ready for AirTag spoofing");
        glog("Error: BLE not ready for spoofing\n");
        return;
    }

    AirTagDevice *tag_to_spoof = &discovered_airtags[selected_airtag_index];

    struct ble_gap_adv_params adv_params;
    int rc;

    // Find the start of the Apple Manufacturer Data (0xFF) in the payload
    uint8_t *mfg_data_start = NULL;
    size_t mfg_data_len = 0;
    size_t current_index = 0;
    while (current_index < tag_to_spoof->payload_len) {
        uint8_t field_len = tag_to_spoof->payload[current_index];

        if (field_len == 0 || current_index + field_len >= tag_to_spoof->payload_len) break;
        uint8_t field_type = tag_to_spoof->payload[current_index + 1];
        if (field_type == 0xFF && field_len >= 3) { // Manufacturer Specific Data
            mfg_data_start = &tag_to_spoof->payload[current_index + 2];
            mfg_data_len = field_len - 1;
            break;
        }
        current_index += field_len + 1;
    }

    uint8_t adv_buf[31];
    size_t adv_len = 0;

    // Build proper BLE advertisement format
    // Start with flags
    adv_buf[adv_len++] = 0x02;  // Length
    adv_buf[adv_len++] = 0x01;  // Type: Flags
    adv_buf[adv_len++] = 0x1A;  // Data: General Discoverable + BR/EDR Not Supported

    // Add manufacturer data if available
    if (mfg_data_start && mfg_data_len > 0) {
        size_t space = sizeof(adv_buf) - adv_len;
        size_t max_mfg_len = space - 2; // Reserve space for length and type bytes

        if (max_mfg_len >= 3) {
            size_t copy_len = mfg_data_len;
            if (copy_len > max_mfg_len) {
                copy_len = max_mfg_len;
                ESP_LOGW(TAG, "Truncated manufacturer data from %zu to %zu bytes", mfg_data_len, copy_len);
            }

            adv_buf[adv_len++] = (uint8_t)(copy_len + 1); // Length (type + data)
            adv_buf[adv_len++] = 0xFF; // Type: Manufacturer Specific Data
            memcpy(&adv_buf[adv_len], mfg_data_start, copy_len);
            adv_len += copy_len;
        }
    } else if (tag_to_spoof->payload_len > 2) {
        // Fallback: use raw payload starting from byte 2
        size_t space = sizeof(adv_buf) - adv_len;
        size_t max_mfg_len = space - 2;

        if (max_mfg_len >= 3) {
            size_t use = tag_to_spoof->payload_len - 2;
            if (use > max_mfg_len) {
                use = max_mfg_len;
            }

            adv_buf[adv_len++] = (uint8_t)(use + 1);
            adv_buf[adv_len++] = 0xFF;
            memcpy(&adv_buf[adv_len], &tag_to_spoof->payload[2], use);
            adv_len += use;
        }
    } else {
        ESP_LOGE(TAG, "No valid data to advertise");
        return;
    }

    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // Clear any existing adv data
    (void)ble_gap_adv_set_data(NULL, 0);
    (void)ble_gap_adv_rsp_set_data(NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(10));

    rc = ble_gap_adv_set_data(adv_buf, adv_len);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error setting advertisement data; rc=%d", rc);
        glog("Error setting adv data; rc=%d\n", rc);
        return;
    }

    // Configure advertisement parameters
    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_NON; // Non-connectable
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN; // General discoverable

    // Start advertising using the selected AirTag's address
    uint8_t own_addr_type;

    if (tag_to_spoof->addr.type == BLE_ADDR_RANDOM) {
        uint8_t rnd_addr[6];
        memcpy(rnd_addr, tag_to_spoof->addr.val, 6);
        if ((rnd_addr[5] & 0xC0) == 0xC0) {
            rc = ble_hs_id_set_rnd(rnd_addr);
        } else {
            rnd_addr[5] = (rnd_addr[5] & 0x3F) | 0xC0;
            if ((rnd_addr[0] | rnd_addr[1] | rnd_addr[2] | rnd_addr[3] | rnd_addr[4] | (rnd_addr[5] & 0x3F)) == 0x00) {
                rnd_addr[0] = 0x01;
            }
            rc = ble_hs_id_set_rnd(rnd_addr);
        }
        if (rc != 0) {
            ESP_LOGE(TAG, "Failed to set random address for spoofing; rc=%d", rc);
            rc = ble_hs_id_infer_auto(0, &own_addr_type);
            if (rc != 0) {
                ESP_LOGE(TAG, "Error inferring own address; rc=%d", rc);
                return;
            }
            ESP_LOGW(TAG, "Using default inferred address type %d", own_addr_type);
        } else {
            own_addr_type = BLE_OWN_ADDR_RANDOM;
            ESP_LOGI(TAG, "Set random address successfully");
            glog("Using spoofed random address.\n");
        }
    } else {
        ESP_LOGW(TAG, "Cannot spoof non-random address type %d. Using default address.", tag_to_spoof->addr.type);
        rc = ble_hs_id_infer_auto(0, &own_addr_type);
        if (rc != 0) {
            ESP_LOGE(TAG, "Error inferring own address; rc=%d", rc);
            return;
        }
    }

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event_general, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error starting spoofing advertisement; rc=%d", rc);
        glog("Error starting spoof adv; rc=%d\n", rc);
        return;
    }

    airtag_spoofing_active = true;
    char macAddress[18];
    format_mac_address(tag_to_spoof->addr.val, macAddress, sizeof(macAddress), false);

    glog("Started spoofing AirTag [%d]\n"
         "     MAC: %s\n",
         selected_airtag_index, macAddress);
    status_display_show_status("AirTag Spoof On");
    pulse_once(&rgb_manager, 0, 255, 0);
}

bool airtag_scan_spoof_device(const uint8_t *mac, uint8_t addr_type,
                              const uint8_t *payload, size_t payload_len, int8_t rssi) {
    if (mac == NULL || payload == NULL || payload_len == 0) {
        return false;
    }

    int index = -1;
    for (int i = 0; i < discovered_airtag_count; i++) {
        if (discovered_airtags[i].addr.type == addr_type &&
            memcmp(discovered_airtags[i].addr.val, mac, 6) == 0) {
            index = i;
            break;
        }
    }

    if (index < 0) {
        if (discovered_airtag_count >= MAX_AIRTAGS) {
            glog("Error: AirTag list is full.\n");
            return false;
        }
        index = discovered_airtag_count++;
        memset(&discovered_airtags[index], 0, sizeof(discovered_airtags[index]));
        discovered_airtags[index].addr.type = addr_type;
        memcpy(discovered_airtags[index].addr.val, mac, 6);
    }

    discovered_airtags[index].payload_len =
        (payload_len > BLE_HS_ADV_MAX_SZ) ? BLE_HS_ADV_MAX_SZ : payload_len;
    memcpy(discovered_airtags[index].payload, payload, discovered_airtags[index].payload_len);
    discovered_airtags[index].rssi = rssi;

    airtag_scan_select(index);
    airtag_scan_start_spoofing();
    return airtag_spoofing_active;
}

/**
 * @brief Stop spoofing the AirTag
 */
void airtag_scan_stop_spoofing(void) {
    if (ble_gap_adv_active()) {
        int rc = ble_gap_adv_stop();
        if (rc == 0) {
            glog("Stopped AirTag spoofing advertisement.\n");
            status_display_show_status("AirTag Spoof Off");
        } else {
            ESP_LOGE(TAG, "Error stopping spoofing advertisement; rc=%d", rc);
            glog("Error stopping spoof adv; rc=%d\n", rc);
            status_display_show_status("Spoof Stop Fail");
        }
        // Reset selected index after stopping spoof
        selected_airtag_index = -1;
        airtag_spoofing_active = false;
    } else {
        glog("No spoofing advertisement active.\n");
        status_display_show_status("No Spoof Active");
    }
}
