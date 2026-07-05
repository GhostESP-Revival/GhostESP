// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
//
// PicoPass / iCLASS protocol layer for ST25R3916.
//
// Based on Momentum-Firmware's picopass application:
//   https://github.com/Next-Flip/Momentum-Firmware (GPL-3.0)
// Original picopass app by Eric Betts and contributors:
//   https://github.com/bettse/picopass
//
// loclass crypto library from holiman/loclass and RfidResearchGroup/proxmark3:
//   Copyright (C) 2014 Martin Holst Swende
//   Copyright (C) Proxmark3 contributors (GPL-3.0)
//
// Wiegand/PACS parsing based on Flipper Zero Momentum-Firmware picopass_device.c
//   by Eric Betts (bettse), Patrick Cunningham, Tiernan Messmer.

// This entire file is ST25R3916-specific; every caller already guards its
// use of picopass_* with #ifdef CONFIG_NFC_ST25R3916 (see nfc_cli.c), so
// compiling to nothing here on boards without that hardware is safe -- the
// st25r3916.h include path is also only added to the build when this config
// is set (see main/CMakeLists.txt), so without this guard the file fails to
// compile at all on those boards.
#ifdef CONFIG_NFC_ST25R3916

#include "managers/nfc/picopass.h"
#include "managers/nfc/loclass/optimized_cipher.h"
#include "st25r3916.h"
#include "st25r3916_reg.h"
#include "st25r3916_iso15693.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "managers/nfc/loclass/standalone_des.h"

static const char *TAG = "picopass";

/* --- Key constants (from Momentum picopass_keys.c) ----------------------- */

const uint8_t picopass_iclass_key[PICOPASS_BLOCK_LEN] = {
    0xaf, 0xa7, 0x85, 0xa7, 0xda, 0xb3, 0x33, 0x78};
const uint8_t picopass_factory_credit_key[PICOPASS_BLOCK_LEN] = {
    0x76, 0x65, 0x54, 0x43, 0x32, 0x21, 0x10, 0x00};
const uint8_t picopass_factory_debit_key[PICOPASS_BLOCK_LEN] = {
    0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5, 0x96, 0x87};

/* --- PicoPass command bytes ---------------------------------------------- */

#define PICOPASS_CMD_READ_OR_IDENTIFY 0x0C
#define PICOPASS_CMD_READ4            0x06
#define PICOPASS_CMD_UPDATE           0x87
#define PICOPASS_CMD_READCHECK_KD     0x88
#define PICOPASS_CMD_READCHECK_KC     0x18
#define PICOPASS_CMD_CHECK            0x05
#define PICOPASS_CMD_ACTALL           0x0A
#define PICOPASS_CMD_ACT              0x8E
#define PICOPASS_CMD_SELECT           0x81
#define PICOPASS_CMD_DETECT           0x0F
#define PICOPASS_CMD_HALT             0x00
#define PICOPASS_CMD_PAGESEL          0x84

/* iCLASS decryption key for 3DES-encrypted PACS */
static const uint8_t picopass_iclass_decryptionkey[16] = {
    0xb4, 0x21, 0x2c, 0xca, 0xb7, 0xed, 0x21, 0x0f,
    0x7b, 0x93, 0xd4, 0x59, 0x39, 0xc7, 0xdd, 0x36};

/* --- PicoPass CRC (ISO13239, init 0xE012, no final inversion) ----------- */

static uint16_t picopass_update_ccitt(uint16_t crc, uint8_t data) {
    uint8_t dat = data;
    dat ^= (uint8_t)(crc & 0xFFU);
    dat ^= (dat << 4);
    crc = (crc >> 8) ^ (((uint16_t)dat) << 8) ^ (((uint16_t)dat) << 3) ^ (((uint16_t)dat) >> 4);
    return crc;
}

uint16_t picopass_calculate_ccitt(uint16_t preload, const uint8_t *buf, uint16_t len) {
    uint16_t crc = preload;
    for(uint16_t i = 0; i < len; i++) {
        crc = picopass_update_ccitt(crc, buf[i]);
    }
    return crc;
}

static void picopass_append_crc(uint8_t *buf, uint16_t size) {
    uint16_t crc = picopass_calculate_ccitt(0xE012, buf, size);
    buf[size] = crc & 0xFF;
    buf[size + 1] = crc >> 8;
}

