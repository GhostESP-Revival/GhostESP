// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
// Clean-room ST25R3916 NFC-A target support authored from ST datasheet DS12484 Rev 8.
// No vendor (ST SLA0044) or other third-party driver source is used.
//
// Target-mode register configuration cross-referenced against the Flipper Zero
// Momentum-Firmware (GPL-3.0, https://github.com/Next-Flip/Momentum-Firmware):
//   - targets/f7/furi_hal/furi_hal_nfc_iso14443a.c: listener_init() register
//     sequence (OP_CONTROL=0xE3, MODE=0x88, PASSIVE_TARGET=0x5C, PT_MOD=0x0F,
//     MASK_RX_TIMER=0x02, then CMD_GOTO_SENSE).
//   - targets/f7/furi_hal/furi_hal_nfc.c: global PT_MOD / load-modulation /
//     field-threshold setup.
//   - After WU_A (ACTIVE): set d_106_ac_a to disable auto-anticollision so the
//     MCU handles all subsequent Type 2 commands via FIFO (same pattern as
//     Flipper's furi_hal_nfc_iso14443a wait_event).

#include "st25r3916_target.h"
#include "st25r3916.h"
#include "st25r3916_reg.h"

#include <string.h>

#include "esp_rom_sys.h"

#define NFCA_SAK_CASCADE 0x04
#define PT_MEM_A_CONFIG_LEN 15

const char *st25r3916_target_state_name(uint8_t state) {
  switch (state & ST25R3916_PT_STATUS_STATE_MASK) {
    case ST25R3916_PT_STATUS_POWER_OFF:
      return "POWER_OFF";
    case ST25R3916_PT_STATUS_IDLE:
      return "IDLE";
    case ST25R3916_PT_STATUS_READY_L1:
      return "READY_L1";
    case ST25R3916_PT_STATUS_READY_L2:
      return "READY_L2";
    case ST25R3916_PT_STATUS_ACTIVE:
      return "ACTIVE";
    case ST25R3916_PT_STATUS_HALT:
      return "HALT";
    case ST25R3916_PT_STATUS_READY_L1_STAR:
      return "READY_L1*";
    case ST25R3916_PT_STATUS_READY_L2_STAR:
      return "READY_L2*";
    case ST25R3916_PT_STATUS_ACTIVE_STAR:
      return "ACTIVE*";
    default:
      return "RFU";
  }
}

