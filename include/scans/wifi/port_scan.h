/**
 * @file port_scan.h
 * @brief TCP/UDP Port scanning module
 * 
 * This module handles TCP and UDP port scanning operations including:
 * - Subnet-wide port scanning
 * - Individual host port scanning
 * - TCP port range scanning
 * - UDP port scanning
 * - SSH service detection
 */

#ifndef PORT_SCAN_H
#define PORT_SCAN_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * @brief Maximum number of open ports to store per host
 */
#define PORT_SCAN_MAX_OPEN_PORTS 64

/**
 * @brief Port scan result for a single host
 */
typedef struct {
    char ip[16];                              ///< Host IP address
    uint16_t open_ports[PORT_SCAN_MAX_OPEN_PORTS]; ///< Array of open ports
    uint8_t num_open_ports;                   ///< Number of open ports found
} port_scan_result_t;

/**
 * @brief Scanner context for subnet scanning
 */
typedef struct {
    char subnet_prefix[16];                   ///< Subnet prefix (e.g., "192.168.1.")
    size_t num_active_hosts;                  ///< Number of active hosts found
} port_scanner_ctx_t;

/**
 * @brief Scan the local subnet for open ports
 * 
 * Performs a comprehensive scan of the local subnet:
 * 1. Determines the local subnet from WiFi connection
 * 2. Scans each host in the subnet for common TCP ports
 * 3. Scans each host for common UDP ports
 * 4. Reports open ports and identifies possible services
 * 
 * @return true on success, false on failure
 */
bool port_scan_subnet(void);

/**
 * @brief Scan a specific IP address for open TCP ports
 * 
 * Scans a range of TCP ports on the target IP address.
 * 
 * @param target_ip IP address to scan (e.g., "192.168.1.1")
 * @param start_port First port in the range to scan
 * @param end_port Last port in the range to scan (inclusive)
 * @return true on success, false on failure
 */
bool port_scan_ip_range(const char *target_ip, uint16_t start_port, uint16_t end_port);

/**
 * @brief Scan a specific IP address for open UDP ports
 * 
 * Scans a range of UDP ports on the target IP address.
 * 
 * @param target_ip IP address to scan (e.g., "192.168.1.1")
 * @param start_port First port in the range to scan
 * @param end_port Last port in the range to scan (inclusive)
 * @return true on success, false on failure
 */
bool port_scan_udp_ip_range(const char *target_ip, uint16_t start_port, uint16_t end_port);

/**
 * @brief Initialize a port scanner context
 * 
 * Allocates and initializes a scanner context for port scanning.
 * 
 * @return Pointer to initialized context, or NULL on failure
 */
port_scanner_ctx_t *port_scanner_init(void);

/**
 * @brief Clean up a port scanner context
 * 
 * Frees all resources associated with the scanner context.
 * 
 * @param ctx Context to clean up
 */
void port_scanner_cleanup(port_scanner_ctx_t *ctx);

/**
 * @brief Get subnet prefix from current WiFi connection
 * 
 * Determines the subnet prefix from the current WiFi connection.
 * 
 * @param ctx Context to store subnet prefix in
 * @return true on success, false on failure
 */
bool port_scan_get_subnet_prefix(port_scanner_ctx_t *ctx);

/**
 * @brief Check if a host is active (responds to ping)
 * 
 * @param ip_addr IP address to check
 * @return true if host is active, false otherwise
 */
bool port_scan_is_host_active(const char *ip_addr);

/**
 * @brief Scan common TCP ports on a host
 * 
 * Scans the predefined list of common TCP ports on the target.
 * 
 * @param target_ip IP address to scan
 * @param result Pointer to store scan results
 */
void port_scan_scan_tcp_ports(const char *target_ip, port_scan_result_t *result);

/**
 * @brief Scan common UDP ports on a host
 * 
 * Scans the predefined list of common UDP ports on the target.
 * 
 * @param target_ip IP address to scan
 * @param result Pointer to store scan results
 */
void port_scan_scan_udp_ports(const char *target_ip, port_scan_result_t *result);

/**
 * @brief Scan for SSH service on a host
 * 
 * Checks common SSH ports (22, 2222, 2022) and attempts to grab banner.
 * 
 * @param target_ip IP address to scan
 * @param result Pointer to store scan results
 */
void port_scan_ssh(const char *target_ip, port_scan_result_t *result);

/**
 * @brief Calculate IP checksum
 * 
 * Calculates the IP header checksum for raw packet construction.
 * 
 * @param addr Pointer to data
 * @param len Length of data in bytes
 * @return Calculated checksum
 */
uint16_t port_scan_calculate_checksum(uint16_t *addr, int len);

/**
 * @brief Start an async subnet scan in a separate task
 * 
 * This keeps the CLI responsive during the scan. Use port_scan_cancel()
 * to stop the scan and port_scan_is_running() to check status.
 */
void port_scan_subnet_async(void);

/**
 * @brief Cancel an ongoing port scan
 */
void port_scan_cancel(void);

/**
 * @brief Check if a port scan is in progress
 * 
 * @return true if scan is running, false otherwise
 */
bool port_scan_is_running(void);

#endif // PORT_SCAN_H