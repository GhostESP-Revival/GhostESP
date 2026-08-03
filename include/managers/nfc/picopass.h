// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
//
// PicoPass / iCLASS protocol layer for ST25R3916.
// Provides detect, read, auth, PACS/Wiegand parsing, and .picopass file save.
//
// Based on Momentum-Firmware's picopass application:
//   https://github.com/Next-Flip/Momentum-Firmware (GPL-3.0)
// Original picopass app by Eric Betts and contributors:
//   https://github.com/bettse/picopass
//
// loclass crypto library from holiman/loclass and RfidResearchGroup/proxmark3:
//   Copyright (C) 2014 Martin Holst Swende
//   Copyright (C) Proxmark3 contributors (GPL-3.0)

#ifndef MANAGERS_NFC_PICOPASS_H
#define MANAGERS_NFC_PICOPASS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Constants ----------------------------------------------------------- */

#define PICOPASS_UID_LEN        8
#define PICOPASS_BLOCK_LEN      8
#define PICOPASS_MAX_APP_LIMIT  32

#define PICOPASS_CSN_BLOCK_INDEX        0
#define PICOPASS_CONFIG_BLOCK_INDEX     1
#define PICOPASS_SECURE_EPURSE_BLOCK_INDEX 2
#define PICOPASS_SECURE_KD_BLOCK_INDEX  3
#define PICOPASS_SECURE_KC_BLOCK_INDEX  4
#define PICOPASS_SECURE_AIA_BLOCK_INDEX 5
#define PICOPASS_NONSECURE_AIA_BLOCK_INDEX 2
#define PICOPASS_ICLASS_PACS_CFG_BLOCK_INDEX 6

/* Fuses */
#define PICOPASS_FUSE_PERS   0x80
#define PICOPASS_FUSE_CRYPT1 0x10
#define PICOPASS_FUSE_CRYPT0 0x08
#define PICOPASS_FUSE_CRYPT10 (PICOPASS_FUSE_CRYPT1 | PICOPASS_FUSE_CRYPT0)
#define PICOPASS_FUSE_RA     0x01

/* Encryption types from config block */
typedef enum {
    PicopassEncryptionUnknown = 0,
    PicopassEncryptionNone    = 0x14,
    PicopassEncryptionDES     = 0x15,
    PicopassEncryption3DES    = 0x17,
} PicopassEncryption;

/* --- Data structures ----------------------------------------------------- */

typedef struct {
    uint8_t data[PICOPASS_BLOCK_LEN];
} PicopassBlock;

typedef struct {
    bool valid;
    uint8_t bitLength;
    uint8_t FacilityCode;
    uint16_t CardNumber;
} PicopassWiegandRecord;

typedef struct {
    bool legacy;
    bool se_enabled;
    bool sio;
    bool biometrics;
    uint8_t key[PICOPASS_BLOCK_LEN];
    bool elite_kdf;
    uint8_t pin_length;
    PicopassEncryption encryption;
    uint8_t credential[PICOPASS_BLOCK_LEN];
    uint8_t pin0[PICOPASS_BLOCK_LEN];
    uint8_t pin1[PICOPASS_BLOCK_LEN];
    PicopassWiegandRecord record;
} PicopassPacs;

typedef struct {
    PicopassBlock AA1[PICOPASS_MAX_APP_LIMIT];
    PicopassPacs pacs;
} PicopassDeviceData;

/* --- Key constants ------------------------------------------------------- */

extern const uint8_t picopass_iclass_key[PICOPASS_BLOCK_LEN];
extern const uint8_t picopass_factory_credit_key[PICOPASS_BLOCK_LEN];
extern const uint8_t picopass_factory_debit_key[PICOPASS_BLOCK_LEN];

/* --- PicoPass CRC (ISO13239, init 0xE012, no final inversion) ------------ */

uint16_t picopass_calculate_ccitt(uint16_t preload, const uint8_t *buf, uint16_t len);

/* --- High-level operations ----------------------------------------------- */

/**
 * @brief Detect a PicoPass/iCLASS card and read CSN, config, AIA.
 *
 * Requires ST25R3916 backend with ISO15693 mode already initialized.
 * After this call, AA1[0..5] are populated.
 *
 * @param[out] data  Device data to populate.
 * @return ESP_OK on success.
 */
esp_err_t picopass_detect(PicopassDeviceData *data);

/**
 * @brief Authenticate with a dictionary of keys and read the full card.
 *
 * Tries standard KDF keys, then user dictionary if available.
 * On success, AA1 is fully populated and PACS is parsed.
 *
 * @param[in,out] data  Device data with CSN/config/AIA already populated.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no key matched.
 */
esp_err_t picopass_auth_and_read(PicopassDeviceData *data);

/**
 * @brief Parse PACS credential from populated AA1 blocks.
 */
esp_err_t picopass_parse_credential(PicopassBlock *AA1, PicopassPacs *pacs);

/**
 * @brief Parse Wiegand record from credential block.
 */
esp_err_t picopass_parse_wiegand(uint8_t *credential, PicopassWiegandRecord *record);

/**
 * @brief Save a .picopass file to the given path.
 *
 * @param path      Full file path (e.g. "/mnt/ghostesp/nfc/card.picopass").
 * @param data      Device data to save.
 * @return ESP_OK on success.
 */
esp_err_t picopass_save_file(const char *path, const PicopassDeviceData *data);

/**
 * @brief Check if a memory region is all the same byte.
 */
bool picopass_is_memset(const uint8_t *data, uint8_t val, size_t len);

#ifdef __cplusplus
}
#endif

#endif // MANAGERS_NFC_PICOPASS_H
