// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
// Clean-room ST25R3916 support authored from ST datasheet DS12484 Rev 8.
// No vendor (ST SLA0044) or other third-party driver source is used.

#include "hb_nfc_bus.h"
#include "hb_nfc_spi.h"

#include "esp_log.h"

static const char *TAG = "hb_nfc_bus";

static const hb_nfc_bus_ops_t *s_ops = NULL;

esp_err_t hb_nfc_bus_init(const st25r3916_hw_config_t *config) {
  if (config == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  if (config->bus == ST25R3916_BUS_I2C) {
    esp_err_t err = hb_nfc_i2c_init(config);
    if (err != ESP_OK) {
      return err;
    }
    s_ops = &hb_nfc_i2c_ops;
    ESP_LOGI(TAG, "transport=I2C port=%d addr=0x%02X", config->i2c_port, config->i2c_addr);
    return ESP_OK;
  }

  hb_nfc_spi_config_t spi_cfg = {
      .spi_host = config->spi_host,
      .pin_mosi = config->pin_mosi,
      .pin_miso = config->pin_miso,
      .pin_sclk = config->pin_sclk,
      .pin_cs = config->pin_cs,
      .mode = config->spi_mode,
      .clock_hz = config->spi_clock_hz,
  };
  esp_err_t err = hb_nfc_spi_init(&spi_cfg);
  if (err != ESP_OK) {
    return err;
  }
  s_ops = &hb_nfc_spi_ops;
  ESP_LOGI(TAG, "transport=SPI host=%d cs=%d", config->spi_host, config->pin_cs);
  return ESP_OK;
}

void hb_nfc_bus_deinit(void) {
  if (s_ops != NULL && s_ops->deinit != NULL) {
    s_ops->deinit();
  }
  s_ops = NULL;
}

esp_err_t hb_nfc_bus_reg_read(uint8_t addr, uint8_t *out_value) {
  if (s_ops == NULL) return ESP_ERR_INVALID_STATE;
  return s_ops->reg_read(addr, out_value);
}

esp_err_t hb_nfc_bus_reg_write(uint8_t addr, uint8_t value) {
  if (s_ops == NULL) return ESP_ERR_INVALID_STATE;
  return s_ops->reg_write(addr, value);
}

esp_err_t hb_nfc_bus_reg_modify(uint8_t addr, uint8_t mask, uint8_t value) {
  if (s_ops == NULL) return ESP_ERR_INVALID_STATE;
  return s_ops->reg_modify(addr, mask, value);
}

esp_err_t hb_nfc_bus_fifo_load(const uint8_t *data, size_t len) {
  if (s_ops == NULL) return ESP_ERR_INVALID_STATE;
  return s_ops->fifo_load(data, len);
}

esp_err_t hb_nfc_bus_fifo_read(uint8_t *data, size_t len) {
  if (s_ops == NULL) return ESP_ERR_INVALID_STATE;
  return s_ops->fifo_read(data, len);
}

esp_err_t hb_nfc_bus_pt_memory_load(uint8_t area, const uint8_t *data, size_t len) {
  if (s_ops == NULL) return ESP_ERR_INVALID_STATE;
  if (s_ops->pt_memory_load == NULL) return ESP_ERR_NOT_SUPPORTED;
  return s_ops->pt_memory_load(area, data, len);
}

esp_err_t hb_nfc_bus_direct_cmd(uint8_t cmd) {
  if (s_ops == NULL) return ESP_ERR_INVALID_STATE;
  return s_ops->direct_cmd(cmd);
}

esp_err_t hb_nfc_bus_raw_xfer(const uint8_t *tx, uint8_t *rx, size_t len) {
  if (s_ops == NULL) return ESP_ERR_INVALID_STATE;
  return s_ops->raw_xfer(tx, rx, len);
}
