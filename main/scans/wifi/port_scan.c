/**
 * @file port_scan.c
 * @brief TCP/UDP Port scanning implementation
 * 
 * This module handles TCP and UDP port scanning operations including:
 * - Subnet-wide port scanning
 * - Individual host port scanning
 * - TCP port range scanning
 * - UDP port scanning
 * - SSH service detection
 */

#include "scans/wifi/port_scan.h"
#include "core/network_constants.h"
#include "core/glog.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/icmp.h"
#include "lwip/ip4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "freertos/task.h"

// Constants
#define START_HOST 1
#define END_HOST 254
#define SCAN_TIMEOUT_MS 100
#define HOST_TIMEOUT_MS 100

// Module tag for logging
static const char *TAG = "PortScan";

// Cancellation flag for async scans
static volatile bool g_port_scan_cancel = false;

// Task handle for async scan
static TaskHandle_t g_port_scan_task_handle = NULL;

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Calculate IP checksum
 * 
 * Calculates the IP header checksum for raw packet construction.
 */
uint16_t port_scan_calculate_checksum(uint16_t *addr, int len) {
    int nleft = len;
    uint32_t sum = 0;
    uint16_t *w = addr;
    uint16_t answer = 0;

    while (nleft > 1) {
        sum += *w++;
        nleft -= 2;
    }

    if (nleft == 1) {
        *(unsigned char *)(&answer) = *(unsigned char *)w;
        sum += answer;
    }

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    answer = ~sum;
    return answer;
}

// ============================================================================
// Context Management Functions
// ============================================================================

/**
 * @brief Initialize a port scanner context
 */
port_scanner_ctx_t *port_scanner_init(void) {
    port_scanner_ctx_t *ctx = malloc(sizeof(port_scanner_ctx_t));
    if (!ctx) {
        return NULL;
    }

    ctx->num_active_hosts = 0;
    memset(ctx->subnet_prefix, 0, sizeof(ctx->subnet_prefix));

    return ctx;
}

/**
 * @brief Clean up a port scanner context
 */
void port_scanner_cleanup(port_scanner_ctx_t *ctx) {
    if (ctx) {
        free(ctx);
    }
}

// ============================================================================
// Network Utility Functions
// ============================================================================

/**
 * @brief Get subnet prefix from current WiFi connection
 */
bool port_scan_get_subnet_prefix(port_scanner_ctx_t *ctx) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        glog("Failed to get WiFi interface\n");
        return false;
    }

    // Check if WiFi is connected
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        glog("WiFi is not connected\n");
        return false;
    }

    // Get IP info
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        glog("Failed to get IP info\n");
        return false;
    }

    uint32_t network = ip_info.ip.addr & ip_info.netmask.addr;
    struct in_addr network_addr;
    network_addr.s_addr = network;

    char *network_str = inet_ntoa(network_addr);
    char *last_dot = strrchr(network_str, '.');
    if (last_dot == NULL) {
        glog("Invalid network address format\n");
        return false;
    }

    size_t prefix_len = last_dot - network_str + 1;
    memcpy(ctx->subnet_prefix, network_str, prefix_len);
    ctx->subnet_prefix[prefix_len] = '\0';

    glog("Determined subnet prefix: %s\n", ctx->subnet_prefix);
    return true;
}

/**
 * @brief Check if a host is active (responds to ping)
 */
bool port_scan_is_host_active(const char *ip_addr) {
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        return false;
    }

    struct timeval tv;
    tv.tv_sec = HOST_TIMEOUT_MS / 1000;
    tv.tv_usec = (HOST_TIMEOUT_MS % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    inet_pton(AF_INET, ip_addr, &dest_addr.sin_addr.s_addr);

    // Build ICMP echo request
    uint8_t packet[64];
    memset(packet, 0, sizeof(packet));
    struct icmp_echo_hdr *icmp = (struct icmp_echo_hdr *)packet;
    icmp->type = ICMP_ECHO;
    icmp->code = 0;
    icmp->chksum = 0;
    icmp->id = htons((uint16_t)esp_random());
    icmp->seqno = htons(1);
    icmp->chksum = port_scan_calculate_checksum((uint16_t *)packet, sizeof(packet));

    sendto(sock, packet, sizeof(packet), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));

    uint8_t buf[128];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);

    close(sock);
    return n > 0;
}

