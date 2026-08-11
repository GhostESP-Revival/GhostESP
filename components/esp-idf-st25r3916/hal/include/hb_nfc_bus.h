// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
// Clean-room ST25R3916 support authored from ST datasheet DS12484 Rev 8.
// No vendor (ST SLA0044) or other third-party driver source is used.

#ifndef HB_NFC_BUS_H
#define HB_NFC_BUS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "st25r3916_hw_config.h"

/**
 * @brief Transport operations implemented by each bus backend (SPI / I2C).
 *
 * The register/FIFO/command semantics are identical across buses; only the
 * framing differs (full-duplex SPI vs. I2C write/read transactions). All
 * functions return ESP_OK on success.
 */
typedef struct {
  esp_err_t (*reg_read)(uint8_t addr, uint8_t *out_value);
  esp_err_t (*reg_write)(uint8_t addr, uint8_t value);
  esp_err_t (*reg_modify)(uint8_t addr, uint8_t mask, uint8_t value);
  esp_err_t (*fifo_load)(const uint8_t *data, size_t len);
  esp_err_t (*fifo_read)(uint8_t *data, size_t len);
  esp_err_t (*pt_memory_load)(uint8_t area, const uint8_t *data, size_t len);
  esp_err_t (*direct_cmd)(uint8_t cmd);
  esp_err_t (*raw_xfer)(const uint8_t *tx, uint8_t *rx, size_t len);
  void (*deinit)(void);
} hb_nfc_bus_ops_t;

/**
 * @brief Bring up the configured bus and select the active transport.
 *
 * @param config Hardware configuration. Must not be NULL.
 * @return ESP_OK on success, an error code otherwise.
 */
esp_err_t hb_nfc_bus_init(const st25r3916_hw_config_t *config);

/** @brief Tear down the active bus transport. */
void hb_nfc_bus_deinit(void);

/* --- Dispatched register / FIFO / command access (see ops table). --- */
esp_err_t hb_nfc_bus_reg_read(uint8_t addr, uint8_t *out_value);
esp_err_t hb_nfc_bus_reg_write(uint8_t addr, uint8_t value);
esp_err_t hb_nfc_bus_reg_modify(uint8_t addr, uint8_t mask, uint8_t value);
esp_err_t hb_nfc_bus_fifo_load(const uint8_t *data, size_t len);
esp_err_t hb_nfc_bus_fifo_read(uint8_t *data, size_t len);
esp_err_t hb_nfc_bus_pt_memory_load(uint8_t area, const uint8_t *data, size_t len);
esp_err_t hb_nfc_bus_direct_cmd(uint8_t cmd);
esp_err_t hb_nfc_bus_raw_xfer(const uint8_t *tx, uint8_t *rx, size_t len);

/* Backend ops tables and init hooks (implemented in hb_nfc_spi.c / hb_nfc_i2c.c). */
#ifdef CONFIG_NFC_ST25R3916_SPI
extern const hb_nfc_bus_ops_t hb_nfc_spi_ops;
#endif
#ifdef CONFIG_NFC_ST25R3916_I2C
extern const hb_nfc_bus_ops_t hb_nfc_i2c_ops;
esp_err_t hb_nfc_i2c_init(const st25r3916_hw_config_t *config);
#endif

#ifdef __cplusplus
}
#endif

#endif // HB_NFC_BUS_H
