/**
 * @file arp_scan.c
 * @brief ARP network scanning implementation
 * 
 * This module handles ARP-based network scanning operations including:
 * - Scanning subnets for active hosts
 * - Resolving MAC addresses from IP addresses
 * - Managing ARP scan results
 */

#include "scans/wifi/arp_scan.h"
#include "core/glog.h"
#include "core/scan_saver.h"
#include "core/utils.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Module tag for logging
static const char *TAG = "ARPScan";

// Scan configuration
#define START_HOST 1
#define END_HOST 254
#define BATCH_SIZE 10
#define ARP_REQUEST_DELAY_MS 10
#define ARP_RESPONSE_WAIT_MS 250
#define MAX_RETRIES 3

// ============================================================================
// Internal Helpers (Module-specific)
// ============================================================================

/**
 * @brief Format a host entry for display (single source of truth)
 */
static void format_host_entry(char *buffer, size_t size, size_t index, const char *ip, const uint8_t *mac) {
    char mac_str[18];
    format_mac_address(mac, mac_str, sizeof(mac_str), true);
    snprintf(buffer, size, "%2zu. %s [%s]", index, ip, mac_str);
}

/**
 * @brief Log and save a host entry
 */
static void log_host_entry(scan_file_t *sf, size_t index, const char *ip, const uint8_t *mac) {
    char entry[80];
    format_host_entry(entry, sizeof(entry), index, ip, mac);
    glog("%s\n", entry);
    if (sf && sf->fp) {
        scan_file_printf(sf, "%s\n", entry);
    }
}

/**
 * @brief Report scan progress
 */
static void report_progress(int scanned, int total, size_t hosts_found) {
    glog("Progress: %d/%d scanned, %zu hosts found\n", scanned, total, hosts_found);
    ESP_LOGI(TAG, "Progress: %d/%d, found %zu hosts so far", scanned, total, hosts_found);
}

// ============================================================================
// Context Management Functions
// ============================================================================

/**
 * @brief Initialize ARP scanner context
 */
arp_scanner_ctx_t *arp_scanner_init(void) {
    arp_scanner_ctx_t *ctx = malloc(sizeof(arp_scanner_ctx_t));
    if (!ctx) {
        return NULL;
    }

    ctx->max_hosts = END_HOST - START_HOST + 1;
    ctx->hosts = malloc(sizeof(arp_host_t) * ctx->max_hosts);
    if (!ctx->hosts) {
        free(ctx);
        return NULL;
    }

    ctx->num_active_hosts = 0;
    memset(ctx->subnet_prefix, 0, sizeof(ctx->subnet_prefix));
    return ctx;
}

/**
 * @brief Clean up ARP scanner context
 */
void arp_scanner_cleanup(arp_scanner_ctx_t *ctx) {
    if (ctx) {
        if (ctx->hosts) {
            free(ctx->hosts);
        }
        free(ctx);
    }
}

// ============================================================================
// ARP Request Functions
// ============================================================================

/**
 * @brief Send ARP request to target IP using raw WiFi transmission
 */
