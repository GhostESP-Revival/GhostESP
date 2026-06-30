// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
// Clean-room ST25R3916 driver authored from ST datasheet DS12484 Rev 8.
// No vendor (ST SLA0044) or other third-party driver source is used.

#include "st25r3916.h"
#include "st25r3916_reg.h"
#include "hb_nfc_bus.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "st25r3916";

typedef struct {
  bool init;
  bool field_on;
  gpio_num_t pin_irq;
  /* Latched interrupt status. Reading the chip's IRQ registers clears them, so
   * we accumulate observed bits here (DS Tables 62-64). */
  uint8_t irq_main;
  uint8_t irq_timer;
  uint8_t irq_error;
  uint8_t irq_pt;
} st25r_state_t;

static st25r_state_t s = {0};

/* --- small helpers ------------------------------------------------------ */

static inline void delay_us(uint32_t us) { esp_rom_delay_us(us); }
static inline void delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1)); }

esp_err_t st25r3916_reg_read(uint8_t addr, uint8_t *out_value) {
  return hb_nfc_bus_reg_read(addr, out_value);
}
esp_err_t st25r3916_reg_write(uint8_t addr, uint8_t value) {
  return hb_nfc_bus_reg_write(addr, value);
}
esp_err_t st25r3916_reg_modify(uint8_t addr, uint8_t mask, uint8_t value) {
  return hb_nfc_bus_reg_modify(addr, mask, value);
}
esp_err_t st25r3916_cmd(uint8_t direct_cmd) {
  return hb_nfc_bus_direct_cmd(direct_cmd);
}

/* --- lifecycle ---------------------------------------------------------- */

static void hard_reset(gpio_num_t pin_rst) {
  if (pin_rst == GPIO_NUM_NC) return;
  gpio_config_t io = {
      .pin_bit_mask = 1ULL << pin_rst,
      .mode = GPIO_MODE_OUTPUT,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&io);
  gpio_set_level(pin_rst, 1);  // ST25R3916 reset is active high
  delay_ms(1);
  gpio_set_level(pin_rst, 0);
  delay_ms(2);
}

esp_err_t st25r3916_init(const st25r3916_hw_config_t *config) {
  if (config == NULL) return ESP_ERR_INVALID_ARG;

  s.pin_irq = config->pin_irq;
  s.field_on = false;
  s.irq_main = s.irq_timer = s.irq_error = s.irq_pt = 0;

  hard_reset(config->pin_rst);

  if (config->pin_irq != GPIO_NUM_NC) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << config->pin_irq,
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
  }

  esp_err_t err = hb_nfc_bus_init(config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "bus init failed: %s", esp_err_to_name(err));
    return err;
  }

  /* Power-up: Set Default then enable the oscillator/regulator and wait for it
   * to stabilize (DS 4.4.1 Set default; Table 62 I_osc). */
  st25r3916_cmd(ST25R3916_CMD_SET_DEFAULT);
  delay_ms(1);
  st25r3916_irq_clear();

  st25r3916_reg_write(ST25R3916_REG_OP_CONTROL, ST25R3916_OP_CONTROL_EN);

  bool osc_ok = false;
  for (int i = 0; i < 100; i++) {  // ~10 ms budget
    uint8_t m = 0, t = 0, e = 0;
    st25r3916_irq_update(&m, &t, &e);
    if (s.irq_main & ST25R3916_IRQ_MAIN_OSC) {
      osc_ok = true;
      break;
    }
    delay_us(100);
  }
  if (!osc_ok) {
    ESP_LOGW(TAG, "oscillator-stable IRQ not seen; continuing");
  }

  uint8_t id = 0, type = 0, rev = 0;
  if (st25r3916_check_id(&id, &type, &rev) != ESP_OK) {
    ESP_LOGE(TAG, "chip not detected (IC_IDENTITY read invalid)");
    hb_nfc_bus_deinit();
    return ESP_ERR_NOT_FOUND;
  }

  /* Calibrate regulators and TX driver timing (DS 4.4.10 / 4.4.11). */
  st25r3916_cmd(ST25R3916_CMD_ADJUST_REGULATORS);
  delay_ms(6);
  st25r3916_cmd(ST25R3916_CMD_CALIBRATE_DRIVER_TIMING);
  delay_ms(1);

  s.init = true;
  ESP_LOGI(TAG, "ready: IC_IDENTITY=0x%02X (type 0x%02X rev 0x%02X)", id, type, rev);
  return ESP_OK;
}