esp_err_t st25r3916_target_nfca_start(const uint8_t *uid, size_t uid_len, uint16_t atqa,
                                      uint8_t sak) {
  if (uid == NULL || (uid_len != 4 && uid_len != 7)) return ESP_ERR_INVALID_ARG;
  if (!st25r3916_is_ready()) return ESP_ERR_INVALID_STATE;

  uint8_t pt[PT_MEM_A_CONFIG_LEN] = {0};
  memcpy(pt, uid, uid_len);
  /* Use the same byte order observed in the reader FIFO: ATQA/SENS_RES LSB first on the air. */
  pt[10] = (uint8_t)(atqa & 0xFF);
  pt[11] = (uint8_t)(atqa >> 8);
  pt[12] = (uid_len == 7) ? NFCA_SAK_CASCADE : (sak & (uint8_t)~NFCA_SAK_CASCADE);
  pt[13] = sak & (uint8_t)~NFCA_SAK_CASCADE;
  pt[14] = sak & (uint8_t)~NFCA_SAK_CASCADE;

  st25r3916_cmd(ST25R3916_CMD_STOP);
  st25r3916_reg_write(ST25R3916_REG_OP_CONTROL, ST25R3916_OP_CONTROL_EN);
  st25r3916_reg_modify(ST25R3916_REG_IO_CONF2, ST25R3916_IO_CONF2_IO_DRV_LVL,
                       ST25R3916_IO_CONF2_IO_DRV_LVL);

  esp_err_t err = st25r3916_pt_memory_load(0, pt, sizeof(pt));
  if (err != ESP_OK) return err;

  err = st25r3916_reg_modify(ST25R3916_REG_AUX, ST25R3916_AUX_NFC_ID_MASK,
                             uid_len == 7 ? ST25R3916_AUX_NFC_ID_7_BYTES
                                          : ST25R3916_AUX_NFC_ID_4_BYTES);
  if (err != ESP_OK) return err;

  st25r3916_reg_modify(ST25R3916_REG_TX_DRIVER, ST25R3916_TX_DRIVER_LM_DRI,
                       ST25R3916_TX_DRIVER_LM_DRI);
  st25r3916_reg_modify(ST25R3916_REG_AUX_MOD,
                       ST25R3916_AUX_MOD_LM_EXT | ST25R3916_AUX_MOD_LM_DRI,
                       ST25R3916_AUX_MOD_LM_EXT | ST25R3916_AUX_MOD_LM_DRI);
  st25r3916_reg_write(ST25R3916_REG_PT_MOD, ST25R3916_PT_MOD_INTERNAL_LM);
  st25r3916_reg_write(ST25R3916_REG_PT_DEF, 0x5C);  // fdel=2,1 + d_ac_ap2p + d_212_424_1r
  st25r3916_reg_write(ST25R3916_REG_MASK_RX_TIMER, 0x02);
  st25r3916_reg_write(ST25R3916_REG_FIELD_THRESH_ACT, ST25R3916_FIELD_THRESH_ACT_MOMENTUM);
  st25r3916_reg_write(ST25R3916_REG_FIELD_THRESH_DEACT, ST25R3916_FIELD_THRESH_DEACT_MOMENTUM);
  st25r3916_reg_write(ST25R3916_REG_BIT_RATE, 0x00);  // 106 kb/s TX and RX
  st25r3916_reg_modify(ST25R3916_REG_ISO14443A_NFC,
                       ST25R3916_ISO14443A_NO_TX_PAR | ST25R3916_ISO14443A_NO_RX_PAR |
                           ST25R3916_ISO14443A_ANTCL,
                       0x00);
  st25r3916_reg_write(ST25R3916_REG_MODE, ST25R3916_MODE_TARGET_NFCA);

  /* RX analog front-end config matching Flipper's listener init
   * (furi_hal_nfc_iso14443a_common_init). Optimises receiver sensitivity
   * for card-emulation at 106 kbps. */
  st25r3916_reg_write(ST25R3916_REG_RX_CONF1, 0x08);  // z600k
  st25r3916_reg_write(ST25R3916_REG_RX_CONF2, 0x2B);  // agc6_3|agc_m|agc_en|sqm_dyn
  st25r3916_reg_write(ST25R3916_REG_RX_CONF3, 0x00);  // HF, full AM/PM gain
  st25r3916_reg_write(ST25R3916_REG_RX_CONF4, 0x00);  // no gain reduction
  st25r3916_reg_write(ST25R3916_REG_CORR_CONF1, ST25R3916_CORR_CONF1_MOMENTUM);
  st25r3916_reg_write(ST25R3916_REG_CORR_CONF2, 0x00);
  st25r3916_reg_modify(ST25R3916_REG_EMD_SUP_CONF, ST25R3916_EMD_SUP_CONF_RX_START_EMV,
                       ST25R3916_EMD_SUP_CONF_RX_START_EMV);

  st25r3916_irq_clear();
  st25r3916_reg_write(ST25R3916_REG_OP_CONTROL,
                      ST25R3916_OP_CONTROL_EN | ST25R3916_OP_CONTROL_RX_EN |
                          ST25R3916_OP_CONTROL_EN_FD_AUTO);
  st25r3916_cmd(ST25R3916_CMD_UNMASK_RECEIVE_DATA);
  return st25r3916_cmd(ST25R3916_CMD_GOTO_SENSE);
}

esp_err_t st25r3916_target_nfca_status(st25r3916_target_status_t *status) {
  if (status == NULL) return ESP_ERR_INVALID_ARG;
  uint8_t timer_irq = 0;
  esp_err_t err = st25r3916_irq_update(NULL, &timer_irq, NULL);
  if (err != ESP_OK) return err;
  uint8_t pt_irq = 0;
  err = st25r3916_irq_update_pt(&pt_irq);
  if (err != ESP_OK) return err;
  uint8_t pt_status = 0;
  err = st25r3916_reg_read(ST25R3916_REG_PT_STATUS, &pt_status);
  if (err != ESP_OK) return err;
  status->irq_timer = timer_irq;
  status->irq_pt = pt_irq;
  status->state = (uint8_t)(pt_status & ST25R3916_PT_STATUS_STATE_MASK);
  return ESP_OK;
}

esp_err_t st25r3916_target_nfca_receive(uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len,
                                        int timeout_ms) {
  return st25r3916_target_nfca_receive_ex(rx, rx_cap, rx_len, timeout_ms, NULL);
}

