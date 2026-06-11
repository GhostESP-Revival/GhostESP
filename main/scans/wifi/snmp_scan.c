/**
 * @file snmp_scan.c
 * @brief SNMP probing and enumeration implementation
 *
 * This module handles SNMP probing operations including:
 * - Scanning hosts for SNMP v1/v2c services
 * - Testing common community strings (public, private)
 * - Retrieving sysDescr and basic system information
 */

#include "scans/wifi/snmp_scan.h"
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
#define SNMP_RECV_TIMEOUT_MS 250
#define SNMP_PORT 161
#define SNMP_MAX_COMMUNITIES 2
#define SNMP_BUFFER_SIZE 512

// Module tag for logging
static const char *TAG = "SNMPScan";

// Common SNMP community strings
static const char *SNMP_COMMUNITIES[] = {"public", "private"};

// Shared cancellation flag for all network scans
static volatile bool g_network_scan_cancel = false;

// ============================================================================
// Cancellation Control
// ============================================================================

void snmp_scan_cancel(void) {
    g_network_scan_cancel = true;
}

void snmp_scan_reset_cancel(void) {
    g_network_scan_cancel = false;
}

// ============================================================================
// SNMP Packet Construction
// ============================================================================

/**
 * @brief Encode an integer in ASN.1 BER format
 */
static size_t encode_int(uint8_t *buf, size_t offset, int32_t value) {
    buf[offset++] = 0x02; // INTEGER
    if (value >= -128 && value <= 127) {
        buf[offset++] = 0x01;
        buf[offset++] = (uint8_t)(value & 0xFF);
    } else {
        buf[offset++] = 0x02;
        buf[offset++] = (uint8_t)((value >> 8) & 0xFF);
        buf[offset++] = (uint8_t)(value & 0xFF);
    }
    return offset;
}

/**
 * @brief Encode an OID in ASN.1 BER format
 *
 * OID: 1.3.6.1.2.1.1.1.0 (sysDescr)
 */
static size_t encode_oid(uint8_t *buf, size_t offset) {
    buf[offset++] = 0x06; // OBJECT IDENTIFIER
    // 1.3.6.1.2.1.1.1.0 = 9 bytes
    // 1*40+3 = 43, then 6, 1, 2, 1, 1, 1, 0
    static const uint8_t oid[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x01, 0x01, 0x00};
    buf[offset++] = sizeof(oid);
    memcpy(&buf[offset], oid, sizeof(oid));
    offset += sizeof(oid);
    return offset;
}

/**
 * @brief Encode an OCTET STRING in ASN.1 BER format
 */
static size_t encode_octet_string(uint8_t *buf, size_t offset, const char *str) {
    size_t len = strlen(str);
    buf[offset++] = 0x04; // OCTET STRING
    buf[offset++] = (uint8_t)len;
    memcpy(&buf[offset], str, len);
    offset += len;
    return offset;
}

/**
 * @brief Build SNMPv1 GetRequest packet for sysDescr
 */
static size_t build_snmp_get_request(uint8_t *buf, size_t buf_size,
                                      const char *community, uint32_t request_id) {
    if (buf_size < 128) return 0;

    // Build inner content first
    size_t inner = 0;

    // Version: 0 (SNMPv1)
    inner = encode_int(buf, inner, 0);

    // Community string
    inner = encode_octet_string(buf, inner, community);

    // PDU: GetRequest
    // Build PDU content
    size_t pdu_start = inner;
    inner = encode_int(buf, inner, (int32_t)request_id);
    inner = encode_int(buf, inner, 0); // error-status
    inner = encode_int(buf, inner, 0); // error-index

    // Variable-bindings sequence
    size_t vblist_start = inner;
    // VarBind sequence
    size_t vb_start = inner;
    inner = encode_oid(buf, inner); // OID
    inner = encode_int(buf, inner, 0); // value = NULL (but encode as integer 0 for simplicity)
    // Actually for GetRequest the value should be NULL
    buf[inner++] = 0x05; // NULL
    buf[inner++] = 0x00;

    // Wrap VarBind
    size_t vb_len = inner - vb_start;
    memmove(&buf[vb_start + 2], &buf[vb_start], vb_len);
    buf[vb_start] = 0x30;
    buf[vb_start + 1] = (uint8_t)vb_len;
    inner += 2;

    // Wrap VarBindList
    size_t vblist_len = inner - vblist_start;
    memmove(&buf[vblist_start + 2], &buf[vblist_start], vblist_len);
    buf[vblist_start] = 0x30;
    buf[vblist_start + 1] = (uint8_t)vblist_len;
    inner += 2;

    // Wrap PDU (GetRequest = 0xA0)
    size_t pdu_len = inner - pdu_start;
    memmove(&buf[pdu_start + 2], &buf[pdu_start], pdu_len);
    buf[pdu_start] = 0xA0;
    buf[pdu_start + 1] = (uint8_t)pdu_len;
    inner += 2;

    // Wrap SNMP message
    size_t msg_len = inner;
    memmove(&buf[2], &buf[0], msg_len);
    buf[0] = 0x30;
    buf[1] = (uint8_t)msg_len;
    return msg_len + 2;
}

// ============================================================================
// SNMP Response Parsing
// ============================================================================

/**
 * @brief Parse SNMP response to extract sysDescr string
 */
