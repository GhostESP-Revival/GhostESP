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
#define SNMP_RECV_TIMEOUT_MS 250
#define SNMP_PORT 161
#define SNMP_MAX_COMMUNITIES 2
#define SNMP_BUFFER_SIZE 512
#define SNMP_WALK_MAX_ENTRIES 10000

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

static bool asn1_read_len(const uint8_t *buf, size_t len, size_t *pos, size_t *out_len) {
    if (*pos >= len) return false;

    uint8_t first = buf[(*pos)++];
    if ((first & 0x80) == 0) {
        *out_len = first;
        return *pos + *out_len <= len;
    }

    uint8_t bytes = first & 0x7F;
    if (bytes == 0 || bytes > sizeof(size_t) || *pos + bytes > len) return false;

    size_t value = 0;
    for (uint8_t i = 0; i < bytes; i++) {
        value = (value << 8) | buf[(*pos)++];
    }
    *out_len = value;
    return *pos + *out_len <= len;
}

static bool asn1_skip_tlv(const uint8_t *buf, size_t len, size_t *pos, uint8_t expected_tag) {
    if (*pos >= len || buf[(*pos)++] != expected_tag) return false;
    size_t value_len = 0;
    if (!asn1_read_len(buf, len, pos, &value_len)) return false;
    *pos += value_len;
    return *pos <= len;
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

    size_t pos = 1;
    size_t seq_len = 0;
    if (!asn1_read_len(buf, len, &pos, &seq_len)) return false;
    size_t seq_end = pos + seq_len;
    if (seq_end > len) return false;

    // Skip version
    if (!asn1_skip_tlv(buf, seq_end, &pos, 0x02)) return false;

    // Skip community
    if (!asn1_skip_tlv(buf, seq_end, &pos, 0x04)) return false;

    // PDU type (should be 0xA2 = GetResponse)
    if (pos >= seq_end || buf[pos++] != 0xA2) return false;
    size_t pdu_len = 0;
    if (!asn1_read_len(buf, seq_end, &pos, &pdu_len)) return false;
    size_t pdu_end = pos + pdu_len;
    if (pdu_end > seq_end) return false;

    // Skip request-id, error-status, error-index
    for (int i = 0; i < 3; i++) {
        if (!asn1_skip_tlv(buf, pdu_end, &pos, 0x02)) return false;
    }

    // VarBindList
    if (pos >= pdu_end || buf[pos++] != 0x30) return false;
    size_t vblist_len = 0;
    if (!asn1_read_len(buf, pdu_end, &pos, &vblist_len)) return false;
    size_t vblist_end = pos + vblist_len;
    if (vblist_end > pdu_end) return false;

    // VarBind
    if (pos >= vblist_end || buf[pos++] != 0x30) return false;
    size_t vb_len = 0;
    if (!asn1_read_len(buf, vblist_end, &pos, &vb_len)) return false;
    size_t vb_end = pos + vb_len;
    if (vb_end > vblist_end) return false;

    // Skip OID
    if (!asn1_skip_tlv(buf, vb_end, &pos, 0x06)) return false;

    // Value (should be OCTET STRING for sysDescr)
    if (pos >= vb_end || buf[pos++] != 0x04) return false;
    size_t val_len = 0;
    if (!asn1_read_len(buf, vb_end, &pos, &val_len)) return false;
    if (pos + val_len > vb_end) return false;

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
    uint32_t req_id = esp_random() & 0x7FFF;
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
    g_network_scan_cancel = false;

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

    snmp_scan_subnet_prefix(subnet_prefix);
}

