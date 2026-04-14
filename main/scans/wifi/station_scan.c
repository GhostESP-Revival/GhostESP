/**
 * @file station_scan.c
 * @brief WiFi Station (client) scanning implementation
 * 
 * This module handles WiFi station/client scanning operations including:
 * - Starting and stopping station scans with channel hopping
 * - Managing station scan results storage
 * - Selecting stations for further operations
 * - Printing scan results with vendor OUI lookup
 */

#include "scans/wifi/station_scan.h"
#include "scans/wifi/ap_scan.h"
#include "core/scan_saver.h"
#include "core/ouis.h"
#include "core/glog.h"
#include "core/utils.h"
#include "managers/ap_manager.h"
#include "managers/rgb_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Channel hopping configuration
#define SCANSTA_CHANNEL_HOP_INTERVAL_MS 250  ///< Hop channel every 250ms
#define SCANSTA_MAX_WIFI_CHANNEL 13          ///< Scan channels 1-13

// Module tag for logging
static const char *TAG = "StationScan";

// Scan result storage (exported for legacy compatibility)
station_ap_pair_t station_ap_list[STATION_SCAN_MAX_RESULTS];
int station_count = 0;

// Selection storage (exported for legacy compatibility)
station_ap_pair_t selected_station;
bool station_selected = false;

// Multi-selection storage
static station_ap_pair_t *selected_stations = NULL;
static int selected_station_count = 0;

// Module-local state
static bool scan_active = false;
static esp_timer_handle_t scansta_channel_hop_timer = NULL;
static bool scansta_hopping_active = false;
static uint8_t scansta_current_channel = 1;

// External dependencies
extern RGBManager_t rgb_manager;

// Forward declarations
static bool station_exists(const uint8_t *station_mac, const uint8_t *ap_bssid);
static void add_station_ap_pair(const uint8_t *station_mac, const uint8_t *ap_bssid);
static esp_err_t start_scansta_channel_hopping(void);
static void stop_scansta_channel_hopping(void);

// Helper macro to check for broadcast/multicast addresses
#define IS_BROADCAST_OR_MULTICAST(addr) (((addr)[0] & 0x01) || (memcmp((addr), "\xff\xff\xff\xff\xff\xff", 6) == 0))

// 802.11 frame control parsing helpers
#define IEEE80211_FC_TYPE(fc) (((fc) >> 2) & 0x3)
#define IEEE80211_FC_SUBTYPE(fc) (((fc) >> 4) & 0xF)
#define IEEE80211_FC_TO_DS(fc) (((fc) >> 8) & 0x1)
#define IEEE80211_FC_FROM_DS(fc) (((fc) >> 9) & 0x1)

#define IEEE80211_TYPE_MGMT 0
#define IEEE80211_TYPE_DATA 2

static bool is_relevant_mgmt_subtype(uint8_t subtype) {
    switch (subtype) {
        case 0x0: // Assoc Request
        case 0x1: // Assoc Response
        case 0x2: // Reassoc Request
        case 0x3: // Reassoc Response
        case 0xA: // Disassociation
        case 0xB: // Authentication
        case 0xC: // Deauthentication
        case 0xD: // Action
            return true;
        default:
            return false;
    }
}

