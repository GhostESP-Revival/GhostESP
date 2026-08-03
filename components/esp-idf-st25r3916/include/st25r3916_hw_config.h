// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
// Clean-room ST25R3916 support authored from ST datasheet DS12484 Rev 8.
// No vendor (ST SLA0044) or other third-party driver source is used.

#ifndef ST25R3916_HW_CONFIG_H
#define ST25R3916_HW_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "driver/gpio.h"

/** @brief Which physical bus connects the ESP to the ST25R3916. */
typedef enum {
  ST25R3916_BUS_SPI = 0,
  ST25R3916_BUS_I2C = 1,
} st25r3916_bus_kind_t;

/** Default 7-bit I2C device address (A1=A0=0). The ST25R3916 occupies
 *  0x50..0x53 depending on the A0/A1 address straps. */
#define ST25R3916_DEFAULT_I2C_ADDR 0x50

/**
 * @brief Hardware/bus configuration for st25r3916_core_init().
 */
typedef struct {
  st25r3916_bus_kind_t bus; /**< Selects SPI or I2C transport. */

  /* Common control lines. */
  gpio_num_t pin_irq; /**< ST25R3916 IRQ output pin (active high). GPIO_NUM_NC if unused. */
  gpio_num_t pin_rst; /**< Optional hard-reset pin. GPIO_NUM_NC if not wired. */

  /* SPI parameters (used when bus == ST25R3916_BUS_SPI). */
  int spi_host;          /**< ESP-IDF SPI host index (SPI2_HOST = 1, SPI3_HOST = 2). */
  gpio_num_t pin_mosi;   /**< SPI MOSI. */
  gpio_num_t pin_miso;   /**< SPI MISO. */
  gpio_num_t pin_sclk;   /**< SPI SCLK. */
  gpio_num_t pin_cs;     /**< SPI chip-select (active low). */
  uint8_t spi_mode;      /**< SPI mode. ST25R3916 requires mode 1. */
  uint32_t spi_clock_hz; /**< SPI clock in Hz. Datasheet max 6 MHz. */

  /* I2C parameters (used when bus == ST25R3916_BUS_I2C). */
  int i2c_port;          /**< ESP-IDF I2C port number. */
  gpio_num_t pin_sda;    /**< I2C SDA. */
  gpio_num_t pin_scl;    /**< I2C SCL. */
  uint8_t i2c_addr;      /**< 7-bit device address (see ST25R3916_DEFAULT_I2C_ADDR). */
  uint32_t i2c_clock_hz; /**< I2C clock in Hz (datasheet max 1 MHz). */
} st25r3916_hw_config_t;

#ifdef __cplusplus
}
#endif

#endif // ST25R3916_HW_CONFIG_H
