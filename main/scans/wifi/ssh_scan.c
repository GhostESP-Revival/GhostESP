/**
 * @file ssh_scan.c
 * @brief SSH service detection scanning implementation
 * 
 * This module handles SSH service detection operations including:
 * - Scanning individual hosts for SSH services
 * - Scanning subnets for SSH services
 * - Banner grabbing for SSH identification
 */

#include "scans/wifi/ssh_scan.h"
#include "core/scan_saver.h"
#include "core/glog.h"
#include "core/utils.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

// Constants
#define SSH_SCAN_TIMEOUT_SEC 3
#define SSH_BANNER_TIMEOUT_SEC 2
#define SSH_BANNER_BUFFER_SIZE 256

// Module tag for logging
static const char *TAG = "SSHScan";

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Clean up a banner string by removing newlines
 * 
 * @param banner Banner string to clean (modified in place)
 */
static void clean_banner_string(char *banner) {
    if (banner == NULL) return;
    
    char *newline = strchr(banner, '\r');
    if (newline) *newline = '\0';
    newline = strchr(banner, '\n');
    if (newline) *newline = '\0';
}

/**
 * @brief Scan a single SSH port and attempt banner grab
 * 
 * @param target_ip IP address to scan
 * @param port Port number to scan
 * @param sf Scan file for saving results (can be NULL)
 * @return true if SSH service found (port open), false otherwise
 */
static bool scan_ssh_port(const char *target_ip, uint16_t port, scan_file_t *sf) {
    char banner[SSH_BANNER_BUFFER_SIZE];
    
    ESP_LOGI(TAG, "Testing port %d on %s", port, target_ip);
    
    // Connect with timeout
    int sock = tcp_connect_with_timeout(target_ip, port, SSH_SCAN_TIMEOUT_SEC);
    if (sock < 0) {
        ESP_LOGD(TAG, "Port %d connection failed on %s", port, target_ip);
        return false;
    }
    
    // Port is open - try to grab banner
    ESP_LOGI(TAG, "Port %d is OPEN on %s", port, target_ip);
    
    int bytes = tcp_recv_with_timeout(sock, banner, sizeof(banner), SSH_BANNER_TIMEOUT_SEC);
    tcp_close_socket(&sock);
    
    if (bytes > 0) {
        clean_banner_string(banner);
        ESP_LOGI(TAG, "SSH banner from %s:%d: %s", target_ip, port, banner);
        
        glog("[%s:%d] Status: OPEN,\n"
             "          Banner: %s\n",
             target_ip, port, banner);
        
        if (sf != NULL) {
            scan_file_printf(sf, "[%s:%d] Status: OPEN, Banner: %s\n",
                            target_ip, port, banner);
        }
    } else {
        glog("[%s:%d] Status: OPEN,\n"
             "          Banner: (none)\n",
             target_ip, port);
        
        if (sf != NULL) {
            scan_file_printf(sf, "[%s:%d] Status: OPEN, Banner: (none)\n",
                            target_ip, port);
        }
    }
    
    return true;
}

// ============================================================================
// Public API Implementation
// ============================================================================

/**
 * @brief Scan a specific host for SSH services
 * 
 * Checks common SSH ports (22, 2222, 2022) and attempts to grab banner.
 */
void ssh_scan_host(const char *target_ip) {
    if (target_ip == NULL) {
        ESP_LOGE(TAG, "NULL target IP provided");
        return;
    }
    
    // Common SSH ports to check
    static const uint16_t ssh_ports[] = {22, 2222, 2022};
    static const size_t num_ssh_ports = sizeof(ssh_ports) / sizeof(ssh_ports[0]);
    
    ESP_LOGI(TAG, "Starting SSH scan on host: %s", target_ip);
    
    glog("SSH scanning host: %s\n", target_ip);
    
    int open_ports_found = 0;
    
    for (size_t i = 0; i < num_ssh_ports; i++) {
        if (scan_ssh_port(target_ip, ssh_ports[i], NULL)) {
            open_ports_found++;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    glog("SSH scan completed on %s - found %d open ports\n", target_ip, open_ports_found);
}

/**
 * @brief Scan the local subnet for SSH services
 * 
 * Performs a comprehensive scan of the local subnet for SSH services.
 */
void ssh_scan_subnet(void) {
    char subnet_prefix[16];
    
    if (!get_wifi_subnet_prefix(subnet_prefix, sizeof(subnet_prefix))) {
        glog("SSH Scan: Failed to get subnet prefix - not connected to WiFi?\n");
        return;
    }
    
    glog("SSH Scan: Scanning subnet %s*\n", subnet_prefix);
    
    scan_file_t sf = SCAN_FILE_INIT;
    bool saving = (scan_file_open(&sf, "ssh_scan", "txt") == ESP_OK);
    
    if (saving) {
        scan_file_printf(&sf, "--- SSH Scan Results (Subnet %s*) ---\n", subnet_prefix);
    }
    
    int total_hosts_with_ssh = 0;
    int total_open_ports = 0;
    
    // Scan all hosts in the subnet (1-254)
    for (int host = 1; host <= 254; host++) {
        char target_ip[16];
        build_ip_string(target_ip, sizeof(target_ip), subnet_prefix, host);
        
        glog("SSH Scan: Checking %s...\n", target_ip);
        
        // Common SSH ports to check
        static const uint16_t ssh_ports[] = {22, 2222, 2022};
        static const size_t num_ssh_ports = sizeof(ssh_ports) / sizeof(ssh_ports[0]);
        
        int host_open_ports = 0;
        
        for (size_t i = 0; i < num_ssh_ports; i++) {
            if (scan_ssh_port(target_ip, ssh_ports[i], &sf)) {
                host_open_ports++;
                total_open_ports++;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        
        if (host_open_ports > 0) {
            total_hosts_with_ssh++;
        }
        
        // Small delay between hosts
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    glog("SSH Scan: Subnet scan complete - found %d hosts with %d open SSH ports\n",
         total_hosts_with_ssh, total_open_ports);
    
    if (saving) {
        scan_file_printf(&sf, "--- SSH Scan Summary ---\n");
        scan_file_printf(&sf, "Hosts with SSH: %d, Total open ports: %d\n",
                        total_hosts_with_ssh, total_open_ports);
        scan_file_close(&sf);
    }
}
