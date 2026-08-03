#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "managers/nfc/pn532_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

// Synthesizes a blank NTAG21x image (random UID, standard CC/lock/config
// pages) carrying the given NDEF TLV blob starting at page 4, and writes it
// out as a Flipper-format .nfc file under /mnt/ghostesp/nfc so the existing
// Saved/Write/Emulate file browsers pick it up automatically.
//
// name_hint is used to derive part of the filename (sanitized); may be NULL.
// out_path (if non-NULL) receives the written path, truncated to out_path_cap.
bool ndef_tag_gen_save_file(NTAG2XX_MODEL model,
                            const uint8_t *ndef_tlv, size_t ndef_tlv_len,
                            const char *name_hint,
                            char *out_path, size_t out_path_cap);

#ifdef __cplusplus
}
#endif
