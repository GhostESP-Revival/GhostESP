/* P4 DHCP diagnostics: log message type and transaction ID, never payloads. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_log.h"

static inline void sta_dhcp_trace(const char *stage, const void *frame,
                                 size_t len, int result)
{
#if CONFIG_IDF_TARGET_ESP32P4
    const uint8_t *p = frame;
    /* Ethernet / IPv4 / UDP. Ignore fragments and unrelated traffic. */
    if (!p || len < 42 || p[12] != 0x08 || p[13] != 0x00 ||
        (p[14] >> 4) != 4 || p[23] != 17 || (p[20] & 0x3f) || p[21]) {
        return;
    }
    size_t ihl = (p[14] & 0x0f) * 4;
    size_t ip_len = ((size_t)p[16] << 8) | p[17];
    if (ihl < 20 || ip_len < ihl + 8 || ip_len > len - 14) return;
    const uint8_t *udp = p + 14 + ihl;
    unsigned src = ((unsigned)udp[0] << 8) | udp[1];
    unsigned dst = ((unsigned)udp[2] << 8) | udp[3];
    if (!((src == 68 && dst == 67) || (src == 67 && dst == 68))) return;
    size_t udp_len = ((size_t)udp[4] << 8) | udp[5];
    if (udp_len < 248 || udp_len > ip_len - ihl) return;
    const uint8_t *dhcp = udp + 8;
    if (dhcp[236] != 99 || dhcp[237] != 130 || dhcp[238] != 83 || dhcp[239] != 99) return;
    unsigned type = 0;
    for (size_t pos = 240; pos < udp_len - 8;) {
        unsigned opt = dhcp[pos++];
        if (opt == 255) break;
        if (opt == 0) continue;
        if (pos >= udp_len - 8) break;
        size_t opt_len = dhcp[pos++];
        if (opt_len > udp_len - 8 - pos) break;
        if (opt == 53 && opt_len == 1) {
            type = dhcp[pos];
            break;
        }
        pos += opt_len;
    }
    const char *name = type == 1 ? "DISCOVER" : type == 2 ? "OFFER" :
                       type == 3 ? "REQUEST" : type == 5 ? "ACK" :
                       type == 6 ? "NAK" : "OTHER";
    ESP_LOGI("P4_DHCP", "%s %s xid=%02x%02x%02x%02x result=%d", stage, name,
             dhcp[4], dhcp[5], dhcp[6], dhcp[7], result);
#else
    (void)stage; (void)frame; (void)len; (void)result;
#endif
}
