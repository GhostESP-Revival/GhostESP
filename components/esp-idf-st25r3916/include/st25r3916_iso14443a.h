// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
// Clean-room ST25R3916 support authored from ST datasheet DS12484 Rev 8.
// No vendor (ST SLA0044) or other third-party driver source is used.
//
// ISO14443-A reader layer: a byte-oriented transceive primitive plus REQA/WUPA,
// anti-collision cascade and SELECT, built on the core FIFO/IRQ driver. The
// transmit/receive procedure follows DS 4.4 (Transmit with/without CRC,
// Transmit REQA/WUPA) and the FIFO holds received CRC bytes (DS 4.2).

#ifndef ST25R3916_ISO14443A_H
#define ST25R3916_ISO14443A_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/**
 * @brief Transmit a frame and receive the response (whole bytes).
 *
 * @param tx          Bytes to transmit (may be NULL when tx_len == 0).
 * @param tx_len      Number of complete bytes to transmit.
 * @param with_crc    true: hardware appends TX CRC and the 2 received CRC bytes
 *                    are stripped from the response; false: raw (no CRC).
 * @param anticoll    true: send as an ISO14443-A anti-collision frame (antcl).
 * @param[out] rx     Response buffer.
 * @param rx_cap      Capacity of @p rx.
 * @param[out] rx_len Number of response bytes returned (CRC already stripped).
 * @param timeout_ms  Maximum time to wait for the response.
 * @return ESP_OK, ESP_ERR_TIMEOUT (no response), or a protocol error.
 */
esp_err_t st25r3916_nfca_transceive(const uint8_t *tx, uint16_t tx_len, bool with_crc,
                                    bool anticoll, uint8_t *rx, uint16_t rx_cap,
                                    uint16_t *rx_len, int timeout_ms);

/**
 * @brief Bit-level transceive with caller-supplied parity (MIFARE Crypto1).
 *
 * Sets no_tx_par/no_rx_par (DS Table 27) so parity bits are taken from / placed
 * into the FIFO bitstream instead of being generated/checked by hardware. Each
 * input byte is followed by its parity bit on the air; the response is unpacked
 * the same way. No CRC is appended (encrypted frames carry their own CRC in the
 * plaintext). 106 kbit/s only.
 *
 * @param tx_data   Bytes to transmit.
 * @param tx_par    Parity bit (0/1) for each tx byte.
 * @param n_tx      Number of tx bytes.
 * @param[out] rx_data Response bytes.
 * @param[out] rx_par  Optional parity bit per response byte.
 * @param rx_cap    Capacity of rx_data/rx_par (bytes).
 * @param[out] n_rx  Number of whole response bytes recovered.
 * @param[out] rx_residual_bits Optional: bit count of a trailing partial group
 *                  (e.g. a 4-bit MIFARE ACK/NAK) that did not form a full byte.
 * @param[out] rx_residual      Optional: those residual bits, LSB-aligned.
 * @param timeout_ms Response timeout.
 */
esp_err_t st25r3916_nfca_transceive_bits(const uint8_t *tx_data, const uint8_t *tx_par,
                                         uint16_t n_tx, uint8_t *rx_data, uint8_t *rx_par,
                                         uint16_t rx_cap, uint16_t *n_rx,
                                         uint8_t *rx_residual_bits, uint8_t *rx_residual,
                                         int timeout_ms);

/** @brief Send REQA (short frame); returns the 2-byte ATQA. */
esp_err_t st25r3916_nfca_reqa(uint16_t *atqa, int timeout_ms);

/** @brief Send WUPA (short frame); returns the 2-byte ATQA. */
esp_err_t st25r3916_nfca_wupa(uint16_t *atqa, int timeout_ms);

/**
 * @brief Full ISO14443-3A activation: REQA + anti-collision cascade + SELECT.
 *
 * @param[out] uid      Receives the UID (4, 7 or 10 bytes, cascade tags removed).
 * @param[out] uid_len  Receives the UID length.
 * @param[out] atqa     Optional: 2-byte ATQA (SENS_RES).
 * @param[out] sak      Optional: final SAK (SEL_RES).
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no tag answered.
 */
esp_err_t st25r3916_nfca_activate(uint8_t *uid, uint8_t *uid_len, uint16_t *atqa, uint8_t *sak);

#ifdef __cplusplus
}
#endif

#endif // ST25R3916_ISO14443A_H
