// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
// Clean-room ST25R3916 support authored from ST datasheet DS12484 Rev 8.
// No vendor (ST SLA0044) or other third-party driver source is used.

#include "st25r3916_adapter.h"
#include "st25r3916.h"
#include "st25r3916_iso14443a.h"
#include "st25r3916_mifare.h"
#include "crypto1.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "st25r3916_adapter";

typedef struct {
  st25r3916_hw_config_t cfg;
  uint8_t retries;       /**< Passive-activation retries (pn532 semantics; 0xFF = forever-ish). */
  bool activated;        /**< A tag is currently selected. */
  uint8_t uid[10];
  uint8_t uid_len;
  uint16_t atqa;
  uint8_t sak;
  /* MIFARE Classic software-Crypto1 session. Armed by an auth via
   * data_exchange; cleared on (re)selection. */
  crypto1_t cipher;
  bool armed;
} adapter_state_t;

static inline adapter_state_t *state_of(pn532_io_handle_t io) {
  return (io && io->driver_data) ? (adapter_state_t *)io->driver_data : NULL;
}

/* --- high-level vtable implementations ---------------------------------- */

static esp_err_t hl_activate_once(adapter_state_t *st) {
  esp_err_t err = st25r3916_nfca_activate_ex(st->uid, &st->uid_len, &st->atqa, &st->sak, 2, 20);
  st->activated = (err == ESP_OK);
  st->armed = false;  // (re)selection resets any MIFARE Crypto1 session
  return err;
}

static esp_err_t adapter_list_passive_target(pn532_io_handle_t io) {
  adapter_state_t *st = state_of(io);
  if (!st) return ESP_ERR_INVALID_STATE;
  int tries = (st->retries == 0xFF || st->retries == 0) ? 3 : st->retries;
  if (tries > 5) tries = 5;
  esp_err_t err = ESP_ERR_NOT_FOUND;
  for (int i = 0; i < tries; i++) {
    err = hl_activate_once(st);
    if (err == ESP_OK) break;
  }
  return err;
}

static esp_err_t adapter_read_target_id_ex(pn532_io_handle_t io, uint8_t baud_rate_and_card_type,
                                           uint8_t *uid, uint8_t *uid_length, uint16_t *atqa,
                                           uint8_t *sak, int32_t timeout) {
  (void)baud_rate_and_card_type;
  (void)timeout;
  adapter_state_t *st = state_of(io);
  if (!st) return ESP_ERR_INVALID_STATE;
  esp_err_t err = adapter_list_passive_target(io);
  if (err != ESP_OK) return err;
  if (uid_length) *uid_length = st->uid_len;
  if (uid) memcpy(uid, st->uid, st->uid_len);
  if (atqa) *atqa = st->atqa;
  if (sak) *sak = st->sak;
  return ESP_OK;
}

static esp_err_t adapter_exchange(pn532_io_handle_t io, const uint8_t *tx, uint8_t tx_len,
                                  uint8_t *rx, uint8_t *rx_len, bool with_crc) {
  adapter_state_t *st = state_of(io);
  if (!st) return ESP_ERR_INVALID_STATE;
  uint16_t cap = rx_len ? *rx_len : 0;
  uint16_t got = 0;
  esp_err_t err = st25r3916_nfca_transceive(tx, tx_len, with_crc, false, rx, cap, &got, 100);
  if (rx_len) *rx_len = (uint8_t)((got > 0xFF) ? 0xFF : got);
  return err;
}

/* InDataExchange-equivalent.
 *  - A 12-byte MIFARE auth (0x60/0x61 + block + 6-byte key + 4-byte UID) is the
 *    command the PN532 services in hardware. Here we intercept it and run the
 *    software-Crypto1 handshake, arming the session cipher.
 *  - While a MIFARE session is armed, every command is an encrypted exchange.
 *  - Otherwise it is a plain transceive with hardware CRC (NTAG/Ultralight,
 *    ISO14443-4 APDUs). */
static esp_err_t adapter_data_exchange(pn532_io_handle_t io, const uint8_t *tx, uint8_t tx_len,
                                       uint8_t *rx, uint8_t *rx_len) {
  adapter_state_t *st = state_of(io);
  if (!st) return ESP_ERR_INVALID_STATE;

  if (tx_len == 12 && (tx[0] == 0x60 || tx[0] == 0x61)) {
    uint8_t block = tx[1];
    const uint8_t *key = &tx[2];
    const uint8_t *uid = &tx[8];  // pn532 auth carries the 4-byte (N)UID
    esp_err_t err = st25r3916_mifare_auth(&st->cipher, uid, 4, block, tx[0], key);
    st->armed = (err == ESP_OK);
    if (rx_len) *rx_len = 0;
    return err;
  }

  if (st->armed) {
    if (tx_len == 2 && (tx[0] == 0x60 || tx[0] == 0x61)) {
      uint8_t nt[4] = {0};
      uint8_t par[4] = {0};
      esp_err_t err = st25r3916_mifare_nested_auth_raw(&st->cipher, tx[0], tx[1], nt, par);
      st->armed = false;
      if (err != ESP_OK) {
        if (rx_len) *rx_len = 0;
        return err;
      }
      uint16_t cap = rx_len ? *rx_len : 0;
      if (cap >= 8 && rx) {
        memcpy(rx, nt, 4);
        memcpy(rx + 4, par, 4);
        *rx_len = 8;
      } else if (cap >= 4 && rx) {
        memcpy(rx, nt, 4);
        *rx_len = 4;
      } else if (rx_len) {
        *rx_len = 0;
        return ESP_ERR_INVALID_SIZE;
      }
      return ESP_OK;
    }

    uint16_t cap = rx_len ? *rx_len : 0;
    uint16_t got = 0;
    esp_err_t err = st25r3916_mifare_xfer(&st->cipher, tx, tx_len, rx, cap, &got);
    if (rx_len) *rx_len = (uint8_t)((got > 0xFF) ? 0xFF : got);
    return err;
  }

  return adapter_exchange(io, tx, tx_len, rx, rx_len, true);
}

