// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
// Clean-room ST25R3916 support authored from ST datasheet DS12484 Rev 8.
// No vendor (ST SLA0044) or other third-party driver source is used.
//
// SPI transport for the ST25R3916. Implements the hb_nfc_bus ops table.
// SPI framing (DS12484 Rev 8, Table 11 "SPI operation modes"): the first two
// bits of the first byte select the operation -- 00 register write,
// 01 register read, 10000000 FIFO load, 10011111 FIFO read, 11 direct command.
// The device uses SPI mode 1 and tolerates clocks up to 6 MHz.

#ifndef HB_NFC_SPI_H
#define HB_NFC_SPI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

/** @brief SPI bus/device parameters for hb_nfc_spi_init(). */
typedef struct {
  int spi_host;        /**< ESP-IDF SPI host index (SPI2_HOST = 1, SPI3_HOST = 2). */
  gpio_num_t pin_mosi; /**< MOSI pin. */
  gpio_num_t pin_miso; /**< MISO pin. */
  gpio_num_t pin_sclk; /**< SCLK pin. */
  gpio_num_t pin_cs;   /**< Chip-select pin (active low). */
  uint8_t mode;        /**< SPI mode (ST25R3916 requires mode 1). */
  uint32_t clock_hz;   /**< SPI clock in Hz (datasheet max 6 MHz). */
} hb_nfc_spi_config_t;

/** @brief Initialize the SPI bus and add the ST25R3916 device. */
esp_err_t hb_nfc_spi_init(const hb_nfc_spi_config_t *config);

/** @brief Remove the device and free the SPI bus. */
void hb_nfc_spi_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // HB_NFC_SPI_H
