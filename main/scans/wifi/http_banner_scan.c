/**
 * @file http_banner_scan.c
 * @brief HTTP/HTTPS banner grabbing implementation
 *
 * This module handles HTTP banner grabbing operations including:
 * - Scanning hosts for HTTP/HTTPS services and retrieving banners
 * - Subnet-wide HTTP service discovery
 * - Identifying web servers, applications, and frameworks
 */

#include "scans/wifi/http_banner_scan.h"
#include "core/scan_saver.h"
#include "core/glog.h"
#include "core/utils.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"

// Constants
#define HTTP_SCAN_TIMEOUT_MS 1000
#define HTTP_BANNER_BUFFER_SIZE 1024
#define HTTP_MAX_PORTS 5

// Module tag for logging
static const char *TAG = "HttpBannerScan";

// Common HTTP/HTTPS ports
static const uint16_t HTTP_PORTS[] = {80, 8080, 8000, 443, 8443};
static const char *HTTP_PORT_NAMES[] = {"http", "http-alt", "http-alt", "https", "https-alt"};

// Shared cancellation flag for all network scans
static volatile bool g_network_scan_cancel = false;

// ============================================================================
// Cancellation Control
// ============================================================================

void http_banner_scan_cancel(void) {
    g_network_scan_cancel = true;
}

void http_banner_scan_reset_cancel(void) {
    g_network_scan_cancel = false;
}

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Send a simple HTTP probe and read the response banner
 */
static bool grab_http_banner(const char *target_ip, uint16_t port,
                              const char *scheme,
                              char *banner_out, size_t banner_size,
                               scan_file_t *sf) {
    if (g_network_scan_cancel) return false;

    int sock = tcp_connect_with_timeout_cancel(target_ip, port, HTTP_SCAN_TIMEOUT_MS,
                                               &g_network_scan_cancel);
    if (sock < 0) {
        return false;
    }
    if (g_network_scan_cancel) {
        tcp_close_socket(&sock);
        return false;
    }

    // Port is open - send a simple HTTP request
    ESP_LOGI(TAG, "Port %d is OPEN on %s", port, target_ip);

    const char *request =
        "HEAD / HTTP/1.0\r\n"
        "Host: ";
    send(sock, request, strlen(request), 0);
    send(sock, target_ip, strlen(target_ip), 0);
    const char *req_end = "\r\n"
        "User-Agent: Mozilla/5.0 (compatible; GhostESP/1.0)\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n\r\n";
    send(sock, req_end, strlen(req_end), 0);

    // Read response
    int bytes = tcp_recv_with_timeout_cancel(sock, banner_out, banner_size - 1,
                                             HTTP_SCAN_TIMEOUT_MS,
                                             &g_network_scan_cancel);
    tcp_close_socket(&sock);

    if (bytes > 0) {
        banner_out[bytes] = '\0';

        // Truncate at first empty line to keep just headers
        char *end = strstr(banner_out, "\r\n\r\n");
        if (end) *end = '\0';
        end = strstr(banner_out, "\n\n");
        if (end) *end = '\0';

        // Extract Server header
        const char *server = NULL;
        char *line = banner_out;
        while (line) {
            char *next = strchr(line, '\n');
            if (next) *next = '\0';

            if (strncasecmp(line, "Server:", 7) == 0) {
                server = line + 7;
                while (*server == ' ' || *server == '\t') server++;
                break;
            }
            if (strncasecmp(line, "HTTP/", 5) == 0) {
                // Keep status line
            }
            if (next) {
                *next = '\n';
                line = next + 1;
            } else {
                break;
            }
        }

        if (server) {
            glog("[%s:%d] (%s) Server: %s\n", target_ip, port, scheme, server);
        } else {
            glog("[%s:%d] (%s) Response: %.80s...\n", target_ip, port, scheme, banner_out);
        }

        if (sf != NULL) {
            if (server) {
                scan_file_printf(sf, "[%s:%d] (%s) Server: %s\n",
                                 target_ip, port, scheme, server);
            } else {
                scan_file_printf(sf, "[%s:%d] (%s) Response: %s\n",
                                 target_ip, port, scheme, banner_out);
            }
        }
        return true;
    } else {
        glog("[%s:%d] (%s) Status: OPEN, no banner\n", target_ip, port, scheme);
        if (sf != NULL) {
            scan_file_printf(sf, "[%s:%d] (%s) Status: OPEN, no banner\n",
                             target_ip, port, scheme);
        }
        return true;
    }
}

