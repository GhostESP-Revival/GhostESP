/**
 * @file netbios_scan.c
 * @brief NetBIOS Name Service (NBNS) scanning implementation
 *
 * This module handles NetBIOS/NBNS scanning operations including:
 * - Broadcasting NetBIOS Name Service queries on the local subnet
 * - Discovering Windows hosts and their NetBIOS names
 * - Scanning individual hosts for NetBIOS information
 */

#include "scans/wifi/netbios_scan.h"
#include "core/scan_saver.h"
#include "core/glog.h"
#include "core/utils.h"
#include "esp_random.h"
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
#define NETBIOS_RECV_TIMEOUT_MS 250
#define NETBIOS_PORT 137
#define NETBIOS_BROADCAST_RETRIES 2
#define NETBIOS_QUERY_PACKET_SIZE 50

// Module tag for logging
static const char *TAG = "NetbiosScan";

// Shared cancellation flag for all network scans
static volatile bool g_network_scan_cancel = false;

// ============================================================================
// Cancellation Control
// ============================================================================

void netbios_scan_cancel(void) {
    g_network_scan_cancel = true;
}

void netbios_scan_reset_cancel(void) {
    g_network_scan_cancel = false;
}

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Encode a NetBIOS name into the 16-byte padded format
 *
 * @param name Input name (up to 15 chars)
 * @param out  Output buffer (32 bytes)
 */
static void encode_netbios_name(const char *name, uint8_t *out) {
    char padded[16];
    memset(padded, ' ', 16);
    size_t len = strlen(name);
    if (len > 15) len = 15;
    memcpy(padded, name, len);
    for (int i = 0; i < 16; i++) {
        uint8_t c = (uint8_t)padded[i];
        out[i * 2]     = (uint8_t)(((c >> 4) & 0x0F) + 'A');
        out[i * 2 + 1] = (uint8_t)((c & 0x0F) + 'A');
    }
}

/**
 * @brief Build a NetBIOS Name Service query packet
 *
 * @param tx_id Transaction ID
 * @param name NetBIOS name to query (null for wildcard)
 * @param packet Output buffer
 * @param packet_size Size of output buffer
 * @return Length of built packet
 */
static size_t build_nbns_query(uint16_t tx_id, const char *name, uint16_t qtype,
                               uint8_t *packet, size_t packet_size) {
    if (packet_size < NETBIOS_QUERY_PACKET_SIZE) return 0;

    memset(packet, 0, packet_size);

    // Transaction ID
    packet[0] = (uint8_t)(tx_id >> 8);
    packet[1] = (uint8_t)(tx_id & 0xFF);

    // Flags: 0x0000 = standard query, recursion desired
    packet[2] = 0x00;
    packet[3] = 0x00;

    // Questions: 1
    packet[4] = 0x00;
    packet[5] = 0x01;

    // Answer RRs: 0
    packet[6] = 0x00;
    packet[7] = 0x00;

    // Authority RRs: 0
    packet[8] = 0x00;
    packet[9] = 0x00;

    // Additional RRs: 0
    packet[10] = 0x00;
    packet[11] = 0x00;

    // Name field
    size_t offset = 12;
    if (name && strlen(name) > 0) {
        packet[offset++] = 0x20; // Length = 32
        uint8_t enc[32];
        encode_netbios_name(name, enc);
        memcpy(&packet[offset], enc, 32);
        offset += 32;
    } else {
        // Wildcard query: CKAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
        packet[offset++] = 0x20;
        static const uint8_t wildcard[] = {
            0x43, 0x4B, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
            0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
            0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
            0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41
        };
        memcpy(&packet[offset], wildcard, 32);
        offset += 32;
    }
    packet[offset++] = 0x00; // Name terminator

    packet[offset++] = (uint8_t)(qtype >> 8);
    packet[offset++] = (uint8_t)(qtype & 0xFF);

    // Class: IN (0x0001)
    packet[offset++] = 0x00;
    packet[offset++] = 0x01;

    return offset;
}

/**
 * @brief Parse NBNS response and log discovered host
 */
