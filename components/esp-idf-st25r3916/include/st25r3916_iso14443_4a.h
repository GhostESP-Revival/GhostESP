// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
//
// ISO14443-4A protocol layer for ST25R3916.
// Ported from Flipper Zero / Momentum-Firmware (GPL-3.0).
// See st25r3916_iso14443_4a.c for full attribution and protocol references.

#ifndef ST25R3916_ISO14443_4A_H
#define ST25R3916_ISO14443_4A_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/** Opaque ISO14443-4A session (state: PCB/ATS/CID/NAD/activation). */
typedef struct st25r3916_iso14443_4a_session st25r3916_iso14443_4a_session_t;

/** Allocate a session. Free with st25r3916_iso14443_4a_session_free(). */
esp_err_t st25r3916_iso14443_4a_session_alloc(st25r3916_iso14443_4a_session_t **out_session);

/** Free a session (does not DESELECT the card). Safe on NULL. */
void st25r3916_iso14443_4a_session_free(st25r3916_iso14443_4a_session_t *session);

/**
 * Activate ISO14443-4 on the currently-selected ISO14443-3A card:
 * sends RATS, parses ATS, and initialises the I/R/S-block state machine.
 * Must be called after st25r3916_nfca_activate/_reselect_uid returned SAK with
 * the ISO14443-4 bit set (e.g. SAK 0x20 for DESFire).
 */
esp_err_t st25r3916_iso14443_4a_activate(st25r3916_iso14443_4a_session_t *session);

/** (Re)read ATS via RATS into the session (rarely needed directly). */
esp_err_t st25r3916_iso14443_4a_read_ats(st25r3916_iso14443_4a_session_t *session);

/**
 * Send a single ISO14443-4 block with WTX handling.
 * tx/rx carry the INF payload (e.g. a wrapped ISO7816 APDU); PCB/CID/NAD are
 * added/stripped by the layer. rx_len receives the INF length.
 */
esp_err_t st25r3916_iso14443_4a_send_block(st25r3916_iso14443_4a_session_t *session,
                                           const uint8_t *tx, uint16_t tx_len,
                                           uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len);

/** Like send_block but with PCB-mismatch retransmission and WTX response retries. */
esp_err_t st25r3916_iso14443_4a_send_block_pwt_ext(st25r3916_iso14443_4a_session_t *session,
                                                   const uint8_t *tx, uint16_t tx_len,
                                                   uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len);

/** Send a chained I-block (sets the chaining bit for this transfer). */
esp_err_t st25r3916_iso14443_4a_send_chain_block(st25r3916_iso14443_4a_session_t *session,
                                                 const uint8_t *tx, uint16_t tx_len,
                                                 uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len);

/** Send an R-block (acknowledged=true for ACK, false for NAK). */
esp_err_t st25r3916_iso14443_4a_send_r_block(st25r3916_iso14443_4a_session_t *session,
                                             bool acknowledged,
                                             const uint8_t *tx, uint16_t tx_len,
                                             uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len);

/** Send an S-block (deselect=true for DESELECT, false for WTX). */
esp_err_t st25r3916_iso14443_4a_send_s_block(st25r3916_iso14443_4a_session_t *session,
                                             bool deselect,
                                             const uint8_t *tx, uint16_t tx_len,
                                             uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len);

/**
 * Convenience APDU exchange: sends a full APDU (e.g. DESFire native command
 * in 0x90 ... framing) over an activated session, automatically chaining
 * across I-blocks when the APDU exceeds the card's frame size (FSCI).
 * Handles WTX and block-number toggling. rx receives the concatenated INF
 * payloads of the response chain.
 */
esp_err_t st25r3916_iso14443_4a_transceive(st25r3916_iso14443_4a_session_t *session,
                                           const uint8_t *apdu, uint16_t apdu_len,
                                           uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len,
                                           int timeout_ms);

/** Send S(DESELECT) and mark the session inactive. Timeout counts as success. */
esp_err_t st25r3916_iso14443_4a_halt(st25r3916_iso14443_4a_session_t *session);

/** Max INF payload per I-block from the card's FSCI (0 if unknown). */
uint16_t st25r3916_iso14443_4a_get_frame_size_max(const st25r3916_iso14443_4a_session_t *session);

/** ATS historical bytes (T1...Tk), or NULL when absent. */
const uint8_t *st25r3916_iso14443_4a_get_historical_bytes(
    const st25r3916_iso14443_4a_session_t *session, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif // ST25R3916_ISO14443_4A_H