/* --- Low-level PicoPass transceive --------------------------------------- */

static esp_err_t picopass_transceive(const uint8_t *tx, uint16_t tx_len,
                                     uint8_t *rx, uint16_t rx_cap,
                                     uint16_t *rx_len, int timeout_ms) {
    /* PicoPass over ISO15693: no hardware CRC (PicoPass uses its own CRC-16) */
    return st25r3916_nfcv_transceive(tx, tx_len, false, rx, rx_cap, rx_len, timeout_ms);
}

/* --- PicoPass protocol primitives ---------------------------------------- */

static esp_err_t picopass_check_presence(void) {
    uint8_t tx[1] = {PICOPASS_CMD_ACTALL};
    uint8_t rx[32] = {0};
    uint16_t rx_len = 0;

    /* Debug: dump key registers before ACTALL */
    uint8_t mode = 0, bitrate = 0, iso15 = 0, aux = 0, opctrl = 0;
    st25r3916_reg_read(ST25R3916_REG_MODE, &mode);
    st25r3916_reg_read(ST25R3916_REG_BIT_RATE, &bitrate);
    st25r3916_reg_read(ST25R3916_REG_ISO14443B_FELICA, &iso15);
    st25r3916_reg_read(ST25R3916_REG_AUX, &aux);
    st25r3916_reg_read(ST25R3916_REG_OP_CONTROL, &opctrl);
    ESP_LOGI(TAG, "PRESENCE: MODE=0x%02X BITRATE=0x%02X ISO15=0x%02X AUX=0x%02X OP=0x%02X",
             mode, bitrate, iso15, aux, opctrl);

    /* First try standard ISO15693 inventory to verify RF layer works */
    uint8_t inv_uid[8] = {0};
    esp_err_t inv_err = st25r3916_nfcv_inventory(inv_uid, 30);
    ESP_LOGI(TAG, "PRESENCE: ISO15693 inventory: %s uid=%02X%02X%02X%02X%02X%02X%02X%02X",
             inv_err == ESP_OK ? "OK" : esp_err_to_name(inv_err),
             inv_uid[0], inv_uid[1], inv_uid[2], inv_uid[3],
             inv_uid[4], inv_uid[5], inv_uid[6], inv_uid[7]);

    ESP_LOGI(TAG, "PRESENCE: sending ACTALL (0x%02X), %d bytes", tx[0], 1);
    esp_err_t err = picopass_transceive(tx, 1, rx, sizeof(rx), &rx_len, 30);
    ESP_LOGI(TAG, "PRESENCE: transceive returned %s, rx_len=%d", esp_err_to_name(err), (int)rx_len);

    if(err == ESP_ERR_INVALID_RESPONSE) return ESP_OK; /* collision = present */
    if(err == ESP_OK) return ESP_OK; /* data received = present */
    return err; /* timeout = not present */
}

static esp_err_t picopass_identify(uint8_t *csn) {
    uint8_t tx[1] = {PICOPASS_CMD_READ_OR_IDENTIFY};
    uint8_t rx[10] = {0}; // 8 CSN + 2 CRC
    uint16_t rx_len = 0;

    esp_err_t err = picopass_transceive(tx, 1, rx, sizeof(rx), &rx_len, 20);
    if(err != ESP_OK) return err;
    if(rx_len < 8) return ESP_ERR_INVALID_RESPONSE;

    memcpy(csn, rx, PICOPASS_UID_LEN);
    return ESP_OK;
}

static esp_err_t picopass_select(const uint8_t *csn, uint8_t *real_csn) {
    uint8_t tx[9] = {0};
    tx[0] = PICOPASS_CMD_SELECT;
    memcpy(&tx[1], csn, PICOPASS_UID_LEN);

    uint8_t rx[10] = {0};
    uint16_t rx_len = 0;

    esp_err_t err = picopass_transceive(tx, 9, rx, sizeof(rx), &rx_len, 20);
    if(err != ESP_OK) return err;
    if(rx_len < 8) return ESP_ERR_INVALID_RESPONSE;

    memcpy(real_csn, rx, PICOPASS_UID_LEN);
    return ESP_OK;
}

