#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"
#ifdef CONFIG_NFC_PN532
#include "pn532.h"
#endif
#include "esp_err.h"
#include "managers/nfc/ntag_t2.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    NTAG2XX_MODEL model;
    uint8_t uid[10];
    uint8_t uid_len;
    int pages_total;          // total pages reported by file
    int first_user_page;      // usually 4 for Type 2
    uint8_t *full_pages;      // pages_total * 4 bytes (pages 0..pages_total-1)
} ntag_file_image_t;

bool ntag_file_load(const char *path, ntag_file_image_t *out);
void ntag_file_free(ntag_file_image_t *img);

// Builds a details string similar to scan details. Caller must free.
char *ntag_file_build_details(const ntag_file_image_t *img);

// Write image to tag using PN532. Returns true on success.
#ifdef CONFIG_NFC_PN532
bool ntag_write_to_tag(pn532_io_handle_t io,
                       const ntag_file_image_t *img,
                       bool (*progress_cb)(int current, int total, void *user),
                       void *user);
#endif

#ifdef __cplusplus
}
#endif
