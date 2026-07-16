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
#include "core/callbacks.h"
#include "core/glog.h"
#include "core/scan_saver.h"
#include "core/utils.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "managers/wifi_manager.h"
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

// Persistent result storage (heap-allocated)
static arp_host_t *g_arp_results = NULL;
static int g_arp_result_count = 0;
static volatile bool g_arp_scan_running = false;
static volatile bool g_arp_scan_done = false;

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
    if (scan_file_is_open(sf)) {
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
static bool arp_get_subnet_prefix(char *subnet_prefix, size_t prefix_size) {
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
    if (!arp_get_subnet_prefix(ctx->subnet_prefix, sizeof(ctx->subnet_prefix))) {
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
        if (!g_arp_scan_running) {
            glog("ARP scan cancelled\n");
            arp_scanner_cleanup(ctx);
            return false;
        }

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

    // Copy results to persistent storage
    g_arp_result_count = 0;
    int limit = (int)ctx->num_active_hosts;
    if (limit > ARP_SCAN_MAX_RESULTS) limit = ARP_SCAN_MAX_RESULTS;
    for (int i = 0; i < limit; i++) {
        memcpy(&g_arp_results[i], &ctx->hosts[i], sizeof(arp_host_t));
    }
    g_arp_result_count = limit;

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
 * @brief FreeRTOS task wrapper for async ARP scan
 */
static void arp_scan_task(void *pvParameters) {
    (void)pvParameters;
    arp_scan_subnet();
    g_arp_scan_done = true;
    vTaskDelete(NULL);
}

esp_err_t arp_scan_start_async(void) {
    if (g_arp_scan_running) {
        return ESP_ERR_INVALID_STATE;
    }
    arp_scan_clear_results();
    g_arp_results = malloc(sizeof(arp_host_t) * ARP_SCAN_MAX_RESULTS);
    if (!g_arp_results) {
        return ESP_ERR_NO_MEM;
    }
    memset(g_arp_results, 0, sizeof(arp_host_t) * ARP_SCAN_MAX_RESULTS);
    g_arp_scan_running = true;
    g_arp_scan_done = false;
    BaseType_t ret = xTaskCreate(arp_scan_task, "arp_scan", 8192, NULL, 5, NULL);
    if (ret != pdPASS) {
        g_arp_scan_running = false;
        free(g_arp_results);
        g_arp_results = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool arp_scan_check_done(void) {
    return g_arp_scan_done;
}

void arp_scan_finish_async(void) {
    g_arp_scan_running = false;
}

bool arp_scan_is_running(void) {
    return g_arp_scan_running && !g_arp_scan_done;
}

void arp_scan_cancel(void) {
    g_arp_scan_running = false;
}

int arp_scan_get_count(void) {
    return g_arp_result_count;
}

const arp_host_t* arp_scan_get_host(int index) {
    if (index < 0 || index >= g_arp_result_count) {
        return NULL;
    }
    return &g_arp_results[index];
}

void arp_scan_clear_results(void) {
    g_arp_result_count = 0;
    if (g_arp_results) {
        free(g_arp_results);
        g_arp_results = NULL;
    }
}

/**
 * @brief Print ARP scan results (placeholder for future use)
 */
void arp_scan_print_results(void) {
    // Results are printed during scan in the current implementation
    // This function is provided for API consistency
}

// ============================================================================
// Compact Wi-Fi Packet Monitor
// ============================================================================

static volatile bool g_passive_running = false;
static volatile int g_passive_count = 0;
static volatile uint32_t g_passive_data_frames = 0;
static volatile uint32_t g_passive_protected_frames = 0;
static volatile uint32_t g_packet_feed_emitted = 0;

#define PASSIVE_ARP_MAX_HOSTS 64
typedef struct {
    uint32_t ip_addr;
    uint8_t mac[6];
} passive_arp_host_t;

static passive_arp_host_t *g_passive_hosts = NULL;
static int g_passive_host_count = 0;

// Simple OUI vendor lookup table (top vendors)
typedef struct {
    uint8_t prefix[3];
    const char *vendor;
} oui_entry_t;

static const oui_entry_t oui_table[] = {
    {{0x00, 0x50, 0x56}, "VMware"},
    {{0x00, 0x0C, 0x29}, "VMware"},
    {{0x08, 0x00, 0x27}, "VirtualBox"},
    {{0x0A, 0x00, 0x27}, "VirtualBox"},
    {{0xB8, 0x27, 0xEB}, "Raspberry Pi"},
    {{0xDC, 0xA6, 0x32}, "Raspberry Pi"},
    {{0xE4, 0x5F, 0x01}, "Raspberry Pi"},
    {{0x28, 0xCD, 0xC1}, "Raspberry Pi"},
    {{0x00, 0x1A, 0x79}, "Dell"},
    {{0x00, 0x14, 0x22}, "Dell"},
    {{0xF8, 0xBC, 0x12}, "Dell"},
    {{0x00, 0x25, 0xB5}, "Dell"},
    {{0x00, 0x1E, 0x67}, "Intel"},
    {{0x00, 0x1B, 0x21}, "Intel"},
    {{0x68, 0x05, 0xCA}, "Intel"},
    {{0xA4, 0x34, 0xD9}, "Intel"},
    {{0x3C, 0x22, 0xFB}, "Apple"},
    {{0x00, 0x1C, 0xB3}, "Apple"},
    {{0xF0, 0x18, 0x98}, "Apple"},
    {{0xAC, 0xDE, 0x48}, "Apple"},
    {{0x00, 0x26, 0xBB}, "Apple"},
    {{0x78, 0x7B, 0x8A}, "Apple"},
    {{0x00, 0x03, 0x93}, "Apple"},
    {{0x94, 0xE9, 0x79}, "Apple"},
    {{0x00, 0x17, 0x88}, "Philips Hue"},
    {{0x00, 0x12, 0x47}, "TP-Link"},
    {{0x50, 0xC7, 0xBF}, "TP-Link"},
    {{0xE8, 0xDE, 0x27}, "TP-Link"},
    {{0x30, 0xB5, 0xC2}, "TP-Link"},
    {{0x00, 0x0E, 0x8F}, "Netgear"},
    {{0x20, 0xE5, 0x2A}, "Netgear"},
    {{0x44, 0x94, 0xFC}, "Netgear"},
    {{0x00, 0x1F, 0x33}, "Netgear"},
    {{0x00, 0x24, 0xD4}, "ASUS"},
    {{0xF4, 0x6D, 0x04}, "ASUS"},
    {{0x2C, 0x56, 0xDC}, "ASUS"},
    {{0x00, 0x13, 0x02}, "Samsung"},
    {{0x00, 0x1A, 0x8A}, "Samsung"},
    {{0x5C, 0x3C, 0x27}, "Samsung"},
    {{0x00, 0x21, 0xD1}, "Samsung"},
    {{0x00, 0x16, 0x32}, "Samsung"},
    {{0x00, 0x0D, 0xB9}, "Samsung"},
    {{0x00, 0x07, 0xAB}, "Samsung"},
    {{0x00, 0x02, 0x78}, "Samsung"},
    {{0x34, 0x23, 0xBA}, "Samsung"},
    {{0xF0, 0x5B, 0x7B}, "Samsung"},
    {{0xFC, 0xF1, 0x36}, "Samsung"},
    {{0x00, 0x0E, 0x6D}, "D-Link"},
    {{0x00, 0x1B, 0x11}, "D-Link"},
    {{0x1C, 0x7E, 0xE5}, "D-Link"},
    {{0x00, 0x15, 0xE9}, "D-Link"},
    {{0x00, 0x1C, 0xF0}, "D-Link"},
    {{0x30, 0x23, 0x03}, "Belkin"},
    {{0x00, 0x11, 0x50}, "Belkin"},
    {{0x00, 0x1D, 0xD8}, "HP"},
    {{0x00, 0x0D, 0x9D}, "HP"},
    {{0x00, 0x08, 0x02}, "HP"},
    {{0x00, 0x1A, 0x4B}, "HP"},
    {{0x00, 0x0B, 0xCD}, "HP"},
    {{0x00, 0x13, 0x21}, "HP"},
    {{0x00, 0x08, 0x22}, "Xiaomi"},
    {{0x28, 0x6C, 0x07}, "Xiaomi"},
    {{0x7C, 0x1D, 0xD9}, "Xiaomi"},
    {{0x64, 0x09, 0x80}, "Xiaomi"},
    {{0x78, 0x11, 0xDC}, "Xiaomi"},
    {{0xF8, 0xA4, 0x5F}, "Xiaomi"},
    {{0x00, 0x0E, 0x58}, "Sony"},
    {{0x00, 0x04, 0x1B}, "Sony"},
    {{0x00, 0x1A, 0x80}, "Sony"},
    {{0x00, 0x1E, 0x45}, "Sony"},
    {{0x00, 0x0A, 0x95}, "Cisco"},
    {{0x00, 0x01, 0x42}, "Cisco"},
    {{0x00, 0x01, 0x63}, "Cisco"},
    {{0x00, 0x01, 0x96}, "Cisco"},
    {{0x00, 0x01, 0xC7}, "Cisco"},
    {{0x00, 0x02, 0x16}, "Cisco"},
    {{0x00, 0x02, 0x4A}, "Cisco"},
    {{0x00, 0x02, 0xB9}, "Cisco"},
    {{0x00, 0x03, 0x6B}, "Cisco"},
    {{0x00, 0x03, 0xE3}, "Cisco"},
    {{0x00, 0x04, 0x9D}, "Cisco"},
    {{0x00, 0x04, 0xC1}, "Cisco"},
    {{0x00, 0x05, 0x9B}, "Cisco"},
    {{0x00, 0x06, 0x28}, "Cisco"},
    {{0x00, 0x07, 0x0D}, "Cisco"},
    {{0x00, 0x07, 0x85}, "Cisco"},
    {{0x00, 0x08, 0x20}, "Cisco"},
    {{0x00, 0x08, 0x7C}, "Cisco"},
    {{0x00, 0x09, 0x43}, "Cisco"},
    {{0x00, 0x09, 0x44}, "Cisco"},
    {{0x00, 0x09, 0x7C}, "Cisco"},
    {{0x00, 0x09, 0xB6}, "Cisco"},
    {{0x00, 0x0A, 0x41}, "Cisco"},
    {{0x00, 0x0A, 0x8A}, "Cisco"},
    {{0x00, 0x0A, 0xB8}, "Cisco"},
    {{0x00, 0x0A, 0xB6}, "Cisco"},
    {{0x00, 0x0B, 0x46}, "Cisco"},
    {{0x00, 0x0B, 0x5F}, "Cisco"},
    {{0x00, 0x0B, 0x85}, "Cisco"},
    {{0x00, 0x0B, 0xBE}, "Cisco"},
    {{0x00, 0x0B, 0xFC}, "Cisco"},
    {{0x00, 0x0C, 0x30}, "Cisco"},
    {{0x00, 0x0C, 0x85}, "Cisco"},
    {{0x00, 0x0C, 0x86}, "Cisco"},
    {{0x00, 0x0C, 0xDB}, "Cisco"},
    {{0x00, 0x0D, 0x29}, "Cisco"},
    {{0x00, 0x0D, 0x65}, "Cisco"},
    {{0x00, 0x0D, 0xBC}, "Cisco"},
    {{0x00, 0x0D, 0xEC}, "Cisco"},
    {{0x00, 0x0E, 0x38}, "Cisco"},
    {{0x00, 0x0E, 0xD7}, "Cisco"},
    {{0x00, 0x0F, 0x24}, "Cisco"},
    {{0x00, 0x0F, 0x34}, "Cisco"},
    {{0x00, 0x0F, 0x8F}, "Cisco"},
    {{0x00, 0x12, 0x00}, "Cisco"},
    {{0x00, 0x12, 0x43}, "Cisco"},
    {{0x00, 0x12, 0x7F}, "Cisco"},
    {{0x00, 0x12, 0xD9}, "Cisco"},
    {{0x00, 0x13, 0x19}, "Cisco"},
    {{0x00, 0x13, 0x7F}, "Cisco"},
    {{0x00, 0x13, 0x80}, "Cisco"},
    {{0x00, 0x14, 0x6A}, "Cisco"},
    {{0x00, 0x14, 0xA8}, "Cisco"},
    {{0x00, 0x16, 0x46}, "Cisco"},
    {{0x00, 0x16, 0x47}, "Cisco"},
    {{0x00, 0x17, 0x0E}, "Cisco"},
    {{0x00, 0x17, 0x3B}, "Cisco"},
    {{0x00, 0x17, 0x94}, "Cisco"},
    {{0x00, 0x17, 0x95}, "Cisco"},
    {{0x00, 0x18, 0x73}, "Cisco"},
    {{0x00, 0x19, 0x2F}, "Cisco"},
    {{0x00, 0x19, 0xAA}, "Cisco"},
    {{0x00, 0x19, 0xE7}, "Cisco"},
    {{0x00, 0x1A, 0x2F}, "Cisco"},
    {{0x00, 0x1A, 0x6D}, "Cisco"},
    {{0x00, 0x1A, 0xA1}, "Cisco"},
    {{0x00, 0x1A, 0xA2}, "Cisco"},
    {{0x00, 0x1B, 0x0C}, "Cisco"},
    {{0x00, 0x1B, 0x53}, "Cisco"},
    {{0x00, 0x1B, 0x54}, "Cisco"},
    {{0x00, 0x1B, 0xD4}, "Cisco"},
    {{0x00, 0x1C, 0x0E}, "Cisco"},
    {{0x00, 0x1C, 0x0F}, "Cisco"},
    {{0x00, 0x1C, 0x10}, "Cisco"},
    {{0x00, 0x1C, 0x11}, "Cisco"},
    {{0x00, 0x1E, 0x13}, "Cisco"},
    {{0x00, 0x1E, 0x14}, "Cisco"},
    {{0x00, 0x1E, 0x49}, "Cisco"},
    {{0x00, 0x1E, 0x4A}, "Cisco"},
    {{0x00, 0x1E, 0xBE}, "Cisco"},
    {{0x00, 0x1E, 0xF7}, "Cisco"},
    {{0x00, 0x20, 0x3F}, "Cisco"},
    {{0x00, 0x21, 0x55}, "Cisco"},
    {{0x00, 0x21, 0x56}, "Cisco"},
    {{0x00, 0x22, 0x55}, "Cisco"},
    {{0x00, 0x22, 0x56}, "Cisco"},
    {{0x00, 0x23, 0x04}, "Cisco"},
    {{0x00, 0x23, 0x33}, "Cisco"},
    {{0x00, 0x23, 0x5D}, "Cisco"},
    {{0x00, 0x24, 0x13}, "Cisco"},
    {{0x00, 0x24, 0x14}, "Cisco"},
    {{0x00, 0x24, 0x50}, "Cisco"},
    {{0x00, 0x24, 0xC3}, "Cisco"},
    {{0x00, 0x25, 0x45}, "Cisco"},
    {{0x00, 0x25, 0x83}, "Cisco"},
    {{0x00, 0x25, 0x84}, "Cisco"},
    {{0x00, 0x26, 0x0A}, "Cisco"},
    {{0x00, 0x26, 0x51}, "Cisco"},
    {{0x00, 0x26, 0x98}, "Cisco"},
    {{0x00, 0x26, 0x99}, "Cisco"},
    {{0x00, 0x26, 0x9A}, "Cisco"},
    {{0x00, 0x26, 0xB0}, "Cisco"},
    {{0x00, 0x26, 0xB1}, "Cisco"},
    {{0x00, 0x26, 0xF2}, "Cisco"},
    {{0x00, 0x27, 0x0C}, "Cisco"},
    {{0x00, 0x50, 0x56}, "VMware"},
    {{0x00, 0x50, 0x57}, "VMware"},
    {{0x00, 0x50, 0x58}, "VMware"},
    {{0x00, 0x0C, 0x29}, "VMware"},
    {{0x00, 0x05, 0x69}, "VMware"},
};

static const int oui_table_size = sizeof(oui_table) / sizeof(oui_table[0]);

static const char *lookup_oui_vendor(const uint8_t *mac) {
    for (int i = 0; i < oui_table_size; i++) {
        if (mac[0] == oui_table[i].prefix[0] &&
            mac[1] == oui_table[i].prefix[1] &&
            mac[2] == oui_table[i].prefix[2]) {
            return oui_table[i].vendor;
        }
    }
    return NULL;
}

static void format_short_mac(const uint8_t *mac, char out[7]) {
    snprintf(out, 7, "%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static const char *packet_type_name(uint8_t type, uint8_t subtype) {
    if (type == 0) {
        switch (subtype) {
            case 0: return "ASQ";
            case 1: return "ASR";
            case 4: return "PRQ";
            case 5: return "PRS";
            case 8: return "BCN";
            case 10: return "DIS";
            case 11: return "AUT";
            case 12: return "DEA";
            default: return "MGT";
        }
    }
    if (type == 1) {
        switch (subtype) {
            case 11: return "RTS";
            case 12: return "CTS";
            case 13: return "ACK";
            default: return "CTL";
        }
    }
    if (type == 2) return (subtype & 0x08) ? "QOS" : "DAT";
    return "UNK";
}

static void format_compact_packet(const wifi_promiscuous_pkt_t *pkt,
                                  wifi_promiscuous_pkt_type_t packet_type,
                                  char *out, size_t out_size) {
    const uint8_t *frame = pkt->payload;
    size_t len = pkt->rx_ctrl.sig_len;
    uint16_t fc = (uint16_t)frame[0] | ((uint16_t)frame[1] << 8);
    uint8_t type = (uint8_t)((fc >> 2) & 0x03);
    uint8_t subtype = (uint8_t)((fc >> 4) & 0x0F);
    char flags[3] = {0};
    int flag_pos = 0;
    if (fc & 0x4000) flags[flag_pos++] = 'P';
    if (fc & 0x0800) flags[flag_pos++] = 'R';

    char dst[7] = "------";
    char src[7] = "------";
    if (len >= 10) format_short_mac(&frame[4], dst);
    if (len >= 16 && !(type == 1 && (subtype == 12 || subtype == 13))) {
        format_short_mac(&frame[10], src);
    }

    const char *name = packet_type_name(type, subtype);
    if (packet_type == WIFI_PKT_DATA && len >= 24) {
        bool to_ds = (fc & 0x0100) != 0;
        bool from_ds = (fc & 0x0200) != 0;
        size_t header_len = (to_ds && from_ds) ? 30 : 24;
        if (subtype & 0x08) {
            header_len += 2;
            if (fc & 0x8000) header_len += 4;
        }
        if (len >= header_len + 8) {
            const uint8_t *llc = frame + header_len;
            if (llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03) {
                uint16_t ether_type = ((uint16_t)llc[6] << 8) | llc[7];
                if (ether_type == 0x0806 && len >= header_len + 8 + 28) {
                    const uint8_t *arp = llc + 8;
                    snprintf(out, out_size, "%02u %4d ARP %u.%u>%u.%u %s",
                             (unsigned int)pkt->rx_ctrl.channel, pkt->rx_ctrl.rssi,
                             (unsigned int)arp[16], (unsigned int)arp[17],
                             (unsigned int)arp[26], (unsigned int)arp[27], src);
                    return;
                }
                if (ether_type == 0x888E) name = "EAP";
                else if (ether_type == 0x0800) name = "IP4";
                else if (ether_type == 0x86DD) name = "IP6";
            }
        }
    }

    snprintf(out, out_size, "%02u %4d %s %s>%s %u%s",
             (unsigned int)pkt->rx_ctrl.channel, pkt->rx_ctrl.rssi, name, src, dst,
             (unsigned int)len, flags);
}

static bool passive_neighbor_is_new(uint32_t ip_addr, const uint8_t *mac) {
    if (!g_passive_hosts) return false;
    for (int i = 0; i < g_passive_host_count; i++) {
        if (g_passive_hosts[i].ip_addr != ip_addr) continue;
        if (memcmp(g_passive_hosts[i].mac, mac, 6) == 0) return false;
        memcpy(g_passive_hosts[i].mac, mac, 6);
        return true;
    }

    if (g_passive_host_count >= PASSIVE_ARP_MAX_HOSTS) return false;
    g_passive_hosts[g_passive_host_count].ip_addr = ip_addr;
    memcpy(g_passive_hosts[g_passive_host_count].mac, mac, 6);
    g_passive_host_count++;
    return true;
}

static void poll_passive_arp_table(void) {
    for (size_t i = 0; i < ARP_TABLE_SIZE; i++) {
        ip4_addr_t *ip = NULL;
        struct netif *entry_netif = NULL;
        struct eth_addr *eth = NULL;
        if (!etharp_get_entry(i, &ip, &entry_netif, &eth) || !ip || !eth) continue;
        if (!passive_neighbor_is_new(ip->addr, eth->addr)) continue;

        const uint8_t *ip_bytes = (const uint8_t *)&ip->addr;
        char short_mac[7];
        format_short_mac(eth->addr, short_mac);
        const char *vendor = lookup_oui_vendor(eth->addr);
        if (vendor) {
            glog("NBR %u.%u %s %.16s\n",
                 (unsigned int)ip_bytes[2], (unsigned int)ip_bytes[3],
                 short_mac, vendor);
        } else {
            glog("NBR %u.%u %s\n",
                 (unsigned int)ip_bytes[2], (unsigned int)ip_bytes[3], short_mac);
        }
    }
}

// Lock-free ring buffer for packet lines (observer writes, main loop reads)
#define MONITOR_RING_SIZE 64
#define MONITOR_LINE_MAX 48
static char (*g_monitor_ring)[MONITOR_LINE_MAX] = NULL;
static volatile uint32_t g_monitor_ring_head = 0;
static volatile uint32_t g_monitor_ring_tail = 0;

static void monitor_ring_push(const char *line) {
    if (!g_monitor_ring) return;
    uint32_t next = (g_monitor_ring_head + 1) % MONITOR_RING_SIZE;
    if (next == g_monitor_ring_tail) return; // full, drop
    memcpy(g_monitor_ring[g_monitor_ring_head], line, MONITOR_LINE_MAX);
    g_monitor_ring[g_monitor_ring_head][MONITOR_LINE_MAX - 1] = '\0';
    __sync_synchronize();
    g_monitor_ring_head = next;
}

/**
 * @brief Lightweight observer attached to the Wireshark raw capture callback.
 *
 * Only pushes formatted lines into a lock-free ring buffer. No FreeRTOS
 * or glog calls from this context.
 */
static void packet_feed_observer(const wifi_promiscuous_pkt_t *pkt,
                                 wifi_promiscuous_pkt_type_t type) {
    if (!g_passive_running || !pkt || pkt->rx_ctrl.sig_len < 24) return;

    g_passive_data_frames++;
    uint16_t fc = (uint16_t)pkt->payload[0] | ((uint16_t)pkt->payload[1] << 8);
    if (fc & 0x4000) g_passive_protected_frames++;

    // Rate limit: emit every 4th packet
    if ((g_passive_data_frames & 3) != 0) return;

    char line[MONITOR_LINE_MAX];
    format_compact_packet(pkt, type, line, sizeof(line));
    if (strstr(line, " ARP ")) g_passive_count++;
    monitor_ring_push(line);
    g_packet_feed_emitted++;
}

/**
 * @brief Start a compact local Wi-Fi packet feed.
 *
 * Uses the same raw callback as Wireshark streaming, but emits bounded text
 * summaries to the local terminal instead of binary PCAP records.
 */
void arp_scan_start_passive(int duration_sec) {
    if (g_passive_running) {
        glog("Packet Monitor: Already running\n");
        return;
    }

    glog("Packet Monitor: CH RSSI TYP SRC>DST LEN FLAGS\n");
    glog("Packet Monitor: Press Back to stop\n\n");

    g_passive_hosts = calloc(PASSIVE_ARP_MAX_HOSTS, sizeof(*g_passive_hosts));
    g_monitor_ring = calloc(MONITOR_RING_SIZE, sizeof(*g_monitor_ring));
    if (!g_passive_hosts || !g_monitor_ring) {
        free(g_passive_hosts);
        free(g_monitor_ring);
        g_passive_hosts = NULL;
        g_monitor_ring = NULL;
        glog("Packet Monitor: insufficient memory\n");
        return;
    }

    g_passive_running = true;
    g_passive_count = 0;
    g_passive_data_frames = 0;
    g_passive_protected_frames = 0;
    g_packet_feed_emitted = 0;
    g_passive_host_count = 0;
    g_monitor_ring_head = 0;
    g_monitor_ring_tail = 0;

    /* The raw callback normally also feeds the binary PCAP writer. This local
     * text monitor only needs its observer tap. */
    wifi_callbacks_set_pcap_enabled(false);
    wifi_raw_set_observer(packet_feed_observer);
    wifi_manager_start_monitor_mode(wifi_raw_scan_callback);
    wifi_manager_start_wireshark_channel_hop();

    int elapsed_ticks = 0;
    while (g_passive_running &&
           (duration_sec <= 0 || elapsed_ticks < duration_sec * 20)) {
        // Drain ring buffer (safe: main loop is the only reader)
        while (g_monitor_ring_tail != g_monitor_ring_head) {
            __sync_synchronize();
            glog("%s\n", g_monitor_ring[g_monitor_ring_tail]);
            g_monitor_ring_tail = (g_monitor_ring_tail + 1) % MONITOR_RING_SIZE;
        }
        elapsed_ticks++;
        if (elapsed_ticks % 10 == 0) poll_passive_arp_table();
        if (elapsed_ticks % 200 == 0) {
            glog("MON %lup %da %lue %dn\n",
                 (unsigned long)g_passive_data_frames,
                 g_passive_count,
                 (unsigned long)g_packet_feed_emitted,
                 g_passive_host_count);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    wifi_raw_set_observer(NULL);
    wifi_manager_stop_wireshark_channel_hop();
    wifi_manager_stop_monitor_mode();
    wifi_callbacks_set_pcap_enabled(true);
    g_passive_running = false;
    free(g_passive_hosts);
    free(g_monitor_ring);
    g_passive_hosts = NULL;
    g_monitor_ring = NULL;

    glog("\nPacket Monitor: Stopped (%lup %da %luP)\n",
         (unsigned long)g_passive_data_frames,
         g_passive_count,
         (unsigned long)g_passive_protected_frames);
}

/**
 * @brief Stop the compact Wi-Fi packet monitor
 */
void arp_scan_stop_passive(void) {
    g_passive_running = false;
}