void st25r3916_deinit(void) {
  if (!s.init) return;
  st25r3916_field_off();
  st25r3916_cmd(ST25R3916_CMD_SET_DEFAULT);
  hb_nfc_bus_deinit();
  if (s.pin_irq != GPIO_NUM_NC) gpio_reset_pin(s.pin_irq);
  s.init = false;
  s.field_on = false;
}

bool st25r3916_is_ready(void) { return s.init; }

esp_err_t st25r3916_check_id(uint8_t *out_id, uint8_t *out_type, uint8_t *out_rev) {
  uint8_t v = 0;
  esp_err_t err = st25r3916_reg_read(ST25R3916_REG_IC_IDENTITY, &v);
  if (err != ESP_OK) return err;
  if (v == 0x00 || v == 0xFF) return ESP_ERR_NOT_FOUND;  // bus read failure pattern
  if (out_id) *out_id = v;
  if (out_type) *out_type = (uint8_t)((v >> ST25R3916_IC_TYPE_SHIFT) & ST25R3916_IC_TYPE_MASK);
  if (out_rev) *out_rev = (uint8_t)(v & ST25R3916_IC_REV_MASK);
  return ESP_OK;
}

/* --- RF field ----------------------------------------------------------- */

esp_err_t st25r3916_field_on(void) {
  if (!s.init) return ESP_ERR_INVALID_STATE;
  if (s.field_on) return ESP_OK;
  st25r3916_reg_write(ST25R3916_REG_OP_CONTROL, ST25R3916_OP_CONTROL_FIELD_ON);
  delay_ms(5);  // field settle
  st25r3916_cmd(ST25R3916_CMD_RESET_RXGAIN);
  s.field_on = true;
  return ESP_OK;
}

esp_err_t st25r3916_field_off(void) {
  if (!s.init) return ESP_OK;
  st25r3916_reg_write(ST25R3916_REG_OP_CONTROL, ST25R3916_OP_CONTROL_FIELD_OFF);
  s.field_on = false;
  return ESP_OK;
}

bool st25r3916_field_is_on(void) { return s.field_on; }

esp_err_t st25r3916_set_mode_nfca(void) {
  if (!s.init) return ESP_ERR_INVALID_STATE;
  st25r3916_reg_write(ST25R3916_REG_MODE, ST25R3916_MODE_POLL_NFCA);
  st25r3916_reg_write(ST25R3916_REG_BIT_RATE, 0x00);  // 106 kb/s TX and RX
  /* Normal framing: hardware handles parity, no anti-collision split byte.
   * (Crypto1 later flips NO_TX_PAR/NO_RX_PAR for manual parity.) */
  st25r3916_reg_modify(ST25R3916_REG_ISO14443A_NFC,
                       ST25R3916_ISO14443A_NO_TX_PAR | ST25R3916_ISO14443A_NO_RX_PAR |
                           ST25R3916_ISO14443A_ANTCL,
                       0x00);
  return ESP_OK;
}

/* --- FIFO --------------------------------------------------------------- */

uint16_t st25r3916_fifo_count(void) {
  uint16_t bytes = 0;
  st25r3916_fifo_status(&bytes, NULL);
  return bytes;
}

