// SPDX-License-Identifier: GPL-3.0-or-later

#include "sdkconfig.h"

#if defined(CONFIG_NFC_ST25R3916) || defined(CONFIG_NFC_PN532)

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/glog.h"
#include "core/commands.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "managers/nfc/desfire.h"
#include "managers/nfc/mifare_classic.h"
#include "managers/nfc/nfc_backend.h"
#include "managers/nfc/ntag_t2.h"
#include "managers/sd_card_manager.h"
#include "pn532.h"
#include "pn532_driver.h"

#ifdef CONFIG_NFC_PN532
#include "driver/i2c_types.h"
#include "pn532_driver_i2c.h"
#endif

#ifdef CONFIG_NFC_ST25R3916
#include "st25r3916.h"
#include "st25r3916_adapter.h"
#include "st25r3916_reg.h"
#include "st25r3916_target.h"
#endif

#define NFC_CLI_TASK_NAME "nfc_cli"

typedef struct {
    pn532_io_t io;
    pn532_io_handle_t nfc;
    bool parse;
    bool once;
    bool save;
    bool hardnested;
    uint8_t hn_known_block;
    bool hn_known_key_b;
    uint8_t hn_known_key[6];
    uint8_t hn_target_block;
    bool hn_target_key_b;
    uint16_t hn_samples;
    bool emulate;
    uint8_t emu_uid[10];
    uint8_t emu_uid_len;
    uint16_t emu_atqa;
    uint8_t emu_sak;
    uint8_t *emu_mem;
    size_t emu_mem_len;
    uint16_t emu_pages_total;
    bool emu_writeable;
} nfc_cli_ctx_t;

static TaskHandle_t s_nfc_cli_task = NULL;

bool nfc_cli_stop(void) {
    if (!s_nfc_cli_task) return false;
    xTaskNotifyGive(s_nfc_cli_task);
    return true;
}

static void nfc_cli_release(nfc_cli_ctx_t *ctx) {
    if (ctx && ctx->nfc) {
        pn532_release(ctx->nfc);
        pn532_delete_driver(ctx->nfc);
        ctx->nfc = NULL;
    }
}

static bool nfc_cli_init_st25r(nfc_cli_ctx_t *ctx) {
#ifdef CONFIG_NFC_ST25R3916
    if (!ctx) return false;
#ifdef CONFIG_NFC_ST25R3916_SPI
    glog("NFC: ST25R3916 SPI host=%d mosi=%d miso=%d sclk=%d cs=%d irq=%d rst=%d\n",
         CONFIG_NFC_ST25R3916_SPI_HOST,
         CONFIG_NFC_ST25R3916_SPI_MOSI_PIN,
         CONFIG_NFC_ST25R3916_SPI_MISO_PIN,
         CONFIG_NFC_ST25R3916_SPI_SCLK_PIN,
         CONFIG_NFC_ST25R3916_SPI_CS_PIN,
         CONFIG_NFC_IRQ_PIN,
         CONFIG_NFC_RST_PIN);
    esp_err_t err = st25r3916_new_driver_spi(
        CONFIG_NFC_ST25R3916_SPI_HOST,
        (gpio_num_t)CONFIG_NFC_ST25R3916_SPI_MOSI_PIN,
        (gpio_num_t)CONFIG_NFC_ST25R3916_SPI_MISO_PIN,
        (gpio_num_t)CONFIG_NFC_ST25R3916_SPI_SCLK_PIN,
        (gpio_num_t)CONFIG_NFC_ST25R3916_SPI_CS_PIN,
        (gpio_num_t)CONFIG_NFC_RST_PIN,
        (gpio_num_t)CONFIG_NFC_IRQ_PIN,
        CONFIG_NFC_ST25R3916_SPI_CLOCK_HZ,
        ctx->nfc);
#else
    glog("NFC: ST25R3916 I2C sda=%d scl=%d irq=%d rst=%d addr=0x%02X\n",
         CONFIG_NFC_SDA_PIN,
         CONFIG_NFC_SCL_PIN,
         CONFIG_NFC_IRQ_PIN,
         CONFIG_NFC_RST_PIN,
         CONFIG_NFC_ST25R3916_I2C_ADDR);
    esp_err_t err = st25r3916_new_driver_i2c(
        (gpio_num_t)CONFIG_NFC_SDA_PIN,
        (gpio_num_t)CONFIG_NFC_SCL_PIN,
        (gpio_num_t)CONFIG_NFC_RST_PIN,
        (gpio_num_t)CONFIG_NFC_IRQ_PIN,
        I2C_NUM_0,
        CONFIG_NFC_ST25R3916_I2C_ADDR,
        ctx->nfc);
#endif
    if (err != ESP_OK) {
        glog("NFC: driver create failed: %s\n", esp_err_to_name(err));
        ctx->nfc = NULL;
        return false;
    }

    err = st25r3916_adapter_init(ctx->nfc);
    if (err != ESP_OK) {
        glog("NFC: ST25R3916 init failed: %s\n", esp_err_to_name(err));
        nfc_cli_release(ctx);
        return false;
    }

    uint8_t id = 0, type = 0, rev = 0;
    if (st25r3916_check_id(&id, &type, &rev) == ESP_OK) {
        glog("NFC: ST25R3916 id=0x%02X type=0x%02X rev=%u\n", id, type, rev);
    }
    return true;
#else
    (void)ctx;
    return false;
#endif
}

static bool nfc_cli_init_pn532(nfc_cli_ctx_t *ctx) {
#ifdef CONFIG_NFC_PN532
    if (!ctx) return false;
    glog("NFC: PN532 I2C sda=%d scl=%d irq=%d rst=%d\n",
         CONFIG_NFC_SDA_PIN, CONFIG_NFC_SCL_PIN, CONFIG_NFC_IRQ_PIN, CONFIG_NFC_RST_PIN);
    esp_err_t err = pn532_new_driver_i2c(
        (gpio_num_t)CONFIG_NFC_SDA_PIN,
        (gpio_num_t)CONFIG_NFC_SCL_PIN,
        (gpio_num_t)CONFIG_NFC_RST_PIN,
        (gpio_num_t)CONFIG_NFC_IRQ_PIN,
        I2C_NUM_0,
        ctx->nfc);
    if (err != ESP_OK || pn532_init(ctx->nfc) != ESP_OK) {
        glog("NFC: PN532 init failed\n");
        nfc_cli_release(ctx);
        return false;
    }
    pn532_set_passive_activation_retries(ctx->nfc, 0xFF);
    return true;
#else
    (void)ctx;
    return false;
#endif
}

static bool nfc_cli_init(nfc_cli_ctx_t *ctx) {
    if (!ctx) return false;
    if (ctx->nfc) return true;

    memset(&ctx->io, 0, sizeof(ctx->io));
    ctx->nfc = &ctx->io;

    nfc_backend_t backend = ctx->emulate ? NFC_BACKEND_ST25R3916 : nfc_backend_get();
    bool ok = false;
    if (backend == NFC_BACKEND_PN532) {
        ok = nfc_cli_init_pn532(ctx);
    } else if (backend == NFC_BACKEND_ST25R3916) {
        ok = nfc_cli_init_st25r(ctx);
    } else {
        ok = nfc_cli_init_pn532(ctx);
        if (!ok) {
            ctx->nfc = &ctx->io;
            ok = nfc_cli_init_st25r(ctx);
        }
    }

    if (!ok) ctx->nfc = NULL;
    return ok;
}

static void nfc_cli_uid_text(const uint8_t *uid, uint8_t uid_len, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    size_t pos = 0;
    for (uint8_t i = 0; i < uid_len && pos < out_len; i++) {
        int n = snprintf(out + pos, out_len - pos, "%02X%s", uid[i], (i + 1 < uid_len) ? ":" : "");
        if (n <= 0) break;
        pos += (size_t)n;
    }
}

static const char *nfc_cli_type_name(uint16_t atqa, uint8_t sak) {
    if (mfc_is_classic_sak(sak)) return "MIFARE Classic";
    if (desfire_is_desfire_candidate(atqa, sak)) return "MIFARE DESFire";
    if ((sak & 0x20) != 0) return "ISO14443-4";
    return "ISO14443-A";
}

static uint16_t nfc_cli_ntag_pages_from_model(NTAG2XX_MODEL model);

