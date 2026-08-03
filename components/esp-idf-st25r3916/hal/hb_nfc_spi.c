// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
// Clean-room ST25R3916 support authored from ST datasheet DS12484 Rev 8.
// No vendor (ST SLA0044) or other third-party driver source is used.

#include "hb_nfc_spi.h"
#include "hb_nfc_bus.h"

#include <string.h>

#include "driver/spi_master.h"
#include "esp_log.h"

static const char *TAG = "hb_nfc_spi";

/* Operation-mode selector bits, DS12484 Rev 8 Table 11. Space-B registers use
 * the 0xFB direct-command prefix followed by the normal Space-A address. */
#define HB_SPI_OP_REG_READ 0x40 /**< 01xxxxxx: register read. */
#define HB_SPI_ADDR_MASK   0x3F /**< Register address in bits [5:0]. */
#define HB_SPI_SPACE_B     0x40 /**< Internal marker bit for Space-B registers. */
#define HB_SPI_OP_SPACE_B_ACCESS 0xFB /**< Direct command: following register is Space-B. */
#define HB_SPI_OP_FIFO_LD  0x80 /**< 10000000: FIFO load. */
#define HB_SPI_OP_FIFO_RD  0x9F /**< 10011111: FIFO read. */
#define HB_SPI_OP_PT_A_LD  0xA0 /**< 10100000: PT memory A-config load (loc 0). */
#define HB_SPI_OP_PT_F_LD  0xA8 /**< 10101000: PT memory F-config load (loc 15). */
#define HB_SPI_OP_PT_TSN_LD 0xAC /**< 10101100: PT memory TSN load (loc 36). */
#define HB_SPI_FIFO_MAX    512   /**< ST25R3916 FIFO depth. */
#define HB_SPI_RAW_MAX     64    /**< Cap for raw transfers. */
#define HB_SPI_PT_MAX      48    /**< ST25R3916 PT memory locations 0..47. */

static spi_device_handle_t s_dev = NULL;
static int s_host = -1;
static bool s_init = false;

static esp_err_t xfer(const void *tx, void *rx, size_t len) {
  if (s_dev == NULL) return ESP_ERR_INVALID_STATE;
  spi_transaction_t t = {
      .length = len * 8,
      .tx_buffer = tx,
      .rx_buffer = rx,
  };
  esp_err_t err = spi_device_transmit(s_dev, &t);
  if (err != ESP_OK) ESP_LOGE(TAG, "spi xfer fail: %s", esp_err_to_name(err));
  return err;
}

esp_err_t hb_nfc_spi_init(const hb_nfc_spi_config_t *config) {
  if (config == NULL) return ESP_ERR_INVALID_ARG;
  if (s_init) return ESP_OK;

  spi_bus_config_t bus = {
      .mosi_io_num = config->pin_mosi,
      .miso_io_num = config->pin_miso,
      .sclk_io_num = config->pin_sclk,
      .quadwp_io_num = GPIO_NUM_NC,
      .quadhd_io_num = GPIO_NUM_NC,
      .max_transfer_sz = HB_SPI_FIFO_MAX + 4,
  };
  esp_err_t err = spi_bus_initialize(config->spi_host, &bus, SPI_DMA_CH_AUTO);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {  // INVALID_STATE: bus already init by another device
    ESP_LOGE(TAG, "bus init fail: %s", esp_err_to_name(err));
    return err;
  }

  spi_device_interface_config_t dev = {
      .clock_speed_hz = config->clock_hz ? (int)config->clock_hz : 1000000,
      .mode = config->mode,           // ST25R3916 requires mode 1
      .spics_io_num = config->pin_cs,
      .queue_size = 1,
      .cs_ena_pretrans = 1,
      .cs_ena_posttrans = 1,
  };
  err = spi_bus_add_device(config->spi_host, &dev, &s_dev);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "add device fail: %s", esp_err_to_name(err));
    return err;
  }

  s_host = config->spi_host;
  s_init = true;
  ESP_LOGI(TAG, "SPI OK: host=%d mode=%u clk=%lu cs=%d", config->spi_host, config->mode,
           (unsigned long)config->clock_hz, config->pin_cs);
  return ESP_OK;
}

void hb_nfc_spi_deinit(void) {
  if (!s_init) return;
  if (s_dev) {
    spi_bus_remove_device(s_dev);
    s_dev = NULL;
  }
  if (s_host >= 0) {
    spi_bus_free(s_host);
    s_host = -1;
  }
  s_init = false;
}

