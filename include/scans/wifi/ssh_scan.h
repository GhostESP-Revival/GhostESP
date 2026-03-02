/**
 * @file ssh_scan.h
 * @brief SSH service detection scanning module
 * 
 * This module handles SSH service detection operations including:
 * - Scanning individual hosts for SSH services
 * - Scanning subnets for SSH services
 * - Banner grabbing for SSH identification
 */

#ifndef SSH_SCAN_H
#define SSH_SCAN_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Maximum number of open SSH ports to store per host
 */
#define SSH_SCAN_MAX_PORTS 32

/**
 * @brief SSH scan result for a single host
 */
typedef struct {
    char ip[16];                              ///< Host IP address
    uint16_t open_ports[SSH_SCAN_MAX_PORTS];  ///< Array of open SSH ports
    int num_open_ports;                       ///< Number of open SSH ports found
} ssh_scan_result_t;

/**
 * @brief Scan a specific host for SSH services
 * 
 * Checks common SSH ports (22, 2222, 2022) and attempts to grab banner.
 * 
 * @param target_ip IP address to scan (e.g., "192.168.1.1")
 */
void ssh_scan_host(const char *target_ip);

/**
 * @brief Scan the local subnet for SSH services
 * 
 * Performs a comprehensive scan of the local subnet:
 * 1. Determines the local subnet from WiFi connection
 * 2. Scans each host in the subnet for SSH services
 * 3. Reports hosts with SSH services available
 */
void ssh_scan_subnet(void);

#endif // SSH_SCAN_H