static bool nfc_cli_save_type2_file(pn532_io_handle_t io, const uint8_t *uid, uint8_t uid_len,
                                    uint16_t atqa, uint8_t sak, char *out_path,
                                    size_t out_path_len) {
    if (!io || !uid || uid_len == 0) return false;
    const char *dir = "/mnt/ghostesp/nfc";
    bool susp = false;
    if (!sd_card_jit_begin(&susp, true)) return false;
    sd_card_create_directory(dir);

    uint8_t page0_3[16] = {0};
    if (ntag2xx_read_page(io, 0, page0_3, sizeof(page0_3)) != ESP_OK) {
        sd_card_jit_end(susp);
        return false;
    }

    NTAG2XX_MODEL model = NTAG2XX_UNKNOWN;
    switch (page0_3[14]) {
        case 0x12: model = NTAG2XX_NTAG213; break;
        case 0x3E:
        case 0x3F: model = NTAG2XX_NTAG215; break;
        case 0x6D:
        case 0x6F: model = NTAG2XX_NTAG216; break;
        default: model = NTAG2XX_UNKNOWN; break;
    }
    int pages_total = nfc_cli_ntag_pages_from_model(model);
    const char *model_str = ntag_t2_model_str(model);

    char uid_part[40] = {0};
    int up = 0;
    for (uint8_t i = 0; i < uid_len && up < (int)sizeof(uid_part) - 3; i++) {
        up += snprintf(uid_part + up, sizeof(uid_part) - up, "%02X%s", uid[i],
                       (i + 1 < uid_len) ? "-" : "");
    }

    char path[192];
    snprintf(path, sizeof(path), "%s/%s_%s.nfc", dir, model_str, uid_part);
    if (out_path && out_path_len) snprintf(out_path, out_path_len, "%s", path);

    char header[512];
    int pos = 0;
    pos += snprintf(header + pos, sizeof(header) - pos, "Filetype: Flipper NFC device\n");
    pos += snprintf(header + pos, sizeof(header) - pos, "Version: 4\n");
    pos += snprintf(header + pos, sizeof(header) - pos, "Device type: NTAG/Ultralight\n");
    pos += snprintf(header + pos, sizeof(header) - pos, "UID:");
    for (uint8_t i = 0; i < uid_len && pos < (int)sizeof(header) - 4; i++) {
        pos += snprintf(header + pos, sizeof(header) - pos, " %02X", uid[i]);
    }
    pos += snprintf(header + pos, sizeof(header) - pos, "\nATQA: %02X %02X\nSAK: %02X\n",
                    (atqa >> 8) & 0xFF, atqa & 0xFF, sak);
    pos += snprintf(header + pos, sizeof(header) - pos,
                    "Data format version: 2\nNTAG/Ultralight type: %s\n", model_str);
    if (sd_card_write_file(path, header, (size_t)pos) != ESP_OK) {
        sd_card_jit_end(susp);
        return false;
    }

    char meta[160];
    pos = snprintf(meta, sizeof(meta), "Pages total: %d\n", pages_total);
    sd_card_append_file(path, meta, (size_t)pos);

    int pages_read = 0;
    char line[48];
    for (int pg = 0; pg < pages_total; pg += 4) {
        uint8_t block[16] = {0};
        bool ok = (ntag2xx_read_page(io, (uint8_t)pg, block, sizeof(block)) == ESP_OK);
        int chunk = (pages_total - pg >= 4) ? 4 : (pages_total - pg);
        if (ok) pages_read += chunk;
        for (int off = 0; off < chunk; off++) {
            const uint8_t *d = &block[off * 4];
            pos = snprintf(line, sizeof(line), "Page %d: %02X %02X %02X %02X\n", pg + off,
                           d[0], d[1], d[2], d[3]);
            sd_card_append_file(path, line, (size_t)pos);
        }
    }
    pos = snprintf(meta, sizeof(meta), "Pages read: %d\nFailed authentication attempts: 0\n", pages_read);
    sd_card_append_file(path, meta, (size_t)pos);

    sd_card_jit_end(susp);
    return true;
}

static bool nfc_cli_save_detected(pn532_io_handle_t io, const uint8_t *uid, uint8_t uid_len,
                                  uint16_t atqa, uint8_t sak) {
    char path[192] = {0};
    if (mfc_is_classic_sak(sak)) {
        char *summary = mfc_build_details_summary(io, uid, uid_len, atqa, sak);
        if (summary) {
            glog("%s\n", summary);
            free(summary);
        }
        bool susp = false;
        bool mounted = sd_card_jit_begin(&susp, true);
        bool ok = false;
        if (mounted) {
            ok = mfc_save_flipper_file(NULL, uid, uid_len, atqa, sak, "/mnt/ghostesp/nfc", path, sizeof(path));
            if (!ok) ok = mfc_save_flipper_file(io, uid, uid_len, atqa, sak, "/mnt/ghostesp/nfc", path, sizeof(path));
            sd_card_jit_end(susp);
        }
        if (ok) glog("NFC: saved MIFARE Classic dump: %s\n", path);
        else glog("NFC: failed to save MIFARE Classic dump\n");
        return ok;
    }

    bool ok = nfc_cli_save_type2_file(io, uid, uid_len, atqa, sak, path, sizeof(path));
    if (ok) glog("NFC: saved Type 2/NTAG dump: %s\n", path);
    else glog("NFC: failed to save Type 2/NTAG dump\n");
    return ok;
}

static void nfc_cli_print_parsed(pn532_io_handle_t io, const uint8_t *uid, uint8_t uid_len,
                                 uint16_t atqa, uint8_t sak) {
    char *text = NULL;

    if (mfc_is_classic_sak(sak)) {
        text = mfc_build_details_summary(io, uid, uid_len, atqa, sak);
    } else if (desfire_is_desfire_candidate(atqa, sak)) {
        desfire_version_t ver;
        bool have_ver = desfire_get_version(io, &ver);
        text = desfire_build_details_summary(have_ver ? &ver : NULL, uid, uid_len, atqa, sak);
    } else {
        uint8_t *mem = NULL;
        size_t mem_len = 0;
        NTAG2XX_MODEL model = NTAG2XX_UNKNOWN;
        if (ntag_t2_read_user_memory(io, &mem, &mem_len, &model)) {
            text = ntag_t2_build_details_from_mem(mem, mem_len, uid, uid_len, model);
            free(mem);
        }
    }

    if (text) {
        glog("%s\n", text);
        free(text);
    } else {
        glog("NFC: no parser details available for this tag\n");
    }
}

