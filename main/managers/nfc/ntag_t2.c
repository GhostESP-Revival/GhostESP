#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_ST25R3916)
#include "pn532.h"
#endif
#include "managers/nfc/ntag_t2.h"
#include "managers/nfc/ndef.h"
#include <stdio.h>

uint16_t ntag_t2_pages_for_model(NTAG2XX_MODEL model) {
    switch (model) {
        case NTAG2XX_NTAG213: return 45;
        case NTAG2XX_NTAG215: return 135;
        case NTAG2XX_NTAG216: return 231;
        default: return 0;
    }
}

uint16_t ntag_t2_user_bytes_for_model(NTAG2XX_MODEL model) {
    switch (model) {
        case NTAG2XX_NTAG213: return 144;
        case NTAG2XX_NTAG215: return 504;
        case NTAG2XX_NTAG216: return 888;
        default: return 0;
    }
}

uint8_t ntag_t2_last_user_page_for_model(NTAG2XX_MODEL model) {
    switch (model) {
        case NTAG2XX_NTAG213: return 39;
        case NTAG2XX_NTAG215: return 129;
        case NTAG2XX_NTAG216: return 225;
        default: return 0;
    }
}

NTAG2XX_MODEL ntag_t2_model_from_cc_size(uint8_t cc_size) {
    switch (cc_size) {
        case 0x12: return NTAG2XX_NTAG213;
        case 0x3E:
        case 0x3F: return NTAG2XX_NTAG215;
        case 0x6D:
        case 0x6F: return NTAG2XX_NTAG216;
        default: return NTAG2XX_UNKNOWN;
    }
}

NTAG2XX_MODEL ntag_t2_model_from_version(const uint8_t version[8]) {
    if (!version) return NTAG2XX_UNKNOWN;
    if (version[0] != 0x00 || version[1] != 0x04) return NTAG2XX_UNKNOWN;
    switch (version[6]) {
        case 0x0F: return NTAG2XX_NTAG213;
        case 0x11: return NTAG2XX_NTAG215;
        case 0x13: return NTAG2XX_NTAG216;
        default: return NTAG2XX_UNKNOWN;
    }
}

static void ntag_t2_fill_layout(ntag_t2_info_t *info) {
    if (!info) return;
    info->pages_total = ntag_t2_pages_for_model(info->model);
    info->first_user_page = 4;
    info->last_user_page = ntag_t2_last_user_page_for_model(info->model);
    info->user_bytes = ntag_t2_user_bytes_for_model(info->model);
}

static bool ntag_t2_static_locks_page(const ntag_t2_info_t *info, uint8_t page) {
    if (!info || page < 3 || page > 15) return false;
    uint16_t locks = (uint16_t)info->page0_3[10] | ((uint16_t)info->page0_3[11] << 8);
    uint8_t bit = (uint8_t)(page - 3);
    return (locks & (1U << bit)) != 0;
}

bool ntag_t2_can_write_page(const ntag_t2_info_t *info, uint8_t page,
                            char *reason, size_t reason_len) {
    const char *why = NULL;
    if (!info) why = "missing tag info";
    else if (page < 4) why = "manufacturer/CC pages are not writable";
    else if (info->last_user_page && page > info->last_user_page) why = "outside user memory";
    else if (info->cc_read_only) why = "capability container marks tag read-only";
    else if (info->password_protected && page >= info->auth0) why = "page is password protected";
    else if (ntag_t2_static_locks_page(info, page)) why = "page is statically locked";
    else if (info->dynamic_locked && page > 15) why = "dynamic lock bytes are set";

    if (why) {
        if (reason && reason_len) snprintf(reason, reason_len, "%s", why);
        return false;
    }
    if (reason && reason_len) reason[0] = '\0';
    return true;
}