static bool extract_station_for_bssid(const wifi_ieee80211_hdr_t *hdr,
                                      const uint8_t *bssid,
                                      uint8_t frame_type,
                                      uint8_t frame_subtype,
                                      bool to_ds,
                                      bool from_ds,
                                      const uint8_t **station_out) {
    const uint8_t *candidate = NULL;

    if (frame_type == IEEE80211_TYPE_DATA) {
        // Infrastructure data paths
        if (to_ds && !from_ds) {
            // STA -> AP: addr1 is AP/BSSID, addr2 is station
            if (memcmp(hdr->addr1, bssid, 6) == 0) {
                candidate = hdr->addr2;
            }
        } else if (!to_ds && from_ds) {
            // AP -> STA: addr2 is AP/BSSID, addr1 is station
            if (memcmp(hdr->addr2, bssid, 6) == 0) {
                candidate = hdr->addr1;
            }
        } else if (!to_ds && !from_ds && memcmp(hdr->addr3, bssid, 6) == 0) {
            // Fallback for non-DS frames that still carry the BSSID in addr3
            if (memcmp(hdr->addr2, bssid, 6) != 0 && !IS_BROADCAST_OR_MULTICAST(hdr->addr2)) {
                candidate = hdr->addr2;
            } else if (memcmp(hdr->addr1, bssid, 6) != 0 && !IS_BROADCAST_OR_MULTICAST(hdr->addr1)) {
                candidate = hdr->addr1;
            }
        }
    } else if (frame_type == IEEE80211_TYPE_MGMT) {
        if (!is_relevant_mgmt_subtype(frame_subtype)) {
            return false;
        }

        if (memcmp(hdr->addr1, bssid, 6) == 0 && memcmp(hdr->addr2, bssid, 6) != 0) {
            candidate = hdr->addr2;
        } else if (memcmp(hdr->addr2, bssid, 6) == 0 && memcmp(hdr->addr1, bssid, 6) != 0) {
            candidate = hdr->addr1;
        } else if (memcmp(hdr->addr3, bssid, 6) == 0) {
            if (memcmp(hdr->addr2, bssid, 6) != 0 && !IS_BROADCAST_OR_MULTICAST(hdr->addr2)) {
                candidate = hdr->addr2;
            } else if (memcmp(hdr->addr1, bssid, 6) != 0 && !IS_BROADCAST_OR_MULTICAST(hdr->addr1)) {
                candidate = hdr->addr1;
            }
        }
    }

    if (candidate == NULL || IS_BROADCAST_OR_MULTICAST(candidate) || memcmp(candidate, bssid, 6) == 0) {
        return false;
    }

    *station_out = candidate;
    return true;
}

// ============================================================================
// Reusable Helper Functions
// ============================================================================

/**
 * @brief Format a MAC address to string (uppercase, with colons)
 * 
 * @param mac 6-byte MAC address
 * @param buffer Output buffer (must be at least 18 bytes)
 * @param buffer_size Size of output buffer
 */
static void format_mac_upper(const uint8_t *mac, char *buffer, size_t buffer_size) {
    format_mac_address(mac, buffer, buffer_size, true);
}

/**
 * @brief Look up vendor name for a MAC address with default fallback
 * 
 * @param mac 6-byte MAC address
 * @param vendor_buf Output buffer for vendor name
 * @param buf_size Size of vendor buffer
 */
static void lookup_vendor_with_default(const uint8_t *mac, char *vendor_buf, size_t buf_size) {
    char mac_str[18];
    format_mac_upper(mac, mac_str, sizeof(mac_str));
    
    if (!ouis_lookup_vendor(mac_str, vendor_buf, buf_size)) {
        strncpy(vendor_buf, "Unknown", buf_size - 1);
        vendor_buf[buf_size - 1] = '\0';
    }
}

/**
 * @brief Sanitize SSID string and handle hidden networks
 * 
 * @param input_ssid Raw SSID bytes from WiFi record
 * @param output_buffer Buffer to store sanitized SSID string
 * @param buffer_size Size of output buffer
 */
