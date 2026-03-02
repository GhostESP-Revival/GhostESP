/**
 * @file flipper_scan.h
 * @brief Flipper Zero device detection scan interface
 * 
 * This module handles BLE scanning for Flipper Zero devices including:
 * - Starting and stopping Flipper detection scans
 * - Managing discovered device storage
 * - Listing and selecting discovered Flippers
 * - Tracking selected Flipper devices
 */

#ifndef FLIPPER_SCAN_H
#define FLIPPER_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Start scanning for Flipper Zero devices
 * 
 * Initializes BLE if needed, clears any previous results, and starts
 * a BLE scan with the Flipper detection callback registered.
 */
void flipper_scan_start(void);

/**
 * @brief Stop scanning for Flipper Zero devices
 * 
 * Saves any discovered Flippers to file and unregisters the scan callback.
 */
void flipper_scan_stop(void);

/**
 * @brief Get the count of discovered Flipper devices
 * 
 * @return int Number of Flippers discovered during the current scan
 */
int flipper_scan_get_count(void);

/**
 * @brief Print the list of discovered Flipper devices
 * 
 * Outputs the MAC address, name, and RSSI of each discovered Flipper.
 * Also saves results to a file if SD card is available.
 */
void flipper_scan_print_results(void);

/**
 * @brief Check if a Flipper scan is currently active
 * 
 * @return true if scan is running, false otherwise
 */
bool flipper_scan_is_active(void);

/**
 * @brief Get data for a specific discovered Flipper
 * 
 * @param index Index of the Flipper in the discovered list
 * @param mac Output buffer for MAC address (6 bytes)
 * @param rssi Output pointer for RSSI value
 * @param name Output buffer for device name
 * @param name_len Length of name output buffer
 * @return int 0 on success, -1 on invalid index
 */
int flipper_scan_get_device_data(int index, uint8_t *mac, int8_t *rssi, char *name, size_t name_len);

/**
 * @brief Select a Flipper for tracking
 * 
 * @param index Index of the Flipper to select
 */
void flipper_scan_select(int index);

/**
 * @brief Start tracking the currently selected Flipper
 * 
 * Begins continuous monitoring of the selected Flipper's RSSI
 * to track its proximity.
 */
void flipper_scan_start_tracking(void);

#endif // FLIPPER_SCAN_H