/**
 * @file arp_scan.h
 * @brief ARP network scanning interface
 * 
 * This module handles ARP-based network scanning operations including:
 * - Scanning subnets for active hosts
 * - Resolving MAC addresses from IP addresses
 * - Managing ARP scan results
 */

#ifndef ARP_SCAN_H
#define ARP_SCAN_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * @brief Structure representing a discovered ARP host
 */
typedef struct {
    char ip[16];          ///< IP address string
    uint8_t mac[6];       ///< MAC address bytes
    bool is_active;       ///< Host active status
} arp_host_t;

/**
 * @brief Structure representing ARP scanner context
 */
typedef struct {
    char subnet_prefix[16];   ///< Subnet prefix (e.g., "192.168.1.")
    arp_host_t *hosts;        ///< Array of discovered hosts
    size_t max_hosts;         ///< Maximum number of hosts
    size_t num_active_hosts;  ///< Number of active hosts found
} arp_scanner_ctx_t;

/**
 * @brief Initialize ARP scanner context
 * 
 * @return arp_scanner_ctx_t* Pointer to initialized context, or NULL on failure
 */
arp_scanner_ctx_t *arp_scanner_init(void);

/**
 * @brief Clean up ARP scanner context
 * 
 * @param ctx Context to clean up
 */
void arp_scanner_cleanup(arp_scanner_ctx_t *ctx);

/**
 * @brief Scan subnet for active hosts using ARP
 * 
 * @return true on success, false on failure
 */
bool arp_scan_subnet(void);

/**
 * @brief Print ARP scan results
 */
void arp_scan_print_results(void);

/**
 * @brief Send ARP request to target IP
 * 
 * @param target_ip Target IP address string
 * @return true if ARP request sent successfully, false otherwise
 */
bool send_arp_request(const char *target_ip);

/**
 * @brief Get ARP table entry for IP address
 * 
 * @param ip IP address string
 * @param mac Buffer to store MAC address (6 bytes)
 * @return true if entry found, false otherwise
 */
bool get_arp_table_entry(const char *ip, uint8_t *mac);

#endif // ARP_SCAN_H