/* InCommunicateThru-equivalent: raw frame, no CRC appended. */
static esp_err_t adapter_communicate_thru(pn532_io_handle_t io, const uint8_t *tx, uint8_t tx_len,
                                          uint8_t *rx, uint8_t *rx_len) {
  return adapter_exchange(io, tx, tx_len, rx, rx_len, false);
}

static esp_err_t adapter_set_retries(pn532_io_handle_t io, uint8_t retries) {
  adapter_state_t *st = state_of(io);
  if (!st) return ESP_ERR_INVALID_STATE;
  st->retries = retries;
  return ESP_OK;
}

/* --- lifecycle (low-level pn532_io_t hooks) ----------------------------- */

static void adapter_release_io(pn532_io_handle_t io) {
  /* Lightweight teardown between scans: drop the RF field but keep the chip up. */
  (void)io;
  st25r3916_field_off();
}

static void adapter_release_driver(pn532_io_handle_t io) {
  if (!io) return;
  st25r3916_deinit();
  if (io->driver_data) {
    free(io->driver_data);
    io->driver_data = NULL;
  }
}

static void bind_vtable(pn532_io_handle_t io, adapter_state_t *st, gpio_num_t rst, gpio_num_t irq) {
  io->driver_data = st;
  io->reset = rst;
  io->irq = irq;
  io->isSAMConfigDone = false;

  /* Low-level PN532 frame hooks are unused for this backend. */
  io->pn532_init_io = NULL;
  io->pn532_read = NULL;
  io->pn532_write = NULL;
  io->pn532_init_extra = NULL;
  io->pn532_is_ready = NULL;
  io->pn532_release_io = adapter_release_io;
  io->pn532_release_driver = adapter_release_driver;

  /* High-level frontend vtable: this is what the shared NFC code dispatches to. */
  io->hl_list_passive_target = adapter_list_passive_target;
  io->hl_read_target_id_ex = adapter_read_target_id_ex;
  io->hl_data_exchange = adapter_data_exchange;
  io->hl_communicate_thru = adapter_communicate_thru;
  io->hl_set_passive_activation_retries = adapter_set_retries;
}

static adapter_state_t *alloc_state(void) {
  adapter_state_t *st = heap_caps_calloc(1, sizeof(adapter_state_t), MALLOC_CAP_DEFAULT);
  if (st) st->retries = 1;
  return st;
}

esp_err_t st25r3916_new_driver_spi(int spi_host, gpio_num_t mosi, gpio_num_t miso,
                                   gpio_num_t sclk, gpio_num_t cs, gpio_num_t rst,
                                   gpio_num_t irq, uint32_t clock_hz, pn532_io_handle_t io) {
  if (io == NULL) return ESP_ERR_INVALID_ARG;
  adapter_state_t *st = alloc_state();
  if (!st) return ESP_ERR_NO_MEM;

  st->cfg = (st25r3916_hw_config_t){
      .bus = ST25R3916_BUS_SPI,
      .pin_irq = irq,
      .pin_rst = rst,
      .spi_host = spi_host,
      .pin_mosi = mosi,
      .pin_miso = miso,
      .pin_sclk = sclk,
      .pin_cs = cs,
      .spi_mode = 1,
      .spi_clock_hz = clock_hz ? clock_hz : 1000000,
  };
  bind_vtable(io, st, rst, irq);
  return ESP_OK;
}

esp_err_t st25r3916_new_driver_i2c(gpio_num_t sda, gpio_num_t scl, gpio_num_t rst,
                                   gpio_num_t irq, int i2c_port, uint8_t addr,
                                   pn532_io_handle_t io) {
  if (io == NULL) return ESP_ERR_INVALID_ARG;
  adapter_state_t *st = alloc_state();
  if (!st) return ESP_ERR_NO_MEM;

  st->cfg = (st25r3916_hw_config_t){
      .bus = ST25R3916_BUS_I2C,
      .pin_irq = irq,
      .pin_rst = rst,
      .i2c_port = i2c_port,
      .pin_sda = sda,
      .pin_scl = scl,
      .i2c_addr = addr ? addr : ST25R3916_DEFAULT_I2C_ADDR,
      .i2c_clock_hz = 400000,
  };
  bind_vtable(io, st, rst, irq);
  return ESP_OK;
}

esp_err_t st25r3916_adapter_init(pn532_io_handle_t io) {
  adapter_state_t *st = state_of(io);
  if (!st) return ESP_ERR_INVALID_STATE;

  esp_err_t err = st25r3916_init(&st->cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "chip init failed: %s", esp_err_to_name(err));
    return err;
  }
  st25r3916_set_mode_nfca();
  st25r3916_field_on();
  io->isSAMConfigDone = true;  // reuse the "ready" flag the view checks
  ESP_LOGI(TAG, "ST25R3916 frontend ready (%s)",
           st->cfg.bus == ST25R3916_BUS_I2C ? "I2C" : "SPI");
  return ESP_OK;
}
