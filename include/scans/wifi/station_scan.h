/**
 * @file station_scan.h
 * @brief WiFi Station (client) scanning module
 * 
 * This module handles WiFi station/client scanning operations including:
 * - Starting and stopping station scans with channel hopping
 * - Managing station scan results storage
 * - Selecting stations for further operations
 * - Printing scan results with vendor OUI lookup
 */

#ifndef STATION_SCAN_H
#define STATION_SCAN_H

#include "esp_err.h"
#include "managers/wifi_manager.h"
#include <stdbool.h>

/**
 * @brief Maximum number of stations to store in scan results
 */
#define STATION_SCAN_MAX_RESULTS MAX_STATIONS

/**
 * @brief Start a station scan
 * 
 * Initiates a station scan in monitor mode with channel hopping.
 * If no APs have been scanned previously, performs an AP scan first
 * to build a list of known AP BSSIDs to match against.
 */
void station_scan_start(void);

/**
 * @brief Stop an active station scan
 * 
 * Stops the station scan, channel hopping timer, and monitor mode.
 */
void station_scan_stop(void);

/**
 * @brief Print the station scan results
 * 
 * Prints all detected stations with their MAC addresses, associated AP SSIDs,
 * and vendor information from OUI lookup. Results are saved to file if SD
 * card is available.
 */
void station_scan_print_results(void);

/**
 * @brief Check if a station scan is currently active
 * 
 * @return true if station scan is running, false otherwise
 */
bool station_scan_is_active(void);

/**
 * @brief Get the number of detected stations
 * 
 * @return Number of stations in the scan results
 */
int station_scan_get_count(void);

/**
 * @brief Select a station by index
 * 
 * Selects a station from the scan results for further operations.
 * 
 * @param index Index of the station in the scan results (0-based)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if index is out of range
 */
esp_err_t station_scan_select(int index);

/**
 * @brief Get the selected station
 * 
 * Returns the currently selected station for operations.
 * 
 * @param station Pointer to store the selected station-AP pair
 * @return true if a station is selected, false otherwise
 */
bool station_scan_get_selection(station_ap_pair_t *station);

/**
 * @brief Check if a station is currently selected
 * 
 * @return true if a station is selected, false otherwise
 */
bool station_scan_has_selection(void);

/**
 * @brief Clear station selection
 * 
 * Clears the currently selected station.
 */
void station_scan_clear_selection(void);

/**
 * @brief Select multiple stations by indices
 * 
 * Selects multiple stations from the scan results for bulk operations.
 * 
 * @param indices Array of indices to select
 * @param count Number of indices in the array
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if any index is invalid
 */
esp_err_t station_scan_select_multiple(int *indices, int count);

/**
 * @brief Get the selected stations
 * 
 * Returns the currently selected stations array and count.
 * 
 * @param stations Pointer to store the selected stations array pointer
 * @param count Pointer to store the number of selected stations
 */
void station_scan_get_selected_stations(station_ap_pair_t **stations, int *count);

/**
 * @brief Clear all station scan results
 * 
 * Clears the station list and resets the scan state.
 */
void station_scan_clear_results(void);

/**
 * @brief External access to scan results (for legacy compatibility)
 * 
 * These variables provide direct access to the scan results for code
 * that hasn't been updated to use the accessor functions.
 */
extern station_ap_pair_t station_ap_list[STATION_SCAN_MAX_RESULTS];
extern int station_count;

/**
 * @brief External access to selected station (for legacy compatibility)
 */
extern station_ap_pair_t selected_station;
extern bool station_selected;

#endif // STATION_SCAN_H