static esp_err_t picopass_readcheck(uint8_t *ccnr) {
    uint8_t tx[2] = {PICOPASS_CMD_READCHECK_KD, 0x02};
    uint8_t rx[10] = {0}; // 8 CCNR + 2 CRC
    uint16_t rx_len = 0;

    esp_err_t err = picopass_transceive(tx, 2, rx, sizeof(rx), &rx_len, 20);
    if(err != ESP_OK) return err;
    if(rx_len < 8) return ESP_ERR_INVALID_RESPONSE;

    memcpy(ccnr, rx, 8);
    return ESP_OK;
}

static esp_err_t picopass_check(const uint8_t *mac, uint8_t *chip_mac) {
    uint8_t tx[9] = {0};
    tx[0] = PICOPASS_CMD_CHECK;
    memset(&tx[1], 0, 4); // null bytes
    memcpy(&tx[5], mac, 4);

    uint8_t rx[6] = {0};
    uint16_t rx_len = 0;

    esp_err_t err = picopass_transceive(tx, 9, rx, sizeof(rx), &rx_len, 20);
    if(err != ESP_OK) return err;
    if(rx_len < 4) return ESP_ERR_INVALID_RESPONSE;

    memcpy(chip_mac, rx, 4);
    return ESP_OK;
}

static esp_err_t picopass_read_block(uint8_t block_num, uint8_t *data) {
    uint8_t tx[4] = {PICOPASS_CMD_READ_OR_IDENTIFY, block_num, 0, 0};
    uint16_t crc = picopass_calculate_ccitt(0xE012, &tx[1], 1);
    memcpy(&tx[2], &crc, sizeof(uint16_t));

    uint8_t rx[10] = {0}; // 8 data + 2 CRC
    uint16_t rx_len = 0;

    esp_err_t err = picopass_transceive(tx, 4, rx, sizeof(rx), &rx_len, 20);
    if(err != ESP_OK) return err;
    if(rx_len < 8) return ESP_ERR_INVALID_RESPONSE;

    memcpy(data, rx, PICOPASS_BLOCK_LEN);
    return ESP_OK;
}

/* --- High-level operations ----------------------------------------------- */

bool picopass_is_memset(const uint8_t *data, uint8_t val, size_t len) {
    for(size_t i = 0; i < len; i++) {
        if(data[i] != val) return false;
    }
    return true;
}

static esp_err_t picopass_read_preauth(PicopassDeviceData *data) {
    PicopassBlock *AA1 = data->AA1;
    uint8_t anti_csn[PICOPASS_UID_LEN] = {0};
    uint8_t real_csn[PICOPASS_UID_LEN] = {0};

    /* Small delay after ACTALL before IDENTIFY */
    esp_rom_delay_us(1000);

    esp_err_t err = picopass_identify(anti_csn);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "identify failed: %s", esp_err_to_name(err));
        return err;
    }

    err = picopass_select(anti_csn, real_csn);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "select failed: %s", esp_err_to_name(err));
        return err;
    }

    memcpy(AA1[PICOPASS_CSN_BLOCK_INDEX].data, real_csn, PICOPASS_UID_LEN);

    uint8_t cfg[PICOPASS_BLOCK_LEN] = {0};
    err = picopass_read_block(PICOPASS_CONFIG_BLOCK_INDEX, cfg);
    if(err == ESP_OK) {
        memcpy(AA1[PICOPASS_CONFIG_BLOCK_INDEX].data, cfg, PICOPASS_BLOCK_LEN);
    }

    uint8_t aia[PICOPASS_BLOCK_LEN] = {0};
    err = picopass_read_block(PICOPASS_SECURE_AIA_BLOCK_INDEX, aia);
    if(err == ESP_OK) {
        memcpy(AA1[PICOPASS_SECURE_AIA_BLOCK_INDEX].data, aia, PICOPASS_BLOCK_LEN);
    }

    return ESP_OK;
}