bool send_arp_request(const char *target_ip) {
    if (!target_ip) {
        ESP_LOGW(TAG, "send_arp_request: target_ip is NULL");
        return false;
    }

    ESP_LOGD(TAG, "Sending ARP request to %s", target_ip);
    
    esp_netif_t *netif = get_wifi_sta_netif();
    if (!netif) {
        ESP_LOGW(TAG, "send_arp_request: Failed to get WiFi STA interface");
        return false;
    }

    // Get our own IP and MAC
    esp_netif_ip_info_t ip_info;
    uint8_t our_mac[6];
    if (!get_own_ip_and_mac(netif, &ip_info, our_mac)) {
        return false;
    }

    // Parse target IP
    esp_ip4_addr_t target_addr;
    if (inet_pton(AF_INET, target_ip, &target_addr) != 1) {
        return false;
    }
    
    // Create ARP request packet
    uint8_t arp_packet[42] = {
        // Ethernet header
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Destination MAC (broadcast)
        our_mac[0], our_mac[1], our_mac[2], our_mac[3], our_mac[4], our_mac[5], // Source MAC
        0x08, 0x06, // EtherType (ARP)
        
        // ARP header
        0x00, 0x01, // Hardware type (Ethernet)
        0x08, 0x00, // Protocol type (IPv4)
        0x06,       // Hardware address length
        0x04,       // Protocol address length
        0x00, 0x01, // Operation (ARP request)
        
        // Sender hardware address (our MAC)
        our_mac[0], our_mac[1], our_mac[2], our_mac[3], our_mac[4], our_mac[5],
        
        // Sender protocol address (our IP)
        (ip_info.ip.addr >> 0) & 0xFF,
        (ip_info.ip.addr >> 8) & 0xFF,
        (ip_info.ip.addr >> 16) & 0xFF,
        (ip_info.ip.addr >> 24) & 0xFF,
        
        // Target hardware address (unknown, all zeros)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        
        // Target protocol address (target IP)
        (target_addr.addr >> 0) & 0xFF,
        (target_addr.addr >> 8) & 0xFF,
        (target_addr.addr >> 16) & 0xFF,
        (target_addr.addr >> 24) & 0xFF
    };

    // Send raw ARP packet using esp_wifi_80211_tx with retry logic
    ESP_LOGD(TAG, "Sending ARP packet to %s via esp_wifi_80211_tx", target_ip);
    
    esp_err_t err = ESP_FAIL;
    int retry_count = 0;
    
    while (retry_count < MAX_RETRIES) {
        err = esp_wifi_80211_tx(WIFI_IF_STA, arp_packet, sizeof(arp_packet), false);
        
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "ARP packet sent successfully to %s", target_ip);
            return true;
        } else if (err == ESP_ERR_NO_MEM) {
            // WiFi buffer exhaustion - wait and retry
            retry_count++;
            ESP_LOGD(TAG, "WiFi buffer full for %s, retry %d/%d", target_ip, retry_count, MAX_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            // Other error - don't retry
            ESP_LOGW(TAG, "Failed to send ARP packet to %s: %s", target_ip, esp_err_to_name(err));
            break;
        }
    }
    
    if (err == ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "Failed to send ARP packet to %s after %d retries: WiFi buffers exhausted", target_ip, MAX_RETRIES);
    }
    
    return false;
}

/**
 * @brief Send ARP request using lwIP stack
 */
static bool send_arp_request_lwip(const char *target_ip) {
    if (!target_ip) {
        return false;
    }

    // Parse target IP
    ip4_addr_t target_addr;
    if (!ip4addr_aton(target_ip, &target_addr)) {
        return false;
    }

    // Get STA network interface
    struct netif *netif = netif_default;
    if (!netif) {
        ESP_LOGW(TAG, "netif_default is NULL");
        return false;
    }

    // Send ARP request using lwIP
    err_t result = etharp_request(netif, &target_addr);
    return (result == ERR_OK);
}

/**
 * @brief Get ARP table entry for IP address
 */
bool get_arp_table_entry(const char *ip, uint8_t *mac) {
    if (!ip || !mac) {
        return false;
    }

    // Parse target IP
    ip4_addr_t target_addr;
    if (!ip4addr_aton(ip, &target_addr)) {
        return false;
    }

    // Search ARP table using NULL netif (searches all interfaces)
    struct eth_addr *eth_ret = NULL;
    const ip4_addr_t *ip_ret = NULL;
    
    s8_t arp_idx = etharp_find_addr(NULL, &target_addr, &eth_ret, &ip_ret);
    if (arp_idx >= 0 && eth_ret) {
        memcpy(mac, eth_ret->addr, 6);
        return true;
    }

    return false;
}

// ============================================================================
// Subnet Helper Functions
// ============================================================================

/**
 * @brief Get subnet prefix from current WiFi connection
 */
