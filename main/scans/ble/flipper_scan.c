/**
 * @file flipper_scan.c
 * @brief Flipper Zero device detection scan implementation
 * 
 * This module handles BLE scanning for Flipper Zero devices including:
 * - Starting and stopping Flipper detection scans
 * - Managing discovered device storage
 * - Listing and selecting discovered Flippers
 * - Tracking selected Flipper devices
 */

#include "scans/ble/flipper_scan.h"
#include "core/scan_saver.h"
#include "core/glog.h"
#include "core/utils.h"
#include "managers/ble_manager.h"
#include "managers/rgb_manager.h"
#include "managers/status_display_manager.h"
#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "nimble/ble.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Maximum number of Flippers to track based on available memory
#ifdef CONFIG_SPIRAM
#define MAX_FLIPPERS 50
#else
#define MAX_FLIPPERS 16
#endif

// Flipper UUID identifiers (little-endian)
#define FLIPPER_UUID_WHITE      0x3082
#define FLIPPER_UUID_BLACK      0x3081
#define FLIPPER_UUID_TRANSPARENT 0x3083

// BLE advertisement field types
#define BLE_AD_TYPE_16BIT_UUID_COMPLETE   0x03
#define BLE_AD_TYPE_16BIT_UUID_PARTIAL    0x02
#define BLE_AD_TYPE_32BIT_UUID_COMPLETE   0x05
#define BLE_AD_TYPE_32BIT_UUID_PARTIAL    0x04
#define BLE_AD_TYPE_128BIT_UUID_COMPLETE  0x07
#define BLE_AD_TYPE_128BIT_UUID_PARTIAL   0x06
#define BLE_AD_TYPE_NAME_COMPLETE         0x09
#define BLE_AD_TYPE_NAME_SHORT            0x08

// Module tag for logging
static const char *TAG = "FlipperScan";

// Discovered Flipper device storage
typedef struct {
    ble_addr_t addr;
    char name[32];
    int8_t rssi;
} FlipperDevice;

static FlipperDevice discovered_flippers[MAX_FLIPPERS];
static int discovered_flipper_count = 0;
static int selected_flipper_index = -1;
static volatile bool flipper_scan_active = false;
static TickType_t flipper_adv_last_log_tick = 0;
static uint32_t flipper_adv_suppressed_logs = 0;

// External RGB manager
extern RGBManager_t rgb_manager;

// Forward declarations
static const char *detect_flipper_type_from_adv(const uint8_t *data, size_t len);
static void ble_findtheflippers_callback(struct ble_gap_event *event, size_t len);

// ============================================================================
// Reusable Helper Functions
// ============================================================================

/**
 * @brief Get Flipper type string from UUID value
 * 
 * @param uuid 16-bit UUID value to check
 * @return const char* Type string ("White", "Black", "Transparent") or NULL
 */
static const char *get_flipper_type_from_uuid(uint16_t uuid) {
    switch (uuid) {
        case FLIPPER_UUID_WHITE:
            return "White";
        case FLIPPER_UUID_BLACK:
            return "Black";
        case FLIPPER_UUID_TRANSPARENT:
            return "Transparent";
        default:
            return NULL;
    }
}

/**
 * @brief Check if a 16-bit value matches a Flipper UUID
 * 
 * @param value 16-bit value to check
 * @return true if value is a Flipper UUID
 */
static bool is_flipper_uuid(uint16_t value) {
    return value == FLIPPER_UUID_WHITE ||
           value == FLIPPER_UUID_BLACK ||
           value == FLIPPER_UUID_TRANSPARENT;
}

/**
 * @brief Find Flipper device index by MAC address
 * 
 * @param addr BLE address to search for
 * @return int Index of device if found, -1 otherwise
 */
