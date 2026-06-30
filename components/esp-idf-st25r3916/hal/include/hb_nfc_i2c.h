// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
// Clean-room ST25R3916 support authored from ST datasheet DS12484 Rev 8.
// No vendor (ST SLA0044) or other third-party driver source is used.

#ifndef HB_NFC_I2C_H
#define HB_NFC_I2C_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include "st25r3916_hw_config.h"

/**
 * @brief Initialize the I2C transport for the ST25R3916.
 *
 * Joins (or creates) the shared I2C bus for the configured port so the chip can
 * coexist with other I2C peripherals on the same SDA/SCL.
 *
 * @param config Hardware configuration (i2c_* fields used). Must not be NULL.
 * @return ESP_OK on success, an error code otherwise.
 */
esp_err_t hb_nfc_i2c_init(const st25r3916_hw_config_t *config);

#ifdef __cplusplus
}
#endif

#endif // HB_NFC_I2C_H