static bool get_subnet_prefix(char *subnet_prefix, size_t prefix_size) {
    esp_netif_t *netif = get_wifi_sta_netif();
    if (!netif) {
        glog("Failed to get WiFi interface\n");
        return false;
    }

    if (!is_wifi_sta_connected()) {
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

    size_t len = last_dot - network_str + 1;
    if (len >= prefix_size) {
        return false;
    }
    memcpy(subnet_prefix, network_str, len);
    subnet_prefix[len] = '\0';

    ESP_LOGI(TAG, "Determined subnet prefix: %s", subnet_prefix);
    return true;
}

// ============================================================================
// Host Management Functions
// ============================================================================

/**
 * @brief Add a discovered host to the context
 */
static bool add_discovered_host(arp_scanner_ctx_t *ctx, const char *ip, const uint8_t *mac) {
    if (ctx->num_active_hosts >= ctx->max_hosts) {
        return false;
    }
    
    arp_host_t *host = &ctx->hosts[ctx->num_active_hosts];
    strncpy(host->ip, ip, sizeof(host->ip) - 1);
    host->ip[sizeof(host->ip) - 1] = '\0';
    memcpy(host->mac, mac, 6);
    host->is_active = true;
    ctx->num_active_hosts++;
    return true;
}

/**
 * @brief Process a batch of hosts - send ARP requests and collect responses
 */
static void process_batch(arp_scanner_ctx_t *ctx, int batch_start, int batch_end) {
    char current_ip[26];
    
    // Send batch of ARP requests using lwIP
    for (int host = batch_start; host <= batch_end; host++) {
        build_ip_string(current_ip, sizeof(current_ip), ctx->subnet_prefix, host);
        send_arp_request_lwip(current_ip);
        vTaskDelay(pdMS_TO_TICKS(ARP_REQUEST_DELAY_MS));
    }
    
    // Wait for responses to arrive
    vTaskDelay(pdMS_TO_TICKS(ARP_RESPONSE_WAIT_MS));
    
    // Check ARP table for this batch
    for (int host = batch_start; host <= batch_end; host++) {
        build_ip_string(current_ip, sizeof(current_ip), ctx->subnet_prefix, host);
        
        uint8_t mac[6];
        if (get_arp_table_entry(current_ip, mac)) {
            add_discovered_host(ctx, current_ip, mac);
        }
    }
}

// ============================================================================
// Main Scan Functions
// ============================================================================

/**
 * @brief Scan subnet for active hosts using ARP
 */
bool arp_scan_subnet(void) {
    arp_scanner_ctx_t *ctx = arp_scanner_init();
    if (!ctx) {
        glog("Failed to initialize ARP scanner context\n");
        return false;
    }

    // Get subnet information
    if (!get_subnet_prefix(ctx->subnet_prefix, sizeof(ctx->subnet_prefix))) {
        glog("Failed to get network information. Make sure WiFi is connected.\n");
        arp_scanner_cleanup(ctx);
        return false;
    }

    glog("Starting ARP scan on %s0/24\n", ctx->subnet_prefix);
    glog("Scanning network using ARP requests...\n");
    ESP_LOGI(TAG, "Starting ARP scan, scanning %s1-%d", ctx->subnet_prefix, END_HOST);
    
    ctx->num_active_hosts = 0;
    const int total_hosts = END_HOST - START_HOST + 1;
    
    for (int batch_start = START_HOST; batch_start <= END_HOST; batch_start += BATCH_SIZE) {
        int batch_end = (batch_start + BATCH_SIZE - 1 > END_HOST) ? END_HOST : batch_start + BATCH_SIZE - 1;
        
        // Progress update
        glog("Scanning %s%d-%d...\n", ctx->subnet_prefix, batch_start, batch_end);
        ESP_LOGI(TAG, "Sending ARP batch %d-%d", batch_start, batch_end);
        
        // Process this batch
        process_batch(ctx, batch_start, batch_end);
        
        // Progress update every 5 batches or at end
        int scanned = batch_end - START_HOST + 1;
        if (scanned % 50 == 0 || batch_end == END_HOST) {
            report_progress(scanned, total_hosts, ctx->num_active_hosts);
        }
    }

    // Open scan file for saving results
    scan_file_t sf = SCAN_FILE_INIT;
    bool saving = (scan_file_open(&sf, "arp_scan", "txt") == ESP_OK);

    // Final summary
    glog("\n=== ARP Scan Results ===\n");
    glog("Found %zu active hosts on %s0/24:\n", ctx->num_active_hosts, ctx->subnet_prefix);
    
    if (saving) {
        scan_file_printf(&sf, "--- ARP Scan Results (%zu hosts) ---\n", ctx->num_active_hosts);
        scan_file_printf(&sf, "Subnet: %s0/24\n\n", ctx->subnet_prefix);
    }
    
    if (ctx->num_active_hosts > 0) {
        glog("\nActive hosts:\n");
        
        for (size_t i = 0; i < ctx->num_active_hosts; i++) {
            log_host_entry(&sf, i + 1, ctx->hosts[i].ip, ctx->hosts[i].mac);
        }
    } else {
        glog("No active hosts found.\n");
    }
    
    glog("\nARP scan completed.\n");
    ESP_LOGI(TAG, "ARP scan completed. Found %zu active hosts", ctx->num_active_hosts);

    if (saving) {
        scan_file_close(&sf);
    }

    arp_scanner_cleanup(ctx);
    return true;
}

/**
 * @brief Print ARP scan results (placeholder for future use)
 */
void arp_scan_print_results(void) {
    // Results are printed during scan in the current implementation
    // This function is provided for API consistency
}