static bool parse_snmp_response(const uint8_t *buf, size_t len, char *out, size_t out_size) {
    if (len < 20 || buf[0] != 0x30) return false;

    size_t pos = 2;
    if (buf[1] & 0x80) {
        int extra = buf[1] & 0x7F;
        pos += extra;
    }

    // Skip version
    if (buf[pos] != 0x02) return false;
    pos += 2 + buf[pos + 1];

    // Skip community
    if (buf[pos] != 0x04) return false;
    pos += 2 + buf[pos + 1];

    // PDU type (should be 0xA2 = GetResponse)
    if (buf[pos] != 0xA2) return false;
    pos += 2;
    if (buf[pos - 1] & 0x80) {
        int extra = buf[pos - 1] & 0x7F;
        pos += extra;
    }

    // Skip request-id, error-status, error-index
    for (int i = 0; i < 3; i++) {
        if (buf[pos] != 0x02) return false;
        pos += 2 + buf[pos + 1];
    }

    // VarBindList
    if (buf[pos] != 0x30) return false;
    pos += 2;

    // VarBind
    if (buf[pos] != 0x30) return false;
    pos += 2;

    // Skip OID
    if (buf[pos] != 0x06) return false;
    pos += 2 + buf[pos + 1];

    // Value (should be OCTET STRING for sysDescr)
    if (buf[pos] != 0x04) return false;
    size_t val_len = buf[pos + 1];
    pos += 2;

    if (val_len >= out_size) val_len = out_size - 1;
    memcpy(out, &buf[pos], val_len);
    out[val_len] = '\0';
    return true;
}

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Probe a single host for SNMP with given community
 */
static bool probe_snmp(const char *target_ip, const char *community,
                        char *sysdescr, size_t sysdescr_size, scan_file_t *sf) {
    if (g_network_scan_cancel) return false;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return false;
    }

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = SNMP_RECV_TIMEOUT_MS * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(SNMP_PORT);
    inet_pton(AF_INET, target_ip, &dest_addr.sin_addr);

    uint8_t packet[256];
    uint32_t req_id = esp_random() & 0x7FFFFFFF;
    size_t pkt_len = build_snmp_get_request(packet, sizeof(packet), community, req_id);

    if (pkt_len == 0) {
        close(sock);
        return false;
    }

    sendto(sock, packet, pkt_len, 0,
           (struct sockaddr *)&dest_addr, sizeof(dest_addr));

    uint8_t rx_buf[SNMP_BUFFER_SIZE];
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);

    int n = recvfrom(sock, rx_buf, sizeof(rx_buf), 0,
                     (struct sockaddr *)&from_addr, &from_len);
    close(sock);

    if (n > 0) {
        if (parse_snmp_response(rx_buf, (size_t)n, sysdescr, sysdescr_size)) {
            if (strlen(sysdescr) > 0) {
                glog("[SNMP] %s (community: %s) sysDescr: %s\n",
                     target_ip, community, sysdescr);
                if (sf != NULL) {
                    scan_file_printf(sf, "[%s] community=%s sysDescr=%s\n",
                                     target_ip, community, sysdescr);
                }
                return true;
            }
        }
    }

    return false;
}

// ============================================================================
// Public API Implementation
// ============================================================================

/**
 * @brief Scan a specific host for SNMP services
 */
void snmp_scan_host(const char *target_ip) {
    if (target_ip == NULL) {
        ESP_LOGE(TAG, "NULL target IP provided");
        return;
    }

    ESP_LOGI(TAG, "Starting SNMP scan on host: %s", target_ip);
    glog("SNMP scanning host: %s\n", target_ip);

    char sysdescr[256];
    bool found = false;

    for (int i = 0; i < SNMP_MAX_COMMUNITIES; i++) {
        if (probe_snmp(target_ip, SNMP_COMMUNITIES[i], sysdescr, sizeof(sysdescr), NULL)) {
            found = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!found) {
        glog("SNMP scan on %s: No response\n", target_ip);
    }

    glog("SNMP scan completed on %s\n", target_ip);
}

/**
 * @brief Scan the local subnet for SNMP services
 */
void snmp_scan_subnet(void) {
    char subnet_prefix[16];

    if (!get_wifi_subnet_prefix(subnet_prefix, sizeof(subnet_prefix))) {
        glog("SNMP Scan: Failed to get subnet prefix - not connected to WiFi?\n");
        return;
    }

    glog("SNMP Scan: Scanning subnet %s*\n", subnet_prefix);

    scan_file_t sf = SCAN_FILE_INIT;
    bool saving = (scan_file_open(&sf, "snmp_scan", "txt") == ESP_OK);

    if (saving) {
        scan_file_printf(&sf, "--- SNMP Scan Results (Subnet %s*) ---\n", subnet_prefix);
    }

    int hosts_found = 0;
    char sysdescr[256];
    g_network_scan_cancel = false;

    glog("SNMP Scan: Scanning 254 hosts...\n");

    // Scan all hosts in the subnet (1-254)
    for (int host = 1; host <= 254 && !g_network_scan_cancel; host++) {
        // Progress update every 25 hosts
        if (host % 25 == 0) {
            glog("SNMP Scan: Progress %d/254 hosts\n", host);
        }

        char target_ip[16];
        build_ip_string(target_ip, sizeof(target_ip), subnet_prefix, host);

        bool found = false;
        for (int i = 0; i < SNMP_MAX_COMMUNITIES && !found && !g_network_scan_cancel; i++) {
            if (probe_snmp(target_ip, SNMP_COMMUNITIES[i], sysdescr, sizeof(sysdescr), &sf)) {
                found = true;
                hosts_found++;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (g_network_scan_cancel) {
        glog("SNMP Scan: Cancelled. Found %d SNMP host(s)\n", hosts_found);
    } else {
        glog("SNMP Scan: Subnet scan complete - found %d SNMP host(s)\n", hosts_found);
    }

    if (saving) {
        scan_file_printf(&sf, "--- SNMP Scan Summary ---\n");
        scan_file_printf(&sf, "Hosts with SNMP: %d\n", hosts_found);
        scan_file_close(&sf);
    }
}
