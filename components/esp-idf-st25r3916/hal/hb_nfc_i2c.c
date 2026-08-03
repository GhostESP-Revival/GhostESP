// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
// Clean-room ST25R3916 support authored from ST datasheet DS12484 Rev 8.
// No vendor (ST SLA0044) or other third-party driver source is used.

#include "hb_nfc_i2c.h"
#include "hb_nfc_bus.h"

#include <string.h>

#include "esp_log.h"
#include "i2c_shared.h"
#include "i2c_bus_lock.h"

static const char *TAG = "hb_nfc_i2c";

/* ST25R3916 operation-mode selector bits (shared with the SPI framing). Space-B
 * registers use the 0xFB direct-command prefix followed by the Space-A address. */
#define HB_NFC_I2C_READ_FLAG      0x40 /**< Bit 6 set selects a register read. */
#define HB_NFC_I2C_ADDR_MASK      0x3F /**< Register address occupies bits [5:0]. */
#define HB_NFC_I2C_SPACE_B        0x40 /**< Internal marker bit for Space-B registers. */
#define HB_NFC_I2C_SPACE_B_ACCESS 0xFB /**< Direct command: following register is Space-B. */
#define HB_NFC_I2C_FIFO_LOAD_BYTE 0x80 /**< FIFO load (write) prefix. */
#define HB_NFC_I2C_FIFO_READ_BYTE 0x9F /**< FIFO read prefix. */
#define HB_NFC_I2C_PT_A_LOAD_BYTE 0xA0 /**< PT memory A-config load (loc 0). */
#define HB_NFC_I2C_PT_F_LOAD_BYTE 0xA8 /**< PT memory F-config load (loc 15). */
#define HB_NFC_I2C_PT_TSN_LOAD_BYTE 0xAC /**< PT memory TSN load (loc 36). */
#define HB_NFC_I2C_FIFO_MAX_BYTES 512  /**< ST25R3916 FIFO capacity. */
#define HB_NFC_I2C_PT_MAX_BYTES   48   /**< ST25R3916 PT memory locations 0..47. */
#define HB_NFC_I2C_XFER_TIMEOUT_MS 100 /**< Per-transaction timeout. */

typedef struct {
  bool is_init;
  int port;
  uint16_t addr;
  uint32_t clock_hz;
  i2c_master_bus_handle_t bus;
  bool owns_bus;
} hb_nfc_i2c_state_t;

static hb_nfc_i2c_state_t s_i2c = {0};

static inline esp_err_t i2c_tx(const uint8_t *data, size_t len) {
  if (!s_i2c.is_init) return ESP_ERR_INVALID_STATE;
  bool locked = i2c_bus_lock(s_i2c.port, 300);
  if (!locked) return ESP_ERR_TIMEOUT;
  esp_err_t r = i2c_shared_transmit_to_addr(s_i2c.bus, s_i2c.addr, s_i2c.clock_hz, data, len,
                                            HB_NFC_I2C_XFER_TIMEOUT_MS);
  i2c_bus_unlock(s_i2c.port);
  return r;
}

static inline esp_err_t i2c_tx_rx(const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len) {
  if (!s_i2c.is_init) return ESP_ERR_INVALID_STATE;
  bool locked = i2c_bus_lock(s_i2c.port, 300);
  if (!locked) return ESP_ERR_TIMEOUT;
  esp_err_t r = i2c_shared_transmit_receive_from_addr(s_i2c.bus, s_i2c.addr, s_i2c.clock_hz, tx,
                                                      tx_len, rx, rx_len,
                                                      HB_NFC_I2C_XFER_TIMEOUT_MS);
  i2c_bus_unlock(s_i2c.port);
  return r;
}

static esp_err_t i2c_reg_read(uint8_t addr, uint8_t *out_value) {
  if (out_value == NULL) return ESP_ERR_INVALID_ARG;
  uint8_t sel[2] = {0};
  size_t sel_len = 1;
  if (addr & HB_NFC_I2C_SPACE_B) {
    sel[0] = HB_NFC_I2C_SPACE_B_ACCESS;
    sel[1] = (uint8_t)(HB_NFC_I2C_READ_FLAG | (addr & HB_NFC_I2C_ADDR_MASK));
    sel_len = 2;
  } else {
    sel[0] = (uint8_t)(HB_NFC_I2C_READ_FLAG | (addr & HB_NFC_I2C_ADDR_MASK));
  }
  esp_err_t err = i2c_tx_rx(sel, sel_len, out_value, 1);
  if (err != ESP_OK) *out_value = 0;
  return err;
}

static esp_err_t i2c_reg_write(uint8_t addr, uint8_t value) {
  if (addr & HB_NFC_I2C_SPACE_B) {
    uint8_t tx[3] = {HB_NFC_I2C_SPACE_B_ACCESS, (uint8_t)(addr & HB_NFC_I2C_ADDR_MASK), value};
    return i2c_tx(tx, sizeof(tx));
  }
  uint8_t tx[2] = {(uint8_t)(addr & HB_NFC_I2C_ADDR_MASK), value};
  return i2c_tx(tx, sizeof(tx));
}

