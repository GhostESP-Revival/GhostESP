/**
 * @file gatt_scan.h
 * @brief GATT service enumeration scan interface
 * 
 * This module handles BLE scanning for connectable devices and GATT service
 * enumeration including:
 * - Starting and stopping GATT device discovery scans
 * - Managing discovered device storage
 * - Listing and selecting discovered devices
 * - Enumerating GATT services on selected devices
 * - Tracking selected devices via RSSI monitoring
 */

#ifndef GATT_SCAN_H
#define GATT_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Start scanning for connectable BLE devices
 * 
 * Initializes BLE if needed, clears any previous results, and starts
 * a BLE scan with the GATT device detection callback registered.
 */
void gatt_scan_start(void);

/**
 * @brief Stop scanning for BLE devices
 * 
 * Saves any discovered devices to file and unregisters the scan callback.
 */
void gatt_scan_stop(void);

/**
 * @brief Get the count of discovered BLE devices
 * 
 * @return int Number of devices discovered during the current scan
 */
int gatt_scan_get_device_count(void);

/**
 * @brief Print the list of discovered BLE devices
 * 
 * Outputs the MAC address, name, RSSI, and tracker type of each discovered device.
 * Also saves results to a file if SD card is available.
 */
void gatt_scan_print_devices(void);

/**
 * @brief Check if a GATT scan is currently active
 * 
 * @return true if scan is running, false otherwise
 */
bool gatt_scan_is_active(void);

/**
 * @brief Select a device for service enumeration
 * 
 * @param index Index of the device in the discovered list
 */
void gatt_scan_select_device(int index);

/**
 * @brief Enumerate GATT services on the selected device
 * 
 * Connects to the selected device and discovers all GATT services.
 * Reads device information, battery level, and current time if available.
 */
void gatt_scan_enumerate_services(void);

/**
 * @brief Start tracking the selected device
 * 
 * Begins monitoring the selected device's RSSI to help locate it physically.
 */
void gatt_scan_track_device(void);

/**
 * @brief Stop tracking the current device
 */
void gatt_scan_stop_tracking(void);

/**
 * @brief Get data for a discovered device
 * 
 * @param index Index of the device in the discovered list
 * @param mac Output buffer for MAC address (6 bytes)
 * @param rssi Output pointer for RSSI value
 * @param name Output buffer for device name
 * @param name_len Length of name buffer
 * @return int 0 on success, -1 on error
 */
int gatt_scan_get_device_data(int index, uint8_t *mac, int8_t *rssi, char *name, size_t name_len);

#endif // GATT_SCAN_H