// ============================================================================
// Public API Implementation
// ============================================================================

/**
 * @brief Scan a specific host for HTTP/HTTPS banners
 */
void http_banner_scan_host(const char *target_ip) {
    if (target_ip == NULL) {
        ESP_LOGE(TAG, "NULL target IP provided");
        return;
    }

    ESP_LOGI(TAG, "Starting HTTP banner scan on host: %s", target_ip);
    glog("HTTP banner scanning host: %s\n", target_ip);

    int open_ports = 0;
    char banner[HTTP_BANNER_BUFFER_SIZE];

    for (int i = 0; i < HTTP_MAX_PORTS && !g_network_scan_cancel; i++) {
        if (grab_http_banner(target_ip, HTTP_PORTS[i], HTTP_PORT_NAMES[i],
                              banner, sizeof(banner), NULL)) {
            open_ports++;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    glog("HTTP banner scan completed on %s - found %d HTTP service(s)\n",
         target_ip, open_ports);
}

/**
 * @brief Scan the local subnet for HTTP/HTTPS services
 */
void http_banner_scan_subnet(void) {
    char subnet_prefix[16];

    if (!get_wifi_subnet_prefix(subnet_prefix, sizeof(subnet_prefix))) {
        glog("HTTP Banner Scan: Failed to get subnet prefix - not connected to WiFi?\n");
        return;
    }

    glog("HTTP Banner Scan: Scanning subnet %s*\n", subnet_prefix);

    scan_file_t sf = SCAN_FILE_INIT;
    bool saving = (scan_file_open(&sf, "http_banner_scan", "txt") == ESP_OK);

    if (saving) {
        scan_file_printf(&sf, "--- HTTP Banner Scan Results (Subnet %s*) ---\n", subnet_prefix);
    }

    int total_hosts_with_http = 0;
    int total_services = 0;
    char banner[HTTP_BANNER_BUFFER_SIZE];
    g_network_scan_cancel = false;

    glog("HTTP Banner Scan: Scanning 254 hosts...\n");

    // Scan all hosts in the subnet (1-254)
    for (int host = 1; host <= 254 && !g_network_scan_cancel; host++) {
        // Progress update every 25 hosts
        if (host % 25 == 0) {
            glog("HTTP Banner Scan: Progress %d/254 hosts\n", host);
        }

        char target_ip[16];
        build_ip_string(target_ip, sizeof(target_ip), subnet_prefix, host);

        int host_services = 0;
        for (int i = 0; i < HTTP_MAX_PORTS && !g_network_scan_cancel; i++) {
            if (grab_http_banner(target_ip, HTTP_PORTS[i], HTTP_PORT_NAMES[i],
                                  banner, sizeof(banner), &sf)) {
                host_services++;
                total_services++;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        if (host_services > 0) {
            total_hosts_with_http++;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (g_network_scan_cancel) {
        glog("HTTP Banner Scan: Cancelled. Found %d hosts with %d HTTP service(s)\n",
             total_hosts_with_http, total_services);
    } else {
        glog("HTTP Banner Scan: Subnet scan complete - found %d hosts with %d HTTP service(s)\n",
             total_hosts_with_http, total_services);
    }

    if (saving) {
        scan_file_printf(&sf, "--- HTTP Banner Scan Summary ---\n");
        scan_file_printf(&sf, "Hosts with HTTP: %d, Total services: %d\n",
                         total_hosts_with_http, total_services);
        scan_file_close(&sf);
    }
}