static void sanitize_ssid(const uint8_t* input_ssid, char* output_buffer, size_t buffer_size) {
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
 * @brief Find an AP in the scanned list by BSSID
 * 
 * @param bssid BSSID to search for
 * @param scanned_aps Array of scanned APs
 * @param ap_count Number of APs in array
 * @param ssid_out Output buffer for SSID (can be NULL)
 * @param ssid_out_size Size of SSID output buffer
 * @return Index of found AP, or -1 if not found
 */
static int find_ap_by_bssid(const uint8_t *bssid, wifi_ap_record_t *scanned_aps, 
                            uint16_t ap_count, char *ssid_out, size_t ssid_out_size) {
    if (scanned_aps == NULL || bssid == NULL) {
        return -1;
    }
    
    for (int i = 0; i < ap_count; i++) {
        if (memcmp(scanned_aps[i].bssid, bssid, 6) == 0) {
            if (ssid_out != NULL && ssid_out_size > 0) {
                sanitize_ssid(scanned_aps[i].ssid, ssid_out, ssid_out_size);
            }
            return i;
        }
    }
    return -1;
}

/**
 * @brief Log station information to glog with consistent formatting
 * 
 * @param index Station index (or -1 for no index)
 * @param station_mac Station MAC address
 * @param ap_bssid AP BSSID
 * @param ssid AP SSID (can be NULL)
 * @param prefix Optional prefix for log message (can be NULL)
 */
static void log_station_info(int index, const uint8_t *station_mac, 
                             const uint8_t *ap_bssid, const char *ssid,
                             const char *prefix) {
    char sta_mac_str[18], ap_mac_str[18];
    char sta_vendor[64] = "Unknown", ap_vendor[64] = "Unknown";
    char ssid_display[33] = "(Unknown AP)";
    
    format_mac_upper(station_mac, sta_mac_str, sizeof(sta_mac_str));
    format_mac_upper(ap_bssid, ap_mac_str, sizeof(ap_mac_str));
    
    lookup_vendor_with_default(station_mac, sta_vendor, sizeof(sta_vendor));
    lookup_vendor_with_default(ap_bssid, ap_vendor, sizeof(ap_vendor));
    
    if (ssid != NULL && strlen(ssid) > 0) {
        strncpy(ssid_display, ssid, sizeof(ssid_display) - 1);
        ssid_display[sizeof(ssid_display) - 1] = '\0';
    }
    
    if (index >= 0) {
        glog("[%d] Station MAC: %s,\n"
             "     Station Vendor: %s,\n"
             "     Associated AP: %s,\n"
             "     AP BSSID: %s,\n"
             "     AP Vendor: %s\n",
             index, sta_mac_str, sta_vendor, ssid_display, ap_mac_str, ap_vendor);
    } else {
        glog("%sStation: %s,\n"
             "     STA Vendor: %s,\n"
             "     Associated AP: %s,\n"
             "     AP BSSID: %s,\n"
             "     AP Vendor: %s\n",
             prefix ? prefix : "", sta_mac_str, sta_vendor, ssid_display, ap_mac_str, ap_vendor);
    }
}

// ============================================================================
// Station List Management Functions
// ============================================================================

/**
 * @brief Check if a station-AP pair already exists in the list
 * 
 * @param station_mac Station MAC address
 * @param ap_bssid AP BSSID
 * @return true if pair exists, false otherwise
 */
static bool station_exists(const uint8_t *station_mac, const uint8_t *ap_bssid) {
    for (int i = 0; i < station_count; i++) {
        if (memcmp(station_ap_list[i].station_mac, station_mac, 6) == 0 &&
            memcmp(station_ap_list[i].ap_bssid, ap_bssid, 6) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Add a station-AP pair to the list
 * 
 * @param station_mac Station MAC address
 * @param ap_bssid AP BSSID
 */
static void add_station_ap_pair(const uint8_t *station_mac, const uint8_t *ap_bssid) {
    if (station_count < STATION_SCAN_MAX_RESULTS) {
        memcpy(station_ap_list[station_count].station_mac, station_mac, 6);
        memcpy(station_ap_list[station_count].ap_bssid, ap_bssid, 6);
        station_count++;
    } else {
        glog("Station list full\nCan't add more stations.\n");
    }
}

// ============================================================================
// Channel Hopping Functions
// ============================================================================

/**
 * @brief Channel hopping timer callback
 */
static void scansta_channel_hop_timer_callback(void *arg) {
    if (!scansta_hopping_active) return;

    scansta_current_channel = (scansta_current_channel % SCANSTA_MAX_WIFI_CHANNEL) + 1;
    esp_wifi_set_channel(scansta_current_channel, WIFI_SECOND_CHAN_NONE);
}

/**
 * @brief Start the channel hopping timer for station scanning
 * 
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t start_scansta_channel_hopping(void) {
    if (scansta_channel_hop_timer != NULL) {
        ESP_LOGW(TAG, "Scansta channel hop timer already exists. Stopping and deleting first.");
        esp_timer_stop(scansta_channel_hop_timer);
        esp_timer_delete(scansta_channel_hop_timer);
        scansta_channel_hop_timer = NULL;
    }

    scansta_current_channel = 1;
    esp_wifi_set_channel(scansta_current_channel, WIFI_SECOND_CHAN_NONE);

    esp_timer_create_args_t timer_args = {
        .callback = scansta_channel_hop_timer_callback,
        .name = "scansta_channel_hop"
    };

    esp_err_t err = esp_timer_create(&timer_args, &scansta_channel_hop_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create scansta channel hop timer: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_timer_start_periodic(scansta_channel_hop_timer, SCANSTA_CHANNEL_HOP_INTERVAL_MS * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start scansta channel hop timer: %s", esp_err_to_name(err));
        esp_timer_delete(scansta_channel_hop_timer);
        scansta_channel_hop_timer = NULL;
        return err;
    }

    scansta_hopping_active = true;
    ESP_LOGI(TAG, "Station Scan Channel Hopping Started.");
    return ESP_OK;
}

/**
 * @brief Stop the channel hopping timer for station scanning
 */
static void stop_scansta_channel_hopping(void) {
    if (scansta_channel_hop_timer) {
        esp_timer_stop(scansta_channel_hop_timer);
        esp_timer_delete(scansta_channel_hop_timer);
        scansta_channel_hop_timer = NULL;
        scansta_hopping_active = false;
        ESP_LOGI(TAG, "Station Scan Channel Hopping Stopped.");
    }
}

// ============================================================================
// Sniffer Callback
// ============================================================================

/**
 * @brief WiFi promiscuous mode callback for station detection
 * 
 * This callback processes management and data frames to detect station-AP associations.
 * 
 * @param buf Packet buffer
 * @param type Packet type
 */
void wifi_stations_sniffer_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    // Focus on Management and Data frames
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) {
        return;
    }

    // Get AP scan results for BSSID matching
    uint16_t ap_count = 0;
    wifi_ap_record_t *scanned_aps = NULL;
    ap_scan_get_results(&ap_count, &scanned_aps);

    // Check if we have scanned APs to compare against
    if (scanned_aps == NULL || ap_count == 0) {
        ESP_LOGW(TAG, "No scanned APs in callback");
        return;
    }

    if (buf == NULL) {
        return;
    }

    const wifi_promiscuous_pkt_t *packet = (const wifi_promiscuous_pkt_t *)buf;
    if (packet->rx_ctrl.sig_len < sizeof(wifi_ieee80211_hdr_t)) {
        return;
    }

    const wifi_ieee80211_hdr_t *hdr = (const wifi_ieee80211_hdr_t *)packet->payload;

    uint16_t frame_ctrl = hdr->frame_ctrl;
    uint8_t frame_type = IEEE80211_FC_TYPE(frame_ctrl);
    uint8_t frame_subtype = IEEE80211_FC_SUBTYPE(frame_ctrl);
    bool to_ds = IEEE80211_FC_TO_DS(frame_ctrl);
    bool from_ds = IEEE80211_FC_FROM_DS(frame_ctrl);

    if (frame_type != IEEE80211_TYPE_MGMT && frame_type != IEEE80211_TYPE_DATA) {
        return;
    }

    if (frame_type == IEEE80211_TYPE_MGMT && !is_relevant_mgmt_subtype(frame_subtype)) {
        return;
    }

    const uint8_t *station_mac = NULL;
    const uint8_t *ap_bssid = NULL;
    int matched_ap_index = -1;

    // Iterate through known APs (from last scan)
    for (int i = 0; i < ap_count; i++) {
        const uint8_t *bssid = scanned_aps[i].bssid;
        if (extract_station_for_bssid(hdr, bssid, frame_type, frame_subtype,
                                      to_ds, from_ds, &station_mac)) {
            ap_bssid = bssid;
            matched_ap_index = i;
            break;
        }
    }

    // If no known AP BSSID found, ignore
    if (matched_ap_index == -1) {
        return;
    }

    // Ensure we are capturing a station, not an AP or broadcast
    if (memcmp(station_mac, ap_bssid, 6) == 0 || IS_BROADCAST_OR_MULTICAST(station_mac)) {
        return;
    }

    // Check if this station/AP pair has already been seen
    if (!station_exists(station_mac, ap_bssid)) {
        // Get the SSID of the matched AP
        char ssid_str[33];
        sanitize_ssid(scanned_aps[matched_ap_index].ssid, ssid_str, sizeof(ssid_str));
        
        // Log the new station using the helper
        log_station_info(-1, station_mac, ap_bssid, ssid_str, "New Station:\n");

        // Add the station and the specific AP BSSID to the list
        add_station_ap_pair(station_mac, ap_bssid);
    }
}

// ============================================================================
// Scan Operations
// ============================================================================

void station_scan_start(void) {
    // Get AP scan results
    uint16_t ap_count = 0;
    wifi_ap_record_t *scanned_aps = NULL;
    ap_scan_get_results(&ap_count, &scanned_aps);

    // Ensure we have a list of APs to compare against first
    if (scanned_aps == NULL || ap_count == 0) {
        glog("No APs scanned previously. Performing initial scan...\n");

        // Perform a synchronous scan
        ap_manager_stop_services();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());

        wifi_scan_config_t scan_config = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = true,
            .scan_time = {.active.min = 450, .active.max = 500, .passive = 500}
        };

        esp_err_t err = esp_wifi_scan_start(&scan_config, true);

        if (err == ESP_OK) {
            uint16_t initial_ap_count = 0;
            err = esp_wifi_scan_get_ap_num(&initial_ap_count);
            if (err == ESP_OK) {
                glog("Initial scan found %u access points\n", initial_ap_count);

                if (initial_ap_count > 0) {
                    // Use ap_scan module to store results
                    ap_scan_stop();
                    
                    // Refresh our pointers after ap_scan_stop
                    ap_scan_get_results(&ap_count, &scanned_aps);

                    glog("--- Known AP BSSIDs for Station Scan ---\n");
                    for (int k = 0; k < ap_count; k++) {
                        char sanitized_ssid[33];
                        char bssid_str[18];
                        sanitize_ssid(scanned_aps[k].ssid, sanitized_ssid, sizeof(sanitized_ssid));
                        format_mac_upper(scanned_aps[k].bssid, bssid_str, sizeof(bssid_str));
                        glog("[%d] BSSID: %s, SSID: %s\n", k, bssid_str, sanitized_ssid);
                    }
                    glog("----------------------------------------\n");
                } else {
                    glog("Initial scan found no access points\n");
                }
            } else {
                glog("Failed to get AP count after initial scan: %s\n", esp_err_to_name(err));
            }
        } else {
            glog("Initial AP scan failed: %s\n", esp_err_to_name(err));
        }

        // Stop STA mode before setting monitor mode
        ESP_ERROR_CHECK(esp_wifi_stop());
    } else {
        glog("Using previously scanned AP list (%d APs).\n", ap_count);
    }

    // Set scan active flag
    scan_active = true;

    // Start monitor mode with management + data frame filtering for station discovery
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA
    };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(wifi_stations_sniffer_callback);

    // Start channel hopping for station scan
    start_scansta_channel_hopping();

    glog("Started Station Scan (Channel Hopping Enabled)...\n");
}