// ============================================================================
// Port Scanning Functions
// ============================================================================

/**
 * @brief Scan common TCP ports on a host
 */
void port_scan_scan_tcp_ports(const char *target_ip, port_scan_result_t *result) {
    struct sockaddr_in server_addr;
    int sock;
    int scan_result;
    struct timeval timeout;
    fd_set fdset;
    int flags;

    strcpy(result->ip, target_ip);
    result->num_open_ports = 0;

    server_addr.sin_family = AF_INET;
    inet_pton(AF_INET, target_ip, &server_addr.sin_addr.s_addr);

    glog("Scanning TCP ports on %s...\n", target_ip);

    for (size_t i = 0; i < NUM_PORTS; i++) {
        if (result->num_open_ports >= PORT_SCAN_MAX_OPEN_PORTS)
            break;

        uint16_t port = COMMON_PORTS[i];
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0)
            continue;

        flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        server_addr.sin_port = htons(port);
        scan_result = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));

        if (scan_result < 0 && errno == EINPROGRESS) {
            timeout.tv_sec = SCAN_TIMEOUT_MS / 1000;
            timeout.tv_usec = (SCAN_TIMEOUT_MS % 1000) * 1000;

            FD_ZERO(&fdset);
            FD_SET(sock, &fdset);

            scan_result = select(sock + 1, NULL, &fdset, NULL, &timeout);

            if (scan_result > 0) {
                int error = 0;
                socklen_t len = sizeof(error);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) >= 0 && error == 0) {
                    result->open_ports[result->num_open_ports++] = port;
                    glog("  Port %d: OPEN\n", port);
                }
            }
        }

        close(sock);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief Build a UDP probe packet for specific ports
 */
static size_t build_udp_probe(uint16_t port, uint8_t *buf, size_t bufsize) {
    if (port == 53 && bufsize >= 64) {
        // DNS query
        uint8_t *p = buf;
        uint16_t id = (uint16_t)esp_random();
        *(uint16_t *)(p + 0) = htons(id);
        *(uint16_t *)(p + 2) = htons(0x0100);
        *(uint16_t *)(p + 4) = htons(1);
        *(uint16_t *)(p + 6) = 0;
        *(uint16_t *)(p + 8) = 0;
        *(uint16_t *)(p + 10) = 0;
        p += 12;
        const char *name = "example.com";
        const char *dot = name;
        while (*dot) {
            const char *start = dot;
            while (*dot && *dot != '.') dot++;
            size_t len = (size_t)(dot - start);
            *p++ = (uint8_t)len;
            memcpy(p, start, len);
            p += len;
            if (*dot == '.') dot++;
        }
        *p++ = 0;
        *(uint16_t *)p = htons(1);
        p += 2;
        *(uint16_t *)p = htons(1);
        p += 2;
        return (size_t)(p - buf);
    }
    if (port == 123 && bufsize >= 48) {
        // NTP request
        memset(buf, 0, 48);
        buf[0] = 0x1b;
        return 48;
    }
    if (port == 69 && bufsize >= 64) {
        // TFTP request
        uint8_t *p = buf;
        *(uint16_t *)p = htons(1);
        p += 2;
        const char *fname = "test";
        memcpy(p, fname, strlen(fname));
        p += strlen(fname);
        *p++ = 0;
        const char *mode = "octet";
        memcpy(p, mode, strlen(mode));
        p += strlen(mode);
        *p++ = 0;
        return (size_t)(p - buf);
    }
    if (port == 1900 && bufsize >= 256) {
        // SSDP M-SEARCH
        const char *msearch = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: ssdp:all\r\n\r\n";
        size_t len = strlen(msearch);
        memcpy(buf, msearch, len);
        return len;
    }
    if (bufsize >= 1) {
        buf[0] = 0x00;
        return 1;
    }
    return 0;
}

/**
 * @brief Check if a UDP port is open
 */
static bool udp_port_is_open(const char *target_ip, uint16_t port, uint32_t wait_ms) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return false;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, target_ip, &addr.sin_addr.s_addr);

    struct timeval tv;
    tv.tv_sec = wait_ms / 1000;
    tv.tv_usec = (wait_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t probe[256];
    size_t probe_len = build_udp_probe(port, probe, sizeof(probe));
    if (probe_len == 0) {
        close(sock);
        return false;
    }
    sendto(sock, probe, probe_len, 0, (struct sockaddr *)&addr, sizeof(addr));

    uint8_t buf[512];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
    if (n > 0) {
        close(sock);
        return true;
    }
    int err = errno;
    close(sock);
    if (err == ECONNREFUSED) return false;
    return false;
}

