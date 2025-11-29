#pragma once

#include "sdkconfig.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Lightweight model classification for DESFire capacity
typedef enum {
    DESFIRE_MODEL_UNKNOWN = 0,
    DESFIRE_MODEL_2K,
    DESFIRE_MODEL_4K,
    DESFIRE_MODEL_8K,
} DESFIRE_MODEL;

#define DESFIRE_PICC_VERSION_MAX 32

// Basic version / capacity info extracted from GET_VERSION (when available)
typedef struct {
    DESFIRE_MODEL model;
    uint8_t       size_byte;      // Raw storage size byte from GET_VERSION
    uint32_t      storage_bytes;  // Decoded approximate storage size in bytes (0 if unknown)
    uint8_t       picc_version[DESFIRE_PICC_VERSION_MAX];
    uint8_t       picc_version_len;
} desfire_version_t;

// Heuristic check for DESFire-like tags based on ATQA / SAK.
// This is intentionally conservative to avoid false positives.
bool desfire_is_desfire_candidate(uint16_t atqa, uint8_t sak);

#ifdef CONFIG_NFC_PN532
#include "pn532.h"  // for pn532_io_handle_t

// Try to query DESFire version info via native GET_VERSION.
// Returns true on success and fills out struct, false if command fails or
// response is not recognized.
bool desfire_get_version(pn532_io_handle_t io, desfire_version_t *out);
#endif

// Human-readable model name for UI/CLI summaries.
const char *desfire_model_str(DESFIRE_MODEL m);

// Build a single "PICC Version: ..." line for Flipper-style saves.
bool desfire_build_picc_version_line(const desfire_version_t *ver,
                                     char *out,
                                     size_t out_cap);

// Build a compact DESFire summary string similar to the MIFARE / NTAG helpers.
// Returns a malloc'd string the caller must free. "ver" may be NULL when
// version/capacity could not be determined.
char *desfire_build_details_summary(const desfire_version_t *ver,
                                    const uint8_t *uid,
                                    uint8_t uid_len,
                                    uint16_t atqa,
                                    uint8_t sak);

#ifdef __cplusplus
}
#endif