void station_scan_stop(void) {
    if (!scan_active) {
        return;
    }

    scan_active = false;

    // Stop channel hopping
    stop_scansta_channel_hopping();

    // Stop monitor mode
    esp_wifi_set_promiscuous(false);

    glog("Station Scan Stopped. Found %d stations.\n", station_count);
}

void station_scan_print_results(void) {
    if (station_count == 0) {
        glog("No stations found.\n");
        return;
    }

    // Get AP scan results for SSID lookup
    uint16_t ap_count = 0;
    wifi_ap_record_t *scanned_aps = NULL;
    ap_scan_get_results(&ap_count, &scanned_aps);

    scan_file_t sf = SCAN_FILE_INIT;
    bool saving = (scan_file_open(&sf, "station_scan", "txt") == ESP_OK);

    glog("--- Station List (%d entries) ---\n", station_count);
    if (saving) scan_file_printf(&sf, "--- Station List (%d entries) ---\n", station_count);

    for (int i = 0; i < station_count; i++) {
        char sanitized_ssid[33] = "(Unknown AP)";
        
        // Find AP SSID using helper
        if (find_ap_by_bssid(station_ap_list[i].ap_bssid, scanned_aps, ap_count, 
                             sanitized_ssid, sizeof(sanitized_ssid)) < 0) {
            strcpy(sanitized_ssid, "(Unknown AP)");
        }

        // Log station info using helper
        log_station_info(i, station_ap_list[i].station_mac, station_ap_list[i].ap_bssid, 
                         sanitized_ssid, NULL);

        if (saving) {
            char sta_mac_str[18], ap_mac_str[18];
            char sta_vendor[64] = "Unknown", ap_vendor[64] = "Unknown";
            
            format_mac_upper(station_ap_list[i].station_mac, sta_mac_str, sizeof(sta_mac_str));
            format_mac_upper(station_ap_list[i].ap_bssid, ap_mac_str, sizeof(ap_mac_str));
            lookup_vendor_with_default(station_ap_list[i].station_mac, sta_vendor, sizeof(sta_vendor));
            lookup_vendor_with_default(station_ap_list[i].ap_bssid, ap_vendor, sizeof(ap_vendor));
            
            scan_file_printf(&sf, "[%d] STA: %s (%s) -> AP: %s BSSID: %s (%s)\n",
                             i, sta_mac_str, sta_vendor,
                             sanitized_ssid, ap_mac_str, ap_vendor);
        }
    }

    if (saving) scan_file_close(&sf);
}