void st25r3916_fifo_status(uint16_t *byte_count, uint8_t *last_bits) {
  uint8_t lsb = 0, msb = 0;
  st25r3916_reg_read(ST25R3916_REG_FIFO_STATUS1, &lsb);
  st25r3916_reg_read(ST25R3916_REG_FIFO_STATUS2, &msb);
  if (byte_count) {
    *byte_count = (uint16_t)((((uint16_t)(msb & ST25R3916_FIFO_STATUS2_FTC_MASK)) << 2) | lsb);
  }
  if (last_bits) {
    *last_bits = (uint8_t)((msb & ST25R3916_FIFO_STATUS2_LB_MASK) >> ST25R3916_FIFO_STATUS2_LB_SHIFT);
  }
}

void st25r3916_fifo_clear(void) { st25r3916_cmd(ST25R3916_CMD_CLEAR_FIFO); }

esp_err_t st25r3916_fifo_load(const uint8_t *data, size_t len) {
  return hb_nfc_bus_fifo_load(data, len);
}

esp_err_t st25r3916_fifo_read(uint8_t *out, size_t len) {
  return hb_nfc_bus_fifo_read(out, len);
}

esp_err_t st25r3916_pt_memory_load(uint8_t area, const uint8_t *data, size_t len) {
  return hb_nfc_bus_pt_memory_load(area, data, len);
}

void st25r3916_set_num_tx_bytes(uint16_t nbytes, uint8_t nbits) {
  /* DS Tables 70/71: reg1 = ntx[12:5], reg2 = ntx[4:0]<<3 | nbtx[2:0]. */
  st25r3916_reg_write(ST25R3916_REG_NUM_TX_BYTES1, (uint8_t)((nbytes >> 5) & 0xFF));
  st25r3916_reg_write(ST25R3916_REG_NUM_TX_BYTES2,
                      (uint8_t)(((nbytes & 0x1F) << 3) |
                                (nbits & ST25R3916_NUM_TX_BYTES2_NTX_BITS_MASK)));
}

/* --- interrupts (status-register polling; IRQ pin optional) ------------- */

void st25r3916_irq_clear(void) {
  /* Read-to-clear any pending status, then reset the latch. */
  uint8_t v;
  st25r3916_reg_read(ST25R3916_REG_MAIN_IRQ, &v);
  st25r3916_reg_read(ST25R3916_REG_TIMER_NFC_IRQ, &v);
  st25r3916_reg_read(ST25R3916_REG_ERROR_IRQ, &v);
  st25r3916_reg_read(ST25R3916_REG_PT_IRQ, &v);
  s.irq_main = s.irq_timer = s.irq_error = s.irq_pt = 0;
}

esp_err_t st25r3916_irq_update(uint8_t *main, uint8_t *timer, uint8_t *error) {
  uint8_t m = 0, t = 0, e = 0;
  esp_err_t err = st25r3916_reg_read(ST25R3916_REG_MAIN_IRQ, &m);
  if (err != ESP_OK) return err;
  st25r3916_reg_read(ST25R3916_REG_TIMER_NFC_IRQ, &t);
  st25r3916_reg_read(ST25R3916_REG_ERROR_IRQ, &e);
  s.irq_main |= m;
  s.irq_timer |= t;
  s.irq_error |= e;
  if (main) *main = s.irq_main;
  if (timer) *timer = s.irq_timer;
  if (error) *error = s.irq_error;
  return ESP_OK;
}

esp_err_t st25r3916_irq_update_pt(uint8_t *pt) {
  uint8_t p = 0;
  esp_err_t err = st25r3916_reg_read(ST25R3916_REG_PT_IRQ, &p);
  if (err != ESP_OK) return err;
  s.irq_pt |= p;
  if (pt) *pt = s.irq_pt;
  return ESP_OK;
}

esp_err_t st25r3916_irq_wait_main(uint8_t main_mask, int timeout_ms) {
  int iters = timeout_ms * 10;  // poll every ~100 us
  if (iters < 1) iters = 1;
  for (int i = 0; i < iters; i++) {
    st25r3916_irq_update(NULL, NULL, NULL);
    if (s.irq_main & main_mask) return ESP_OK;
    delay_us(100);
  }
  return ESP_ERR_TIMEOUT;
}
