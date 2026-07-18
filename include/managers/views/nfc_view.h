#ifndef NFC_VIEW_H
#define NFC_VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "managers/display_manager.h"

extern View nfc_view;

/* The extension facade intentionally exposes only complete Type-2 NDEF data. */
#define NFC_VIEW_T2_NDEF_MAX 1024

typedef struct {
    uint8_t uid[10];
    uint8_t uid_len;
    char model[24];
    uint16_t user_bytes;
    uint16_t ndef_length;
    bool ndef_present;
    bool read_only;
    bool password_protected;
    bool static_locked;
    bool dynamic_locked;
} nfc_view_t2_tag_info_t;

#ifdef __cplusplus
extern "C" {
#endif

bool nfc_api_get_last_uid(uint8_t *uid_out, uint8_t *uid_len_out);

/* A session is retained only after a Type-2 tag was successfully read. */
bool nfc_view_t2_scan_start(void);
bool nfc_view_t2_scan_stop(void);
bool nfc_view_t2_scan_active(void);
bool nfc_view_t2_read(nfc_view_t2_tag_info_t *out_info,
                      uint8_t *ndef_out, size_t max_ndef_bytes,
                      size_t *ndef_bytes_out);
bool nfc_view_t2_write_ndef(const uint8_t *ndef, size_t ndef_len);

#ifdef __cplusplus
}
#endif

#endif
