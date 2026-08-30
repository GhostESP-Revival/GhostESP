#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* P4/C6 diagnostic protocol. Fixed-width fields; no credentials or payloads. */
#define GHOST_STA_DIAG_VERSION 1u

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t xid[4];
    uint8_t source_mac[6];
    uint8_t client_mac[6];
    uint8_t ip_checksum_ok;
    uint8_t udp_checksum_ok; /* A zero IPv4 UDP checksum is valid. */
} ghost_sta_dhcp_summary_t;

typedef struct __attribute__((packed)) {
    uint32_t version;
    uint32_t host_frames;
    uint32_t wifi_tx_ok;
    uint32_t wifi_tx_fail;
    uint32_t disconnected_drops;
    uint32_t wifi_rx_frames;
    uint32_t dhcp_tx_frames;
    uint32_t dhcp_rx_frames;
    uint32_t offers;
    uint32_t acks;
    uint32_t naks;
    int32_t last_tx_result;
    int32_t mac_result;
    uint8_t radio_mac[6];
    ghost_sta_dhcp_summary_t last_dhcp_tx;
    ghost_sta_dhcp_summary_t last_dhcp_rx;
} ghost_sta_diag_t;

static inline uint16_t ghost_sta_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static inline uint32_t ghost_sta_checksum_sum(const uint8_t *p, size_t len,
                                             uint32_t sum)
{
    while (len >= 2) {
        sum += ghost_sta_be16(p);
        p += 2;
        len -= 2;
    }
    if (len) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return sum;
}

/* Read-only parser; reject truncation, fragmentation and malformed TLVs. */
static inline bool ghost_sta_dhcp_summary(const void *frame, size_t len,
                                          ghost_sta_dhcp_summary_t *out)
{
    const uint8_t *p = frame;
    if (!p || !out || len < 42 || ghost_sta_be16(p + 12) != 0x0800 ||
        (p[14] >> 4) != 4 || p[23] != 17 ||
        (ghost_sta_be16(p + 20) & 0x3fffu)) return false;
    size_t ihl = (p[14] & 15u) * 4u;
    size_t ip_len = ghost_sta_be16(p + 16);
    if (ihl < 20 || ip_len < ihl + 8 || ip_len > len - 14) return false;
    const uint8_t *udp = p + 14 + ihl;
    uint16_t src = ghost_sta_be16(udp), dst = ghost_sta_be16(udp + 2);
    if (!((src == 68 && dst == 67) || (src == 67 && dst == 68))) return false;
    size_t udp_len = ghost_sta_be16(udp + 4);
    if (udp_len < 248 || udp_len > ip_len - ihl) return false;
    const uint8_t *dhcp = udp + 8;
    if (dhcp[236] != 99 || dhcp[237] != 130 ||
        dhcp[238] != 83 || dhcp[239] != 99 || dhcp[2] != 6) return false;
    uint8_t type = 0;
    for (size_t pos = 240; pos < udp_len - 8;) {
        unsigned opt = dhcp[pos++];
        if (opt == 255) break;
        if (opt == 0) continue;
        if (pos >= udp_len - 8) return false;
        size_t opt_len = dhcp[pos++];
        if (opt_len > udp_len - 8 - pos) return false;
        if (opt == 53 && opt_len == 1) type = dhcp[pos];
        pos += opt_len;
    }
    if (!type) return false;
    memset(out, 0, sizeof(*out));
    out->type = type;
    memcpy(out->xid, dhcp + 4, sizeof(out->xid));
    memcpy(out->source_mac, p + 6, sizeof(out->source_mac));
    memcpy(out->client_mac, dhcp + 28, sizeof(out->client_mac));
    out->ip_checksum_ok = ghost_sta_checksum_sum(p + 14, ihl, 0) == 0xffffu;
    uint32_t sum = ghost_sta_checksum_sum(p + 26, 8, 17u + udp_len);
    out->udp_checksum_ok = ghost_sta_be16(udp + 6) == 0 ||
                          ghost_sta_checksum_sum(udp, udp_len, sum) == 0xffffu;
    return true;
}
