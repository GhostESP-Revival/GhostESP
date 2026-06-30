// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
// Clean-room ST25R3916 driver authored from ST datasheet DS12484 Rev 8.
// No vendor (ST SLA0044) or other third-party driver source is used.
//
// Low-level ST25R3916 reader driver: chip bring-up, RF field control, FIFO and
// IRQ primitives. ISO14443-A activation/transceive and the pn532-compatible
// frontend adapter are layered on top of this in separate files.

#ifndef ST25R3916_H
#define ST25R3916_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "st25r3916_hw_config.h"

/* --- Lifecycle ---------------------------------------------------------- */

/** @brief Power up and configure the chip over the configured bus (SPI/I2C). */
esp_err_t st25r3916_init(const st25r3916_hw_config_t *config);

/** @brief Power down the RF field and release the bus. */
void st25r3916_deinit(void);

/** @brief True once st25r3916_init() has succeeded. */
bool st25r3916_is_ready(void);

/** @brief Read the IC identity register; returns type/rev (bits per DS Table 117). */
esp_err_t st25r3916_check_id(uint8_t *out_id, uint8_t *out_type, uint8_t *out_rev);

/* --- RF field ----------------------------------------------------------- */

esp_err_t st25r3916_field_on(void);
esp_err_t st25r3916_field_off(void);
bool st25r3916_field_is_on(void);

/** @brief Configure the analog/protocol front-end for ISO14443-A polling. */
esp_err_t st25r3916_set_mode_nfca(void);

/* --- Register / command pass-through (through the active bus) ------------ */

esp_err_t st25r3916_reg_read(uint8_t addr, uint8_t *out_value);
esp_err_t st25r3916_reg_write(uint8_t addr, uint8_t value);
esp_err_t st25r3916_reg_modify(uint8_t addr, uint8_t mask, uint8_t value);
esp_err_t st25r3916_cmd(uint8_t direct_cmd);

/* --- FIFO --------------------------------------------------------------- */

/** @brief Number of bytes currently held in the FIFO (DS Table 66/67). */
uint16_t st25r3916_fifo_count(void);
void st25r3916_fifo_status(uint16_t *byte_count, uint8_t *last_bits);
void st25r3916_fifo_clear(void);
esp_err_t st25r3916_fifo_load(const uint8_t *data, size_t len);
esp_err_t st25r3916_fifo_read(uint8_t *out, size_t len);
esp_err_t st25r3916_pt_memory_load(uint8_t area, const uint8_t *data, size_t len);

/**
 * @brief Program the number of bytes/bits to transmit (DS Table 70/71).
 *
 * @param nbytes Complete bytes to transmit.
 * @param nbits  Extra bits of an incomplete final byte (0..7).
 */
void st25r3916_set_num_tx_bytes(uint16_t nbytes, uint8_t nbits);

/* --- Interrupts --------------------------------------------------------- */

/** @brief Clear the latched interrupt accumulator. */
void st25r3916_irq_clear(void);

/**
 * @brief Read pending interrupts and OR them into the latch.
 *
 * Reading the chip's interrupt registers clears them, so callers should use the
 * latching helpers rather than reading registers directly.
 *
 * @param[out] main  Optional: latched main IRQ bits (DS Table 62).
 * @param[out] timer Optional: latched timer/NFC IRQ bits (DS Table 63).
 * @param[out] error Optional: latched error/wake-up IRQ bits (DS Table 64).
 */
esp_err_t st25r3916_irq_update(uint8_t *main, uint8_t *timer, uint8_t *error);

/** @brief Read and latch passive-target IRQ bits (DS Table 65). */
esp_err_t st25r3916_irq_update_pt(uint8_t *pt);

/**
 * @brief Poll until any of the requested main-IRQ bits latch or timeout.
 *
 * @param main_mask Bits from ST25R3916_IRQ_MAIN_* to wait for.
 * @param timeout_ms Maximum wait.
 * @return ESP_OK if a requested bit latched, ESP_ERR_TIMEOUT otherwise.
 */
esp_err_t st25r3916_irq_wait_main(uint8_t main_mask, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // ST25R3916_H