static void parse_nbns_response(const uint8_t *buf, size_t len,
                               const char *src_ip, scan_file_t *sf) {
    if (len < 57) return;

    // Check response flags (should be 0x8400 for response + recursion available)
    uint16_t flags = ((uint16_t)buf[2] << 8) | buf[3];
    if ((flags & 0x8000) == 0) return; // Not a response

    uint16_t answers = ((uint16_t)buf[6] << 8) | buf[7];
    if (answers == 0) return;

    // Find name in response (skip header + name)
    size_t pos = 12;
    if (buf[pos] == 0x20) pos += 34; // length byte + 32-byte name + terminator
    else {
        while (pos < len && buf[pos] != 0x00) pos += buf[pos] + 1;
        pos++;
    }
    if (pos + 4 > len) return;
    pos += 4; // skip type + class

    // Parse answer records
    for (int a = 0; a < answers && pos + 12 <= len; a++) {
        // Skip name pointer or inline name
        if (buf[pos] == 0xC0) {
            pos += 2;
        } else {
            while (pos < len && buf[pos] != 0x00) pos += buf[pos] + 1;
            pos++;
        }
        if (pos + 10 > len) break;

        // Type, class, TTL
        uint16_t type = ((uint16_t)buf[pos] << 8) | buf[pos + 1];
        pos += 8; // type + class + TTL

        uint16_t rdlength = ((uint16_t)buf[pos] << 8) | buf[pos + 1];
        pos += 2;

        if (type == 0x21 && rdlength >= 1 && pos + rdlength <= len) {
            size_t rdata_end = pos + rdlength;
            uint8_t name_count = buf[pos++];
            glog("[NetBIOS] Host: %s  Names: ", src_ip);
            if (sf != NULL) {
                scan_file_printf(sf, "Host: %s  Names: ", src_ip);
            }

            bool printed = false;
            for (uint8_t i = 0; i < name_count && pos + 18 <= rdata_end; i++) {
                char nb_name[16];
                memcpy(nb_name, &buf[pos], 15);
                nb_name[15] = '\0';
                for (int j = 14; j >= 0 && nb_name[j] == ' '; j--) {
                    nb_name[j] = '\0';
                }
                uint8_t suffix = buf[pos + 15];
                uint16_t nb_flags = ((uint16_t)buf[pos + 16] << 8) | buf[pos + 17];
                pos += 18;

                if (nb_name[0] == '\0') continue;
                glog("%s%s<%02X>/0x%04X", printed ? ", " : "", nb_name, suffix, nb_flags);
                if (sf != NULL) {
                    scan_file_printf(sf, "%s%s<%02X>/0x%04X", printed ? ", " : "", nb_name, suffix, nb_flags);
                }
                printed = true;
            }
            glog("%s\n", printed ? "" : "none");
            if (sf != NULL) {
                scan_file_printf(sf, "%s\n", printed ? "" : "none");
            }
            pos = rdata_end;
        } else if (type == 0x20 && rdlength >= 6 && pos + 6 <= len) {
            uint16_t nb_flags = ((uint16_t)buf[pos] << 8) | buf[pos + 1];
            pos += 2;

            uint32_t ip_addr = ((uint32_t)buf[pos] << 24) |
                               ((uint32_t)buf[pos + 1] << 16) |
                               ((uint32_t)buf[pos + 2] << 8) |
                               buf[pos + 3];
            pos += 4;

            struct in_addr addr;
            addr.s_addr = htonl(ip_addr);

            glog("[NetBIOS] Host: %s  IP: %s  Flags: 0x%04X\n",
                 src_ip, inet_ntoa(addr), nb_flags);

            if (sf != NULL) {
                scan_file_printf(sf, "Host: %s  IP: %s  Flags: 0x%04X\n",
                                 src_ip, inet_ntoa(addr), nb_flags);
            }
        } else {
            pos += rdlength;
        }
    }
}

/**
 * @brief Send NBNS query and listen for responses
 */