static esp_err_t picopass_try_auth(PicopassDeviceData *data, const uint8_t *key, bool elite) {
    PicopassBlock *AA1 = data->AA1;
    PicopassPacs *pacs = &data->pacs;

    uint8_t *csn = AA1[PICOPASS_CSN_BLOCK_INDEX].data;
    uint8_t *div_key = AA1[PICOPASS_SECURE_KD_BLOCK_INDEX].data;

    uint8_t ccnr[12] = {0};
    uint8_t mac[4] = {0};
    uint8_t chip_mac[4] = {0};

    esp_err_t err = picopass_readcheck(ccnr);
    if(err != ESP_OK) return err;

    loclass_iclass_calc_div_key(csn, (uint8_t *)key, div_key, elite);
    loclass_opt_doReaderMAC(ccnr, div_key, mac);

    err = picopass_check(mac, chip_mac);
    if(err != ESP_OK) return err;

    memcpy(pacs->key, key, PICOPASS_BLOCK_LEN);
    pacs->elite_kdf = elite;
    return ESP_OK;
}

static esp_err_t picopass_read_card(PicopassDeviceData *data) {
    PicopassBlock *AA1 = data->AA1;

    size_t app_limit = AA1[PICOPASS_CONFIG_BLOCK_INDEX].data[0];
    if(app_limit > PICOPASS_MAX_APP_LIMIT) app_limit = PICOPASS_MAX_APP_LIMIT;
    if(app_limit < 6) app_limit = 6; // minimum: blocks 0-5

    for(size_t i = 2; i < app_limit; i++) {
        if(i == PICOPASS_SECURE_KD_BLOCK_INDEX) continue; // Kd from auth

        uint8_t block[PICOPASS_BLOCK_LEN] = {0};
        esp_err_t err = picopass_read_block((uint8_t)i, block);
        if(err != ESP_OK) {
            ESP_LOGE(TAG, "read block %d failed: %s", (int)i, esp_err_to_name(err));
            return err;
        }
        memcpy(AA1[i].data, block, PICOPASS_BLOCK_LEN);
    }

    return ESP_OK;
}

static esp_err_t picopass_decrypt_block(const uint8_t *enc, uint8_t *dec) {
    standalone_des3_context ctx;
    standalone_des3_set2key_dec(&ctx, picopass_iclass_decryptionkey);
    standalone_des3_crypt_ecb(&ctx, enc, dec);
    return ESP_OK;
}

esp_err_t picopass_parse_credential(PicopassBlock *AA1, PicopassPacs *pacs) {
    pacs->biometrics = AA1[6].data[4];
    pacs->pin_length = AA1[6].data[6] & 0x0F;
    pacs->encryption = (PicopassEncryption)AA1[6].data[7];

    if(pacs->encryption == PicopassEncryption3DES) {
        picopass_decrypt_block(AA1[7].data, pacs->credential);
        picopass_decrypt_block(AA1[8].data, pacs->pin0);
        picopass_decrypt_block(AA1[9].data, pacs->pin1);
    } else if(pacs->encryption == PicopassEncryptionNone) {
        memcpy(pacs->credential, AA1[7].data, PICOPASS_BLOCK_LEN);
        memcpy(pacs->pin0, AA1[8].data, PICOPASS_BLOCK_LEN);
        memcpy(pacs->pin1, AA1[9].data, PICOPASS_BLOCK_LEN);
    } else {
        ESP_LOGW(TAG, "unsupported encryption type 0x%02X", pacs->encryption);
    }

    pacs->sio = (AA1[10].data[0] == 0x30);
    return ESP_OK;
}

esp_err_t picopass_parse_wiegand(uint8_t *credential, PicopassWiegandRecord *record) {
    uint32_t *halves = (uint32_t *)credential;
    if(halves[0] == 0) {
        uint8_t leading0s = __builtin_clz(__builtin_bswap32(halves[1]));
        record->bitLength = 31 - leading0s;
    } else {
        uint8_t leading0s = __builtin_clz(__builtin_bswap32(halves[0]));
        record->bitLength = 63 - leading0s;
    }

    if(record->bitLength == 0 || record->bitLength > 63) {
        record->valid = false;
        return ESP_OK;
    }

    uint64_t sentinel = __builtin_bswap64(1ULL << record->bitLength);
    uint64_t swapped = 0;
    memcpy(&swapped, credential, sizeof(uint64_t));
    swapped = swapped ^ sentinel;
    memcpy(credential, &swapped, sizeof(uint64_t));

    if(record->bitLength == 26) {
        uint8_t *v4 = credential + 4;
        uint32_t bot = v4[3] | (v4[2] << 8) | (v4[1] << 16) | (v4[0] << 24);
        record->CardNumber = (bot >> 1) & 0xFFFF;
        record->FacilityCode = (bot >> 17) & 0xFF;
        record->valid = true;
    } else {
        record->CardNumber = 0;
        record->FacilityCode = 0;
        record->valid = false;
    }
    return ESP_OK;
}

