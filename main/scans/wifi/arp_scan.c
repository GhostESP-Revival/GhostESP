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
#include "core/ouis.h"
#include "core/scan_saver.h"
#include "core/system_manager.h"
#include "core/utils.h"
#include "vendor/wifi_l2.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "managers/wifi_manager.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/tcpip.h"

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
// Batches are kept at 8 because lwIP's default ARP_TABLE_SIZE is 10:
// a larger batch evicts entries before the harvest pass can read them.
#define BATCH_SIZE 8
#define ARP_REQUEST_DELAY_MS 5
#define ARP_RESPONSE_WAIT_MS 80
#define MAX_RETRIES 3
#define ARP_SCAN_MAX_PREFIX_LEN 20  // cap at /20 (4094 hosts) to keep scan time sane

// Multi-pass scan configuration (technique inspired by DecentLabs/officeAir).
// Multiple passes with inter-pass delays catch power-saving mobile clients
// that miss single-pass probes. Results are unioned across passes.
#define ARP_SCAN_PASSES_SMALL  3     // for /24 and smaller
#define ARP_SCAN_PASSES_LARGE  2     // for larger subnets
#define ARP_SMALL_THRESHOLD  256     // <= this many hosts = "small"
#define ARP_INTER_PASS_DELAY_MS 800

// Persistent result storage (heap-allocated)
static arp_host_t *g_arp_results = NULL;
static int g_arp_result_count = 0;
static volatile bool g_arp_scan_running = false;
static volatile bool g_arp_scan_done = false;
static volatile bool g_arp_task_active = false;  // async task alive (even if cancelled)

// Live progress tracking (read from UI poll timer)
static volatile int g_arp_progress_pass = 0;
static volatile int g_arp_progress_total_passes = 0;
static volatile int g_arp_progress_scanned = 0;
static volatile int g_arp_progress_total_hosts = 0;
static volatile int g_arp_progress_found = 0;

typedef struct {
    ip4_addr_t target;          /* by value: owned by the heap struct */
    uint8_t *mac;
    arp_scanner_ctx_t *scanner;
} arp_tcpip_call_t;

typedef struct {
    arp_scanner_ctx_t *scanner;
    volatile bool done;
} arp_harvest_req_t;

// ============================================================================
// Internal Helpers (Module-specific)
// ============================================================================

/**
 * @brief Format a host entry for display (single source of truth)
 */
static void format_host_entry(char *buffer, size_t size, size_t index, const char *ip, const uint8_t *mac) {
    char mac_str[18];
    format_mac_address(mac, mac_str, sizeof(mac_str), true);
    char vendor[64] = {0};
    if (ouis_lookup_vendor_bytes(mac, vendor, sizeof(vendor))) {
        snprintf(buffer, size, "%2zu. %s [%s] %s", index, ip, mac_str, vendor);
    } else {
        snprintf(buffer, size, "%2zu. %s [%s]", index, ip, mac_str);
    }
}

/**
 * @brief Log and save a host entry
 */
