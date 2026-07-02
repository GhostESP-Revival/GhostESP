// Builds a synthetic blank NTAG21x tag image around a caller-supplied NDEF
// TLV blob and saves it as a Flipper-format .nfc file.
//
// Page layout (UID/BCC/CC in pages 0-3, NDEF from page 4, lock/config/PWD/
// PACK in the last five pages) is standard NTAG21x territory. The special
// page addressing is computed from ntag_t2_last_user_page_for_model() rather
// than hardcoded per model, since the fixed page numbers differ between
// NTAG213/215/216 and are easy to get off-by-one (which would clobber the
// last user-data page or leave the dynamic lock bytes unset).
#include "managers/nfc/ndef_tag_gen.h"
#include "managers/nfc/ntag_t2.h"
#include "managers/sd_card_manager.h"
#include <esp_random.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void gen_uid(uint8_t uid[7]) {
    uid[0] = 0x04; // NXP manufacturer code
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    uid[1] = (uint8_t)(r1 >> 24);
    uid[2] = (uint8_t)(r1 >> 16);
    uid[3] = (uint8_t)(r1 >> 8);
    uid[4] = (uint8_t)(r1);
    uid[5] = (uint8_t)(r2 >> 24);
    uid[6] = (uint8_t)(r2 >> 16);
}

static uint8_t cc_size_for_model(NTAG2XX_MODEL model) {
    switch (model) {
        case NTAG2XX_NTAG213: return 0x12;
        case NTAG2XX_NTAG216: return 0x6D;
        case NTAG2XX_NTAG215:
        default: return 0x3E;
    }
}

// Fills the dynamic-lock/CFG0/CFG1/PWD/PACK pages that follow user memory,
// using NXP's documented factory-default values.
static void set_special_pages(uint8_t *pages, int pages_total, uint8_t last_user_page) {
    int base = (int)last_user_page + 1;
    if (base < 0 || base + 4 >= pages_total) return;
    static const uint8_t lock[4] = { 0x00, 0x00, 0x00, 0xBD };
    static const uint8_t cfg0[4] = { 0x04, 0x00, 0x00, 0xFF };
    static const uint8_t cfg1[4] = { 0x00, 0x05, 0x00, 0x00 };
    static const uint8_t pwd[4]  = { 0xFF, 0xFF, 0xFF, 0xFF };
    static const uint8_t pack[4] = { 0x00, 0x00, 0x00, 0x00 };
    memcpy(pages + (size_t)(base + 0) * 4, lock, 4);
    memcpy(pages + (size_t)(base + 1) * 4, cfg0, 4);
    memcpy(pages + (size_t)(base + 2) * 4, cfg1, 4);
    memcpy(pages + (size_t)(base + 3) * 4, pwd, 4);
    memcpy(pages + (size_t)(base + 4) * 4, pack, 4);
}

static void sanitize_into(char *dst, size_t dst_cap, const char *src) {
    size_t j = 0;
    if (!src) { dst[0] = '\0'; return; }
    for (size_t i = 0; src[i] && j < dst_cap - 1; ++i) {
        char c = src[i];
        dst[j++] = (isalnum((unsigned char)c)) ? c : '_';
    }
    dst[j] = '\0';
}