static void nfc_cli_scan_task(void *arg) {
    nfc_cli_ctx_t *ctx = (nfc_cli_ctx_t *)arg;
    if (!ctx) vTaskDelete(NULL);

    if (!nfc_cli_init(ctx)) {
        free(ctx);
        vTaskDelete(NULL);
    }

    uint8_t last_uid[10] = {0};
    uint8_t last_uid_len = 0;
    int64_t last_print_us = 0;
    int64_t start_us = esp_timer_get_time();

    glog("NFC: scanning%s%s%s%s. Use 'stop' or 'nfc stop' to stop.\n",
         ctx->once ? " once" : "",
         ctx->parse ? " with parsing" : "",
         ctx->save ? " and save" : "",
         ctx->hardnested ? " for hardnested capture" : "");

    for (;;) {
        if (ulTaskNotifyTake(pdTRUE, 0) > 0) {
            glog("NFC: stopped\n");
            break;
        }

        uint8_t uid[10] = {0};
        uint8_t uid_len = 0;
        uint16_t atqa = 0;
        uint8_t sak = 0;
        esp_err_t err = pn532_read_passive_target_id_ex(ctx->nfc, 0x00, uid, &uid_len, &atqa, &sak, 250);

        if (err == ESP_OK && uid_len > 0) {
            int64_t now = esp_timer_get_time();
            bool same = (uid_len == last_uid_len && memcmp(uid, last_uid, uid_len) == 0);
            if (!same || now - last_print_us > 2000000 || ctx->once) {
                char uid_text[32];
                nfc_cli_uid_text(uid, uid_len, uid_text, sizeof(uid_text));
                glog("NFC: %s uid=%s atqa=0x%04X sak=0x%02X\n",
                     nfc_cli_type_name(atqa, sak), uid_text, atqa, sak);
                if (ctx->parse && (!same || ctx->once)) {
                    nfc_cli_print_parsed(ctx->nfc, uid, uid_len, atqa, sak);
                }
                if (ctx->save && (!same || ctx->once)) {
                    nfc_cli_save_detected(ctx->nfc, uid, uid_len, atqa, sak);
                }
                if (ctx->hardnested && (!same || ctx->once)) {
                    if (!mfc_is_classic_sak(sak)) {
                        glog("NFC: hardnested capture needs a MIFARE Classic tag\n");
                    } else {
                        bool susp = false;
                        char path[192] = {0};
                        bool ok = false;
                        if (sd_card_jit_begin(&susp, true)) {
                            ok = mfc_hardnested_capture_file(ctx->nfc, uid, uid_len, atqa, sak,
                                                            ctx->hn_known_block,
                                                            ctx->hn_known_key_b,
                                                            ctx->hn_known_key,
                                                            ctx->hn_target_block,
                                                            ctx->hn_target_key_b,
                                                            ctx->hn_samples,
                                                            "/mnt/ghostesp/nfc",
                                                            path, sizeof(path));
                            sd_card_jit_end(susp);
                        }
                        if (ok) glog("NFC: hardnested capture saved: %s\n", path);
                        else glog("NFC: hardnested capture failed\n");
                    }
                }
                memcpy(last_uid, uid, uid_len);
                last_uid_len = uid_len;
                last_print_us = now;
            }
            if (ctx->once || ctx->hardnested) break;
        } else if (ctx->once && esp_timer_get_time() - start_us > 10000000) {
            glog("NFC: no tag found\n");
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    nfc_cli_release(ctx);
    free(ctx);
    s_nfc_cli_task = NULL;
    vTaskDelete(NULL);
}

#define NFC_CLI_NTAG213_PAGES 45
#define NFC_CLI_NTAG215_PAGES 135
#define NFC_CLI_NTAG216_PAGES 231
#define NFC_CLI_TYPE2_MAX_PAGES NFC_CLI_NTAG216_PAGES
#define NFC_CLI_TYPE2_MAX_BYTES (NFC_CLI_TYPE2_MAX_PAGES * 4)

static uint16_t nfc_cli_ntag_pages_from_model(NTAG2XX_MODEL model) {
    switch (model) {
        case NTAG2XX_NTAG213: return NFC_CLI_NTAG213_PAGES;
        case NTAG2XX_NTAG215: return NFC_CLI_NTAG215_PAGES;
        case NTAG2XX_NTAG216: return NFC_CLI_NTAG216_PAGES;
        default: return NFC_CLI_NTAG213_PAGES;
    }
}

static NTAG2XX_MODEL nfc_cli_ntag_model_from_pages(size_t pages) __attribute__((unused));
static NTAG2XX_MODEL nfc_cli_ntag_model_from_pages(size_t pages) {
    if (pages > NFC_CLI_NTAG215_PAGES) return NTAG2XX_NTAG216;
    if (pages > NFC_CLI_NTAG213_PAGES) return NTAG2XX_NTAG215;
    return NTAG2XX_NTAG213;
}

static uint8_t nfc_cli_ntag_cc_size_byte(uint16_t pages_total) {
    /* CC size byte = user-read/write memory in 8-byte units, per NXP NTAG21x datasheet.
     *   NTAG213 (45p): 144 B user -> 0x12
     *   NTAG215 (135p): 504 B user -> 0x3F
     *   NTAG216 (231p): 888 B user -> 0x6F
     * The old formula (pages-4)*4/8 incorrectly counted dynamic-lock + config pages. */
    if (pages_total > NFC_CLI_NTAG215_PAGES) return 0x6F;  // NTAG216
    if (pages_total > NFC_CLI_NTAG213_PAGES) return 0x3F;  // NTAG215
    return 0x12;  // NTAG213
}

static const uint8_t *nfc_cli_ntag_version(uint16_t pages_total) {
    static const uint8_t v213[8] = {0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x0F, 0x03};
    static const uint8_t v215[8] = {0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x11, 0x03};
    static const uint8_t v216[8] = {0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x13, 0x03};
    if (pages_total > NFC_CLI_NTAG215_PAGES) return v216;
    if (pages_total > NFC_CLI_NTAG213_PAGES) return v215;
    return v213;
}

#ifdef CONFIG_NFC_ST25R3916
static uint8_t nfc_cli_type2_bcc(const uint8_t *data, size_t len) {
    uint8_t b = 0;
    for (size_t i = 0; i < len; i++) b ^= data[i];
    return b;
}

static bool nfc_cli_make_ndef_uri(const char *url, uint8_t *out, size_t out_cap, size_t *out_len) {
    if (!url || !out || !out_len) return false;
    const char *body = url;
    uint8_t prefix = 0x00;
    if (strncmp(url, "https://", 8) == 0) {
        prefix = 0x04;
        body = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        prefix = 0x03;
        body = url + 7;
    }
    size_t body_len = strlen(body);
    size_t ndef_len = 5 + body_len;
    if (ndef_len > 0xFE || 2 + ndef_len + 1 > out_cap) return false;
    size_t p = 0;
    out[p++] = 0x03;  // NDEF TLV
    out[p++] = (uint8_t)ndef_len;
    out[p++] = 0xD1;  // MB/ME/SR + well-known record
    out[p++] = 0x01;  // type length
    out[p++] = (uint8_t)(1 + body_len);
    out[p++] = 0x55;  // URI record
    out[p++] = prefix;
    memcpy(&out[p], body, body_len);
    p += body_len;
    out[p++] = 0xFE;  // terminator TLV
    *out_len = p;
    return true;
}

static bool nfc_cli_make_ndef_text(const char *text, uint8_t *out, size_t out_cap, size_t *out_len) {
    if (!text || !out || !out_len) return false;
    size_t text_len = strlen(text);
    size_t ndef_len = 7 + text_len;
    if (ndef_len > 0xFE || 2 + ndef_len + 1 > out_cap) return false;
    size_t p = 0;
    out[p++] = 0x03;
    out[p++] = (uint8_t)ndef_len;
    out[p++] = 0xD1;
    out[p++] = 0x01;
    out[p++] = (uint8_t)(3 + text_len);
    out[p++] = 0x54;  // Text record
    out[p++] = 0x02;  // UTF-8, two-byte language code
    out[p++] = 'e';
    out[p++] = 'n';
    memcpy(&out[p], text, text_len);
    p += text_len;
    out[p++] = 0xFE;
    *out_len = p;
    return true;
}

static bool nfc_cli_make_ntag_memory(const uint8_t *uid, uint8_t uid_len, const uint8_t *tlv,
                                     size_t tlv_len, NTAG2XX_MODEL model, uint8_t **out_mem,
                                     size_t *out_len, uint16_t *out_pages) {
    if (!uid || uid_len != 7 || !tlv || !out_mem || !out_len) return false;
    uint16_t pages = nfc_cli_ntag_pages_from_model(model);
    size_t mem_len = (size_t)pages * 4;
    if (tlv_len > mem_len - 16) return false;
    uint8_t *mem = (uint8_t *)calloc(1, mem_len);
    if (!mem) return false;

    mem[0] = uid[0];
    mem[1] = uid[1];
    mem[2] = uid[2];
    mem[3] = (uint8_t)(0x88 ^ uid[0] ^ uid[1] ^ uid[2]);
    mem[4] = uid[3];
    mem[5] = uid[4];
    mem[6] = uid[5];
    mem[7] = uid[6];
    mem[8] = nfc_cli_type2_bcc(&uid[3], 4);
    mem[9] = 0x48;  // internal byte typical for NTAG21x
    mem[10] = 0x00;
    mem[11] = 0x00;
    mem[12] = 0xE1;
    mem[13] = 0x10;
    mem[14] = nfc_cli_ntag_cc_size_byte(pages);
    mem[15] = 0x00;
    memcpy(&mem[16], tlv, tlv_len);

    /* NTAG config pages (last 4: CFG0, CFG1, PWD, PACK).  A blank tag ships
     * UNprotected, but calloc leaves CFG0 zeroed which makes AUTH0=0x00 — that
     * tells the reader the whole tag is password-protected from page 0, so
     * Flipper reports "password protected pages" and won't fully read it.  The
     * one byte that must differ from zero is AUTH0=0xFF (first protected page =
     * 0xFF -> none protected).  Everything else (ACCESS=0, PWD/PACK read back as
     * 0 on a real tag) is already correct from calloc. */
    if (pages >= 4) {
        mem[(size_t)(pages - 4) * 4 + 3] = 0xFF;  // CFG0.AUTH0
    }

    *out_mem = mem;
    *out_len = mem_len;
    if (out_pages) *out_pages = pages;
    return true;
}

static void nfc_cli_free_ctx(nfc_cli_ctx_t *ctx) {
    if (!ctx) return;
    free(ctx->emu_mem);
    free(ctx);
}

static uint8_t nfc_cli_type2_mem_byte(const nfc_cli_ctx_t *ctx, size_t off) {
    if (!ctx || !ctx->emu_mem || ctx->emu_mem_len == 0) return 0x00;
    return ctx->emu_mem[off % ctx->emu_mem_len];
}

/* Outcome of handling one reader frame in ACTIVE state. */
typedef enum {
    NFC_T2_UNHANDLED = 0,  /* not a command we answer; no response sent */
    NFC_T2_RESPONDED,      /* answered, link still active -> keep serving */
    NFC_T2_LINK_RESET,     /* HALT/RATS/unknown -> Momentum listener-idle rearm */
    NFC_T2_IGNORED,        /* no response, but keep ACTIVE and wait for next frame */
    NFC_T2_HALT_SLEEP,     /* explicit HLTA -> real Type A HALT/SLEEP */
} nfc_t2_result_t;

static nfc_t2_result_t nfc_cli_type2_response(nfc_cli_ctx_t *ctx, const uint8_t *rx, uint16_t rx_len) {
    if (!ctx || !ctx->emu_mem || !rx || rx_len == 0) return NFC_T2_UNHANDLED;

    /* When hardware auto-anticollision transitions to ACTIVE, some ST25R3916
     * runs leave the last SELECT/SDD bytes in FIFO.  Do not clear FIFO blindly
     * at activation, because a fast reader's first real Type 2 command may have
     * already arrived by the time our task notices ACTIVE.  Instead discard only
     * the recognizable UID-tail residue if receive() hands it to us.  The raw
     * residue is 5 bytes, but receive() trims two trailing bytes like CRC, so we
     * see the first 3 bytes here. */
    if (ctx->emu_uid_len == 7 && rx_len == 3) {
        if (rx[0] == 0x88 && rx[1] == ctx->emu_uid[0] && rx[2] == ctx->emu_uid[1]) {
            return NFC_T2_IGNORED;  // CL1 SELECT residue: CT UID0 UID1 ...
        }
        if (rx[0] == ctx->emu_uid[3] && rx[1] == ctx->emu_uid[4] && rx[2] == ctx->emu_uid[5]) {
            return NFC_T2_IGNORED;  // CL2 SELECT residue: UID3 UID4 UID5 ...
        }
    }

    uint8_t rsp[128] = {0};
    uint16_t rsp_len = 0;

#define NFC_T2_NAK_SLEEP() \
    do { \
        (void)st25r3916_target_nfca_respond_bits(0x00, 4); \
        return NFC_T2_LINK_RESET; \
    } while (0)

#define NFC_T2_NAK_KEEP_ACTIVE() \
    do { \
        return st25r3916_target_nfca_respond_bits(0x00, 4) == ESP_OK \
                   ? NFC_T2_RESPONDED : NFC_T2_UNHANDLED; \
    } while (0)

    switch (rx[0]) {
        case 0x30: {  // READ: 4 pages starting at requested page
            if (rx_len != 2) return NFC_T2_LINK_RESET;
            if (rx[1] >= ctx->emu_pages_total) NFC_T2_NAK_SLEEP();
            size_t off = (size_t)rx[1] * 4;
            for (uint16_t i = 0; i < 16; i++) {
                rsp[i] = nfc_cli_type2_mem_byte(ctx, off + i);
            }
            rsp_len = 16;
            break;
        }
        case 0x3A: {  // FAST_READ: page range, capped to local response buffer
            if (rx_len != 3) return NFC_T2_LINK_RESET;
            if (rx[2] < rx[1] || rx[1] >= ctx->emu_pages_total || rx[2] >= ctx->emu_pages_total) {
                NFC_T2_NAK_SLEEP();
            }
            uint16_t pages = (uint16_t)(rx[2] - rx[1] + 1);
            rsp_len = (uint16_t)(pages * 4);
            if (rsp_len > sizeof(rsp)) rsp_len = sizeof(rsp);
            size_t off = (size_t)rx[1] * 4;
            for (uint16_t i = 0; i < rsp_len; i++) {
                rsp[i] = nfc_cli_type2_mem_byte(ctx, off + i);
            }
            break;
        }
        case 0x60:   // GET_VERSION is one byte; 60 xx is MIFARE Classic AUTH A probe
            if (rx_len != 1) NFC_T2_NAK_KEEP_ACTIVE();
            memcpy(rsp, nfc_cli_ntag_version(ctx->emu_pages_total), 8);
            rsp_len = 8;
            break;
        case 0x1B: {  // PWD_AUTH: unprotected test tags return a stable PACK
            if (rx_len != 5) return NFC_T2_LINK_RESET;
            size_t pwd_off = (ctx->emu_pages_total >= 2) ? (size_t)(ctx->emu_pages_total - 2) * 4 : 0;
            size_t pack_off = (ctx->emu_pages_total >= 1) ? (size_t)(ctx->emu_pages_total - 1) * 4 : 0;
            if (pwd_off + 4 > ctx->emu_mem_len || pack_off + 2 > ctx->emu_mem_len ||
                memcmp(&rx[1], &ctx->emu_mem[pwd_off], 4) != 0) {
                NFC_T2_NAK_SLEEP();
            }
            rsp[0] = ctx->emu_mem[pack_off];
            rsp[1] = ctx->emu_mem[pack_off + 1];
            rsp_len = 2;
            break;
        }
        case 0xA2: {  // WRITE: page + 4 bytes
            if (rx_len != 6) return NFC_T2_LINK_RESET;
            uint8_t page = rx[1];
            if (!ctx->emu_writeable || page < 4 || page >= ctx->emu_pages_total) NFC_T2_NAK_SLEEP();
            memcpy(&ctx->emu_mem[(size_t)page * 4], &rx[2], 4);
            return st25r3916_target_nfca_respond_bits(0x0A, 4) == ESP_OK
                       ? NFC_T2_RESPONDED : NFC_T2_UNHANDLED;
        }
        case 0xA0: {  // COMPAT_WRITE: first frame carries page + 16 bytes on many readers
            if (rx_len != 18) return NFC_T2_LINK_RESET;
            uint8_t page = rx[1];
            if (!ctx->emu_writeable || page < 4 || page >= ctx->emu_pages_total) NFC_T2_NAK_SLEEP();
            size_t off = (size_t)page * 4;
            size_t max_copy = ctx->emu_mem_len - off;
            if (max_copy > 16) max_copy = 16;
            memcpy(&ctx->emu_mem[off], &rx[2], max_copy);
            return st25r3916_target_nfca_respond_bits(0x0A, 4) == ESP_OK
                       ? NFC_T2_RESPONDED : NFC_T2_UNHANDLED;
        }
        case 0x39: {  // READ_CNT
            if (rx_len != 2) return NFC_T2_LINK_RESET;
            if (rx[1] > 2) NFC_T2_NAK_SLEEP();
            rsp[0] = rsp[1] = rsp[2] = 0x00;
            rsp_len = 3;
            break;
        }
        case 0x3C: {  // READ_SIG
            if (rx_len != 2) return NFC_T2_LINK_RESET;
            memset(rsp, 0, 32);
            rsp_len = 32;
            break;
        }
        case 0x1A:  // Ultralight-C AUTH probe; NTAG21x rejects it with NAK then sleeps
            if (rx_len == 2) NFC_T2_NAK_SLEEP();
            return NFC_T2_LINK_RESET;
        /* Momentum's listener worker maps NfcCommandSleep to listener_idle(),
         * i.e. STOP + GOTO_SENSE.  HLTA is the exception: a real tag enters
         * HALT/SLEEP so the reader can exclude it from further REQA rounds. */
        case 0x50:  // HALT
            if (rx_len == 2 && rx[1] == 0x00) return NFC_T2_HALT_SLEEP;
            return NFC_T2_LINK_RESET;
        case 0x52:  // WUPA / REQA received while ACTIVE
        case 0x53:  // Flipper proprietary SelCmd
            return NFC_T2_LINK_RESET;
        case 0xE0:  // RATS probe on a non-ISO-DEP SAK=0 tag: ignore, stay selected
            return NFC_T2_IGNORED;
        case 0x73:
        case 0x80:
        case 0x00:
            if (rx_len == 1) return NFC_T2_IGNORED;
            return NFC_T2_LINK_RESET;
        default:    // unknown / unsupported command
            return NFC_T2_LINK_RESET;
    }

#undef NFC_T2_NAK_SLEEP
#undef NFC_T2_NAK_KEEP_ACTIVE

    return st25r3916_target_nfca_respond(rsp, rsp_len, true) == ESP_OK
               ? NFC_T2_RESPONDED : NFC_T2_UNHANDLED;
}

/* Per-command trace recorded in RAM during the active window and flushed only
 * when the link is idle.  glog() does a *synchronous* UART printf (~3.5 ms per
 * line at 115200) which, if emitted between reads, blows past the reader's
 * frame-delay-time and makes strict readers (Flipper) retry or abort.  We never
 * touch the UART while serving the reader. */
typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t res;
    uint8_t state;
    uint8_t main_irq;
    uint8_t err_irq;
    uint8_t pt_irq;
    uint8_t timer_irq;
    uint8_t last_bits;
    uint8_t err;
    uint16_t fifo_bytes;
    uint8_t b[8];
    uint8_t nb;
} nfc_trace_rec_t;

#define NFC_TRACE_MAX 96
static nfc_trace_rec_t s_nfc_trace[NFC_TRACE_MAX];
static int s_nfc_trace_n;

/* Inline per-state-change logging is a SYNCHRONOUS UART printf (~3.5ms/line).
 * It fires on the READY_L2->ACTIVE transition, i.e. right before we arm to serve
 * the reader's first command, so it blows the frame-delay-time and makes the
 * reader give up.  Off by default; set to 1 only for deep state-machine debug,
 * accepting that card emulation will then be unreliable. */
#ifndef NFC_EMU_LOG_STATE
#define NFC_EMU_LOG_STATE 0
#endif

static void nfc_trace_add(const uint8_t *rx, uint16_t rx_len, nfc_t2_result_t res,
                          const st25r3916_target_status_t *status,
                          const st25r3916_target_rx_info_t *info, esp_err_t err) {
    if (s_nfc_trace_n >= NFC_TRACE_MAX) return;  /* drop overflow; flush will note it */
    nfc_trace_rec_t *r = &s_nfc_trace[s_nfc_trace_n++];
    r->cmd = (rx && rx_len) ? rx[0] : 0xFF;
    r->len = (uint8_t)(rx_len > 255 ? 255 : rx_len);
    r->res = (uint8_t)res;
    r->state = status ? status->state : 0xFF;
    r->pt_irq = status ? status->irq_pt : 0;
    r->timer_irq = status ? status->irq_timer : 0;
    r->main_irq = info ? info->main_irq : 0;
    r->err_irq = info ? info->err_irq : 0;
    r->fifo_bytes = info ? info->fifo_bytes : 0;
    r->last_bits = info ? info->last_bits : 0;
    r->err = (uint8_t)(err == ESP_OK ? 0 : (-err & 0xFF));
    r->nb = (uint8_t)(rx_len < sizeof(r->b) ? rx_len : sizeof(r->b));
    for (uint8_t i = 0; i < r->nb; i++) r->b[i] = rx[i];
}

static void nfc_trace_flush(void) {
    if (s_nfc_trace_n == 0) return;
    for (int i = 0; i < s_nfc_trace_n; i++) {
        nfc_trace_rec_t *r = &s_nfc_trace[i];
        char hex[3 * sizeof(r->b) + 1] = {0};
        int hp = 0;
        for (uint8_t j = 0; j < r->nb; j++) {
            hp += snprintf(hex + hp, sizeof(hex) - hp, "%02X ", r->b[j]);
        }
        glog("NFC: cmd=0x%02X len=%u hex=[%s] handled=%d state=%s pt=0x%02X t=0x%02X main=0x%02X err=0x%02X fifo=%u lb=%u rxerr=0x%02X\n",
             r->cmd, r->len, hex, r->res, st25r3916_target_state_name(r->state),
             r->pt_irq, r->timer_irq, r->main_irq, r->err_irq, r->fifo_bytes,
             r->last_bits, r->err);
    }
    if (s_nfc_trace_n >= NFC_TRACE_MAX) glog("NFC: (trace truncated)\n");
    s_nfc_trace_n = 0;
}

static void nfc_cli_emulate_task(void *arg) {
    nfc_cli_ctx_t *ctx = (nfc_cli_ctx_t *)arg;
    if (!ctx) vTaskDelete(NULL);
    s_nfc_trace_n = 0;

    if (!nfc_cli_init(ctx)) {
        nfc_cli_free_ctx(ctx);
        vTaskDelete(NULL);
    }

    char uid_text[32];
    nfc_cli_uid_text(ctx->emu_uid, ctx->emu_uid_len, uid_text, sizeof(uid_text));
    esp_err_t err = st25r3916_target_nfca_start(ctx->emu_uid, ctx->emu_uid_len,
                                                ctx->emu_atqa, ctx->emu_sak);
    if (err != ESP_OK) {
        glog("NFC: emulate start failed: %s\n", esp_err_to_name(err));
        nfc_cli_release(ctx);
        nfc_cli_free_ctx(ctx);
        s_nfc_cli_task = NULL;
        vTaskDelete(NULL);
    }

    glog("NFC: emulating NFC-A uid=%s atqa=0x%04X sak=0x%02X. Use 'stop' or 'nfc stop' to stop.\n",
         uid_text, ctx->emu_atqa, ctx->emu_sak);

#if NFC_EMU_LOG_STATE
    uint8_t last_state = 0xFF;
    uint8_t last_irq = 0;
#endif
    bool auto_ac_disabled = false;
    for (;;) {
        if (ulTaskNotifyTake(pdTRUE, 0) > 0) {
            nfc_trace_flush();
            glog("NFC: emulation stopped\n");
            break;
        }

        st25r3916_target_status_t status = {0};
        bool have_status = (st25r3916_target_nfca_status(&status) == ESP_OK);
        bool active = false;

        if (have_status) {
#if NFC_EMU_LOG_STATE
            if (status.state != last_state || status.irq_pt != last_irq) {
                glog("NFC: target state=%s irq=0x%02X\n",
                     st25r3916_target_state_name(status.state), status.irq_pt);
                last_state = status.state;
                last_irq = status.irq_pt;
            }
#endif
            active = (status.state == ST25R3916_PT_STATUS_ACTIVE ||
                      status.state == ST25R3916_PT_STATUS_ACTIVE_STAR);
            if (active && !auto_ac_disabled) {
                /* Rising edge into ACTIVE (incl. ACTIVE* woken from HALT): take
                 * over from the hardware auto-anticollision.  Do not clear FIFO
                 * here: on fast readers, the first real Type 2 command can arrive
                 * before this task notices ACTIVE.  Any leftover SELECT residue is
                 * recognized and ignored in nfc_cli_type2_response(). */
                st25r3916_reg_modify(ST25R3916_REG_PT_DEF,
                                     ST25R3916_PT_DEF_DISABLE_AC_A,
                                     ST25R3916_PT_DEF_DISABLE_AC_A);
                st25r3916_cmd(ST25R3916_CMD_UNMASK_RECEIVE_DATA);
                auto_ac_disabled = true;
            } else if (status.irq_timer & ST25R3916_IRQ_TIMER_EOF) {
                /* Real external field loss -> Flipper's listener_idle.  Do not use
                 * PT_STATUS=POWER_OFF as a proxy here: STOP/GOTO_SLEEP can pass
                 * through non-active target states while the reader field is still
                 * present, and re-arming SENSE there makes the same tag look like a
                 * fresh presentation. */
                st25r3916_reg_modify(ST25R3916_REG_PT_DEF,
                                     ST25R3916_PT_DEF_DISABLE_AC_A, 0x00);
                st25r3916_cmd(ST25R3916_CMD_STOP);
                st25r3916_cmd(ST25R3916_CMD_GOTO_SENSE);
                st25r3916_irq_clear();
                auto_ac_disabled = false;
            }
        }

        /* Only touch the FIFO once the hardware auto-anticollision has driven the
         * chip to ACTIVE (WU_A).  Reading/responding earlier consumes the reader's
         * REQA/WUPA/ANTICOLLISION/SELECT frames that the hardware is auto-handling
         * and the STOP/GOTO_SLEEP we'd issue on those bytes resets activation,
         * which is why the state bounced POWER_OFF <-> READY_L2* and never reached
         * ACTIVE.  Below ACTIVE we leave the link to the hardware. */
        if (!active) {
            /* Idle: safe to touch the UART now.  Empty the per-command trace
             * accumulated during the last active session. */
            nfc_trace_flush();
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        /* Serve commands back-to-back while ACTIVE without re-reading PT_STATUS
         * between them.  Each status read is 2-3 SPI transactions that delay how
         * fast we re-arm for the reader's next frame and eat into the response
         * window (a slow/late response makes the reader retry the same page or
         * give up -> "unknown / no NDEF").  Stay tight until a receive times out
         * (reader went quiet / field gone), then fall back to the outer poll so
         * field-loss re-arm and the stop notification are handled promptly. */
        for (;;) {
            uint8_t rx[32] = {0};
            uint16_t rx_len = 0;
            st25r3916_target_rx_info_t rx_info = {0};
            err = st25r3916_target_nfca_receive_ex(rx, sizeof(rx), &rx_len, 20, &rx_info);
            if (err == ESP_OK && rx_len > 0) {
                nfc_t2_result_t res = nfc_cli_type2_response(ctx, rx, rx_len);
                nfc_trace_add(rx, rx_len, res, &status, &rx_info, err);  /* RAM only */
                if (res == NFC_T2_RESPONDED || res == NFC_T2_IGNORED) continue;
                if (res == NFC_T2_HALT_SLEEP) {
                    st25r3916_reg_modify(ST25R3916_REG_PT_DEF,
                                         ST25R3916_PT_DEF_DISABLE_AC_A, 0x00);
                    st25r3916_cmd(ST25R3916_CMD_STOP);
                    st25r3916_cmd(ST25R3916_CMD_GOTO_SLEEP);
                    auto_ac_disabled = false;
                    break;
                }
                /* Momentum nfc_worker_listener maps NfcCommandSleep to
                 * listener_idle(): re-enable auto-AC, STOP, GOTO_SENSE.  Using
                 * GOTO_SLEEP here can deadlock when the reader follows a failed
                 * probe with REQA instead of WUPA. */
                st25r3916_reg_modify(ST25R3916_REG_PT_DEF, ST25R3916_PT_DEF_DISABLE_AC_A, 0x00);
                st25r3916_cmd(ST25R3916_CMD_STOP);
                st25r3916_cmd(ST25R3916_CMD_GOTO_SENSE);
                auto_ac_disabled = false;
                break;
            }
            if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
                nfc_t2_result_t res = rx_info.last_bits ? NFC_T2_IGNORED : NFC_T2_LINK_RESET;
                nfc_trace_add(rx, rx_len, res, &status, &rx_info, err);
                if (res == NFC_T2_IGNORED) continue;
                /* CRC / framing error: same as Momentum bad-frame path -> idle. */
                st25r3916_reg_modify(ST25R3916_REG_PT_DEF, ST25R3916_PT_DEF_DISABLE_AC_A, 0x00);
                st25r3916_cmd(ST25R3916_CMD_STOP);
                st25r3916_cmd(ST25R3916_CMD_GOTO_SENSE);
                auto_ac_disabled = false;
            }
            break;  /* timeout or error handled -> re-poll status in outer loop */
        }
    }

    st25r3916_target_stop();
    nfc_cli_release(ctx);
    nfc_cli_free_ctx(ctx);
    s_nfc_cli_task = NULL;
    vTaskDelete(NULL);
}
#endif

static bool nfc_cli_has_arg(int argc, char **argv, const char *a, const char *b) {
    for (int i = 2; i < argc; i++) {
        if ((a && strcmp(argv[i], a) == 0) || (b && strcmp(argv[i], b) == 0)) return true;
    }
    return false;
}

static void nfc_cli_start(bool once, bool parse, bool save) {
    if (s_nfc_cli_task) {
        glog("NFC: task already running\n");
        return;
    }

    nfc_cli_ctx_t *ctx = (nfc_cli_ctx_t *)calloc(1, sizeof(nfc_cli_ctx_t));
    if (!ctx) {
        glog("NFC: out of memory\n");
        return;
    }
    ctx->once = once;
    ctx->parse = parse;
    ctx->save = save;

    BaseType_t ok = xTaskCreate(nfc_cli_scan_task, NFC_CLI_TASK_NAME, (parse || save) ? 8192 : 4096, ctx, 5, &s_nfc_cli_task);
    if (ok != pdPASS) {
        s_nfc_cli_task = NULL;
        free(ctx);
        glog("NFC: failed to start scan task\n");
    }
}

static void nfc_cli_start_hardnested(uint8_t known_block, bool known_key_b,
                                     const uint8_t known_key[6], uint8_t target_block,
                                     bool target_key_b, uint16_t samples) {
    if (s_nfc_cli_task) {
        glog("NFC: task already running\n");
        return;
    }
    nfc_cli_ctx_t *ctx = (nfc_cli_ctx_t *)calloc(1, sizeof(nfc_cli_ctx_t));
    if (!ctx) {
        glog("NFC: out of memory\n");
        return;
    }
    ctx->once = true;
    ctx->hardnested = true;
    ctx->hn_known_block = known_block;
    ctx->hn_known_key_b = known_key_b;
    memcpy(ctx->hn_known_key, known_key, 6);
    ctx->hn_target_block = target_block;
    ctx->hn_target_key_b = target_key_b;
    ctx->hn_samples = samples ? samples : 64;

    BaseType_t ok = xTaskCreate(nfc_cli_scan_task, NFC_CLI_TASK_NAME, 8192, ctx, 5, &s_nfc_cli_task);
    if (ok != pdPASS) {
        s_nfc_cli_task = NULL;
        free(ctx);
        glog("NFC: failed to start hardnested capture task\n");
    }
}

static bool nfc_cli_parse_u8(const char *s, uint8_t *out) {
    if (!s || !out || !*s) return false;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (!end || *end != '\0' || v > 0xFF) return false;
    *out = (uint8_t)v;
    return true;
}

static bool nfc_cli_parse_u16(const char *s, uint16_t *out) {
    if (!s || !out || !*s) return false;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (!end || *end != '\0' || v > 0xFFFF) return false;
    *out = (uint16_t)v;
    return true;
}

static int nfc_cli_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool nfc_cli_parse_uid(const char *s, uint8_t *uid, uint8_t *uid_len) {
    if (!s || !uid || !uid_len) return false;
    uint8_t out[10] = {0};
    uint8_t n = 0;
    int high = -1;

    for (const char *p = s; *p; p++) {
        if (*p == ':' || *p == '-' || *p == ' ') continue;
        int v = nfc_cli_hex_nibble(*p);
        if (v < 0) return false;
        if (high < 0) {
            high = v;
        } else {
            if (n >= sizeof(out)) return false;
            out[n++] = (uint8_t)((high << 4) | v);
            high = -1;
        }
    }

    if (high >= 0 || (n != 4 && n != 7)) return false;
    memcpy(uid, out, n);
    *uid_len = n;
    return true;
}

static bool nfc_cli_parse_key6(const char *s, uint8_t key[6]) {
    if (!s || !key) return false;
    uint8_t out[6] = {0};
    size_t n = 0;
    int high = -1;
    for (const char *p = s; *p; p++) {
        if (*p == ':' || *p == '-' || *p == ' ') continue;
        int v = nfc_cli_hex_nibble(*p);
        if (v < 0) return false;
        if (high < 0) {
            high = v;
        } else {
            if (n >= sizeof(out)) return false;
            out[n++] = (uint8_t)((high << 4) | v);
            high = -1;
        }
    }
    if (high >= 0 || n != sizeof(out)) return false;
    memcpy(key, out, sizeof(out));
    return true;
}

static bool nfc_cli_parse_key_type(const char *s, bool *key_b) {
    if (!s || !key_b || !*s) return false;
    if (s[0] == 'A' || s[0] == 'a' || s[0] == '0') {
        *key_b = false;
        return true;
    }
    if (s[0] == 'B' || s[0] == 'b' || s[0] == '1') {
        *key_b = true;
        return true;
    }
    return false;
}

#ifdef CONFIG_NFC_ST25R3916
static void nfc_cli_start_emulate(const uint8_t *uid, uint8_t uid_len, uint16_t atqa, uint8_t sak,
                                  uint8_t *mem, size_t mem_len, uint16_t pages_total,
                                  bool writeable) {
    if (s_nfc_cli_task) {
        glog("NFC: task already running\n");
        return;
    }

    nfc_cli_ctx_t *ctx = (nfc_cli_ctx_t *)calloc(1, sizeof(nfc_cli_ctx_t));
    if (!ctx) {
        glog("NFC: out of memory\n");
        return;
    }
    ctx->emulate = true;
    memcpy(ctx->emu_uid, uid, uid_len);
    ctx->emu_uid_len = uid_len;
    ctx->emu_atqa = atqa;
    ctx->emu_sak = sak;
    ctx->emu_mem = mem;
    ctx->emu_mem_len = mem_len;
    ctx->emu_pages_total = pages_total ? pages_total : (uint16_t)(mem_len / 4);
    ctx->emu_writeable = writeable;

    BaseType_t ok = xTaskCreate(nfc_cli_emulate_task, NFC_CLI_TASK_NAME, 4096, ctx, 5, &s_nfc_cli_task);
    if (ok != pdPASS) {
        s_nfc_cli_task = NULL;
        nfc_cli_free_ctx(ctx);
        glog("NFC: failed to start emulation task\n");
    }
}

static bool nfc_cli_parse_hex_bytes(const char *s, uint8_t *out, size_t out_cap, size_t *out_len) {
    if (!s || !out || !out_len) return false;
    size_t n = 0;
    int high = -1;
    for (const char *p = s; *p; p++) {
        int v = nfc_cli_hex_nibble(*p);
        if (v < 0) {
            if (high >= 0 && *p != ' ' && *p != ':' && *p != '-' && *p != '\r' && *p != '\n') return false;
            continue;
        }
        if (high < 0) {
            high = v;
        } else {
            if (n >= out_cap) return false;
            out[n++] = (uint8_t)((high << 4) | v);
            high = -1;
        }
    }
    if (high >= 0) return false;
    *out_len = n;
    return true;
}

static bool nfc_cli_join_args(int argc, char **argv, int first, const char *fallback,
                              char *out, size_t out_len) {
    if (!out || out_len == 0) return false;
    out[0] = '\0';
    if (first >= argc) {
        snprintf(out, out_len, "%s", fallback ? fallback : "");
        return true;
    }
    size_t pos = 0;
    for (int i = first; i < argc && pos + 1 < out_len; i++) {
        int n = snprintf(out + pos, out_len - pos, "%s%s", (i == first) ? "" : " ", argv[i]);
        if (n <= 0) return false;
        pos += (size_t)n;
    }
    return true;
}

static bool nfc_cli_make_default_ndef(uint8_t **out_mem, size_t *out_len, uint8_t *uid,
                                      uint8_t *uid_len, int argc, char **argv, int first_arg) {
    static const uint8_t default_uid[7] = {0x04, 0x47, 0x48, 0x4F, 0x53, 0x54, 0x01};
    memcpy(uid, default_uid, sizeof(default_uid));
    *uid_len = sizeof(default_uid);

    uint8_t tlv[160] = {0};
    size_t tlv_len = 0;
    char payload[96];
    if (first_arg < argc && strcmp(argv[first_arg], "text") == 0) {
        nfc_cli_join_args(argc, argv, first_arg + 1, "GhostESP NFC test", payload, sizeof(payload));
        if (!nfc_cli_make_ndef_text(payload, tlv, sizeof(tlv), &tlv_len)) return false;
    } else {
        if (first_arg < argc && strcmp(argv[first_arg], "url") == 0) first_arg++;
        nfc_cli_join_args(argc, argv, first_arg, "https://ghostesp.net", payload, sizeof(payload));
        if (!nfc_cli_make_ndef_uri(payload, tlv, sizeof(tlv), &tlv_len)) return false;
    }
    uint16_t pages = 0;
    bool ok = nfc_cli_make_ntag_memory(uid, *uid_len, tlv, tlv_len, NTAG2XX_NTAG213, out_mem,
                                      out_len, &pages);
    (void)pages;
    return ok;
}

static bool nfc_cli_load_flipper_file(const char *path_arg, uint8_t *uid, uint8_t *uid_len,
                                      uint16_t *atqa, uint8_t *sak, uint8_t **out_mem,
                                      size_t *out_len) {
    if (!path_arg || !uid || !uid_len || !atqa || !sak || !out_mem || !out_len) return false;
    char path[160];
    if (path_arg[0] == '/') snprintf(path, sizeof(path), "%s", path_arg);
    else snprintf(path, sizeof(path), "/mnt/ghostesp/%s", path_arg);

    bool display_was_suspended = false;
    if (!sd_card_jit_begin(&display_was_suspended, false)) return false;
    FILE *f = fopen(path, "r");
    if (!f) {
        sd_card_jit_end(display_was_suspended);
        return false;
    }

    uint8_t *mem = (uint8_t *)calloc(1, 1024);
    if (!mem) {
        fclose(f);
        sd_card_jit_end(display_was_suspended);
        return false;
    }

    char line[180];
    size_t max_mem = 0;
    size_t pages_total = 0;
    bool have_pages = false;
    bool unsupported_type = false;
    *uid_len = 0;
    *atqa = 0x0044;
    *sak = 0x00;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Device type:", 12) == 0) {
            if (strstr(line, "Mifare Classic") || strstr(line, "DESFire")) unsupported_type = true;
        } else if (strncmp(line, "UID:", 4) == 0) {
            nfc_cli_parse_uid(line + 4, uid, uid_len);
        } else if (strncmp(line, "ATQA:", 5) == 0) {
            uint8_t b[2] = {0};
            size_t n = 0;
            if (nfc_cli_parse_hex_bytes(line + 5, b, sizeof(b), &n) && n >= 2) {
                *atqa = (uint16_t)((b[0] << 8) | b[1]);
            }
        } else if (strncmp(line, "SAK:", 4) == 0) {
            uint8_t b[1] = {0};
            size_t n = 0;
            if (nfc_cli_parse_hex_bytes(line + 4, b, sizeof(b), &n) && n >= 1) *sak = b[0];
        } else if (strncmp(line, "Pages total:", 12) == 0) {
            unsigned long v = strtoul(line + 12, NULL, 10);
            if (v > 0 && v <= 256) pages_total = v;
        } else if (strncmp(line, "NTAG/Ultralight type:", 21) == 0) {
            if (strstr(line, "216")) pages_total = NFC_CLI_NTAG216_PAGES;
            else if (strstr(line, "215")) pages_total = NFC_CLI_NTAG215_PAGES;
            else if (strstr(line, "213")) pages_total = NFC_CLI_NTAG213_PAGES;
        } else if (strncmp(line, "Page ", 5) == 0) {
            char *colon = strchr(line, ':');
            if (!colon) continue;
            unsigned long page = strtoul(line + 5, NULL, 10);
            if (page >= 256) continue;
            uint8_t b[4] = {0};
            size_t n = 0;
            if (nfc_cli_parse_hex_bytes(colon + 1, b, sizeof(b), &n) && n == 4) {
                size_t off = page * 4;
                if (off + 4 <= 1024) {
                    memcpy(&mem[off], b, 4);
                    if (off + 4 > max_mem) max_mem = off + 4;
                    have_pages = true;
                }
            }
        }
    }

    fclose(f);
    sd_card_jit_end(display_was_suspended);

    if (unsupported_type) {
        glog("NFC: this .nfc file is not Type 2/NTAG; MFC/DESFire emulation is not supported yet\n");
        free(mem);
        return false;
    }

    if (!have_pages) {
        free(mem);
        return false;
    }
    if (pages_total > 0 && pages_total * 4 > max_mem && pages_total * 4 <= 1024) {
        max_mem = pages_total * 4;
    }
    if (*uid_len != 7 && max_mem >= 9) {
        uid[0] = mem[0];
        uid[1] = mem[1];
        uid[2] = mem[2];
        uid[3] = mem[4];
        uid[4] = mem[5];
        uid[5] = mem[6];
        uid[6] = mem[7];
        *uid_len = 7;
    }
    if (*uid_len != 4 && *uid_len != 7) {
        free(mem);
        return false;
    }
    *out_mem = mem;
    *out_len = max_mem;
    return true;
}

