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
#include "esp_err.h"

#ifdef CONFIG_SPIRAM
#define ARP_SCAN_MAX_RESULTS 256
#else
#define ARP_SCAN_MAX_RESULTS 64
#endif

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
    uint32_t scan_first;      ///< First host IP (network + 1)
    uint32_t scan_last;       ///< Last host IP (broadcast - 1)
    int total_hosts;          ///< Number of IPs in the scan range
    arp_host_t *hosts;        ///< Array of discovered hosts
    size_t max_hosts;         ///< Maximum number of hosts
    size_t num_active_hosts;  ///< Number of active hosts found
} arp_scanner_ctx_t;

arp_scanner_ctx_t *arp_scanner_init(void);
void arp_scanner_cleanup(arp_scanner_ctx_t *ctx);
bool arp_scan_subnet(void);
void arp_scan_print_results(void);
bool send_arp_request(const char *target_ip);
bool get_arp_table_entry(const char *ip, uint8_t *mac);

esp_err_t arp_scan_start_async(void);
bool arp_scan_check_done(void);
void arp_scan_finish_async(void);
bool arp_scan_is_running(void);
void arp_scan_cancel(void);

int arp_scan_get_count(void);
const arp_host_t* arp_scan_get_host(int index);
void arp_scan_clear_results(void);
void arp_scan_get_progress(int *pass, int *total_passes, int *scanned, int *total_hosts, int *found);

const char *arp_scan_get_vendor(const uint8_t *mac);

// Compact Wi-Fi packet monitor (legacy API names retained for CLI compatibility)
void arp_scan_start_passive(int duration_sec);
void arp_scan_stop_passive(void);

#endif // ARP_SCAN_H