esp_err_t picopass_detect(PicopassDeviceData *data) {
    memset(data, 0, sizeof(PicopassDeviceData));

    ESP_LOGI(TAG, "picopass_detect: checking presence...");

    /* Try ACTALL - card may need one attempt after field power-up */
    esp_err_t err = picopass_check_presence();

    if(err != ESP_OK) {
        ESP_LOGD(TAG, "picopass_detect: no card present (%s)", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "picopass_detect: card present, reading preauth...");

    err = picopass_read_preauth(data);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "picopass_detect: read_preauth failed (%s)", esp_err_to_name(err));
        return err;
    }

    PicopassPacs *pacs = &data->pacs;
    pacs->legacy = picopass_is_memset(data->AA1[5].data, 0xFF, 8);
    pacs->se_enabled = (memcmp(data->AA1[5].data, "\xff\xff\xff\x00\x06\xff\xff\xff", 8) == 0);

    if(pacs->se_enabled) {
        ESP_LOGW(TAG, "picopass_detect: SE-enabled card; not supported");
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "picopass_detect: success");
    return ESP_OK;
}

esp_err_t picopass_auth_and_read(PicopassDeviceData *data) {
    static const uint8_t *standard_keys[] = {
        picopass_iclass_key,
        picopass_factory_debit_key,
        picopass_factory_credit_key,
    };
    static const size_t num_standard_keys = sizeof(standard_keys) / sizeof(standard_keys[0]);

    /* Try standard keys with standard KDF */
    for(size_t i = 0; i < num_standard_keys; i++) {
        esp_err_t err = picopass_try_auth(data, standard_keys[i], false);
        if(err == ESP_OK) {
            ESP_LOGI(TAG, "auth success with standard key %d", (int)i);
            return picopass_read_card(data);
        }
        /* Re-detect after failed auth (card may need re-activation) */
        picopass_detect(data);
    }

    /* Try standard keys with elite KDF */
    for(size_t i = 0; i < num_standard_keys; i++) {
        esp_err_t err = picopass_try_auth(data, standard_keys[i], true);
        if(err == ESP_OK) {
            ESP_LOGI(TAG, "auth success with elite key %d", (int)i);
            return picopass_read_card(data);
        }
        picopass_detect(data);
    }

    ESP_LOGW(TAG, "no key matched; card may require dictionary attack");
    return ESP_ERR_NOT_FOUND;
}

/* --- File save (.picopass format) ---------------------------------------- */

esp_err_t picopass_save_file(const char *path, const PicopassDeviceData *data) {
    if(!path || !data) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(path, "w");
    if(!f) {
        ESP_LOGE(TAG, "failed to open %s for writing", path);
        return ESP_FAIL;
    }

    const PicopassBlock *AA1 = data->AA1;

    /* Flipper Picopass device format. Only the header and space-separated block
     * hex are written, exactly as flipper_format emits, so files load on a real
     * Flipper. Decoded PACS is intentionally not stored (Flipper derives it). */
    fprintf(f, "Filetype: Flipper Picopass device\n");
    fprintf(f, "Version: 1\n");
    fprintf(f, "# Picopass blocks\n");

    size_t app_limit = AA1[PICOPASS_CONFIG_BLOCK_INDEX].data[0];
    if(app_limit > PICOPASS_MAX_APP_LIMIT) app_limit = PICOPASS_MAX_APP_LIMIT;
    if(app_limit < 6) app_limit = 6;

    for(size_t i = 0; i < app_limit; i++) {
        fprintf(f, "Block %d: ", (int)i);
        for(int j = 0; j < PICOPASS_BLOCK_LEN; j++) {
            fprintf(f, "%s%02X", j ? " " : "", AA1[i].data[j]);
        }
        fprintf(f, "\n");
    }

    fclose(f);
    ESP_LOGI(TAG, "saved .picopass to %s (%d blocks)", path, (int)app_limit);
    return ESP_OK;
}

#endif // CONFIG_NFC_ST25R3916