static esp_err_t i2c_reg_modify(uint8_t addr, uint8_t mask, uint8_t value) {
  uint8_t cur;
  esp_err_t err = i2c_reg_read(addr, &cur);
  if (err != ESP_OK) return err;
  uint8_t nv = (uint8_t)((cur & (uint8_t)~mask) | (value & mask));
  return i2c_reg_write(addr, nv);
}

static esp_err_t i2c_fifo_load(const uint8_t *data, size_t len) {
  if (data == NULL || len == 0 || len > HB_NFC_I2C_FIFO_MAX_BYTES) return ESP_ERR_INVALID_ARG;
  static uint8_t tx[1 + HB_NFC_I2C_FIFO_MAX_BYTES];
  tx[0] = HB_NFC_I2C_FIFO_LOAD_BYTE;
  memcpy(&tx[1], data, len);
  return i2c_tx(tx, len + 1);
}

static esp_err_t i2c_fifo_read(uint8_t *data, size_t len) {
  if (data == NULL || len == 0 || len > HB_NFC_I2C_FIFO_MAX_BYTES) return ESP_ERR_INVALID_ARG;
  uint8_t sel = HB_NFC_I2C_FIFO_READ_BYTE;
  return i2c_tx_rx(&sel, 1, data, len);
}

static esp_err_t i2c_pt_memory_load(uint8_t area, const uint8_t *data, size_t len) {
  if (data == NULL || len == 0 || len > HB_NFC_I2C_PT_MAX_BYTES) return ESP_ERR_INVALID_ARG;
  uint8_t mode;
  switch (area) {
    case 0:
      mode = HB_NFC_I2C_PT_A_LOAD_BYTE;
      if (len > 15) return ESP_ERR_INVALID_SIZE;
      break;
    case 15:
      mode = HB_NFC_I2C_PT_F_LOAD_BYTE;
      if (len > 21) return ESP_ERR_INVALID_SIZE;
      break;
    case 36:
      mode = HB_NFC_I2C_PT_TSN_LOAD_BYTE;
      if (len > 12) return ESP_ERR_INVALID_SIZE;
      break;
    default:
      return ESP_ERR_INVALID_ARG;
  }
  uint8_t tx[1 + HB_NFC_I2C_PT_MAX_BYTES];
  tx[0] = mode;
  memcpy(&tx[1], data, len);
  return i2c_tx(tx, len + 1);
}

static esp_err_t i2c_direct_cmd(uint8_t cmd) {
  return i2c_tx(&cmd, 1);
}

static esp_err_t i2c_raw_xfer(const uint8_t *tx, uint8_t *rx, size_t len) {
  /* The SPI raw full-duplex transfer has no direct I2C equivalent and is not
   * used by the reader path; report unsupported rather than guessing framing. */
  (void)tx;
  (void)rx;
  (void)len;
  return ESP_ERR_NOT_SUPPORTED;
}

static void i2c_deinit(void) {
  if (!s_i2c.is_init) return;
  /* Devices are owned by the shared bus layer (keyed by address); only release
   * the bus if this transport created it. */
  if (s_i2c.owns_bus && s_i2c.bus != NULL) {
    i2c_del_master_bus(s_i2c.bus);
  }
  memset(&s_i2c, 0, sizeof(s_i2c));
}

esp_err_t hb_nfc_i2c_init(const st25r3916_hw_config_t *config) {
  if (config == NULL) return ESP_ERR_INVALID_ARG;
  if (s_i2c.is_init) return ESP_OK;

  s_i2c.port = config->i2c_port;
  s_i2c.addr = config->i2c_addr ? config->i2c_addr : ST25R3916_DEFAULT_I2C_ADDR;
  s_i2c.clock_hz = config->i2c_clock_hz ? config->i2c_clock_hz : 400000;

  esp_err_t err = i2c_shared_get_or_create_bus(config->i2c_port, config->pin_sda, config->pin_scl,
                                               true, &s_i2c.bus, &s_i2c.owns_bus);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2c bus create fail: %s", esp_err_to_name(err));
    return err;
  }

  /* Pre-register the device handle with the shared layer (idempotent). */
  i2c_master_dev_handle_t dev = NULL;
  err = i2c_shared_add_device(s_i2c.bus, s_i2c.addr, s_i2c.clock_hz, &dev);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2c add device 0x%02X fail: %s", s_i2c.addr, esp_err_to_name(err));
    if (s_i2c.owns_bus && s_i2c.bus) i2c_del_master_bus(s_i2c.bus);
    memset(&s_i2c, 0, sizeof(s_i2c));
    return err;
  }

  s_i2c.is_init = true;
  ESP_LOGI(TAG, "I2C OK: port=%d addr=0x%02X clk=%lu", s_i2c.port, s_i2c.addr,
           (unsigned long)s_i2c.clock_hz);
  return ESP_OK;
}

/* Bus dispatch table for the I2C transport (see hb_nfc_bus.h). */
const hb_nfc_bus_ops_t hb_nfc_i2c_ops = {
    .reg_read = i2c_reg_read,
    .reg_write = i2c_reg_write,
    .reg_modify = i2c_reg_modify,
    .fifo_load = i2c_fifo_load,
    .fifo_read = i2c_fifo_read,
    .pt_memory_load = i2c_pt_memory_load,
    .direct_cmd = i2c_direct_cmd,
    .raw_xfer = i2c_raw_xfer,
    .deinit = i2c_deinit,
};
