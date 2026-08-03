// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
//
// MIFARE Classic software support for the ST25R3916, which has no hardware
// Crypto1. Implements the reader-side three-pass authentication and the
// encrypted command/response exchange (with encrypted parity) on top of the
// clean-room Crypto1 cipher and the bit-level transceive.

#ifndef ST25R3916_MIFARE_H
#define ST25R3916_MIFARE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "esp_err.h"
#include "crypto1.h"

/**
 * @brief MIFARE Classic three-pass authentication for a sector block.
 *
 * On success @p c holds the synchronized cipher state; subsequent commands must
 * go through st25r3916_mifare_xfer() until the tag is halted or reselected.
 *
 * @param c        Cipher state to initialize/sync.
 * @param uid      Tag UID (Momentum/Flipper CUID bytes are used for Crypto1).
 * @param uid_len  UID length.
 * @param block    Block number to authenticate.
 * @param key_type 0x60 = key A, 0x61 = key B.
 * @param key      6-byte key.
 * @return ESP_OK on success, ESP_ERR_INVALID_RESPONSE on auth failure.
 */
esp_err_t st25r3916_mifare_auth(crypto1_t *c, const uint8_t *uid, uint8_t uid_len, uint8_t block,
                                uint8_t key_type, const uint8_t key[6]);

/**
 * @brief Encrypted MIFARE command/response after a successful auth.
 *
 * Appends CRC_A to @p plain, encrypts it (with encrypted parity), transmits and
 * decrypts the response (CRC stripped). A 4-bit ACK/NAK is returned as a single
 * byte (low nibble) with *out_len == 1.
 */
esp_err_t st25r3916_mifare_xfer(crypto1_t *c, const uint8_t *plain, uint16_t plen, uint8_t *out,
                                uint16_t out_cap, uint16_t *out_len);

/**
 * @brief Send an encrypted nested AUTH command while a Crypto1 session is armed.
 *
 * Returns the raw encrypted tag nonce bytes and their received parity bits. This
 * is intended for offline nested/hardnested capture; the caller should reselect
 * and re-authenticate before collecting another sample.
 */
esp_err_t st25r3916_mifare_nested_auth_raw(crypto1_t *c, uint8_t key_type, uint8_t block,
                                           uint8_t nt_enc[4], uint8_t nt_par[4]);

#ifdef __cplusplus
}
#endif

#endif // ST25R3916_MIFARE_H
