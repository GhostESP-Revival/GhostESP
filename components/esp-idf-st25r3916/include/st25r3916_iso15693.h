// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
//
// ISO15693 / NFC-V reader layer for ST25R3916.
// Built on the core FIFO/IRQ driver. Provides ISO15693 transceive primitives
// and PicoPass/iCLASS command framing on top of NFC-V mode.
//
// PicoPass protocol adaptation based on Momentum-Firmware's picopass application:
//   https://github.com/Next-Flip/Momentum-Firmware (GPL-3.0)
// Original picopass app by Eric Betts (bettse):
//   https://github.com/bettse/picopass

#ifndef ST25R3916_ISO15693_H
#define ST25R3916_ISO15693_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define ST25R3916_NFCV_UID_LEN    8
#define ST25R3916_NFCV_BLOCK_LEN  8

/**
 * @brief Transmit an ISO15693 frame and receive the response.
 *
 * @param tx          Bytes to transmit.
 * @param tx_len      Number of bytes to transmit.
 * @param with_crc    true: standard ISO15693 CRC (preset 0xFFFF) is appended
 *                    in software before encoding and stripped from the
 *                    decoded response; false: raw (no CRC), e.g. for
 *                    PicoPass, which uses a different CRC preset handled by
 *                    the caller.
 * @param[out] rx     Response buffer.
 * @param rx_cap      Capacity of @p rx.
 * @param[out] rx_len Number of response bytes returned (CRC stripped if with_crc).
 * @param timeout_ms  Maximum time to wait for the response.
 */
esp_err_t st25r3916_nfcv_transceive(const uint8_t *tx, uint16_t tx_len, bool with_crc,
                                    uint8_t *rx, uint16_t rx_cap,
                                    uint16_t *rx_len, int timeout_ms);

/**
 * @brief ISO15693 inventory command.
 *
 * Sends an INVENTORY (0x04) command with 1-out-of-4 slot coding and
 * returns the 8-byte UID of the first responding tag.
 *
 * @param[out] uid      8-byte UID buffer.
 * @param timeout_ms    Response timeout.
 * @return ESP_OK if a tag responded, ESP_ERR_NOT_FOUND otherwise.
 */
esp_err_t st25r3916_nfcv_inventory(uint8_t *uid, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // ST25R3916_ISO15693_H
