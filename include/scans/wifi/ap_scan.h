/**
 * @file ap_scan.h
 * @brief WiFi Access Point scanning module
 * 
 * This module handles standard WiFi AP scanning operations including:
 * - Starting and stopping AP scans
 * - Managing scan results storage
 * - Selecting APs for further operations
 * - Printing scan results with vendor OUI lookup
 */

#ifndef AP_SCAN_H
#define AP_SCAN_H

#include "esp_err.h"
#include "esp_wifi.h"
#include <stdbool.h>

/**
 * @brief Maximum number of APs to store in scan results
 */
#define AP_SCAN_MAX_RESULTS 100

/**
 * @brief Start a WiFi AP scan
 * 
 * Initiates a WiFi scan for access points. The scan runs synchronously
 * and blocks until complete. Results are stored internally.
 */
void ap_scan_start(void);

/**
 * @brief Stop an active AP scan
 * 
 * Stops any active WiFi scan and retrieves the results.
 * This is called automatically after ap_scan_start() completes.
 */
void ap_scan_stop(void);

/**
 * @brief Get the AP scan results
 * 
 * Returns the count and pointer to the scanned AP records.
 * The data remains valid until ap_scan_clear_results() is called.
 * 
 * @param count Pointer to store the number of APs found
 * @param aps Pointer to store the AP records array pointer
 */
void ap_scan_get_results(uint16_t *count, wifi_ap_record_t **aps);

/**
 * @brief Clear the AP scan results
 * 
 * Frees the memory used for storing scan results.
 * Should be called when results are no longer needed.
 */
void ap_scan_clear_results(void);

/**
 * @brief Print scan results with OUI vendor lookup
 * 
 * Prints all scanned APs with their SSID, BSSID, RSSI, channel,
 * security information, and vendor (from OUI database).
 * Results are also saved to file if SD card is available.
 */
void ap_scan_print_results(void);

/**
 * @brief Select an AP by index
 * 
 * Selects a single AP from the scan results for further operations.
 * 
 * @param index Index of the AP in the scan results (0-based)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if index is out of range
 */
esp_err_t ap_scan_select(int index);

/**
 * @brief Select multiple APs by indices
 * 
 * Selects multiple APs from the scan results for bulk operations.
 * 
 * @param indices Array of indices to select
 * @param count Number of indices in the array
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if any index is invalid
 */
esp_err_t ap_scan_select_multiple(int *indices, int count);

/**
 * @brief Get the selected APs
 * 
 * Returns the currently selected APs for operations.
 * 
 * @param aps Pointer to store the selected APs array pointer
 * @param count Pointer to store the number of selected APs
 */
void ap_scan_get_selected(wifi_ap_record_t **aps, int *count);

/**
 * @brief Get the single selected AP
 * 
 * Returns the single selected AP (from ap_scan_select).
 * 
 * @param ap Pointer to store the selected AP record
 * @return true if an AP is selected, false otherwise
 */
bool ap_scan_get_selection(wifi_ap_record_t *ap);

/**
 * @brief Get the number of scanned APs
 * 
 * @return Number of APs in the scan results
 */
uint16_t ap_scan_get_count(void);

/**
 * @brief Check if an AP is currently selected
 * 
 * @return true if an AP is selected, false otherwise
 */
bool ap_scan_has_selection(void);

/**
 * @brief Clear AP selection
 * 
 * Clears both single and multiple AP selections.
 */
void ap_scan_clear_selection(void);

/**
 * @brief External access to scan results (for legacy compatibility)
 * 
 * These variables provide direct access to the scan results for code
 * that hasn't been updated to use the accessor functions.
 */
extern wifi_ap_record_t *scanned_aps;
extern uint16_t ap_count;

#endif // AP_SCAN_H