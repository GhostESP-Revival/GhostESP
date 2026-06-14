/**
 * @file netbios_scan.h
 * @brief NetBIOS Name Service (NBNS) scanning module
 *
 * This module handles NetBIOS/NBNS scanning operations including:
 * - Broadcasting NetBIOS Name Service queries on the local subnet
 * - Discovering Windows hosts and their NetBIOS names
 * - Scanning individual hosts for NetBIOS information
 */

#ifndef NETBIOS_SCAN_H
#define NETBIOS_SCAN_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Scan the local subnet for NetBIOS hosts
 *
 * Broadcasts NBNS queries across the local subnet to discover
 * Windows hosts and their NetBIOS names.
 */
void netbios_scan_subnet(void);

/**
 * @brief Scan a specific /24 subnet prefix for NetBIOS hosts.
 *
 * @param subnet_prefix Prefix including trailing dot, e.g. "192.168.4."
 */
void netbios_scan_subnet_prefix(const char *subnet_prefix);

/**
 * @brief Scan a specific host for NetBIOS name information
 *
 * Sends an NBNS name query to the target IP address.
 *
 * @param target_ip IP address to scan (e.g., "192.168.1.1")
 */
void netbios_scan_host(const char *target_ip);

/**
 * @brief Cancel an ongoing NetBIOS scan
 */
void netbios_scan_cancel(void);

#endif // NETBIOS_SCAN_H
