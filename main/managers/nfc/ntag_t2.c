#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef CONFIG_NFC_PN532
#include "pn532.h"
#endif
#include "managers/nfc/ntag_t2.h"
#include "managers/nfc/ndef.h"
#include <stdio.h>

#ifdef CONFIG_NFC_PN532
bool ntag_t2_read_user_memory(pn532_io_handle_t io,
                              uint8_t **out_buf,
                              size_t *out_len,
                              NTAG2XX_MODEL *out_model) {
    if (!io || !out_buf || !out_len) return false;
    *out_buf = NULL; *out_len = 0; if (out_model) *out_model = NTAG2XX_UNKNOWN;

    uint8_t page0_3[16] = {0};
    if (ntag2xx_read_page(io, 0, page0_3, sizeof(page0_3)) != ESP_OK) {
        return false;
    }
    uint8_t size_mul_8 = page0_3[14];
    size_t data_bytes = (size_t)size_mul_8 * 8;
    if (data_bytes == 0 || data_bytes > 1024) data_bytes = 1024;
    if (out_model) {
        switch (size_mul_8) {
            case 0x12: *out_model = NTAG2XX_NTAG213; break;
            case 0x3E: *out_model = NTAG2XX_NTAG215; break;
            case 0x6D: *out_model = NTAG2XX_NTAG216; break;
            default: *out_model = NTAG2XX_UNKNOWN; break;
        }
    }
    uint8_t *buf = (uint8_t*)malloc(data_bytes);
    if (!buf) return false;
    size_t copied = 0;
    while (copied < data_bytes) {
        uint8_t page = 4 + (uint8_t)(copied / 4);
        uint8_t tmp[16] = {0};
        if (ntag2xx_read_page(io, page, tmp, sizeof(tmp)) != ESP_OK) {
            free(buf);
            return false;
        }
        size_t remain = data_bytes - copied;
        size_t to_copy = (remain < sizeof(tmp)) ? remain : sizeof(tmp);
        memcpy(buf + copied, tmp, to_copy);
        copied += to_copy;
        for (size_t i = 0; i + 1 < to_copy; ++i) {
            if (tmp[i] == 0xFE) { *out_buf = buf; *out_len = copied - (to_copy - i - 1); return true; }
        }
    }
    *out_buf = buf;
    *out_len = copied;
    return true;
}
#endif

bool ntag_t2_find_ndef(const uint8_t *mem,
                       size_t mem_len,
                       size_t *msg_off,
                       size_t *msg_len) {
    if (!mem || mem_len == 0 || !msg_off || !msg_len) return false;
    size_t pos = 0; size_t end = mem_len;
    while (pos < end) {
        uint8_t tlv = mem[pos++];
        if (tlv == 0x00) continue; // RFU/Null
        if (tlv == 0xFE) break;    // Terminator
        if (pos >= end) break;
        uint32_t len = 0;
        if (mem[pos] != 0xFF) {
            len = mem[pos++];
        } else {
            if (pos + 3 > end) break;
            pos++;
            len = ((uint32_t)mem[pos] << 8) | mem[pos+1];
            pos += 2;
        }
        if (tlv == 0x03) { *msg_off = pos; *msg_len = len; return true; }
        pos += len;
    }
    return false;
}

const char *ntag_t2_model_str(NTAG2XX_MODEL m) {
    switch (m) {
        case NTAG2XX_NTAG213: return "NTAG213";
        case NTAG2XX_NTAG215: return "NTAG215";
        case NTAG2XX_NTAG216: return "NTAG216";
        default: return "NTAG2xx";
    }
}

char *ntag_t2_build_details_from_mem(const uint8_t *mem,
                                     size_t mem_len,
                                     const uint8_t *uid,
                                     uint8_t uid_len,
                                     NTAG2XX_MODEL model) {
    if (!mem || mem_len == 0) {
        size_t cap = 256;
        char *out = (char*)malloc(cap);
        if (!out) return NULL;
        int n = snprintf(out, cap, "Card: %s\nUID:", ntag_t2_model_str(model));
        size_t pos = (n > 0) ? (size_t)n : 0;
        for (uint8_t i = 0; i < uid_len && pos < cap - 4; ++i) {
            n = snprintf(out + pos, cap - pos, " %02X", uid[i]);
            pos += (n > 0) ? (size_t)n : 0;
        }
        (void)snprintf(out + pos, cap - pos, "\nNo NDEF message found\n");
        return out;
    }
    size_t off = 0, len = 0;
    char label_buf[16];
    const char *label = ntag_t2_model_str(model);
    if (ntag_t2_find_ndef(mem, mem_len, &off, &len) && off + len <= mem_len) {
        return ndef_build_details_from_message(mem + off, len, uid, uid_len, label);
    }
    return ndef_build_details_from_message(NULL, 0, uid, uid_len, label);
}