static void nfc_cli_handle_emulate(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[2], "stop") == 0) {
        glog(nfc_cli_stop() ? "NFC: stopping\n" : "NFC: not running\n");
        return;
    }
    if (argc >= 3 && strcmp(argv[2], "status") == 0) {
        glog(s_nfc_cli_task ? "NFC: task running\n" : "NFC: task idle\n");
        return;
    }

    if (argc >= 3 && strcmp(argv[2], "ndef") == 0) {
        uint8_t uid[10] = {0};
        uint8_t uid_len = 0;
        uint8_t *mem = NULL;
        size_t mem_len = 0;
        if (!nfc_cli_make_default_ndef(&mem, &mem_len, uid, &uid_len, argc, argv, 3)) {
            glog("NFC: failed to build NDEF test tag\n");
            return;
        }
        nfc_cli_start_emulate(uid, uid_len, 0x0044, 0x00, mem, mem_len,
                              (uint16_t)(mem_len / 4), true);
        return;
    }

    if (argc >= 4 && strcmp(argv[2], "file") == 0) {
        uint8_t uid[10] = {0};
        uint8_t uid_len = 0;
        uint16_t atqa = 0;
        uint8_t sak = 0;
        uint8_t *mem = NULL;
        size_t mem_len = 0;
        if (!nfc_cli_load_flipper_file(argv[3], uid, &uid_len, &atqa, &sak, &mem, &mem_len)) {
            glog("NFC: failed to load Type 2/NTAG .nfc file\n");
            return;
        }
        nfc_cli_start_emulate(uid, uid_len, atqa, sak, mem, mem_len,
                              (uint16_t)(mem_len / 4), true);
        return;
    }

    const char *uid_arg = NULL;
    uint16_t atqa = 0;
    uint8_t sak = 0x00;
    bool have_atqa = false;
    bool have_sak = false;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "uid") == 0 && i + 1 < argc) {
            uid_arg = argv[++i];
        } else if (strcmp(argv[i], "atqa") == 0 && i + 1 < argc) {
            have_atqa = nfc_cli_parse_u16(argv[++i], &atqa);
            if (!have_atqa) break;
        } else if (strcmp(argv[i], "sak") == 0 && i + 1 < argc) {
            have_sak = nfc_cli_parse_u8(argv[++i], &sak);
            if (!have_sak) break;
        } else if (!uid_arg) {
            uid_arg = argv[i];
        } else {
            uid_arg = NULL;
            break;
        }
    }

    uint8_t uid[10] = {0};
    uint8_t uid_len = 0;
    if (!uid_arg || !nfc_cli_parse_uid(uid_arg, uid, &uid_len)) {
        glog("NFC: usage: nfc emulate uid <4-byte-or-7-byte-uid> [atqa <hex>] [sak <hex>]\n");
        return;
    }
    if (!have_atqa) atqa = (uid_len == 7) ? 0x0044 : 0x0004;
    if (!have_sak) sak = 0x00;

    nfc_cli_start_emulate(uid, uid_len, atqa, sak, NULL, 0, 0, false);
}
#endif