bool station_scan_is_active(void) {
    return scan_active;
}

int station_scan_get_count(void) {
    return station_count;
}

// ============================================================================
// Selection Operations
// ============================================================================

esp_err_t station_scan_select(int index) {
    if (station_count == 0) {
        glog("No stations found.\n");
        return ESP_ERR_NOT_FOUND;
    }

    if (index < 0 || index >= station_count) {
        glog("Invalid station index: %d. Index should be between 0 and %d\n", index, station_count - 1);
        return ESP_ERR_INVALID_ARG;
    }

    selected_station = station_ap_list[index];

    // Get AP scan results for SSID lookup
    uint16_t ap_count = 0;
    wifi_ap_record_t *scanned_aps = NULL;
    ap_scan_get_results(&ap_count, &scanned_aps);

    char sanitized_ssid[33] = "(Unknown AP)";
    find_ap_by_bssid(selected_station.ap_bssid, scanned_aps, ap_count, 
                     sanitized_ssid, sizeof(sanitized_ssid));

    // Log selection using helper
    char sta_mac_str[18], ap_mac_str[18];
    format_mac_upper(selected_station.station_mac, sta_mac_str, sizeof(sta_mac_str));
    format_mac_upper(selected_station.ap_bssid, ap_mac_str, sizeof(ap_mac_str));
    
    glog("Selected Station %d:\n"
         "     Station MAC: %s,\n"
         "     AP SSID: %s,\n"
         "     AP BSSID: %s\n",
         index, sta_mac_str, sanitized_ssid, ap_mac_str);

    station_selected = true;
    return ESP_OK;
}

