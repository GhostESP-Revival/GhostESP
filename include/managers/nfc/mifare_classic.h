#pragma once
#include "sdkconfig.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "managers/nfc/mifare_attack.h"
#include "managers/nfc/pn532_compat.h"

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
#if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_ST25R3916)
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

bool mfc_hardnested_capture_file(pn532_io_handle_t io,
                                 const uint8_t* uid,
                                 uint8_t uid_len,
                                 uint16_t atqa,
                                 uint8_t sak,
                                 uint8_t known_block,
                                 bool known_key_b,
                                 const uint8_t known_key[6],
                                 uint8_t target_block,
                                 bool target_key_b,
                                 uint16_t samples,
                                 const char* out_dir,
                                 char* out_path,
                                 size_t out_path_len);
bool mfc_hardnested_capture_missing_file(pn532_io_handle_t io,
                                         const uint8_t* uid,
                                         uint8_t uid_len,
                                         uint16_t atqa,
                                         uint8_t sak,
                                         uint16_t samples_per_key,
                                         const char* out_dir,
                                         char* out_path,
                                         size_t out_path_len);
bool mfc_get_hardnested_defaults(uint8_t *known_block,
                                 bool *known_key_b,
                                 uint8_t known_key[6],
                                 uint8_t *target_block,
                                 bool *target_key_b);
bool mfc_has_unread_blocks(void);
#endif

typedef void (*mfc_progress_cb_t)(int current, int total, void* user);
void mfc_set_progress_callback(mfc_progress_cb_t cb, void* user);

void mfc_set_attack_hooks(const mfc_attack_hooks_t *hooks);

// Add a manually-entered MIFARE Classic key (12 hex digits, ' '/':' tolerated)
// to the user dictionary. Returns false if the string isn't exactly 6 bytes.
bool mfc_add_user_key_hex(const char *hex);

// Batch user-dict writes across a brute-force: begin defers per-key SD writes
// (keys stay cached in RAM); end persists all newly-found keys in one mount.
void mfc_user_dict_begin_batch(void);
void mfc_user_dict_end_batch(void);