void snmp_scan_subnet_prefix(const char *subnet_prefix) {
    if (subnet_prefix == NULL || strlen(subnet_prefix) == 0) {
        glog("SNMP Scan: Invalid subnet prefix\n");
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

// ============================================================================
// SNMP Walk Implementation
// ============================================================================

#define OID_SYSTEM      "1.3.6.1.2.1.1"
#define OID_INTERFACES  "1.3.6.1.2.1.2"
#define OID_IP          "1.3.6.1.2.1.4"

static size_t build_snmp_getnext_request(uint8_t *buf, size_t buf_size,
                                          const char *community, uint32_t request_id,
                                          const uint8_t *oid, size_t oid_len) {
    if (buf_size < 128) return 0;

    size_t inner = 0;
    inner = encode_int(buf, inner, 0);
    inner = encode_octet_string(buf, inner, community);

    size_t pdu_start = inner;
    inner = encode_int(buf, inner, (int32_t)request_id);
    inner = encode_int(buf, inner, 0);
    inner = encode_int(buf, inner, 0);

    size_t vblist_start = inner;
    size_t vb_start = inner;

    buf[inner++] = 0x06;
    buf[inner++] = (uint8_t)oid_len;
    memcpy(&buf[inner], oid, oid_len);
    inner += oid_len;

    buf[inner++] = 0x05;
    buf[inner++] = 0x00;

    size_t vb_len = inner - vb_start;
    memmove(&buf[vb_start + 2], &buf[vb_start], vb_len);
    buf[vb_start] = 0x30;
    buf[vb_start + 1] = (uint8_t)vb_len;
    inner += 2;

    size_t vblist_len = inner - vblist_start;
    memmove(&buf[vblist_start + 2], &buf[vblist_start], vblist_len);
    buf[vblist_start] = 0x30;
    buf[vblist_start + 1] = (uint8_t)vblist_len;
    inner += 2;

    size_t pdu_len = inner - pdu_start;
    memmove(&buf[pdu_start + 2], &buf[pdu_start], pdu_len);
    buf[pdu_start] = 0xA1;
    buf[pdu_start + 1] = (uint8_t)pdu_len;
    inner += 2;

    size_t msg_len = inner;
    memmove(&buf[2], &buf[0], msg_len);
    buf[0] = 0x30;
    buf[1] = (uint8_t)msg_len;
    return msg_len + 2;
}

static bool parse_snmp_getnext_response(const uint8_t *buf, size_t len,
                                         uint8_t *out_oid, size_t *out_oid_len,
                                         char *out_value, size_t out_value_size,
                                         uint8_t *out_type) {
    if (len < 20 || buf[0] != 0x30) return false;

    size_t pos = 1;
    size_t seq_len = 0;
    if (!asn1_read_len(buf, len, &pos, &seq_len)) return false;
    size_t seq_end = pos + seq_len;
    if (seq_end > len) return false;

    if (!asn1_skip_tlv(buf, seq_end, &pos, 0x02)) return false;
    if (!asn1_skip_tlv(buf, seq_end, &pos, 0x04)) return false;

    if (pos >= seq_end) return false;
    uint8_t pdu_type = buf[pos++];
    if (pdu_type != 0xA2) return false;

    size_t pdu_len = 0;
    if (!asn1_read_len(buf, seq_end, &pos, &pdu_len)) return false;
    size_t pdu_end = pos + pdu_len;
    if (pdu_end > seq_end) return false;

    for (int i = 0; i < 3; i++) {
        if (!asn1_skip_tlv(buf, pdu_end, &pos, 0x02)) return false;
    }

    if (pos >= pdu_end || buf[pos++] != 0x30) return false;
    size_t vblist_len = 0;
    if (!asn1_read_len(buf, pdu_end, &pos, &vblist_len)) return false;
    size_t vblist_end = pos + vblist_len;
    if (vblist_end > pdu_end) return false;

    if (pos >= vblist_end || buf[pos++] != 0x30) return false;
    size_t vb_len = 0;
    if (!asn1_read_len(buf, vblist_end, &pos, &vb_len)) return false;
    size_t vb_end = pos + vb_len;
    if (vb_end > vblist_end) return false;

    if (pos >= vb_end || buf[pos++] != 0x06) return false;
    size_t oid_data_len = 0;
    if (!asn1_read_len(buf, vb_end, &pos, &oid_data_len)) return false;
    if (pos + oid_data_len > vb_end) return false;

    if (out_oid && out_oid_len) {
        if (oid_data_len > 64) oid_data_len = 64;
        memcpy(out_oid, &buf[pos], oid_data_len);
        *out_oid_len = oid_data_len;
    }
    pos += oid_data_len;

    if (pos >= vb_end) return false;
    uint8_t val_type = buf[pos++];
    if (out_type) *out_type = val_type;

    size_t val_len = 0;
    if (!asn1_read_len(buf, vb_end, &pos, &val_len)) return false;
    if (pos + val_len > vb_end) return false;

    if (out_value && out_value_size > 0) {
        if (val_type == 0x04) {
            size_t copy_len = val_len;
            if (copy_len >= out_value_size) copy_len = out_value_size - 1;
            memcpy(out_value, &buf[pos], copy_len);
            out_value[copy_len] = '\0';
        } else if (val_type == 0x02) {
            int32_t ival = 0;
            if (val_len > 0) {
                ival = (int8_t)buf[pos];
                for (size_t i = 1; i < val_len; i++) {
                    ival = (ival << 8) | buf[pos + i];
                }
            }
            snprintf(out_value, out_value_size, "%ld", (long)ival);
        } else if (val_type == 0x06) {
            size_t out_pos = 0;
            for (size_t i = 0; i < val_len && out_pos < out_value_size - 1; i++) {
                uint8_t b = buf[pos + i];
                int w;
                if (i == 0) {
                    w = snprintf(&out_value[out_pos], out_value_size - out_pos,
                                 "%d.%d", b / 40, b % 40);
                } else if (b & 0x80) {
                    uint32_t val = b & 0x7F;
                    while (i + 1 < val_len && (buf[pos + i + 1] & 0x80)) {
                        i++;
                        val = (val << 7) | (buf[pos + i] & 0x7F);
                    }
                    if (i + 1 < val_len) { i++; val = (val << 7) | buf[pos + i]; }
                    w = snprintf(&out_value[out_pos], out_value_size - out_pos,
                                 ".%lu", (unsigned long)val);
                } else {
                    w = snprintf(&out_value[out_pos], out_value_size - out_pos,
                                 ".%u", b);
                }
                if (w >= 0 && (size_t)w < out_value_size - out_pos) {
                    out_pos += (size_t)w;
                } else {
                    out_pos = out_value_size - 1;
                    break;
                }
            }
            out_value[out_pos] = '\0';
        } else if (val_type == 0x05) {
            strncpy(out_value, "NULL", out_value_size);
        } else {
            size_t out_pos = 0;
            for (size_t i = 0; i < val_len && out_pos + 3 < out_value_size; i++) {
                int w = snprintf(&out_value[out_pos], out_value_size - out_pos,
                                 "%02X", buf[pos + i]);
                if (w > 0) out_pos += (size_t)w;
            }
            out_value[out_pos] = '\0';
        }
    }

    return true;
}

static void format_oid(const uint8_t *oid, size_t oid_len, char *out, size_t out_size) {
    if (oid_len == 0 || out_size < 4) {
        if (out_size > 0) out[0] = '\0';
        return;
    }
    size_t pos = 0;
    for (size_t i = 0; i < oid_len && pos < out_size - 1; i++) {
        int w;
        if (i == 0) {
            w = snprintf(&out[pos], out_size - pos, "%d.%d", oid[0] / 40, oid[0] % 40);
        } else if (oid[i] & 0x80) {
            uint32_t val = oid[i] & 0x7F;
            while (i + 1 < oid_len && (oid[i + 1] & 0x80)) { i++; val = (val << 7) | (oid[i] & 0x7F); }
            if (i + 1 < oid_len) { i++; val = (val << 7) | oid[i]; }
            w = snprintf(&out[pos], out_size - pos, ".%lu", (unsigned long)val);
        } else {
            w = snprintf(&out[pos], out_size - pos, ".%u", oid[i]);
        }
        if (w >= 0 && (size_t)w < out_size - pos) {
            pos += (size_t)w;
        } else {
            pos = out_size - 1;
            break;
        }
    }
    out[pos] = '\0';
}

static bool oid_starts_with(const uint8_t *oid_a, size_t len_a,
                             const uint8_t *prefix, size_t prefix_len) {
    if (len_a < prefix_len) return false;
    return memcmp(oid_a, prefix, prefix_len) == 0;
}

static size_t parse_oid_string(const char *str, uint8_t *out, size_t out_size) {
    if (!str || !out || out_size < 2) return 0;
    int parts[16];
    int num_parts = 0;
    const char *p = str;
    while (*p && num_parts < 16) {
        int val = 0;
        bool found = false;
        while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; found = true; }
        if (!found) break;
        parts[num_parts++] = val;
        if (*p == '.') p++;
    }
    if (num_parts < 2) return 0;
    size_t pos = 0;
    out[pos++] = (uint8_t)(parts[0] * 40 + parts[1]);
    for (int i = 2; i < num_parts && pos < out_size; i++) {
        int val = parts[i];
        if (val < 128) {
            out[pos++] = (uint8_t)val;
        } else {
            uint8_t tmp[5];
            int tpos = 0;
            tmp[tpos++] = (uint8_t)(val & 0x7F);
            val >>= 7;
            while (val > 0 && tpos < 5) { tmp[tpos++] = (uint8_t)((val & 0x7F) | 0x80); val >>= 7; }
            for (int j = tpos - 1; j >= 0 && pos < out_size; j--) out[pos++] = tmp[j];
        }
    }
    return pos;
}

static void snmp_walk_host_internal(const char *target_ip, const char *community,
                                     const char *oid_root, scan_file_t *sf) {
    uint8_t root_oid[64];
    size_t root_oid_len = parse_oid_string(oid_root, root_oid, sizeof(root_oid));
    if (root_oid_len == 0) {
        glog("[SNMP-WALK] Invalid OID: %s\n", oid_root);
        return;
    }

    uint8_t current_oid[64];
    memcpy(current_oid, root_oid, root_oid_len);
    size_t current_oid_len = root_oid_len;

    int walk_count = 0;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return;

    struct timeval tv = { .tv_sec = 0, .tv_usec = SNMP_RECV_TIMEOUT_MS * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest_addr = {0};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(SNMP_PORT);
    inet_pton(AF_INET, target_ip, &dest_addr.sin_addr);

    while (!g_network_scan_cancel && walk_count < SNMP_WALK_MAX_ENTRIES) {
        uint8_t packet[512];
        uint32_t req_id = esp_random() & 0x7FFF;
        size_t pkt_len = build_snmp_getnext_request(packet, sizeof(packet),
                                                     community, req_id,
                                                     current_oid, current_oid_len);
        if (pkt_len == 0) break;

        sendto(sock, packet, pkt_len, 0,
               (struct sockaddr *)&dest_addr, sizeof(dest_addr));

        uint8_t rx_buf[SNMP_BUFFER_SIZE];
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        int n = recvfrom(sock, rx_buf, sizeof(rx_buf), 0,
                         (struct sockaddr *)&from_addr, &from_len);
        if (n <= 0) break;

        size_t check_pos = 1;
        size_t check_len = 0;
        if (!asn1_read_len(rx_buf, (size_t)n, &check_pos, &check_len)) break;
        size_t check_end = check_pos + check_len;
        if (!asn1_skip_tlv(rx_buf, check_end, &check_pos, 0x02)) break;
        if (!asn1_skip_tlv(rx_buf, check_end, &check_pos, 0x04)) break;
        if (check_pos >= check_end || rx_buf[check_pos] != 0xA2) break;

        uint8_t resp_oid[64];
        size_t resp_oid_len = 0;
        char value[256];
        uint8_t val_type = 0;

        if (!parse_snmp_getnext_response(rx_buf, (size_t)n,
                                          resp_oid, &resp_oid_len,
                                          value, sizeof(value), &val_type)) break;

        if (!oid_starts_with(resp_oid, resp_oid_len, root_oid, root_oid_len)) break;

        if (val_type == 0x80 || val_type == 0x81 || val_type == 0x82) break;

        char oid_str[128];
        format_oid(resp_oid, resp_oid_len, oid_str, sizeof(oid_str));

        const char *type_name = "?";
        switch (val_type) {
            case 0x02: type_name = "INTEGER"; break;
            case 0x04: type_name = "STRING"; break;
            case 0x06: type_name = "OID"; break;
            case 0x43: type_name = "TIMETICKS"; break;
            case 0x41: type_name = "COUNTER"; break;
            case 0x42: type_name = "GAUGE"; break;
            case 0x05: type_name = "NULL"; break;
        }

        glog("[SNMP-WALK] %s = %s (%s)\n", oid_str, value, type_name);
        if (sf) scan_file_printf(sf, "%s,%s,%s,%s\n", target_ip, oid_str, type_name, value);

        walk_count++;
        memcpy(current_oid, resp_oid, resp_oid_len);
        current_oid_len = resp_oid_len;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    close(sock);

    if (walk_count == 0) {
        glog("[SNMP-WALK] %s: No entries under OID %s\n", target_ip, oid_root);
    } else if (walk_count >= SNMP_WALK_MAX_ENTRIES) {
        glog("[SNMP-WALK] %s: Reached %d entry limit under OID %s\n",
             target_ip, SNMP_WALK_MAX_ENTRIES, oid_root);
    }
}

void snmp_walk_host(const char *target_ip, const char *oid_root) {
    if (target_ip == NULL) return;

    const char *oid = oid_root ? oid_root : OID_SYSTEM;
    glog("SNMP walk on %s (OID %s)...\n", target_ip, oid);
    g_network_scan_cancel = false;

    for (int i = 0; i < SNMP_MAX_COMMUNITIES && !g_network_scan_cancel; i++) {
        glog("[SNMP-WALK] Trying community: %s\n", SNMP_COMMUNITIES[i]);
        snmp_walk_host_internal(target_ip, SNMP_COMMUNITIES[i], oid, NULL);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    glog("SNMP walk completed on %s\n", target_ip);
}

void snmp_walk_subnet(const char *oid_root) {
    char subnet_prefix[16];
    if (!get_wifi_subnet_prefix(subnet_prefix, sizeof(subnet_prefix))) {
        glog("SNMP Walk: Failed to get subnet prefix\n");
        return;
    }
    snmp_walk_subnet_prefix(subnet_prefix, oid_root);
}

void snmp_walk_subnet_prefix(const char *subnet_prefix, const char *oid_root) {
    if (subnet_prefix == NULL || strlen(subnet_prefix) == 0) return;

    const char *oid = oid_root ? oid_root : OID_SYSTEM;
    glog("SNMP Walk: Walking subnet %s* OID %s\n", subnet_prefix, oid);

    scan_file_t sf = SCAN_FILE_INIT;
    bool saving = (scan_file_open(&sf, "snmp_walk", "txt") == ESP_OK);
    if (saving) scan_file_printf(&sf, "--- SNMP Walk Results (Subnet %s* OID %s) ---\n", subnet_prefix, oid);

    int hosts_walked = 0;
    g_network_scan_cancel = false;

    for (int host = 1; host <= 254 && !g_network_scan_cancel; host++) {
        if (host % 5 == 0) glog("SNMP Walk: Progress %d/254\n", host);

        char target_ip[16];
        build_ip_string(target_ip, sizeof(target_ip), subnet_prefix, host);

        char sysdescr[256];
        bool has_snmp = false;
        for (int i = 0; i < SNMP_MAX_COMMUNITIES && !has_snmp && !g_network_scan_cancel; i++) {
            if (probe_snmp(target_ip, SNMP_COMMUNITIES[i], sysdescr, sizeof(sysdescr), NULL))
                has_snmp = true;
            vTaskDelay(pdMS_TO_TICKS(25));
        }

        if (has_snmp && !g_network_scan_cancel) {
            glog("[SNMP-WALK] Found SNMP on %s, walking...\n", target_ip);
            for (int i = 0; i < SNMP_MAX_COMMUNITIES && !g_network_scan_cancel; i++) {
                snmp_walk_host_internal(target_ip, SNMP_COMMUNITIES[i], oid, &sf);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            hosts_walked++;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (g_network_scan_cancel) {
        glog("SNMP Walk: Cancelled. Walked %d host(s)\n", hosts_walked);
    } else {
        glog("SNMP Walk: Complete. Walked %d host(s)\n", hosts_walked);
    }
    if (saving) {
        scan_file_printf(&sf, "--- SNMP Walk Summary ---\n");
        scan_file_printf(&sf, "Hosts walked: %d\n", hosts_walked);
        scan_file_close(&sf);
    }
}
