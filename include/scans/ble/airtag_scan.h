/**
 * @file airtag_scan.h
 * @brief Apple AirTag device detection scan interface
 * 
 * This module handles BLE scanning for Apple AirTag devices including:
 * - Starting and stopping AirTag detection scans
 * - Managing discovered device storage
 * - Listing and selecting discovered AirTags
 * - Spoofing selected AirTag devices
 */

#ifndef AIRTAG_SCAN_H
#define AIRTAG_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Start scanning for Apple AirTag devices
 * 
 * Initializes BLE if needed, clears any previous results, and starts
 * a BLE scan with the AirTag detection callback registered.
 */
void airtag_scan_start(void);

/**
 * @brief Stop scanning for Apple AirTag devices
 * 
 * Saves any discovered AirTags to file and unregisters the scan callback.
 */
void airtag_scan_stop(void);

/**
 * @brief Get the count of discovered AirTag devices
 * 
 * @return int Number of AirTags discovered during the current scan
 */
int airtag_scan_get_count(void);

/**
 * @brief Print the list of discovered AirTag devices
 * 
 * Outputs the MAC address and RSSI of each discovered AirTag.
 * Also saves results to a file if SD card is available.
 */
void airtag_scan_print_results(void);

/**
 * @brief Check if an AirTag scan is currently active
 * 
 * @return true if scan is running, false otherwise
 */
bool airtag_scan_is_active(void);

/**
 * @brief Select an AirTag for spoofing
 * 
 * @param index Index of the AirTag in the discovered list
 */
void airtag_scan_select(int index);

/**
 * @brief Start spoofing the currently selected AirTag
 * 
 * Begins advertising using the selected AirTag's MAC address and
 * manufacturer data to mimic the device.
 */
void airtag_scan_start_spoofing(void);

/**
 * @brief Stop spoofing the current AirTag
 * 
 * Stops the advertising that was started for AirTag spoofing.
 */
void airtag_scan_stop_spoofing(void);

#endif // AIRTAG_SCAN_H