esp_err_t station_scan_select_multiple(int *indices, int count) {
    if (station_count == 0) {
        glog("No stations found.\n");
        return ESP_ERR_NOT_FOUND;
    }

    if (indices == NULL || count <= 0) {
        glog("Invalid arguments for multi-select.\n");
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < count; i++) {
        if (indices[i] < 0 || indices[i] >= station_count) {
            glog("Invalid station index: %d. Index should be between 0 and %d\n", indices[i], station_count - 1);
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (selected_stations != NULL) {
        free(selected_stations);
        selected_stations = NULL;
    }

    selected_stations = malloc(count * sizeof(station_ap_pair_t));
    if (selected_stations == NULL) {
        glog("Failed to allocate memory for selected stations\n");
        selected_station_count = 0;
        return ESP_ERR_NO_MEM;
    }

    selected_station_count = count;

    for (int i = 0; i < count; i++) {
        selected_stations[i] = station_ap_list[indices[i]];
    }

    char sta_mac_str[18], ap_mac_str[18];
    glog("Selected %d stations:\n", count);
    for (int i = 0; i < count; i++) {
        format_mac_upper(selected_stations[i].station_mac, sta_mac_str, sizeof(sta_mac_str));
        format_mac_upper(selected_stations[i].ap_bssid, ap_mac_str, sizeof(ap_mac_str));
        glog("  [%d] STA: %s -> AP: %s\n", i, sta_mac_str, ap_mac_str);
    }

    return ESP_OK;
}

void station_scan_get_selected_stations(station_ap_pair_t **stations, int *count) {
    if (stations != NULL) {
        *stations = selected_stations;
    }
    if (count != NULL) {
        *count = selected_station_count;
    }
}

bool station_scan_get_selection(station_ap_pair_t *station) {
    if (!station_selected || station == NULL) {
        return false;
    }
    *station = selected_station;
    return true;
}

bool station_scan_has_selection(void) {
    return station_selected;
}

void station_scan_clear_selection(void) {
    station_selected = false;
    memset(&selected_station, 0, sizeof(selected_station));
    if (selected_stations != NULL) {
        free(selected_stations);
        selected_stations = NULL;
    }
    selected_station_count = 0;
}

void station_scan_clear_results(void) {
    station_count = 0;
    station_selected = false;
    if (selected_stations != NULL) {
        free(selected_stations);
        selected_stations = NULL;
    }
    selected_station_count = 0;
    memset(station_ap_list, 0, sizeof(station_ap_list));
    memset(&selected_station, 0, sizeof(selected_station));
}
