#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "managers/nfc/pn532_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    NTAG2XX_MODEL model;
    uint16_t pages_total;
    uint8_t first_user_page;
    uint8_t last_user_page;
    uint16_t user_bytes;

    uint8_t page0_3[16];
    bool version_valid;
    uint8_t version[8];
    bool signature_valid;
    uint8_t signature[32];
    bool counter_valid[3];
    uint32_t counter[3];
    bool tearing_valid[3];
    uint8_t tearing[3];

    bool cc_valid;
    uint8_t cc[4];
    bool cc_read_only;
    bool static_locked;
    bool dynamic_locked;
    uint8_t dynamic_lock[4];

    bool config_valid;
    uint8_t auth0;
    uint8_t access;
    bool password_protected;
} ntag_t2_info_t;

uint16_t ntag_t2_pages_for_model(NTAG2XX_MODEL model);
uint16_t ntag_t2_user_bytes_for_model(NTAG2XX_MODEL model);
uint8_t ntag_t2_last_user_page_for_model(NTAG2XX_MODEL model);
NTAG2XX_MODEL ntag_t2_model_from_cc_size(uint8_t cc_size);
NTAG2XX_MODEL ntag_t2_model_from_version(const uint8_t version[8]);

bool ntag_t2_read_info(pn532_io_handle_t io, ntag_t2_info_t *info);
bool ntag_t2_can_write_page(const ntag_t2_info_t *info, uint8_t page,
                            char *reason, size_t reason_len);

#if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_ST25R3916)
bool ntag_t2_read_user_memory(pn532_io_handle_t io,
                              uint8_t **out_buf,
                              size_t *out_len,
                              NTAG2XX_MODEL *out_model);
bool ntag_t2_read_user_memory_fast(pn532_io_handle_t io,
                                   uint8_t **out_buf,
                                   size_t *out_len,
                                   ntag_t2_info_t *out_info);
#endif

bool ntag_t2_find_ndef(const uint8_t *mem,
                       size_t mem_len,
                       size_t *msg_off,
                       size_t *msg_len);

const char *ntag_t2_model_str(NTAG2XX_MODEL m);

// Convenience: Build a details string from raw Type 2 memory
// Returns malloc'd string which the caller must free.
char *ntag_t2_build_details_from_mem(const uint8_t *mem,
                                     size_t mem_len,
                                     const uint8_t *uid,
                                     uint8_t uid_len,
                                     NTAG2XX_MODEL model);

char *ntag_t2_build_details_from_mem_info(const uint8_t *mem,
                                          size_t mem_len,
                                          const uint8_t *uid,
                                          uint8_t uid_len,
                                          const ntag_t2_info_t *info);

#ifdef __cplusplus
}
#endif