static void nfc_cli_handle_hardnested(int argc, char **argv) {
    uint8_t known_block = 0;
    uint8_t target_block = 0;
    bool known_key_b = false;
    bool target_key_b = false;
    uint8_t known_key[6] = {0};
    uint16_t samples = 64;
    bool have_known = false;
    bool have_target = false;
    bool have_key = false;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "known") == 0 && i + 3 < argc) {
            if (!nfc_cli_parse_u8(argv[++i], &known_block) ||
                !nfc_cli_parse_key_type(argv[++i], &known_key_b) ||
                !nfc_cli_parse_key6(argv[++i], known_key)) {
                have_known = false;
                break;
            }
            have_known = true;
            have_key = true;
        } else if (strcmp(argv[i], "target") == 0 && i + 2 < argc) {
            if (!nfc_cli_parse_u8(argv[++i], &target_block) ||
                !nfc_cli_parse_key_type(argv[++i], &target_key_b)) {
                have_target = false;
                break;
            }
            have_target = true;
        } else if ((strcmp(argv[i], "samples") == 0 || strcmp(argv[i], "-n") == 0) && i + 1 < argc) {
            uint16_t n = 0;
            if (!nfc_cli_parse_u16(argv[++i], &n) || n == 0) break;
            samples = n;
        } else {
            have_known = false;
            break;
        }
    }

    if (!have_known || !have_target || !have_key) {
        glog("NFC: usage: nfc hardnested known <block> <A|B> <12hexkey> target <block> <A|B> [samples N]\n");
        glog("NFC: example: nfc hardnested known 3 A FFFFFFFFFFFF target 7 A samples 128\n");
        return;
    }

    if (samples > 512) samples = 512;
    if (nfc_backend_get() != NFC_BACKEND_ST25R3916) {
        glog("NFC: note: ST25R backend gives raw nonce parity; PN532 may expose less useful samples\n");
    }
    nfc_cli_start_hardnested(known_block, known_key_b, known_key, target_block, target_key_b, samples);
}

