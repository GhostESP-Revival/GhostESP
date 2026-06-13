/**
 * @file http_banner_scan.h
 * @brief HTTP/HTTPS banner grabbing module
 *
 * This module handles HTTP banner grabbing operations including:
 * - Scanning hosts for HTTP/HTTPS services and retrieving banners
 * - Subnet-wide HTTP service discovery
 * - Identifying web servers, applications, and frameworks
 */

#ifndef HTTP_BANNER_SCAN_H
#define HTTP_BANNER_SCAN_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Scan the local subnet for HTTP/HTTPS services and grab banners
 *
 * Probes common HTTP ports (80, 8080, 8000, 443, 8443) on each host
 * and attempts to read the Server header and response banners.
 */
void http_banner_scan_subnet(void);

/**
 * @brief Scan a specific /24 subnet prefix for HTTP/HTTPS services.
 *
 * @param subnet_prefix Prefix including trailing dot, e.g. "192.168.4."
 */
void http_banner_scan_subnet_prefix(const char *subnet_prefix);

/**
 * @brief Scan a specific host for HTTP/HTTPS banners
 *
 * @param target_ip IP address to scan
 */
void http_banner_scan_host(const char *target_ip);

/**
 * @brief Cancel an ongoing HTTP banner scan
 */
void http_banner_scan_cancel(void);

#endif // HTTP_BANNER_SCAN_H