/**
 * @brief Scan common UDP ports on a host
 */
void port_scan_scan_udp_ports(const char *target_ip, port_scan_result_t *result) {
    strcpy(result->ip, target_ip);
    result->num_open_ports = 0;

    glog("Scanning UDP ports on %s...\n", target_ip);

    for (size_t i = 0; i < NUM_UDP_PORTS; i++) {
        if (result->num_open_ports >= PORT_SCAN_MAX_OPEN_PORTS) break;
        uint16_t port = UDP_COMMON_PORTS[i];
        if (udp_port_is_open(target_ip, port, 40)) {
            result->open_ports[result->num_open_ports++] = port;
            glog("  UDP %d: OPEN\n", port);
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

/**
 * @brief Scan for SSH service on a host
 */
void port_scan_ssh(const char *target_ip, port_scan_result_t *result) {
    struct sockaddr_in server_addr;
    int sock;
    int scan_result;
    struct timeval timeout;
    fd_set fdset;
    int flags;
    char banner[256];
    ssize_t bytes_read;
    
    ESP_LOGI(TAG, "Starting SSH scan on host: %s", target_ip);
    
    strcpy(result->ip, target_ip);
    result->num_open_ports = 0;
    
    server_addr.sin_family = AF_INET;
    inet_pton(AF_INET, target_ip, &server_addr.sin_addr.s_addr);
    
    glog("SSH scanning host: %s\n", target_ip);
    
    uint16_t ssh_ports[] = {22, 2222, 2022};
    size_t num_ssh_ports = sizeof(ssh_ports) / sizeof(ssh_ports[0]);
    
    for (size_t i = 0; i < num_ssh_ports; i++) {
        if (result->num_open_ports >= PORT_SCAN_MAX_OPEN_PORTS)
            break;
            
        uint16_t port = ssh_ports[i];
        ESP_LOGI(TAG, "Testing port %d on %s", port, target_ip);
        glog("  Testing SSH port %d...", port);
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0) {
            ESP_LOGE(TAG, "Failed to create socket for port %d: errno=%d", port, errno);
            glog(" FAILED\n");
            continue;
        }
            
        flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        
        server_addr.sin_port = htons(port);
        ESP_LOGD(TAG, "Attempting connection to %s:%d", target_ip, port);
        scan_result = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
        
        if (scan_result < 0 && errno == EINPROGRESS) {
            timeout.tv_sec = 3;
            timeout.tv_usec = 0;
            
            FD_ZERO(&fdset);
            FD_SET(sock, &fdset);
            
            scan_result = select(sock + 1, NULL, &fdset, NULL, &timeout);
            
            if (scan_result > 0) {
                int error = 0;
                socklen_t len = sizeof(error);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) >= 0 && error == 0) {
                    ESP_LOGI(TAG, "Port %d is OPEN on %s", port, target_ip);
                    result->open_ports[result->num_open_ports++] = port;
                    
                    fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
                    
                    timeout.tv_sec = 2;
                    timeout.tv_usec = 0;
                    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                    
                    memset(banner, 0, sizeof(banner));
                    bytes_read = recv(sock, banner, sizeof(banner) - 1, 0);
                    ESP_LOGD(TAG, "Received %d bytes from %s:%d", (int)bytes_read, target_ip, port);
                    
                    if (bytes_read > 0) {
                        banner[bytes_read] = '\0';
                        char *newline = strchr(banner, '\r');
                        if (newline) *newline = '\0';
                        newline = strchr(banner, '\n');
                        if (newline) *newline = '\0';
                        
                        ESP_LOGI(TAG, "SSH banner from %s:%d: %s", target_ip, port, banner);
                        glog(" OPEN: %s\n", banner);
                    } else {
                        glog(" OPEN (no banner)\n");
                    }
                } else {
                    ESP_LOGD(TAG, "Port %d connection failed on %s (getsockopt error)", port, target_ip);
                    glog(" CLOSED\n");
                }
            } else {
                ESP_LOGD(TAG, "Port %d timeout on %s (select result: %d)", port, target_ip, scan_result);
                glog(" TIMEOUT\n");
            }
        } else {
            ESP_LOGD(TAG, "Port %d immediate connection failure on %s (errno: %d)", port, target_ip, errno);
            glog(" CLOSED\n");
        }
        
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    glog("SSH scan completed on %s - found %d open ports\n", target_ip, result->num_open_ports);
}

/**
 * @brief Scan a specific IP address for open TCP ports (range)
 */
bool port_scan_ip_range(const char *target_ip, uint16_t start_port, uint16_t end_port) {
    // Use local result - no need for context allocation
    port_scan_result_t result;
    strcpy(result.ip, target_ip);
    result.num_open_ports = 0;

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    inet_pton(AF_INET, target_ip, &server_addr.sin_addr.s_addr);

    glog("Scanning %s TCP ports %d-%d\n", target_ip, start_port, end_port);

    uint16_t ports_scanned = 0;
    uint16_t total_ports = end_port - start_port + 1;

    for (uint16_t port = start_port; port <= end_port; port++) {
        if (result.num_open_ports >= PORT_SCAN_MAX_OPEN_PORTS)
            break;

        ports_scanned++;
        if (ports_scanned % 100 == 0) {
            glog("Progress: %d/%d ports (%.1f%%)\n", ports_scanned, total_ports,
                 (float)ports_scanned / total_ports * 100);
        }

        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0)
            continue;

        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        server_addr.sin_port = htons(port);
        int scan_result = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));

        if (scan_result < 0 && errno == EINPROGRESS) {
            struct timeval timeout = {.tv_sec = SCAN_TIMEOUT_MS / 1000,
                                      .tv_usec = (SCAN_TIMEOUT_MS % 1000) * 1000};
            fd_set fdset;
            FD_ZERO(&fdset);
            FD_SET(sock, &fdset);

            if (select(sock + 1, NULL, &fdset, NULL, &timeout) > 0) {
                int error = 0;
                socklen_t len = sizeof(error);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) >= 0 && error == 0) {
                    result.open_ports[result.num_open_ports++] = port;
                    glog("  Port %d: OPEN\n", port);
                }
            }
        }
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (result.num_open_ports > 0) {
        glog("Host %s has %d open ports\n", result.ip, result.num_open_ports);
    }

    return true;
}