static void nfc_cli_help(void) {
    glog("NFC commands:\n");
    glog("  nfc backend [auto|pn532|st25r]  show/set local backend\n");
    glog("  nfc scan [parse]   start continuous ISO14443-A scan\n");
    glog("  nfc once [parse]   scan until one tag or 10s timeout\n");
    glog("  nfc save|dump      scan one tag and save Flipper .nfc\n");
    glog("  nfc hardnested known <blk> <A|B> <key> target <blk> <A|B> [samples N]\n");
    glog("  nfc status         show NFC task state\n");
    glog("  nfc stop           stop scan/emulation\n");
    glog("  nfc emulate uid <uid> [atqa <hex>] [sak <hex>]\n");
    glog("  nfc emulate ndef [url <url>|text <text>]\n");
    glog("  nfc emulate file <path.nfc>\n");
}

void handle_nfc_cmd(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        nfc_cli_help();
        return;
    }

    if (strcmp(argv[1], "backend") == 0) {
        if (argc < 3) {
            glog("NFC: backend=%s\n", nfc_backend_name(nfc_backend_get()));
            return;
        }
        nfc_backend_t backend;
        if (!nfc_backend_parse(argv[2], &backend)) {
            glog("NFC: usage: nfc backend [auto|pn532|st25r]\n");
            return;
        }
#ifndef CONFIG_NFC_PN532
        if (backend == NFC_BACKEND_PN532) {
            glog("NFC: PN532 backend is not compiled in\n");
            return;
        }
#endif
#ifndef CONFIG_NFC_ST25R3916
        if (backend == NFC_BACKEND_ST25R3916) {
            glog("NFC: ST25R3916 backend is not compiled in\n");
            return;
        }
#endif
        nfc_backend_set(backend);
        glog("NFC: backend set to %s%s\n", nfc_backend_name(backend),
             s_nfc_cli_task ? " (applies after current NFC task stops)" : "");
    } else if (strcmp(argv[1], "scan") == 0) {
        nfc_cli_start(false, nfc_cli_has_arg(argc, argv, "parse", "-p"), false);
    } else if (strcmp(argv[1], "once") == 0) {
        nfc_cli_start(true, nfc_cli_has_arg(argc, argv, "parse", "-p"), false);
    } else if (strcmp(argv[1], "save") == 0 || strcmp(argv[1], "dump") == 0) {
        nfc_cli_start(true, true, true);
    } else if (strcmp(argv[1], "hardnested") == 0 || strcmp(argv[1], "hn") == 0) {
        nfc_cli_handle_hardnested(argc, argv);
    } else if (strcmp(argv[1], "stop") == 0) {
        glog(nfc_cli_stop() ? "NFC: stopping\n" : "NFC: not running\n");
    } else if (strcmp(argv[1], "status") == 0) {
        glog(s_nfc_cli_task ? "NFC: task running\n" : "NFC: task idle\n");
    } else if (strcmp(argv[1], "emulate") == 0 || strcmp(argv[1], "emu") == 0) {
#ifdef CONFIG_NFC_ST25R3916
        nfc_cli_handle_emulate(argc, argv);
#else
        glog("NFC: emulation requires ST25R3916 support\n");
#endif
    } else {
        nfc_cli_help();
    }
}

void handle_nfctest_cmd(int argc, char **argv) {
#ifdef CONFIG_NFC_ST25R3916
    if (argc >= 3 && strcmp(argv[1], "emulate") == 0 &&
        (strcmp(argv[2], "ndef") == 0 || strcmp(argv[2], "ntag") == 0)) {
        char *emu_argv[6] = {"nfc", "emulate", "ndef", "url", "https://ghostesp.net", NULL};
        handle_nfc_cmd(5, emu_argv);
        return;
    }
#endif
    bool loop = (argc > 1 && strcmp(argv[1], "loop") == 0);
    bool parse = (argc > 1 && (strcmp(argv[1], "parse") == 0 || strcmp(argv[1], "-p") == 0));
    bool save = (argc > 1 && (strcmp(argv[1], "dump") == 0 || strcmp(argv[1], "save") == 0));
    nfc_cli_start(!loop || save, parse || save, save);
}

#endif