static void log_host_entry(scan_file_t *sf, size_t index, const char *ip, const uint8_t *mac) {
    char entry[112];
    format_host_entry(entry, sizeof(entry), index, ip, mac);
    glog("%s\n", entry);
    if (scan_file_is_open(sf)) {
        scan_file_printf(sf, "%s\n", entry);
    }
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

    // Allocate up to ARP_SCAN_MAX_RESULTS (the persistent storage cap)
    ctx->max_hosts = ARP_SCAN_MAX_RESULTS;
    ctx->hosts = malloc(sizeof(arp_host_t) * ctx->max_hosts);
    if (!ctx->hosts) {
        free(ctx);
        return NULL;
    }

    ctx->num_active_hosts = 0;
    ctx->scan_first = 0;
    ctx->scan_last = 0;
    ctx->total_hosts = 0;
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
 * @brief Send ARP request to target IP via the lwIP stack
 *
 * Uses the thread-safe etharp_request() call through the lwIP TCP/IP core
 * instead of crafting raw 802.11 frames. This is more reliable (no WiFi
 * buffer exhaustion) and lets the stack handle retransmits.
 * Technique inspired by DecentLabs/officeAir (MIT-licensed).
 */
static void arp_request_callback(void *ctx) {
    arp_tcpip_call_t *request = (arp_tcpip_call_t *)ctx;
    if (netif_default) {
        etharp_request(netif_default, &request->target);
    }
    free(request);
}

bool send_arp_request(const char *target_ip) {
    if (!target_ip) {
        ESP_LOGW(TAG, "send_arp_request: target_ip is NULL");
        return false;
    }

    ip4_addr_t target_addr;
    if (!ip4addr_aton(target_ip, &target_addr)) {
        ESP_LOGW(TAG, "send_arp_request: invalid IP '%s'", target_ip);
        return false;
    }

    arp_tcpip_call_t *call = malloc(sizeof(arp_tcpip_call_t));
    if (!call) {
        ESP_LOGW(TAG, "send_arp_request: out of memory");
        return false;
    }
    call->target = target_addr;

    if (tcpip_callback_with_block(arp_request_callback, call, 1) == ERR_MEM) {
        free(call);  /* not queued: the callback will never run */
        ESP_LOGW(TAG, "send_arp_request: callback queue full");
        return false;
    }
    return true;  /* queued (or already executed); the callback frees it */
}

/**
 * @brief Get ARP table entry for IP address (thread-safe)
 */
static void arp_lookup_callback(void *ctx) {
    arp_tcpip_call_t *lookup = (arp_tcpip_call_t *)ctx;
    struct eth_addr *eth = NULL;
    const ip4_addr_t *found_ip = NULL;
    if (etharp_find_addr(NULL, &lookup->target, &eth, &found_ip) >= 0 && eth) {
        memcpy(lookup->mac, eth->addr, 6);
        lookup->scanner = (arp_scanner_ctx_t *)(uintptr_t)1; // success flag
    }
    free(lookup);
}

bool get_arp_table_entry(const char *ip, uint8_t *mac) {
    if (!ip || !mac) {
        return false;
    }

    // Parse target IP
    ip4_addr_t target_addr;
    if (!ip4addr_aton(ip, &target_addr)) {
        return false;
    }

    arp_tcpip_call_t *call = malloc(sizeof(arp_tcpip_call_t));
    if (!call) {
        return false;
    }
    call->target = target_addr;
    call->mac = mac;
    call->scanner = NULL;

    if (tcpip_callback_with_block(arp_lookup_callback, call, 1) == ERR_MEM) {
        free(call);  /* not queued: the callback will never run */
        return false;
    }
    return true;  /* queued; the callback writes the MAC and frees the struct */
}

// ============================================================================
// Subnet Helper Functions
// ============================================================================

/**
 * @brief Build an IP string from a uint32_t address
 */
static void ip_to_string(uint32_t addr, char *buf, size_t size) {
    struct in_addr a;
    // addr is in host byte order; inet_ntoa expects network byte order
    a.s_addr = htonl(addr);
    strncpy(buf, inet_ntoa(a), size - 1);
    buf[size - 1] = '\0';
}

/**
 * @brief Determine the scan range from the actual netmask
 *
 * Fills ctx->scan_first, ctx->scan_last, ctx->total_hosts and builds
 * a human-readable subnet_prefix.  Caps at /20 to keep scan time sane.
 */
static bool arp_get_subnet_range(arp_scanner_ctx_t *ctx) {
    esp_netif_t *netif = get_wifi_sta_netif();
    if (!netif) {
        glog("Failed to get WiFi interface\n");
        return false;
    }

    if (!is_wifi_sta_connected()) {
        glog("WiFi is not connected\n");
        return false;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        glog("Failed to get IP info\n");
        return false;
    }

    uint32_t netmask = ntohl(ip_info.netmask.addr);
    uint32_t my_ip   = ntohl(ip_info.ip.addr);

    // Count host bits: trailing ones in ~netmask
    int host_bits = 0;
    uint32_t host_mask = ~netmask;
    for (uint32_t bit = 1; bit && (host_mask & bit); bit <<= 1) {
        host_bits++;
    }

    // Cap at /20 (12 host bits = 4094 hosts) to keep scan time sane.
    // When capping, shrink the effective host_mask so the scan range
    // stays within the first /20 block that contains our IP.
    if (host_bits > (32 - ARP_SCAN_MAX_PREFIX_LEN)) {
        host_bits = 32 - ARP_SCAN_MAX_PREFIX_LEN;
        host_mask = (1u << host_bits) - 1;
    }

    uint32_t network  = my_ip & ~host_mask;
    uint32_t broadcast = network | host_mask;
    uint32_t first_ip = network + 1;
    uint32_t last_ip  = broadcast - 1;

    if (first_ip >= last_ip) {
        glog("Subnet too small to scan\n");
        return false;
    }

    ctx->scan_first  = htonl(first_ip);
    ctx->scan_last   = htonl(last_ip);
    ctx->total_hosts = (int)(last_ip - first_ip + 1);

    // Build a display prefix (e.g. "192.168.1." for /24)
    // For larger subnets this is just the network address
    ip_to_string(network, ctx->subnet_prefix, sizeof(ctx->subnet_prefix));

    int cidr = 32 - host_bits;
    char first_str[16], last_str[16];
    ip_to_string(first_ip, first_str, sizeof(first_str));
    ip_to_string(last_ip, last_str, sizeof(last_str));
    ESP_LOGI(TAG, "Subnet: %s/%d (%d hosts, scan range %s-%s)",
             ctx->subnet_prefix, cidr, ctx->total_hosts, first_str, last_str);
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
 * @brief Check if a host IP is already in the discovered list
 */
static bool is_host_known(const arp_scanner_ctx_t *ctx, const char *ip) {
    for (size_t i = 0; i < ctx->num_active_hosts; i++) {
        if (strcmp(ctx->hosts[i].ip, ip) == 0) return true;
    }
    return false;
}

/**
 * @brief Harvest all entries currently in the lwIP ARP table
 *
 * Called between sub-batches and between passes to pick up early responses.
 * Thread-safe: runs on the TCP/IP core thread while iterating the table.
 */
static void harvest_arp_table_callback(void *ctx) {
    arp_harvest_req_t *req = (arp_harvest_req_t *)ctx;
    arp_scanner_ctx_t *scanner = req->scanner;
    for (size_t i = 0; i < ARP_TABLE_SIZE; i++) {
        ip4_addr_t *ip = NULL;
        struct netif *entry_netif = NULL;
        struct eth_addr *eth = NULL;
        if (!etharp_get_entry(i, &ip, &entry_netif, &eth) || !ip || !eth) continue;

        char ip_str[16];
        ip4addr_ntoa_r(ip, ip_str, sizeof(ip_str));

        if (!is_host_known(scanner, ip_str)) {
            add_discovered_host(scanner, ip_str, eth->addr);
        }
    }
    __sync_synchronize();
    req->done = true;
}

static void harvest_arp_table(arp_scanner_ctx_t *ctx) {
    arp_harvest_req_t *req = malloc(sizeof(arp_harvest_req_t));
    if (!req) {
        ESP_LOGW(TAG, "Failed to allocate ARP table harvest request");
        return;
    }
    req->scanner = ctx;
    req->done = false;

    if (tcpip_callback_with_block(harvest_arp_table_callback, req, 1) == ERR_MEM) {
        free(req);  /* not queued: the callback will never run */
        ESP_LOGW(TAG, "Failed to read ARP table on TCP/IP thread");
        return;
    }
    /* The tcpip thread runs at higher priority, so this bounded spin ends
     * as soon as the callback completes. */
    while (!req->done) {
        taskYIELD();
    }
    __sync_synchronize();
    free(req);
}

/**
 * @brief Process a batch of hosts - send ARP requests and collect responses
 */
static void process_batch(arp_scanner_ctx_t *ctx, uint32_t batch_start, uint32_t batch_end) {
    char current_ip[16];
    
    // Send batch of ARP requests using lwIP
    for (uint32_t ip = batch_start; ip <= batch_end; ip++) {
        ip_to_string(ip, current_ip, sizeof(current_ip));
        if (!is_host_known(ctx, current_ip)) {
            send_arp_request(current_ip);
        }
        vTaskDelay(pdMS_TO_TICKS(ARP_REQUEST_DELAY_MS));
    }
    
    // Wait for responses to arrive
    vTaskDelay(pdMS_TO_TICKS(ARP_RESPONSE_WAIT_MS));
    
    // Harvest the ARP table (picks up responses from this batch and any
    // late arrivals from previous batches)
    harvest_arp_table(ctx);
}

// ============================================================================
// Main Scan Functions
// ============================================================================

/**
 * @brief Scan subnet for active hosts using multi-pass ARP sweeps
 *
 * Respects the actual netmask — scans the full network range (up to /20).
 * Runs multiple passes, unioning results across passes. Inter-pass delays
 * give power-saving mobile clients a chance to wake up and respond.
 * Technique inspired by DecentLabs/officeAir (MIT-licensed).
 *
 * Callers must have set g_arp_scan_running; clearing it requests cancellation.
 * @param report print the result table and save it to the SD card
 */
static bool arp_scan_run(bool report) {
    arp_scanner_ctx_t *ctx = arp_scanner_init();
    if (!ctx) {
        glog("Failed to initialize ARP scanner context\n");
        return false;
    }

    // Get actual subnet range from netmask
    if (!arp_get_subnet_range(ctx)) {
        glog("Failed to get network information. Make sure WiFi is connected.\n");
        arp_scanner_cleanup(ctx);
        return false;
    }

    // Scale passes based on network size
    int num_passes = (ctx->total_hosts <= ARP_SMALL_THRESHOLD)
                     ? ARP_SCAN_PASSES_SMALL : ARP_SCAN_PASSES_LARGE;
    // Derive CIDR from the host bit count (before capping)
    esp_netif_ip_info_t _ip_info;
    esp_netif_get_ip_info(get_wifi_sta_netif(), &_ip_info);
    uint32_t _hm = ~ntohl(_ip_info.netmask.addr);
    int host_bits = 0;
    for (uint32_t bit = 1; bit && (_hm & bit); bit <<= 1) host_bits++;
    int cidr = 32 - host_bits;

    glog("Starting %d-pass ARP scan on %s/%d (%d hosts)\n",
         num_passes, ctx->subnet_prefix, cidr, ctx->total_hosts);
    ESP_LOGI(TAG, "Starting %d-pass ARP scan, %d hosts in range",
             num_passes, ctx->total_hosts);
    
    ctx->num_active_hosts = 0;
    // Work in host byte order for the loop — comparing network-byte-order
    // uint32_t values on little-endian gives wrong IP ordering.
    uint32_t first = ntohl(ctx->scan_first);
    uint32_t last  = ntohl(ctx->scan_last);

    g_arp_progress_total_passes = num_passes;
    g_arp_progress_total_hosts  = ctx->total_hosts;

    for (int pass = 0; pass < num_passes; pass++) {
        if (!g_arp_scan_running) {
            glog("ARP scan cancelled\n");
            arp_scanner_cleanup(ctx);
            return false;
        }

        g_arp_progress_pass = pass + 1;
        g_arp_progress_scanned = 0;

        glog("Pass %d/%d: scanning %s/%d...\n", pass + 1, num_passes, ctx->subnet_prefix, cidr);
        ESP_LOGI(TAG, "Pass %d/%d: subnet sweep", pass + 1, num_passes);

        int scanned_count = 0;
        for (uint32_t batch_start = first; batch_start <= last; batch_start += BATCH_SIZE) {
            if (!g_arp_scan_running) {
                glog("ARP scan cancelled\n");
                arp_scanner_cleanup(ctx);
                return false;
            }

            uint32_t batch_end = batch_start + BATCH_SIZE - 1;
            if (batch_end > last) batch_end = last;
            
            process_batch(ctx, batch_start, batch_end);
            
            scanned_count += (int)(batch_end - batch_start + 1);
            g_arp_progress_scanned = scanned_count;
            g_arp_progress_found = (int)ctx->num_active_hosts;
            if (scanned_count % 500 == 0 || batch_end == last) {
                glog("Pass %d/%d: %d/%d scanned, %d hosts found\n",
                     pass + 1, num_passes, scanned_count, ctx->total_hosts, (int)ctx->num_active_hosts);
                ESP_LOGI(TAG, "Pass %d/%d: %d/%d scanned, %zu hosts",
                         pass + 1, num_passes, scanned_count, ctx->total_hosts, ctx->num_active_hosts);
            }
        }

        // Inter-pass delay: lets power-saving mobile clients wake up
        if (pass < num_passes - 1) {
            glog("Pass %d/%d done (%zu hosts). Waiting %d ms for mobile clients...\n",
                 pass + 1, num_passes, ctx->num_active_hosts, ARP_INTER_PASS_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(ARP_INTER_PASS_DELAY_MS));
        }
    }

    // Final harvest after last pass
    harvest_arp_table(ctx);

    glog("%d-pass scan complete. %zu unique hosts found.\n",
         num_passes, ctx->num_active_hosts);

    // Transfer the scanner's bounded result allocation to the public result
    // owner. This works for both synchronous CLI and asynchronous UI scans.
    int limit = (int)ctx->num_active_hosts;
    if (limit > ARP_SCAN_MAX_RESULTS) limit = ARP_SCAN_MAX_RESULTS;
    free(g_arp_results);
    g_arp_results = ctx->hosts;
    ctx->hosts = NULL;
    g_arp_result_count = limit;

    if (!report) {
        arp_scanner_cleanup(ctx);
        return true;
    }

    // Open scan file for saving results
    scan_file_t sf = SCAN_FILE_INIT;
    bool saving = (scan_file_open(&sf, "arp_scan", "txt") == ESP_OK);

    // Final summary
    glog("\n=== ARP Scan Results ===\n");
    glog("Found %zu active hosts on %s/%d (%d passes):\n",
         ctx->num_active_hosts, ctx->subnet_prefix, cidr, num_passes);
    
    if (saving) {
        scan_file_printf(&sf, "--- ARP Scan Results (%zu hosts, %d passes) ---\n",
                         ctx->num_active_hosts, num_passes);
        scan_file_printf(&sf, "Subnet: %s/%d\n\n", ctx->subnet_prefix, cidr);
    }
    
    if (ctx->num_active_hosts > 0) {
        glog("\nActive hosts:\n");
        
        for (size_t i = 0; i < ctx->num_active_hosts; i++) {
            log_host_entry(&sf, i + 1, g_arp_results[i].ip, g_arp_results[i].mac);
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

static void arp_scan_reset_progress(void) {
    g_arp_progress_pass = 0;
    g_arp_progress_total_passes = 0;
    g_arp_progress_scanned = 0;
    g_arp_progress_total_hosts = 0;
    g_arp_progress_found = 0;
}

bool arp_scan_run_blocking(bool report) {
    if (g_arp_scan_running || g_arp_task_active) {
        return false;
    }
    arp_scan_reset_progress();
    g_arp_scan_running = true;
    bool ok = arp_scan_run(report);
    g_arp_scan_running = false;
    return ok;
}

bool arp_scan_subnet(void) {
    return arp_scan_run_blocking(true);
}

/**
 * @brief FreeRTOS task wrapper for async ARP scan
 */
static void arp_scan_task(void *pvParameters) {
    (void)pvParameters;
    arp_scan_run(true);
    // Release the scan on our own so CLI starts (which have no poll timer to
    // call arp_scan_finish_async) can run again.
    g_arp_scan_running = false;
    g_arp_task_active = false;
    g_arp_scan_done = true;
    vTaskDeleteWithCaps(NULL);
}

esp_err_t arp_scan_start_async(void) {
    // g_arp_task_active also covers a cancelled scan that is still winding down.
    if (g_arp_scan_running || g_arp_task_active) {
        return ESP_ERR_INVALID_STATE;
    }
    arp_scan_clear_results();
    g_arp_scan_running = true;
    g_arp_task_active = true;
    g_arp_scan_done = false;
    arp_scan_reset_progress();
    BaseType_t ret = xTaskCreate_psram(arp_scan_task, "arp_scan", 8192, NULL, 5, NULL);
    if (ret != pdPASS) {
        g_arp_scan_running = false;
        g_arp_task_active = false;
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

void arp_scan_get_progress(int *pass, int *total_passes, int *scanned, int *total_hosts, int *found) {
    if (pass)        *pass        = g_arp_progress_pass;
    if (total_passes)*total_passes= g_arp_progress_total_passes;
    if (scanned)     *scanned     = g_arp_progress_scanned;
    if (total_hosts) *total_hosts = g_arp_progress_total_hosts;
    if (found)       *found       = g_arp_progress_found;
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

typedef struct {
    char line[48];
} passive_arp_line_t;

static passive_arp_host_t *g_passive_hosts = NULL;
static passive_arp_line_t *g_passive_new_lines = NULL;
static int g_passive_host_count = 0;
static volatile bool g_passive_poll_pending = false;
static volatile bool g_passive_poll_ready = false;
static volatile int g_passive_poll_count = 0;

// Full IEEE OUI registry from core/ouis.c (ouis.bin). The hand-written
// "top vendors" table that used to live here drifted out of sync with the
// GUI, which already used the full list.
bool arp_scan_get_vendor_buf(const uint8_t *mac, char *out_vendor, size_t out_sz) {
    if (!mac || !out_vendor || out_sz == 0) return false;
    return ouis_lookup_vendor_bytes(mac, out_vendor, out_sz);
}

const char *arp_scan_get_vendor(const uint8_t *mac) {
    // Deprecated: kept for callers that cannot pass a buffer. Not reentrant.
    static char s_vendor[64];
    s_vendor[0] = '\0';
    if (!arp_scan_get_vendor_buf(mac, s_vendor, sizeof(s_vendor))) {
        return NULL;
    }
    return s_vendor;
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
        /* Shared header geometry plus 802.1Q/802.1ad tag peeling. The old
         * inline version only allowed for the HT Control field on QoS frames,
         * and treated a VLAN tag as the ethertype, so tagged traffic fell
         * through as "DAT" instead of ARP/EAP/IP. */
        wifi_l2_info_t l2;
        if (wifi_l2_parse(frame, len, &l2)) {
            const uint8_t *payload = frame + l2.payload_offset;
            if (l2.ethertype == WIFI_L2_ETHERTYPE_ARP) {
                /* ARP: htype(2) ptype(2) hlen(1) plen(1) oper(2) sha(6) spa(4)
                 * tha(6) tpa(4). Sender protocol address is at +14. */
                if (l2.payload_offset + 28 <= len) {
                    const uint8_t *arp = payload;
                    snprintf(out, out_size, "%02u %4d ARP %u.%u>%u.%u %s",
                             (unsigned int)pkt->rx_ctrl.channel, pkt->rx_ctrl.rssi,
                             (unsigned int)arp[16], (unsigned int)arp[17],
                             (unsigned int)arp[26], (unsigned int)arp[27], src);
                    return;
                }
            } else if (l2.ethertype == WIFI_L2_ETHERTYPE_EAPOL) {
                name = "EAP";
            } else if (l2.ethertype == WIFI_L2_ETHERTYPE_IPV4) {
                name = "IP4";
            } else if (l2.ethertype == WIFI_L2_ETHERTYPE_IPV6) {
                name = "IP6";
            }
            if (l2.vlan_depth > 0) {
                /* Flag the frame so a tagged line is distinguishable from an
                 * untagged one carrying the same protocol. */
                if (flag_pos < 2) {
                    flags[flag_pos++] = 'V';
                }
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

/* Runs on the tcpip thread: the ARP table read is inherently serialized. */
static void passive_arp_table_poll_callback(void *ctx) {
    (void)ctx;
    int new_count = 0;
    for (size_t i = 0; i < ARP_TABLE_SIZE && new_count < PASSIVE_ARP_MAX_HOSTS; i++) {
        ip4_addr_t *ip = NULL;
        struct netif *entry_netif = NULL;
        struct eth_addr *eth = NULL;
        if (!etharp_get_entry(i, &ip, &entry_netif, &eth) || !ip || !eth) continue;
        if (!passive_neighbor_is_new(ip->addr, eth->addr)) continue;

        const uint8_t *ip_bytes = (const uint8_t *)&ip->addr;
        char short_mac[7];
        format_short_mac(eth->addr, short_mac);
        char vendor[64] = {0};
        bool has_vendor = ouis_lookup_vendor_bytes(eth->addr, vendor, sizeof(vendor));
        if (has_vendor) {
            snprintf(g_passive_new_lines[new_count].line, sizeof(g_passive_new_lines[0].line),
                     "NBR %u.%u %s %.16s",
                     (unsigned int)ip_bytes[2], (unsigned int)ip_bytes[3],
                     short_mac, vendor);
        } else {
            snprintf(g_passive_new_lines[new_count].line, sizeof(g_passive_new_lines[0].line),
                     "NBR %u.%u %s",
                     (unsigned int)ip_bytes[2], (unsigned int)ip_bytes[3], short_mac);
        }
        new_count++;
    }
    __sync_synchronize();
    g_passive_poll_count = new_count;
    g_passive_poll_ready = true;
    g_passive_poll_pending = false;
}

static void poll_passive_arp_table(void) {
    if (!g_passive_new_lines) return;

    if (g_passive_poll_ready) {  /* drain results of the previous poll */
        int count = g_passive_poll_count;
        __sync_synchronize();
        for (int i = 0; i < count; i++) {
            glog("%s\n", g_passive_new_lines[i].line);
        }
        g_passive_poll_ready = false;
        return;
    }
    if (g_passive_poll_pending) return;  /* one request in flight */

    g_passive_poll_pending = true;
    if (tcpip_callback(passive_arp_table_poll_callback, NULL) != ERR_OK) {
        g_passive_poll_pending = false;
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
    g_passive_new_lines = calloc(PASSIVE_ARP_MAX_HOSTS, sizeof(*g_passive_new_lines));
    g_monitor_ring = calloc(MONITOR_RING_SIZE, sizeof(*g_monitor_ring));
    if (!g_passive_hosts || !g_passive_new_lines || !g_monitor_ring) {
        free(g_passive_hosts);
        free(g_passive_new_lines);
        free(g_monitor_ring);
        g_passive_hosts = NULL;
        g_passive_new_lines = NULL;
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

    /* Drain any in-flight ARP table poll before freeing the buffers: the
     * callback runs on the tcpip thread and touches g_passive_new_lines. */
    while (g_passive_poll_pending) taskYIELD();

    free(g_passive_hosts);
    free(g_passive_new_lines);
    free(g_monitor_ring);
    g_passive_hosts = NULL;
    g_passive_new_lines = NULL;
    g_monitor_ring = NULL;
    g_passive_poll_ready = false;
    g_passive_poll_count = 0;

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