esp_err_t st25r3916_target_nfca_receive_ex(uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len,
                                           int timeout_ms,
                                           st25r3916_target_rx_info_t *info) {
  if (rx_len) *rx_len = 0;
  if (info) memset(info, 0, sizeof(*info));
  if (rx == NULL || rx_cap == 0) return ESP_ERR_INVALID_ARG;

  /* Poll at ~20us granularity: the response window starts at RXE (end of the
   * reader's command), so minimising RXE-detection jitter directly widens our
   * margin to answer within the reader's frame-delay-time. */
  int iters = timeout_ms * 50;
  if (iters < 1) iters = 1;
  uint8_t main_irq = 0;
  for (int i = 0; i < iters; i++) {
    st25r3916_irq_update(&main_irq, NULL, NULL);
    /* Wait for RXE (end of receive) = the *complete* frame is in the FIFO.
     * Do NOT break on fifo_count>0: during reception the FIFO fills byte by
     * byte (RXS has fired but RXE has not), so an early break hands the caller
     * a truncated command — e.g. a READ with its page byte missing ("30") —
     * which the reader then rejects, showing the tag as unknown / no NDEF. */
    if (main_irq & ST25R3916_IRQ_MAIN_RXE) break;
    esp_rom_delay_us(20);
  }
  if (info) info->main_irq = main_irq;
  if ((main_irq & ST25R3916_IRQ_MAIN_RXE) == 0) return ESP_ERR_TIMEOUT;

  uint8_t err_irq = 0;
  st25r3916_irq_update(NULL, NULL, &err_irq);

  uint16_t n = 0;
  uint8_t last_bits = 0;
  st25r3916_fifo_status(&n, &last_bits);
  if (info) {
    info->err_irq = err_irq;
    info->fifo_bytes = n;
    info->last_bits = last_bits;
  }
  if (err_irq & (ST25R3916_IRQ_ERR_CRC | ST25R3916_IRQ_ERR_PAR)) {
    st25r3916_fifo_clear();
    st25r3916_irq_clear();
    st25r3916_cmd(ST25R3916_CMD_UNMASK_RECEIVE_DATA);
    return ESP_ERR_INVALID_CRC;
  }
  if (err_irq & (ST25R3916_IRQ_ERR_HFE | ST25R3916_IRQ_ERR_SFE)) {
    st25r3916_fifo_clear();
    st25r3916_irq_clear();
    st25r3916_cmd(ST25R3916_CMD_UNMASK_RECEIVE_DATA);
    return ESP_ERR_INVALID_RESPONSE;
  }
  if (n == 0) {
    st25r3916_irq_clear();
    return ESP_ERR_INVALID_RESPONSE;
  }
  if (last_bits != 0) {
    st25r3916_fifo_clear();
    st25r3916_irq_clear();
    st25r3916_cmd(ST25R3916_CMD_UNMASK_RECEIVE_DATA);
    return ESP_ERR_INVALID_RESPONSE;
  }
  if (n > rx_cap) n = rx_cap;
  esp_err_t err = st25r3916_fifo_read(rx, n);
  if (err != ESP_OK) return err;

  /* CRC-checked NFC-A frames still occupy the FIFO with their trailing CRC bytes. */
  if (n >= 2) n = (uint16_t)(n - 2);
  if (rx_len) *rx_len = n;
  st25r3916_irq_clear();
  st25r3916_cmd(ST25R3916_CMD_UNMASK_RECEIVE_DATA);
  return ESP_OK;
}

esp_err_t st25r3916_target_nfca_respond(const uint8_t *tx, uint16_t tx_len, bool with_crc) {
  if (tx == NULL || tx_len == 0) return ESP_ERR_INVALID_ARG;
  st25r3916_fifo_clear();
  esp_err_t err = st25r3916_fifo_load(tx, tx_len);
  if (err != ESP_OK) return err;
  st25r3916_set_num_tx_bytes(tx_len, 0);
  st25r3916_irq_clear();
  st25r3916_cmd(with_crc ? ST25R3916_CMD_TRANSMIT_WITH_CRC : ST25R3916_CMD_TRANSMIT_WITHOUT_CRC);
  err = st25r3916_irq_wait_main(ST25R3916_IRQ_MAIN_TXE, 20);
  st25r3916_cmd(ST25R3916_CMD_UNMASK_RECEIVE_DATA);
  return err;
}

esp_err_t st25r3916_target_nfca_respond_bits(uint8_t bits, uint8_t bit_len) {
  if (bit_len == 0 || bit_len > 7) return ESP_ERR_INVALID_ARG;
  st25r3916_fifo_clear();
  esp_err_t err = st25r3916_fifo_load(&bits, 1);
  if (err != ESP_OK) return err;
  st25r3916_set_num_tx_bytes(0, bit_len);
  st25r3916_irq_clear();
  st25r3916_cmd(ST25R3916_CMD_TRANSMIT_WITHOUT_CRC);
  err = st25r3916_irq_wait_main(ST25R3916_IRQ_MAIN_TXE, 20);
  st25r3916_cmd(ST25R3916_CMD_UNMASK_RECEIVE_DATA);
  return err;
}

void st25r3916_target_stop(void) {
  if (!st25r3916_is_ready()) return;
  st25r3916_cmd(ST25R3916_CMD_STOP);
  st25r3916_reg_write(ST25R3916_REG_OP_CONTROL, ST25R3916_OP_CONTROL_FIELD_OFF);
}
