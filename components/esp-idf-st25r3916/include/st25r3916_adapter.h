// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
// Clean-room ST25R3916 support authored from ST datasheet DS12484 Rev 8.
// No vendor (ST SLA0044) or other third-party driver source is used.
//
// Frontend adapter: presents the ST25R3916 through GhostESP's existing
// pn532_io_t high-level vtable so the shared NFC code (mifare/ntag/desfire)
// drives it exactly like a PN532. Create a driver, then call
// st25r3916_adapter_init() (in place of pn532_init) to bring up the chip.

#ifndef ST25R3916_ADAPTER_H
#define ST25R3916_ADAPTER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"
#include "pn532_driver.h"

/** @brief Bind an SPI-connected ST25R3916 to a pn532 frontend handle. */
esp_err_t st25r3916_new_driver_spi(int spi_host, gpio_num_t mosi, gpio_num_t miso,
                                   gpio_num_t sclk, gpio_num_t cs, gpio_num_t rst,
                                   gpio_num_t irq, uint32_t clock_hz, pn532_io_handle_t io);

/** @brief Bind an I2C-connected ST25R3916 to a pn532 frontend handle. */
esp_err_t st25r3916_new_driver_i2c(gpio_num_t sda, gpio_num_t scl, gpio_num_t rst,
                                   gpio_num_t irq, int i2c_port, uint8_t addr,
                                   pn532_io_handle_t io);

/**
 * @brief Power up the ST25R3916 and enter ISO14443-A reader mode.
 *
 * Call after st25r3916_new_driver_*(), in place of pn532_init().
 */
esp_err_t st25r3916_adapter_init(pn532_io_handle_t io);

#ifdef __cplusplus
}
#endif

#endif // ST25R3916_ADAPTER_H