static esp_err_t spi_reg_read(uint8_t addr, uint8_t *out_value) {
  if (out_value == NULL) return ESP_ERR_INVALID_ARG;
  bool space_b = (addr & HB_SPI_SPACE_B) != 0;
  uint8_t tx[3] = {0};
  uint8_t rx[3] = {0};
  size_t len = space_b ? 3 : 2;
  if (space_b) {
    tx[0] = HB_SPI_OP_SPACE_B_ACCESS;
    tx[1] = (uint8_t)(HB_SPI_OP_REG_READ | (addr & HB_SPI_ADDR_MASK));
  } else {
    tx[0] = (uint8_t)(HB_SPI_OP_REG_READ | (addr & HB_SPI_ADDR_MASK));
  }
  esp_err_t err = xfer(tx, rx, len);
  *out_value = (err == ESP_OK) ? rx[len - 1] : 0;
  return err;
}

static esp_err_t spi_reg_write(uint8_t addr, uint8_t value) {
  if (addr & HB_SPI_SPACE_B) {
    uint8_t tx[3] = {HB_SPI_OP_SPACE_B_ACCESS, (uint8_t)(addr & HB_SPI_ADDR_MASK), value};
    return xfer(tx, NULL, sizeof(tx));
  }
  uint8_t tx[2] = {(uint8_t)(addr & HB_SPI_ADDR_MASK), value};
  return xfer(tx, NULL, sizeof(tx));
}

static esp_err_t spi_reg_modify(uint8_t addr, uint8_t mask, uint8_t value) {
  uint8_t cur;
  esp_err_t err = spi_reg_read(addr, &cur);
  if (err != ESP_OK) return err;
  return spi_reg_write(addr, (uint8_t)((cur & (uint8_t)~mask) | (value & mask)));
}

static esp_err_t spi_fifo_load(const uint8_t *data, size_t len) {
  if (data == NULL || len == 0 || len > HB_SPI_FIFO_MAX) return ESP_ERR_INVALID_ARG;
  static uint8_t tx[1 + HB_SPI_FIFO_MAX];
  tx[0] = HB_SPI_OP_FIFO_LD;
  memcpy(&tx[1], data, len);
  return xfer(tx, NULL, len + 1);
}

static esp_err_t spi_fifo_read(uint8_t *data, size_t len) {
  if (data == NULL || len == 0 || len > HB_SPI_FIFO_MAX) return ESP_ERR_INVALID_ARG;
  static uint8_t tx[1 + HB_SPI_FIFO_MAX];
  static uint8_t rx[1 + HB_SPI_FIFO_MAX];
  memset(tx, 0, len + 1);
  tx[0] = HB_SPI_OP_FIFO_RD;
  esp_err_t err = xfer(tx, rx, len + 1);
  if (err == ESP_OK) memcpy(data, &rx[1], len);  // data follows the prefix byte
  return err;
}

static esp_err_t spi_pt_memory_load(uint8_t area, const uint8_t *data, size_t len) {
  if (data == NULL || len == 0 || len > HB_SPI_PT_MAX) return ESP_ERR_INVALID_ARG;
  uint8_t mode;
  switch (area) {
    case 0:
      mode = HB_SPI_OP_PT_A_LD;
      if (len > 15) return ESP_ERR_INVALID_SIZE;
      break;
    case 15:
      mode = HB_SPI_OP_PT_F_LD;
      if (len > 21) return ESP_ERR_INVALID_SIZE;
      break;
    case 36:
      mode = HB_SPI_OP_PT_TSN_LD;
      if (len > 12) return ESP_ERR_INVALID_SIZE;
      break;
    default:
      return ESP_ERR_INVALID_ARG;
  }
  uint8_t tx[1 + HB_SPI_PT_MAX];
  tx[0] = mode;
  memcpy(&tx[1], data, len);
  return xfer(tx, NULL, len + 1);
}

static esp_err_t spi_direct_cmd(uint8_t cmd) {
  return xfer(&cmd, NULL, 1);  // command opcodes already carry the 11xxxxxx mode bits
}

static esp_err_t spi_raw_xfer(const uint8_t *tx, uint8_t *rx, size_t len) {
  if (tx == NULL || len == 0 || len > HB_SPI_RAW_MAX) return ESP_ERR_INVALID_ARG;
  return xfer(tx, rx, len);
}

const hb_nfc_bus_ops_t hb_nfc_spi_ops = {
    .reg_read = spi_reg_read,
    .reg_write = spi_reg_write,
    .reg_modify = spi_reg_modify,
    .fifo_load = spi_fifo_load,
    .fifo_read = spi_fifo_read,
    .pt_memory_load = spi_pt_memory_load,
    .direct_cmd = spi_direct_cmd,
    .raw_xfer = spi_raw_xfer,
    .deinit = hb_nfc_spi_deinit,
};