#if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_ST25R3916)
bool ntag_t2_read_info(pn532_io_handle_t io, ntag_t2_info_t *info) {
    if (!io || !info) return false;
    memset(info, 0, sizeof(*info));
    info->model = NTAG2XX_UNKNOWN;
    info->auth0 = 0xFF;

    if (ntag2xx_read_page(io, 0, info->page0_3, sizeof(info->page0_3)) != ESP_OK) {
        return false;
    }

    memcpy(info->cc, &info->page0_3[12], sizeof(info->cc));
    info->cc_valid = (info->cc[0] == 0xE1 || info->cc[0] == 0xE2);
    info->cc_read_only = info->cc_valid && ((info->cc[3] & 0x0F) != 0);
    info->static_locked = (info->page0_3[10] != 0 || info->page0_3[11] != 0);

    if (ntag2xx_get_version(io, info->version) == ESP_OK) {
        info->version_valid = true;
        info->model = ntag_t2_model_from_version(info->version);
    }
    if (info->model == NTAG2XX_UNKNOWN && info->cc_valid) {
        info->model = ntag_t2_model_from_cc_size(info->cc[2]);
    }
    ntag_t2_fill_layout(info);
    if (info->pages_total == 0 && info->cc_valid) {
        uint16_t bytes = (uint16_t)info->cc[2] * 8;
        info->user_bytes = bytes;
        info->first_user_page = 4;
        info->last_user_page = (bytes >= 4) ? (uint8_t)(3 + bytes / 4) : 0;
        info->pages_total = (uint16_t)info->last_user_page + 1;
    }

    if (ntag2xx_read_signature(io, info->signature) == ESP_OK) {
        info->signature_valid = true;
    }
    for (uint8_t i = 0; i < 3; i++) {
        uint32_t value = 0;
        if (ntag2xx_read_counter(io, i, &value) == ESP_OK) {
            info->counter_valid[i] = true;
            info->counter[i] = value;
        }
        uint8_t tearing = 0;
        if (ntag2xx_read_tearing(io, i, &tearing) == ESP_OK) {
            info->tearing_valid[i] = true;
            info->tearing[i] = tearing;
        }
    }

    if (info->last_user_page >= 4) {
        uint8_t dyn[16] = {0};
        uint8_t dyn_page = (uint8_t)(info->last_user_page + 1);
        if (ntag2xx_read_page(io, dyn_page, dyn, sizeof(dyn)) == ESP_OK) {
            memcpy(info->dynamic_lock, dyn, sizeof(info->dynamic_lock));
            info->dynamic_locked = (dyn[0] != 0 || dyn[1] != 0 || dyn[2] != 0);
        }
    }

    if (info->last_user_page >= 4) {
        uint8_t cfg[16] = {0};
        uint8_t cfg_page = (uint8_t)(info->last_user_page + 2);
        if (ntag2xx_read_page(io, cfg_page, cfg, sizeof(cfg)) == ESP_OK) {
            info->config_valid = true;
            info->auth0 = cfg[3];
            info->access = cfg[4];
            info->password_protected = (info->auth0 != 0xFF && info->auth0 < info->pages_total);
        }
    }

    return true;
}

bool ntag_t2_read_user_memory(pn532_io_handle_t io,
                              uint8_t **out_buf,
                              size_t *out_len,
                              NTAG2XX_MODEL *out_model) {
    if (!io || !out_buf || !out_len) return false;
    *out_buf = NULL; *out_len = 0; if (out_model) *out_model = NTAG2XX_UNKNOWN;

    ntag_t2_info_t info;
    if (!ntag_t2_read_info(io, &info)) {
        return false;
    }
    uint8_t size_mul_8 = info.cc[2];
    size_t data_bytes = info.user_bytes ? info.user_bytes : (size_t)size_mul_8 * 8;
    if (data_bytes == 0 || data_bytes > 1024) data_bytes = 1024;
    if (out_model) *out_model = info.model;
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
            if (tmp[i] == 0xFE) {
                size_t len = copied - (to_copy - i - 1);
                *out_buf = buf; *out_len = len; return true;
            }
        }
    }
    *out_buf = buf;
    *out_len = copied;
    return true;
}

