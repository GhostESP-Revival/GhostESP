#include "managers/nfc/write_ntag.h"
#ifdef CONFIG_NFC_PN532
#include "pn532.h"
#endif
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "write_ntag";

static NTAG2XX_MODEL infer_model_from_pages(int pages_total) {
    switch (pages_total) {
        case 45: return NTAG2XX_NTAG213;
        case 135: return NTAG2XX_NTAG215;
        case 231: return NTAG2XX_NTAG216;
        default: return NTAG2XX_UNKNOWN;
    }
}

bool ntag_file_load(const char *path, ntag_file_image_t *out) {
    if (!path || !out) return false;
    memset(out, 0, sizeof(*out));
    out->first_user_page = 4;

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGE(TAG, "open failed: %s", path);
        return false;
    }

    char line[192];
    int pages_total = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "NTAG/Ultralight type:", 22) == 0) {
            char model[32] = {0};
            if (sscanf(line + 22, " %31s", model) == 1) {
                if (strstr(model, "213")) out->model = NTAG2XX_NTAG213;
                else if (strstr(model, "215")) out->model = NTAG2XX_NTAG215;
                else if (strstr(model, "216")) out->model = NTAG2XX_NTAG216;
                else out->model = NTAG2XX_UNKNOWN;
            }
        } else if (strncmp(line, "UID:", 4) == 0) {
            uint8_t b; int consumed = 0; const char *p = line + 4; out->uid_len = 0;
            while (*p && out->uid_len < sizeof(out->uid)) {
                while (*p == ' ') ++p;
                if (!*p || *p == '\n' || *p == '\r') break;
                if (sscanf(p, " %2hhX%n", &b, &consumed) == 1) {
                    out->uid[out->uid_len++] = b;
                    p += consumed;
                } else break;
            }
        } else if (sscanf(line, "Pages total: %d", &pages_total) == 1) {
            if (pages_total < 4 || pages_total > 240) pages_total = 0;
            if (pages_total > 0) {
                out->pages_total = pages_total;
                size_t bytes = (size_t)pages_total * 4;
                out->full_pages = (uint8_t*)malloc(bytes);
                if (!out->full_pages) { fclose(f); return false; }
                memset(out->full_pages, 0x00, bytes);
                break; // we'll re-scan from beginning to fill pages
            }
        }
    }

    if (out->pages_total == 0) {
        // try to infer from model
        if (out->model != NTAG2XX_UNKNOWN) {
            switch (out->model) {
                case NTAG2XX_NTAG213: out->pages_total = 45; break;
                case NTAG2XX_NTAG215: out->pages_total = 135; break;
                case NTAG2XX_NTAG216: out->pages_total = 231; break;
                default: break;
            }
        }
        if (out->pages_total == 0) {
            ESP_LOGE(TAG, "pages_total unknown in file: %s", path);
            fclose(f);
            return false;
        }
        size_t bytes = (size_t)out->pages_total * 4;
        out->full_pages = (uint8_t*)malloc(bytes);
        if (!out->full_pages) { fclose(f); return false; }
        memset(out->full_pages, 0x00, bytes);
    }

    // second pass: fill page data
    rewind(f);
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Page ", 5) == 0) {
            int pg; unsigned int d0, d1, d2, d3;
            if (sscanf(line, "Page %d: %02X %02X %02X %02X", &pg, &d0, &d1, &d2, &d3) == 5) {
                if (pg >= 0 && pg < out->pages_total) {
                    size_t off = (size_t)pg * 4;
                    out->full_pages[off + 0] = (uint8_t)d0;
                    out->full_pages[off + 1] = (uint8_t)d1;
                    out->full_pages[off + 2] = (uint8_t)d2;
                    out->full_pages[off + 3] = (uint8_t)d3;
                }
            }
        }
    }
    fclose(f);

    if (out->model == NTAG2XX_UNKNOWN) out->model = infer_model_from_pages(out->pages_total);

    return true;
}

void ntag_file_free(ntag_file_image_t *img) {
    if (!img) return;
    if (img->full_pages) { free(img->full_pages); img->full_pages = NULL; }
    memset(img, 0, sizeof(*img));
}

char *ntag_file_build_details(const ntag_file_image_t *img) {
    if (!img) return NULL;
    const char *label = ntag_t2_model_str(img->model);
    size_t cap = 256; char *out = (char*)malloc(cap);
    if (!out) return NULL;
    int n = snprintf(out, cap, "File: %s\nUID:", label);
    size_t pos = (n > 0) ? (size_t)n : 0;
    for (uint8_t i = 0; i < img->uid_len && pos < cap - 4; ++i) {
        n = snprintf(out + pos, cap - pos, " %02X", img->uid[i]);
        pos += (n > 0) ? (size_t)n : 0;
    }
    snprintf(out + pos, cap - pos, "\nPages total: %d\nFirst user page: %d\n", img->pages_total, img->first_user_page);
    return out;
}

#ifdef CONFIG_NFC_PN532
bool ntag_write_to_tag(pn532_io_handle_t io,
                       const ntag_file_image_t *img,
                       bool (*progress_cb)(int current, int total, void *user),
                       void *user) {
    if (!io || !img || !img->full_pages || img->pages_total <= 0) return false;

    NTAG2XX_MODEL model = NTAG2XX_UNKNOWN;
    (void)ntag2xx_get_model(io, &model);

    int start = img->first_user_page > 0 ? img->first_user_page : 4;
    if (start < 4) start = 4;
    int total = img->pages_total - start;
    if (total <= 0) return false;

    for (int pg = start; pg < img->pages_total; ++pg) {
        if (progress_cb) {
            if (!progress_cb(pg - start, total, user)) return false;
        }
        const uint8_t *data = &img->full_pages[(size_t)pg * 4];
        esp_err_t er = ntag2xx_write_page(io, (uint8_t)pg, data);
        if (er != ESP_OK) {
            ESP_LOGE(TAG, "write page %d failed (%d)", pg, (int)er);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (progress_cb) progress_cb(total, total, user);
    return true;
}
#endif