bool ndef_tag_gen_save_file(NTAG2XX_MODEL model,
                            const uint8_t *ndef_tlv, size_t ndef_tlv_len,
                            const char *name_hint,
                            char *out_path, size_t out_path_cap) {
    if (!ndef_tlv || ndef_tlv_len == 0) return false;
    if (model != NTAG2XX_NTAG213 && model != NTAG2XX_NTAG215 && model != NTAG2XX_NTAG216) {
        model = NTAG2XX_NTAG215;
    }

    int pages_total = (int)ntag_t2_pages_for_model(model);
    uint8_t last_user_page = ntag_t2_last_user_page_for_model(model);
    if (pages_total <= 0 || last_user_page < 4) return false;

    size_t bytes_total = (size_t)pages_total * 4;
    uint8_t *pages = (uint8_t *)calloc(1, bytes_total);
    if (!pages) return false;

    size_t user_capacity = (size_t)(last_user_page - 4 + 1) * 4;
    if (ndef_tlv_len > user_capacity) {
        free(pages);
        return false;
    }

    uint8_t uid[7];
    gen_uid(uid);

    pages[0] = uid[0]; pages[1] = uid[1]; pages[2] = uid[2];
    pages[3] = (uint8_t)(uid[0] ^ uid[1] ^ uid[2]); // BCC0
    pages[4] = uid[3]; pages[5] = uid[4]; pages[6] = uid[5]; pages[7] = uid[6];
    pages[8] = (uint8_t)(uid[3] ^ uid[4] ^ uid[5] ^ uid[6]); // BCC1
    pages[9] = 0x48; // internal byte
    pages[10] = 0x00; pages[11] = 0x00;
    pages[12] = 0xE1; pages[13] = 0x10; pages[14] = cc_size_for_model(model); pages[15] = 0x00;

    memcpy(pages + 16, ndef_tlv, ndef_tlv_len);
    set_special_pages(pages, pages_total, last_user_page);

    size_t cap = 768 + (size_t)pages_total * 24;
    char *buf = (char *)malloc(cap);
    if (!buf) { free(pages); return false; }

    const char *model_str = ntag_t2_model_str(model);
    int pos = 0;
    pos += snprintf(buf + pos, cap - (size_t)pos, "Filetype: Flipper NFC device\n");
    pos += snprintf(buf + pos, cap - (size_t)pos, "Version: 4\n");
    pos += snprintf(buf + pos, cap - (size_t)pos, "Device type: NTAG/Ultralight\n");
    pos += snprintf(buf + pos, cap - (size_t)pos, "UID:");
    for (int i = 0; i < 7; ++i) pos += snprintf(buf + pos, cap - (size_t)pos, " %02X", uid[i]);
    pos += snprintf(buf + pos, cap - (size_t)pos, "\n");
    pos += snprintf(buf + pos, cap - (size_t)pos, "ATQA: 00 44\n");
    pos += snprintf(buf + pos, cap - (size_t)pos, "SAK: 00\n");
    pos += snprintf(buf + pos, cap - (size_t)pos, "Data format version: 2\n");
    pos += snprintf(buf + pos, cap - (size_t)pos, "NTAG/Ultralight type: %s\n", model_str);
    pos += snprintf(buf + pos, cap - (size_t)pos, "Signature:");
    for (int i = 0; i < 32; ++i) pos += snprintf(buf + pos, cap - (size_t)pos, " 00");
    pos += snprintf(buf + pos, cap - (size_t)pos, "\n");
    pos += snprintf(buf + pos, cap - (size_t)pos, "Mifare version: 00 04 04 02 01 00 11 03\n");
    for (int c = 0; c < 3; ++c) {
        pos += snprintf(buf + pos, cap - (size_t)pos, "Counter %d: 0\n", c);
        pos += snprintf(buf + pos, cap - (size_t)pos, "Tearing %d: 00\n", c);
    }
    pos += snprintf(buf + pos, cap - (size_t)pos, "Pages total: %d\n", pages_total);
    pos += snprintf(buf + pos, cap - (size_t)pos, "Pages read: %d\n", pages_total);
    for (int p = 0; p < pages_total; ++p) {
        pos += snprintf(buf + pos, cap - (size_t)pos, "Page %d: %02X %02X %02X %02X\n", p,
                        pages[p * 4], pages[p * 4 + 1], pages[p * 4 + 2], pages[p * 4 + 3]);
    }
    pos += snprintf(buf + pos, cap - (size_t)pos, "Failed authentication attempts: 0\n");
    free(pages);

    if (pos <= 0 || (size_t)pos >= cap) { free(buf); return false; }

    const char *dir = "/mnt/ghostesp/nfc";
    sd_card_create_directory(dir);

    char safe_hint[32];
    sanitize_into(safe_hint, sizeof(safe_hint), name_hint);

    char path[224];
    if (safe_hint[0]) {
        snprintf(path, sizeof(path), "%s/Gen_%s_%02X%02X%02X.nfc", dir, safe_hint, uid[4], uid[5], uid[6]);
    } else {
        snprintf(path, sizeof(path), "%s/Gen_%s_%02X%02X%02X.nfc", dir, model_str, uid[4], uid[5], uid[6]);
    }

    esp_err_t err = sd_card_write_file(path, buf, (size_t)pos);
    free(buf);
    if (err != ESP_OK) return false;

    if (out_path && out_path_cap) {
        strncpy(out_path, path, out_path_cap - 1);
        out_path[out_path_cap - 1] = '\0';
    }
    return true;
}
