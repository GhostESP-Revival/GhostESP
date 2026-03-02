/**
 * @file scan_types.h
 * @brief Common scan types and structures
 * 
 * This file contains common types, structures, and enumerations
 * used across all scan modules (WiFi and BLE).
 */

#ifndef SCAN_TYPES_H
#define SCAN_TYPES_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Common scan result structure
 * 
 * Generic scan result that can be used for various scan types.
 * Contains basic information common to all scan results.
 */
typedef struct {
    char identifier[64];      ///< SSID, MAC, device name, etc.
    int8_t rssi;              ///< Signal strength
    uint8_t channel;          ///< Channel number
    bool is_active;           ///< Whether the target is currently active
} scan_result_t;

/**
 * @brief Scan state enumeration
 * 
 * Represents the current state of a scan operation.
 */
typedef enum {
    SCAN_STATE_IDLE,      ///< Scan is not running
    SCAN_STATE_RUNNING,   ///< Scan is currently in progress
    SCAN_STATE_COMPLETE,  ///< Scan has completed successfully
    SCAN_STATE_ERROR      ///< Scan encountered an error
} scan_state_t;

/**
 * @brief Scan configuration structure
 * 
 * Configuration parameters for scan operations.
 */
typedef struct {
    uint32_t timeout_ms;    ///< Scan timeout in milliseconds
    uint8_t channel;        ///< Specific channel to scan (0 for all)
    bool include_hidden;    ///< Whether to include hidden networks/devices
    bool save_results;      ///< Whether to save results to file
} scan_config_t;

#endif // SCAN_TYPES_H