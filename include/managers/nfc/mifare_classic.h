#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "managers/nfc/mifare_attack.h"
#ifdef CONFIG_NFC_PN532
#include "pn532.h"
#endif

typedef enum {
    MFC_UNKNOWN = 0,
    MFC_MINI,
    MFC_1K,
    MFC_4K,
} MFC_TYPE;

bool mfc_is_classic_sak(uint8_t sak);
MFC_TYPE mfc_type_from_sak(uint8_t sak);
int mfc_sector_count(MFC_TYPE t);
int mfc_blocks_in_sector(MFC_TYPE t, int sector);
int mfc_first_block_of_sector(MFC_TYPE t, int sector);

// Builds a compact summary. Tries default keys, does not write.
// Returns malloc'd string which caller must free.
#ifdef CONFIG_NFC_PN532
char* mfc_build_details_summary(pn532_io_handle_t io,
                                const uint8_t* uid,
                                uint8_t uid_len,
                                uint16_t atqa,
                                uint8_t sak);

// Writes Flipper-compatible MIFARE Classic file (Data format version 2).
// Returns true on success. If out_path is provided, the created path is written there.
bool mfc_save_flipper_file(pn532_io_handle_t io,
                           const uint8_t* uid,
                           uint8_t uid_len,
                           uint16_t atqa,
                           uint8_t sak,
                           const char* out_dir,
                           char* out_path,
                           size_t out_path_len);
#endif

typedef void (*mfc_progress_cb_t)(int current, int total, void* user);
void mfc_set_progress_callback(mfc_progress_cb_t cb, void* user);

void mfc_set_attack_hooks(const mfc_attack_hooks_t *hooks);
