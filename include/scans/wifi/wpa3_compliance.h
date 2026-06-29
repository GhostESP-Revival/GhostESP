/**
 * @file wpa3_compliance.h
 * @brief WPA3 compliance checker for scanned access points
 *
 * Evaluates a WiFi AP record against the WPA3 Personal/Enterprise
 * security requirements (SAE, PMF, transition mode) and reports a
 * short compliance summary suitable for both the CLI and the on-device
 * display.
 */

#ifndef WPA3_COMPLIANCE_H
#define WPA3_COMPLIANCE_H

#include "esp_wifi.h"
#include <stdbool.h>

/**
 * @brief Check WPA3 compliance for a single AP record and print the report.
 *
 * Prints a multi-line report describing whether WPA3 is in use, whether
 * the network is in transition (mixed WPA2/WPA3) mode, the PMF
 * (Protected Management Frames) posture, and a short finding summarising
 * the most important security observation.
 *
 * @param ap Pointer to a populated wifi_ap_record_t. Must not be NULL.
 */
void wpa3_compliance_check_ap(const wifi_ap_record_t *ap);

/**
 * @brief Check WPA3 compliance for the currently selected AP.
 *
 * Convenience wrapper that pulls the active selection from the AP scan
 * module and runs the compliance check on it. If no AP is currently
 * selected, falls back to wpa3_compliance_check_all() so the feature
 * is still useful when invoked directly.
 */
void wpa3_compliance_check_selected(void);

/**
 * @brief Check WPA3 compliance for every AP in the current scan cache.
 *
 * If the scan cache is empty, runs a blocking AP scan first via
 * ap_scan_start() and then reports on the results. Each AP is printed
 * as a single summary line in a compact table.
 */
void wpa3_compliance_check_all(void);

#endif // WPA3_COMPLIANCE_H
