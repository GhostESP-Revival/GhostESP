#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "managers/nfc/pn532_compat.h"
#include "managers/nfc/flipper_nfc_compat.h"

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

// Broad check: SAK bit 5 indicates ISO14443-4 support (DESFire, EMV, etc.).
// Returns true for any tag that could be Type 4.
bool desfire_sak_is_iso14443_4(uint8_t sak);

#if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_ST25R3916)
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

// MIFARE DESFire read-side helpers.
//
// The tree walker is adapted from the MIFARE DESFire poller in Flipper Zero /
// Momentum-Firmware (GPL-3.0):
//   https://github.com/Next-Flip/Momentum-Firmware/lib/nfc/protocols/mf_desfire
// GhostESP reimplementation talks ISO7816 APDUs through the existing
// pn532_io_handle_t abstraction (works on both PN532 and ST25R3916 backends).

// Allocate an empty DESFire tree. Returns NULL on OOM. Free with desfire_tree_free().
MfDesfireData* desfire_tree_alloc(void);

// Free a tree previously returned from desfire_tree_alloc(). Safe on NULL.
void desfire_tree_free(MfDesfireData* data);

// Walk all applications and (plaintext) files on a selected DESFire PICC and
// populate `out`. Files that require authenticated/enciphered access are
// skipped silently. Returns true on success (even partial), false on hard
// failure (no applications listed or ISO14443-4 not selected).
bool desfire_read_tree(pn532_io_handle_t io, MfDesfireData* out);

// Build a complete Flipper Zero .nfc file string from a pre-read tree.
// Returns a malloc'd buffer the caller must free, or NULL on failure.
// Includes PICC version, application IDs, key settings (defaults), file IDs,
// file settings, and file data — fully compatible with Flipper's parser.
char *desfire_build_flipper_text(const MfDesfireData *tree,
                                 const uint8_t *uid, uint8_t uid_len,
                                 uint16_t atqa, uint8_t sak,
                                 const desfire_version_t *ver);

// Reconstruct a DESFire tree from a saved Flipper .nfc (inverse of
// desfire_build_flipper_text): application IDs, file IDs, file settings, and
// plaintext file data. When non-NULL, ver_out/have_ver_out are filled from the
// "PICC Version:" line. Returns a heap tree (free with desfire_tree_free) or
// NULL. Needs no reader hardware; lets saved tags run the same supported-card
// parsers as a live scan.
MfDesfireData *desfire_load_flipper_file(const char *path,
                                         desfire_version_t *ver_out,
                                         bool *have_ver_out);

#ifdef __cplusplus
}
#endif