static void send_nbns_query(const char *target_ip, bool broadcast,
                            scan_file_t *sf) {
    if (g_network_scan_cancel) return;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket");
        return;
    }

    // Enable broadcast if needed
    int broadcast_enable = 1;
    if (broadcast) {
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
                   &broadcast_enable, sizeof(broadcast_enable));
    }

    // Set receive timeout
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = NETBIOS_RECV_TIMEOUT_MS * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(NETBIOS_PORT);
    inet_pton(AF_INET, target_ip, &dest_addr.sin_addr);

    uint8_t packet[512];
    uint16_t tx_id = (uint16_t)(esp_random() & 0xFFFF);
    size_t pkt_len = build_nbns_query(tx_id, NULL, 0x0021, packet, sizeof(packet));

    if (pkt_len == 0) {
        close(sock);
        return;
    }

    sendto(sock, packet, pkt_len, 0,
           (struct sockaddr *)&dest_addr, sizeof(dest_addr));

    ESP_LOGI(TAG, "Sent NBNS query to %s", target_ip);

    // Listen for responses
    uint8_t rx_buf[512];
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);

    while (!g_network_scan_cancel) {
        int n = recvfrom(sock, rx_buf, sizeof(rx_buf), 0,
                         (struct sockaddr *)&from_addr, &from_len);
        if (n <= 0) break;

        // Verify transaction ID matches
        uint16_t resp_tx = ((uint16_t)rx_buf[0] << 8) | rx_buf[1];
        if (resp_tx == tx_id) {
            char from_ip[16];
            inet_ntop(AF_INET, &from_addr.sin_addr, from_ip, sizeof(from_ip));
            parse_nbns_response(rx_buf, (size_t)n, from_ip, sf);
        }
    }

    close(sock);
}

// ============================================================================
// Public API Implementation
// ============================================================================

/**
 * @brief Scan a specific host for NetBIOS name information
 */
void netbios_scan_host(const char *target_ip) {
    if (target_ip == NULL) {
        ESP_LOGE(TAG, "NULL target IP provided");
        return;
    }

    ESP_LOGI(TAG, "Starting NetBIOS scan on host: %s", target_ip);
    glog("NetBIOS scanning host: %s\n", target_ip);

    g_network_scan_cancel = false;
    send_nbns_query(target_ip, false, NULL);

    glog("NetBIOS scan completed on %s\n", target_ip);
}

/**
 * @brief Scan the local subnet for NetBIOS hosts
 */
void netbios_scan_subnet(void) {
    char subnet_prefix[16];

    if (!get_wifi_subnet_prefix(subnet_prefix, sizeof(subnet_prefix))) {
        glog("NetBIOS Scan: Failed to get subnet prefix - not connected to WiFi?\n");
        return;
    }

    netbios_scan_subnet_prefix(subnet_prefix);
}

void netbios_scan_subnet_prefix(const char *subnet_prefix) {
    if (subnet_prefix == NULL || strlen(subnet_prefix) == 0) {
        glog("NetBIOS Scan: Invalid subnet prefix\n");
        return;
    }

    glog("NetBIOS Scan: Scanning subnet %s*\n", subnet_prefix);

    scan_file_t sf = SCAN_FILE_INIT;
    bool saving = (scan_file_open(&sf, "netbios_scan", "txt") == ESP_OK);

    if (saving) {
        scan_file_printf(&sf, "--- NetBIOS Scan Results (Subnet %s*) ---\n", subnet_prefix);
    }

    g_network_scan_cancel = false;
    glog("NetBIOS Scan: Scanning 254 hosts...\n");

    // Scan all hosts in the subnet (1-254)
    for (int host = 1; host <= 254 && !g_network_scan_cancel; host++) {
        // Progress update every 25 hosts
        if (host % 25 == 0) {
            glog("NetBIOS Scan: Progress %d/254 hosts\n", host);
        }

        char target_ip[16];
        build_ip_string(target_ip, sizeof(target_ip), subnet_prefix, host);

        send_nbns_query(target_ip, false, &sf);

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Also send a broadcast query
    if (!g_network_scan_cancel) {
        char broadcast_ip[32];
        snprintf(broadcast_ip, sizeof(broadcast_ip), "%s255", subnet_prefix);
        glog("NetBIOS Scan: Sending broadcast query to %s\n", broadcast_ip);
        send_nbns_query(broadcast_ip, true, &sf);
    }

    if (g_network_scan_cancel) {
        glog("NetBIOS Scan: Cancelled\n");
    } else {
        glog("NetBIOS Scan: Subnet scan complete\n");
    }

    if (saving) {
        scan_file_printf(&sf, "--- NetBIOS Scan Complete ---\n");
        scan_file_close(&sf);
    }
}