static int find_flipper_by_addr(const ble_addr_t *addr) {
    for (int i = 0; i < discovered_flipper_count; i++) {
        if (memcmp(discovered_flippers[i].addr.val, addr->val, 6) == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Check for Flipper UUIDs in a byte array
 * 
 * Scans through byte pairs looking for Flipper UUID patterns.
 * Returns the highest priority type found (White > Transparent > Black).
 * 
 * @param data Byte array to scan
 * @param len Length of array
 * @param step Size of each UUID element (2 for 16-bit, 4 for 32-bit)
 * @return const char* Type string or NULL if not found
 */
static const char *scan_for_flipper_uuid(const uint8_t *data, size_t len, size_t step) {
    const char *found_type = NULL;
    
    for (size_t i = 0; i + step <= len; i += step) {
        uint16_t uuid;
        if (step == 2) {
            uuid = read_u16_le(data + i);
        } else {
            // For 32-bit UUIDs, check the lower 16 bits
            uuid = (uint16_t)(read_u32_le(data + i) & 0xFFFF);
        }
        
        if (is_flipper_uuid(uuid)) {
            const char *type = get_flipper_type_from_uuid(uuid);
            // White has highest priority, then Transparent, then Black
            if (type && (!found_type || 
                (strcmp(type, "White") == 0) ||
                (strcmp(type, "Transparent") == 0 && strcmp(found_type, "Black") == 0))) {
                found_type = type;
            }
        }
    }
    return found_type;
}

/**
 * @brief Check for Flipper UUID in 128-bit UUID data
 * 
 * Scans through 128-bit UUID looking for Flipper UUID patterns.
 * 
 * @param data Pointer to 128-bit UUID data
 * @return const char* Type string or NULL if not found
 */
static const char *scan_128bit_for_flipper(const uint8_t *data) {
    // Scan through the 128-bit UUID for Flipper patterns
    for (int i = 0; i <= 14; i++) {
        uint16_t uuid = read_u16_le(data + i);
        if (is_flipper_uuid(uuid)) {
            return get_flipper_type_from_uuid(uuid);
        }
    }
    return NULL;
}

// ============================================================================
// BLE Advertisement Parsing
// ============================================================================

/**
 * @brief Detect Flipper Zero device type from advertisement data
 * 
 * Parses BLE advertisement data to detect Flipper Zero devices and
 * determine their type (White, Black, or Transparent).
 * 
 * @param data Raw advertisement data
 * @param len Length of advertisement data
 * @return const char* Device type string, or NULL if not a Flipper
 */
static const char *detect_flipper_type_from_adv(const uint8_t *data, size_t len) {
    const uint8_t *p = data;
    size_t remaining = len;
    const char *found_type = NULL;

    while (remaining > 1) {
        uint8_t field_len = p[0];

        if (field_len == 0 || (size_t)(field_len + 1) > remaining) {
            break;
        }

        uint8_t field_type = p[1];
        const uint8_t *payload = p + 2;
        uint8_t payload_len = (field_len >= 1) ? (uint8_t)(field_len - 1) : 0;

        const char *type = NULL;

        // Check for Flipper UUID in 16-bit UUID fields
        if ((field_type == BLE_AD_TYPE_16BIT_UUID_PARTIAL || 
             field_type == BLE_AD_TYPE_16BIT_UUID_COMPLETE) && payload_len >= 2) {
            type = scan_for_flipper_uuid(payload, payload_len, 2);
        }
        // Check for Flipper UUID in 32-bit UUID fields
        else if ((field_type == BLE_AD_TYPE_32BIT_UUID_PARTIAL || 
                  field_type == BLE_AD_TYPE_32BIT_UUID_COMPLETE) && payload_len >= 4) {
            type = scan_for_flipper_uuid(payload, payload_len, 4);
        }
        // Check for Flipper UUID in 128-bit UUID fields
        else if ((field_type == BLE_AD_TYPE_128BIT_UUID_PARTIAL || 
                  field_type == BLE_AD_TYPE_128BIT_UUID_COMPLETE) && payload_len >= 16) {
            // Scan each 128-bit UUID in the field
            for (size_t i = 0; i + 16 <= payload_len; i += 16) {
                type = scan_128bit_for_flipper(payload + i);
                if (type) break;
            }
        }

        // Update found_type with priority: White > Transparent > Black
        if (type) {
            if (!found_type || strcmp(type, "White") == 0 ||
                (strcmp(type, "Transparent") == 0 && strcmp(found_type, "Black") == 0)) {
                found_type = type;
            }
        }

        remaining -= (size_t)(field_len + 1);
        p += (size_t)(field_len + 1);
    }

    return found_type;
}

// ============================================================================
// BLE Callback
// ============================================================================

/**
 * @brief BLE callback for Flipper detection during scan
 * 
 * This callback is invoked for each BLE advertisement received during
 * a Flipper scan. It checks if the device is a Flipper Zero and adds
 * it to the discovered devices list.
 * 
 * @param event BLE gap event containing advertisement data
 * @param len Length of event data
 */
static void ble_findtheflippers_callback(struct ble_gap_event *event, size_t len) {
    (void)len;
    int advertisementRssi = event->disc.rssi;

    char advertisementMac[18];
    format_mac_address(event->disc.addr.val, advertisementMac, sizeof(advertisementMac), false);

    char advertisementName[32];
    parse_ble_device_name(event->disc.data, event->disc.length_data, advertisementName,
                          sizeof(advertisementName));

    const char *type_str = detect_flipper_type_from_adv(event->disc.data, event->disc.length_data);
    TickType_t now = xTaskGetTickCount();
    if (flipper_adv_last_log_tick == 0 ||
        (now - flipper_adv_last_log_tick) >= pdMS_TO_TICKS(1000)) {
        ESP_LOGI(TAG,
                 "FindFlippers: MAC=%s RSSI=%d len=%u name='%s' type=%s (suppressed=%lu)",
                 advertisementMac,
                 advertisementRssi,
                 (unsigned int)event->disc.length_data,
                 (advertisementName[0] != '\0') ? advertisementName : "<none>",
                 type_str ? type_str : "<null>",
                 (unsigned long)flipper_adv_suppressed_logs);
        flipper_adv_last_log_tick = now;
        flipper_adv_suppressed_logs = 0;
    } else {
        flipper_adv_suppressed_logs++;
    }

    if (!type_str) {
        return;
    }

    // Check if this Flipper is already discovered
    int existing_idx = find_flipper_by_addr(&event->disc.addr);
    
    if (existing_idx >= 0) {
        discovered_flippers[existing_idx].rssi = advertisementRssi;
        // If this is the selected Flipper, report proximity
        if (existing_idx == selected_flipper_index) {
            glog("Tracking Flipper %d: RSSI %d dBm (%s)\n", 
                 selected_flipper_index, advertisementRssi, 
                 rssi_to_proximity(advertisementRssi));
        }
        return;
    }
    
    // Add new Flipper to discovered list
    if (discovered_flipper_count < MAX_FLIPPERS) {
        discovered_flippers[discovered_flipper_count].addr = event->disc.addr;
        strncpy(discovered_flippers[discovered_flipper_count].name, advertisementName,
                sizeof(discovered_flippers[discovered_flipper_count].name) - 1);
        discovered_flippers[discovered_flipper_count].name[sizeof(discovered_flippers[0].name) - 1] = '\0';
        discovered_flippers[discovered_flipper_count].rssi = advertisementRssi;
        
        glog("[%d] %s Flipper Found:\n"
             "     MAC: %s,\n"
             "     Name: %s,\n"
             "     RSSI: %d dBm\n",
             discovered_flipper_count, type_str,
advertisementMac, advertisementName, advertisementRssi);
        // Avoid blocking the NimBLE host task inside the discovery callback.
        rgb_manager_set_color(&rgb_manager, -1, 255, 165, 0, false);
        discovered_flipper_count++;
    }
}

// ============================================================================
// Scan Operations
// ============================================================================

void flipper_scan_start(void) {
    if (!ble_is_initialized()) {
        ble_init();
    }

    memset(discovered_flippers, 0, sizeof(discovered_flippers));
    discovered_flipper_count = 0;
    selected_flipper_index = -1;
    flipper_scan_active = true;
    flipper_adv_last_log_tick = 0;
    flipper_adv_suppressed_logs = 0;

    ESP_LOGI(TAG, "Find Flippers: registering handler and starting BLE scan");
    ble_register_handler(ble_findtheflippers_callback);
    ble_start_scanning();
}

void flipper_scan_stop(void) {
    // Save discovered Flippers to file if any were found
    if (flipper_scan_active && discovered_flipper_count > 0) {
        scan_file_t sf = SCAN_FILE_INIT;
        if (scan_file_open(&sf, "flipper_scan", "txt") == ESP_OK) {
            scan_file_printf(&sf, "--- Discovered Flippers (%d) ---\n", discovered_flipper_count);
            for (int i = 0; i < discovered_flipper_count; i++) {
                char mac[18];
                format_mac_address(discovered_flippers[i].addr.val, mac, sizeof(mac), false);
                scan_file_printf(&sf, "[%d] MAC: %s, Name: %s, RSSI: %d dBm\n",
                                 i, mac, discovered_flippers[i].name, discovered_flippers[i].rssi);
            }
            scan_file_close(&sf);
        }
    }

    flipper_scan_active = false;
    ble_unregister_handler(ble_findtheflippers_callback);
}

int flipper_scan_get_count(void) {
    return discovered_flipper_count;
}

void flipper_scan_print_results(void) {
    glog("--- Discovered Flippers (%d) ---\n", discovered_flipper_count);
    if (discovered_flipper_count == 0) {
        glog("No Flippers discovered yet.\n");
        return;
    }

    scan_file_t sf = SCAN_FILE_INIT;
    bool saving = (scan_file_open(&sf, "flipper_scan", "txt") == ESP_OK);
    if (saving) scan_file_printf(&sf, "--- Discovered Flippers (%d) ---\n", discovered_flipper_count);

    for (int i = 0; i < discovered_flipper_count; i++) {
        char mac[18];
        format_mac_address(discovered_flippers[i].addr.val, mac, sizeof(mac), false);

        glog("[%d] MAC: %s,\n"
             "     Name: %s,\n"
             "     RSSI: %d dBm%s\n",
             i, mac, discovered_flippers[i].name, discovered_flippers[i].rssi,
             (i == selected_flipper_index) ? " (Selected)" : "");
        if (saving) {
            scan_file_printf(&sf, "[%d] MAC: %s, Name: %s, RSSI: %d dBm\n",
                             i, mac, discovered_flippers[i].name, discovered_flippers[i].rssi);
        }
    }
    glog("-------------------------------\n");
    if (saving) scan_file_close(&sf);
}

bool flipper_scan_is_active(void) {
    return flipper_scan_active;
}

int flipper_scan_get_device_data(int index, uint8_t *mac, int8_t *rssi, char *name, size_t name_len) {
    if (index < 0 || index >= discovered_flipper_count) return -1;
    if (mac) {
        memcpy(mac, discovered_flippers[index].addr.val, 6);
    }
    if (rssi) *rssi = discovered_flippers[index].rssi;
    if (name && name_len > 0) {
        strncpy(name, discovered_flippers[index].name, name_len - 1);
        name[name_len - 1] = '\0';
    }
    return 0;
}

void flipper_scan_select(int index) {
    if (index < 0 || index >= discovered_flipper_count) {
        glog("Error: Invalid Flipper index %d. Use 'listflippers' to see valid indices.\n", index);
        selected_flipper_index = -1;
        return;
    }

    selected_flipper_index = index;
    char mac[18];
    format_mac_address(discovered_flippers[index].addr.val, mac, sizeof(mac), false);

    glog("Selected Flipper [%d]: MAC %s\n", index, mac);
    flipper_scan_start_tracking();
    glog("Started tracking Flipper %d...\n", index);
}

void flipper_scan_start_tracking(void) {
    if (!ble_is_initialized()) {
        ble_init();
    }

    if (!ble_wait_for_ready()) {
        glog("Error: BLE stack not ready. Cannot start Flipper tracking.\n");
        return;
    }

    // Stop any existing scan
    ble_gap_disc_cancel();
    vTaskDelay(pdMS_TO_TICKS(100));

    // Register Flipper callback for tracking
    ble_register_handler(ble_findtheflippers_callback);

    struct ble_gap_disc_params params = {0};
    params.itvl = BLE_HCI_SCAN_ITVL_DEF;
    params.window = BLE_HCI_SCAN_WINDOW_DEF;
    params.filter_duplicates = 0; // receive all advertisement updates

    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }

    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, ble_gap_event_general, NULL);
    if (rc != 0) {
        glog("Error starting tracker; rc=%d\n", rc);
    }
}

bool flipper_scan_track_device(const uint8_t *mac, uint8_t addr_type, const char *name, int8_t rssi) {
    if (mac == NULL) {
        return false;
    }

    int index = -1;
    for (int i = 0; i < discovered_flipper_count; i++) {
        if (discovered_flippers[i].addr.type == addr_type &&
            memcmp(discovered_flippers[i].addr.val, mac, 6) == 0) {
            index = i;
            break;
        }
    }

    if (index < 0) {
        if (discovered_flipper_count >= MAX_FLIPPERS) {
            glog("Error: Flipper list is full.\n");
            return false;
        }
        index = discovered_flipper_count++;
        memset(&discovered_flippers[index], 0, sizeof(discovered_flippers[index]));
        discovered_flippers[index].addr.type = addr_type;
        memcpy(discovered_flippers[index].addr.val, mac, 6);
    }

    discovered_flippers[index].rssi = rssi;
    if (name != NULL && name[0] != '\0') {
        strncpy(discovered_flippers[index].name, name, sizeof(discovered_flippers[index].name) - 1);
        discovered_flippers[index].name[sizeof(discovered_flippers[index].name) - 1] = '\0';
    }

    flipper_scan_select(index);
    return true;
}
