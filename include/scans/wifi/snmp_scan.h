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
#include <stddef.h>

#define SNMP_COMMUNITIES_DEFAULT_PATH "/mnt/ghostesp/snmp_communities.txt"

/**
 * @brief Override the community string list at runtime
 *
 * @param csv Comma/semicolon/space separated community strings
 */
void snmp_scan_set_communities(const char *csv);

/**
 * @brief Load community strings from a file (one per line, '#' comments)
 *
 * @param path File path; use SNMP_COMMUNITIES_DEFAULT_PATH for the SD default
 * @return true if at least one community was loaded
 */
bool snmp_scan_load_communities_file(const char *path);

/**
 * @brief Scan the local subnet for SNMP services
 *
 * Probes UDP port 161 on each host with common community strings
 * and attempts to retrieve sysDescr.
 */
void snmp_scan_subnet(void);

/**
 * @brief Scan a specific /24 subnet prefix for SNMP services.
 *
 * @param subnet_prefix Prefix including trailing dot, e.g. "192.168.4."
 */
void snmp_scan_subnet_prefix(const char *subnet_prefix);

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

/**
 * @brief Walk SNMP OID tree on a single host
 *
 * Sends GetNextRequest repeatedly to walk the MIB subtree
 * starting from the given root OID.
 *
 * @param target_ip IP address to walk
 * @param oid_root Root OID string (e.g., "1.3.6.1.2.1.1" for system)
 */
void snmp_walk_host(const char *target_ip, const char *oid_root);

/**
 * @brief Walk SNMP OID tree on the local subnet
 *
 * @param oid_root Root OID string to walk
 */
void snmp_walk_subnet(const char *oid_root);

/**
 * @brief Walk SNMP OID tree on a specific subnet prefix
 *
 * @param subnet_prefix Subnet prefix (e.g., "192.168.1.")
 * @param oid_root Root OID string to walk
 */
void snmp_walk_subnet_prefix(const char *subnet_prefix, const char *oid_root);

#endif // SNMP_SCAN_H
