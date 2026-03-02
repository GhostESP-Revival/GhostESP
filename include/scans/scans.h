/**
 * @file scans.h
 * @brief Master header for all scan modules
 * 
 * This header includes all scan module headers for convenience.
 * Individual headers can also be included directly as needed.
 */

#ifndef SCANS_H
#define SCANS_H

// WiFi Scans
#include "scans/wifi/ap_scan.h"
#include "scans/wifi/station_scan.h"
#include "scans/wifi/port_scan.h"
#include "scans/wifi/arp_scan.h"
#include "scans/wifi/ssh_scan.h"

// BLE Scans
#include "scans/ble/flipper_scan.h"
#include "scans/ble/airtag_scan.h"
#include "scans/ble/gatt_scan.h"

// Common
#include "scans/common/scan_types.h"

#endif // SCANS_H