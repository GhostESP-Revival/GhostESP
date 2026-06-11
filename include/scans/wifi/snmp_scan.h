/**
 * @file snmp_scan.h
 * @brief SNMP probing and enumeration module
 *
 * This module handles SNMP probing operations including:
 * - Scanning hosts for SNMP v1/v2c services
 * - Testing common community strings (public, private)
 * - Retrieving sysDescr and basic system information
 */

#ifndef SNMP_SCAN_H
#define SNMP_SCAN_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Scan the local subnet for SNMP services
 *
 * Probes UDP port 161 on each host with common community strings
 * and attempts to retrieve sysDescr.
 */
void snmp_scan_subnet(void);

/**
 * @brief Scan a specific host for SNMP services
 *
 * @param target_ip IP address to scan
 */
void snmp_scan_host(const char *target_ip);

/**
 * @brief Cancel an ongoing SNMP probe
 */
void snmp_scan_cancel(void);

#endif // SNMP_SCAN_H