/**
 * @brief Scan a specific IP address for open UDP ports (range)
 */
bool port_scan_udp_ip_range(const char *target_ip, uint16_t start_port, uint16_t end_port) {
    // Use local result - no need for context allocation
    port_scan_result_t result;
    strcpy(result.ip, target_ip);
    result.num_open_ports = 0;

    glog("Scanning %s UDP ports %d-%d\n", target_ip, start_port, end_port);

    uint16_t ports_scanned = 0;
    uint16_t total_ports = end_port - start_port + 1;

    for (uint16_t port = start_port; port <= end_port; port++) {
        if (result.num_open_ports >= PORT_SCAN_MAX_OPEN_PORTS) break;
        ports_scanned++;
        if (ports_scanned % 200 == 0) {
            glog("Progress: %d/%d ports (%.1f%%)\n", ports_scanned, total_ports,
                 (float)ports_scanned / total_ports * 100);
        }
        if (udp_port_is_open(target_ip, port, 40)) {
            result.open_ports[result.num_open_ports++] = port;
            glog("  UDP %d: OPEN\n", port);
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    if (result.num_open_ports > 0) {
        glog("Host %s has %d UDP ports responding\n", result.ip, result.num_open_ports);
    }

    return true;
}

// ============================================================================
// Main Subnet Scanning Function
// ============================================================================

/**
 * @brief Print service analysis for a scan result
 * 
 * Analyzes open ports and prints possible services and device types.
 * Uses centralized port service definitions from network_constants.
 * 
 * @param result Scan result to analyze
 */
static void print_service_analysis(const port_scan_result_t *result) {
    if (result->num_open_ports == 0) {
        return;
    }

    glog("Host %s has %d open ports:\n", result->ip, result->num_open_ports);
    glog("Possible services/devices:\n");

    for (uint8_t j = 0; j < result->num_open_ports; j++) {
        uint16_t port = result->open_ports[j];
        glog("  - Port %d: ", port);

        const char *description = get_port_service_description(port);
        if (description) {
            glog("%s\n", description);
        } else {
            glog("Unknown Service\n");
        }
    }

    bool has_web = false;
    bool has_db = false;
    bool has_file_sharing = false;

    for (uint8_t j = 0; j < result->num_open_ports; j++) {
        uint16_t port = result->open_ports[j];
        if (is_web_port(port))
            has_web = true;
        if (is_database_port(port))
            has_db = true;
        if (is_file_sharing_port(port))
            has_file_sharing = true;
    }

    printf("\nPossible device type:\n");
    TERMINAL_VIEW_ADD_TEXT("\nPossible device type:\n");

    if (has_web && has_db) {
        printf("- Web Application Server\n");
        TERMINAL_VIEW_ADD_TEXT("- Web Application Server\n");
    }
    if (has_file_sharing) {
        glog("- Windows Server\n");
    }
    glog("\n");
}

/**
 * @brief Scan the local subnet for open ports
 */
bool port_scan_subnet(void) {
    port_scanner_ctx_t *ctx = port_scanner_init();
    if (!ctx) {
        glog("Failed to initialize scanner context\n");
        return false;
    }

    if (!port_scan_get_subnet_prefix(ctx)) {
        glog("Failed to get network information. Make sure WiFi is connected.\n");
        port_scanner_cleanup(ctx);
        return false;
    }

    char current_ip[26];
    ctx->num_active_hosts = 0;
    g_port_scan_cancel = false;  // Reset cancellation flag

    glog("Starting subnet scan on %s0/24\n", ctx->subnet_prefix);
    glog("Scanning 254 hosts...\n");

    for (int host = START_HOST; host <= END_HOST && !g_port_scan_cancel; host++) {
        // Progress indication every 25 hosts
        if (host % 25 == 0) {
            glog("Progress: %d/254 hosts scanned\n", host);
        }
        
        snprintf(current_ip, sizeof(current_ip), "%s%d", ctx->subnet_prefix, host);

        if (port_scan_is_host_active(current_ip)) {
            glog("\n[Host %d] Found active host: %s\n", ctx->num_active_hosts + 1, current_ip);
            ctx->num_active_hosts++;

            // Process results immediately - no storage needed
            port_scan_result_t tcp_result;
            port_scan_result_t udp_result;

            port_scan_scan_tcp_ports(current_ip, &tcp_result);
            port_scan_scan_udp_ports(current_ip, &udp_result);

            // Print TCP results analysis immediately
            print_service_analysis(&tcp_result);

            // Print UDP results
            if (udp_result.num_open_ports > 0) {
                glog("UDP ports on %s:\n", current_ip);
                for (uint8_t k = 0; k < udp_result.num_open_ports; k++) {
                    glog("  UDP %d: OPEN\n", udp_result.open_ports[k]);
                }
            }
        }
    }
    
    glog("\n========================================\n");
    if (g_port_scan_cancel) {
        glog("Scan cancelled. Found %d active hosts.\n", ctx->num_active_hosts);
    } else {
        glog("Scan completed. Found %d active hosts.\n", ctx->num_active_hosts);
    }
    glog("========================================\n");
    
    port_scanner_cleanup(ctx);
    g_port_scan_task_handle = NULL;
    return !g_port_scan_cancel;
}

// ============================================================================
// Async Scan Functions
// ============================================================================

/**
 * @brief Task wrapper for async subnet scanning
 */
static void port_scan_subnet_task(void *pvParameters) {
    port_scan_subnet();
    vTaskDelete(NULL);
}

/**
 * @brief Start an async subnet scan in a separate task
 * 
 * This keeps the CLI responsive during the scan.
 */
void port_scan_subnet_async(void) {
    if (g_port_scan_task_handle != NULL) {
        glog("Port scan already in progress\n");
        return;
    }
    
    g_port_scan_cancel = false;
    xTaskCreate(port_scan_subnet_task, "port_scan", 8192, NULL, 5, &g_port_scan_task_handle);
}

/**
 * @brief Cancel an ongoing port scan
 */
void port_scan_cancel(void) {
    g_port_scan_cancel = true;
}

/**
 * @brief Check if a port scan is in progress
 */
bool port_scan_is_running(void) {
    return g_port_scan_task_handle != NULL;
}