bool ntag_t2_read_user_memory_fast(pn532_io_handle_t io,
                                   uint8_t **out_buf,
                                   size_t *out_len,
                                   ntag_t2_info_t *out_info) {
    if (!io || !out_buf || !out_len || !out_info) return false;
    *out_buf = NULL;
    *out_len = 0;
    memset(out_info, 0, sizeof(*out_info));
    out_info->model = NTAG2XX_UNKNOWN;
    out_info->auth0 = 0xFF;

    if (ntag2xx_read_page(io, 0, out_info->page0_3, sizeof(out_info->page0_3)) != ESP_OK) {
        return false;
    }

    memcpy(out_info->cc, &out_info->page0_3[12], sizeof(out_info->cc));
    out_info->cc_valid = (out_info->cc[0] == 0xE1 || out_info->cc[0] == 0xE2);
    out_info->cc_read_only = out_info->cc_valid && ((out_info->cc[3] & 0x0F) != 0);
    out_info->static_locked = (out_info->page0_3[10] != 0 || out_info->page0_3[11] != 0);
    if (out_info->cc_valid) out_info->model = ntag_t2_model_from_cc_size(out_info->cc[2]);
    ntag_t2_fill_layout(out_info);
    if (out_info->pages_total == 0 && out_info->cc_valid) {
        out_info->user_bytes = (uint16_t)out_info->cc[2] * 8;
        out_info->first_user_page = 4;
        out_info->last_user_page = (out_info->user_bytes >= 4) ? (uint8_t)(3 + out_info->user_bytes / 4) : 0;
        out_info->pages_total = (uint16_t)out_info->last_user_page + 1;
    }

    size_t data_bytes = out_info->user_bytes ? out_info->user_bytes : 1024;
    if (data_bytes == 0 || data_bytes > 1024) data_bytes = 1024;
    uint8_t *buf = (uint8_t*)malloc(data_bytes);
    if (!buf) return false;

    size_t copied = 0;
    while (copied < data_bytes) {
        uint8_t page = (uint8_t)(4 + (copied / 4));
        uint8_t tmp[16] = {0};
        if (ntag2xx_read_page(io, page, tmp, sizeof(tmp)) != ESP_OK) {
            free(buf);
            return false;
        }

        size_t remain = data_bytes - copied;
        size_t to_copy = (remain < sizeof(tmp)) ? remain : sizeof(tmp);
        memcpy(buf + copied, tmp, to_copy);
        copied += to_copy;

        size_t off = 0, len = 0;
        if (ntag_t2_find_ndef(buf, copied, &off, &len)) {
            size_t need = off + len;
            if (need <= copied) {
                *out_buf = buf;
                *out_len = need;
                return true;
            }
        } else if (memchr(buf, 0xFE, copied)) {
            *out_buf = buf;
            *out_len = copied;
            return true;
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
    ntag_t2_info_t info = {0};
    info.model = model;
    ntag_t2_fill_layout(&info);
    return ntag_t2_build_details_from_mem_info(mem, mem_len, uid, uid_len, &info);
}

char *ntag_t2_build_details_from_mem_info(const uint8_t *mem,
                                          size_t mem_len,
                                          const uint8_t *uid,
                                          uint8_t uid_len,
                                          const ntag_t2_info_t *info) {
    NTAG2XX_MODEL model = info ? info->model : NTAG2XX_UNKNOWN;
    if (!mem || mem_len == 0) {
        size_t cap = 256;
        char *out = (char*)malloc(cap);
        if (!out) return NULL;
        int n = snprintf(out, cap, "Type: %s\nUID:", ntag_t2_model_str(model));
        size_t pos = (n > 0) ? (size_t)n : 0;
        for (uint8_t i = 0; i < uid_len && pos < cap - 4; ++i) {
            n = snprintf(out + pos, cap - pos, " %02X", uid[i]);
            pos += (n > 0) ? (size_t)n : 0;
        }
        (void)snprintf(out + pos, cap - pos, "\nNDEF: none\n");
        return out;
    }
    size_t off = 0, len = 0;
    const char *label = ntag_t2_model_str(model);
    char *base = NULL;
    if (ntag_t2_find_ndef(mem, mem_len, &off, &len) && off + len <= mem_len) {
        base = ndef_build_details_from_message(mem + off, len, uid, uid_len, label);
    } else {
        base = ndef_build_details_from_message(NULL, 0, uid, uid_len, label);
    }
    if (!base || !info) return base;

    char meta[384];
    size_t pos = 0;
    pos += snprintf(meta + pos, sizeof(meta) - pos,
                    "Mem: %up / %u B user / p%u-%u\n",
                    (unsigned)info->pages_total, (unsigned)info->user_bytes,
                    (unsigned)info->first_user_page, (unsigned)info->last_user_page);
    if (info->cc_valid) {
        pos += snprintf(meta + pos, sizeof(meta) - pos,
                        "CC: %02X %02X %02X %02X %s\n",
                        info->cc[0], info->cc[1], info->cc[2], info->cc[3],
                        info->cc_read_only ? "RO" : "RW");
    }
    if (info->version_valid) {
        pos += snprintf(meta + pos, sizeof(meta) - pos,
                        "Ver: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                        info->version[0], info->version[1], info->version[2], info->version[3],
                        info->version[4], info->version[5], info->version[6], info->version[7]);
    }
    pos += snprintf(meta + pos, sizeof(meta) - pos,
                    "Lock: S=%s D=%s\n",
                    info->static_locked ? "yes" : "no",
                    info->dynamic_locked ? "yes" : "no");
    if (info->config_valid) {
        pos += snprintf(meta + pos, sizeof(meta) - pos,
                        "Prot: %s A0=%02X AC=%02X\n",
                        info->password_protected ? "pwd" : "none",
                        info->auth0, info->access);
    }
    if (info->signature_valid) {
        pos += snprintf(meta + pos, sizeof(meta) - pos, "Sig: read\n");
    }
    bool any_counter = false;
    for (uint8_t i = 0; i < 3; i++) any_counter = any_counter || info->counter_valid[i];
    if (any_counter) {
        pos += snprintf(meta + pos, sizeof(meta) - pos, "Counters:");
        for (uint8_t i = 0; i < 3; i++) {
            if (info->counter_valid[i]) {
                pos += snprintf(meta + pos, sizeof(meta) - pos, " %u=%lu", (unsigned)i,
                                (unsigned long)info->counter[i]);
                if (info->tearing_valid[i]) {
                    pos += snprintf(meta + pos, sizeof(meta) - pos, "/%02X", info->tearing[i]);
                }
            }
        }
        pos += snprintf(meta + pos, sizeof(meta) - pos, "\n");
    }

    const char *first_nl = strchr(base, '\n');
    if (!first_nl) return base;
    size_t head_len = (size_t)(first_nl - base + 1);
    size_t total = strlen(base) + pos + 2;
    char *out = (char*)malloc(total);
    if (!out) return base;
    memcpy(out, base, head_len);
    memcpy(out + head_len, meta, pos);
    strcpy(out + head_len + pos, first_nl + 1);
    free(base);
    return out;
}
