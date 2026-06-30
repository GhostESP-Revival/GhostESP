// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
// Clean-room ST25R3916 support authored from ST datasheet DS12484 Rev 8.
// No vendor (ST SLA0044) or other third-party driver source is used.

#include "st25r3916_iso14443a.h"
#include "st25r3916.h"
#include "st25r3916_reg.h"

#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "st25r3916_nfca";

/* ISO14443-3 anti-collision SELECT cascade-level codes. */
#define NFCA_SEL_CL1 0x93
#define NFCA_SEL_CL2 0x95
#define NFCA_SEL_CL3 0x97
#define NFCA_NVB_AC  0x20 /**< Number of valid bits: 2 bytes (SEL + NVB), full read. */
#define NFCA_NVB_SEL 0x70 /**< Number of valid bits: 7 bytes (full SELECT). */
#define NFCA_CT      0x88 /**< Cascade tag, prepended to continuation UIDs. */
#define NFCA_SAK_CASCADE 0x04 /**< SAK bit 2 set: UID not complete, cascade. */

#define NFCA_RX_SCRATCH 264

static esp_err_t nfca_wait_rxe(int timeout_ms) {
  uint8_t m = 0;
  int iters = timeout_ms * 10;  // poll ~every 100 us
  if (iters < 1) iters = 1;
  for (int i = 0; i < iters; i++) {
    st25r3916_irq_update(&m, NULL, NULL);
    if (m & ST25R3916_IRQ_MAIN_RXE) return ESP_OK;
    if (m & ST25R3916_IRQ_MAIN_COL) return ESP_ERR_INVALID_RESPONSE;  // bit collision
    esp_rom_delay_us(100);
  }
  return ESP_ERR_TIMEOUT;
}

esp_err_t st25r3916_nfca_transceive(const uint8_t *tx, uint16_t tx_len, bool with_crc,
                                    bool anticoll, uint8_t *rx, uint16_t rx_cap,
                                    uint16_t *rx_len, int timeout_ms) {
  if (rx_len) *rx_len = 0;

  /* Stop any prior activity (also clears FIFO), then arm the new frame. */
  st25r3916_cmd(ST25R3916_CMD_STOP);
  st25r3916_irq_clear();
  st25r3916_reg_modify(ST25R3916_REG_ISO14443A_NFC, ST25R3916_ISO14443A_ANTCL,
                       anticoll ? ST25R3916_ISO14443A_ANTCL : 0x00);
  st25r3916_fifo_clear();

  if (tx_len > 0) {
    esp_err_t e = st25r3916_fifo_load(tx, tx_len);
    if (e != ESP_OK) return e;
  }
  st25r3916_set_num_tx_bytes(tx_len, 0);
  st25r3916_cmd(with_crc ? ST25R3916_CMD_TRANSMIT_WITH_CRC : ST25R3916_CMD_TRANSMIT_WITHOUT_CRC);

  /* Wait for end of transmission, then for the response. */
  st25r3916_irq_wait_main(ST25R3916_IRQ_MAIN_TXE, 20);
  esp_err_t err = nfca_wait_rxe(timeout_ms);
  if (err != ESP_OK) return err;

  uint8_t eerr = 0;
  st25r3916_irq_update(NULL, NULL, &eerr);
  if (with_crc && (eerr & ST25R3916_IRQ_ERR_CRC)) return ESP_ERR_INVALID_CRC;
  if (eerr & ST25R3916_IRQ_ERR_HFE) return ESP_ERR_INVALID_RESPONSE;  // hard framing error

  uint16_t n = st25r3916_fifo_count();
  if (n == 0) return ESP_ERR_INVALID_RESPONSE;
  if (n > NFCA_RX_SCRATCH) n = NFCA_RX_SCRATCH;

  uint8_t scratch[NFCA_RX_SCRATCH];
  err = st25r3916_fifo_read(scratch, n);
  if (err != ESP_OK) return err;

  uint16_t payload = n;
  if (with_crc && payload >= 2) payload -= 2;  // FIFO holds received CRC (DS 4.2); strip it
  if (rx && rx_cap) {
    uint16_t copy = (payload > rx_cap) ? rx_cap : payload;
    memcpy(rx, scratch, copy);
    if (rx_len) *rx_len = copy;
  } else if (rx_len) {
    *rx_len = payload;
  }
  return ESP_OK;
}

/* --- bit-level transceive with manual parity (MIFARE Crypto1) ----------- */

#define NFCA_BITS_SCRATCH 48 /* FIFO bytes; covers 16+2 data*9 bits packed. */

static inline void bits_set(uint8_t *buf, uint16_t pos, uint8_t bit) {
  if (bit) buf[pos >> 3] |= (uint8_t)(1u << (pos & 7));
}
static inline uint8_t bits_get(const uint8_t *buf, uint16_t pos) {
  return (uint8_t)((buf[pos >> 3] >> (pos & 7)) & 1u);
}

esp_err_t st25r3916_nfca_transceive_bits(const uint8_t *tx_data, const uint8_t *tx_par,
                                         uint16_t n_tx, uint8_t *rx_data, uint8_t *rx_par,
                                         uint16_t rx_cap, uint16_t *n_rx,
                                         uint8_t *rx_residual_bits, uint8_t *rx_residual,
                                         int timeout_ms) {
  if (n_rx) *n_rx = 0;
  if (rx_residual_bits) *rx_residual_bits = 0;

  uint16_t total_bits = (uint16_t)(n_tx * 9);
  uint16_t n_fifo_tx = (uint16_t)((total_bits + 7) / 8);
  if (n_fifo_tx > NFCA_BITS_SCRATCH) return ESP_ERR_INVALID_SIZE;

  /* Manual TX/RX parity, standard (non anti-collision) frame. */
  st25r3916_cmd(ST25R3916_CMD_STOP);
  st25r3916_irq_clear();
  st25r3916_reg_modify(ST25R3916_REG_ISO14443A_NFC,
                       ST25R3916_ISO14443A_NO_TX_PAR | ST25R3916_ISO14443A_NO_RX_PAR |
                           ST25R3916_ISO14443A_ANTCL,
                       ST25R3916_ISO14443A_NO_TX_PAR | ST25R3916_ISO14443A_NO_RX_PAR);
  st25r3916_fifo_clear();

  uint8_t txbuf[NFCA_BITS_SCRATCH] = {0};
  uint16_t pos = 0;
  for (uint16_t i = 0; i < n_tx; i++) {
    for (int b = 0; b < 8; b++) bits_set(txbuf, pos++, (uint8_t)((tx_data[i] >> b) & 1));
    bits_set(txbuf, pos++, tx_par ? (uint8_t)(tx_par[i] & 1) : 0);
  }

  esp_err_t err = ESP_OK;
  if (n_fifo_tx) err = st25r3916_fifo_load(txbuf, n_fifo_tx);
  if (err != ESP_OK) goto restore;
  st25r3916_set_num_tx_bytes((uint16_t)(total_bits / 8), (uint8_t)(total_bits % 8));
  st25r3916_cmd(ST25R3916_CMD_TRANSMIT_WITHOUT_CRC);

  st25r3916_irq_wait_main(ST25R3916_IRQ_MAIN_TXE, 20);
  err = nfca_wait_rxe(timeout_ms);
  if (err != ESP_OK) goto restore;

  uint16_t n = st25r3916_fifo_count();
  if (n == 0) {
    err = ESP_ERR_INVALID_RESPONSE;
    goto restore;
  }
  if (n > NFCA_BITS_SCRATCH) n = NFCA_BITS_SCRATCH;
  uint8_t raw[NFCA_BITS_SCRATCH];
  err = st25r3916_fifo_read(raw, n);
  if (err != ESP_OK) goto restore;

  uint8_t st2 = 0;
  st25r3916_reg_read(ST25R3916_REG_FIFO_STATUS2, &st2);
  uint8_t lb = (uint8_t)((st2 >> 1) & 0x07);  // bits in last FIFO byte (0 = full byte)
  uint16_t rx_bits = lb ? (uint16_t)((n - 1) * 8 + lb) : (uint16_t)(n * 8);

  uint16_t groups = (uint16_t)(rx_bits / 9);  // 8 data + 1 parity per group
  uint16_t copy = (groups > rx_cap) ? rx_cap : groups;
  for (uint16_t g = 0; g < copy; g++) {
    uint8_t byte = 0;
    for (int b = 0; b < 8; b++) byte |= (uint8_t)(bits_get(raw, (uint16_t)(g * 9 + b)) << b);
    rx_data[g] = byte;
    if (rx_par) rx_par[g] = bits_get(raw, (uint16_t)(g * 9 + 8));
  }
  if (n_rx) *n_rx = copy;

  uint16_t residual = (uint16_t)(rx_bits - (uint16_t)(groups * 9));
  if (residual > 0 && rx_residual_bits) {
    /* Trailing partial group (e.g. a 4-bit MIFARE ACK/NAK). */
    uint8_t v = 0;
    for (uint16_t b = 0; b < residual && b < 8; b++)
      v |= (uint8_t)(bits_get(raw, (uint16_t)(groups * 9 + b)) << b);
    *rx_residual_bits = (uint8_t)residual;
    if (rx_residual) *rx_residual = v;
  }

restore:
  st25r3916_reg_modify(ST25R3916_REG_ISO14443A_NFC,
                       ST25R3916_ISO14443A_NO_TX_PAR | ST25R3916_ISO14443A_NO_RX_PAR, 0x00);
  return err;
}

static esp_err_t nfca_short_frame(uint8_t direct_cmd, uint16_t *atqa, int timeout_ms) {
  /* REQA/WUPA are issued through dedicated direct commands which generate the
   * 7-bit short frame and disable the response CRC check (DS 4.4.4). */
  st25r3916_cmd(ST25R3916_CMD_STOP);
  st25r3916_irq_clear();
  st25r3916_reg_modify(ST25R3916_REG_ISO14443A_NFC, ST25R3916_ISO14443A_ANTCL, 0x00);
  st25r3916_set_num_tx_bytes(0, 0);
  st25r3916_cmd(direct_cmd);

  st25r3916_irq_wait_main(ST25R3916_IRQ_MAIN_TXE, 20);
  esp_err_t err = nfca_wait_rxe(timeout_ms);
  if (err != ESP_OK) return err;

  uint16_t n = st25r3916_fifo_count();
  if (n < 2) return ESP_ERR_NOT_FOUND;  // expect 2-byte ATQA
  uint8_t buf[4] = {0};
  st25r3916_fifo_read(buf, (n > 2) ? 2 : n);
  if (atqa) *atqa = (uint16_t)((buf[1] << 8) | buf[0]);  // ATQA little-endian on the air
  return ESP_OK;
}

esp_err_t st25r3916_nfca_reqa(uint16_t *atqa, int timeout_ms) {
  return nfca_short_frame(ST25R3916_CMD_TRANSMIT_REQA, atqa, timeout_ms);
}

esp_err_t st25r3916_nfca_wupa(uint16_t *atqa, int timeout_ms) {
  return nfca_short_frame(ST25R3916_CMD_TRANSMIT_WUPA, atqa, timeout_ms);
}

esp_err_t st25r3916_nfca_activate(uint8_t *uid, uint8_t *uid_len, uint16_t *atqa, uint8_t *sak) {
  if (!uid || !uid_len) return ESP_ERR_INVALID_ARG;
  *uid_len = 0;

  uint16_t atqa_local = 0;
  esp_err_t err = st25r3916_nfca_reqa(&atqa_local, 20);
  if (err != ESP_OK) return ESP_ERR_NOT_FOUND;
  if (atqa) *atqa = atqa_local;

  static const uint8_t sel_codes[3] = {NFCA_SEL_CL1, NFCA_SEL_CL2, NFCA_SEL_CL3};
  uint8_t last_sak = 0;

  for (int level = 0; level < 3; level++) {
    /* Anti-collision: SEL + NVB(0x20), no CRC -> 4 UID/CT bytes + BCC. */
    uint8_t ac_tx[2] = {sel_codes[level], NFCA_NVB_AC};
    uint8_t ac_rx[8] = {0};
    uint16_t ac_len = 0;
    err = st25r3916_nfca_transceive(ac_tx, sizeof(ac_tx), false, true, ac_rx, sizeof(ac_rx),
                                    &ac_len, 20);
    if (err != ESP_OK) return err;
    if (ac_len < 5) return ESP_ERR_INVALID_RESPONSE;

    uint8_t bcc = (uint8_t)(ac_rx[0] ^ ac_rx[1] ^ ac_rx[2] ^ ac_rx[3]);
    if (bcc != ac_rx[4]) {
      ESP_LOGW(TAG, "BCC mismatch at cascade level %d", level + 1);
      return ESP_ERR_INVALID_CRC;
    }

    /* SELECT: SEL + NVB(0x70) + 4 UID/CT bytes + BCC, with CRC -> 1-byte SAK. */
    uint8_t sel_tx[7] = {sel_codes[level], NFCA_NVB_SEL, ac_rx[0], ac_rx[1],
                         ac_rx[2],         ac_rx[3],     ac_rx[4]};
    uint8_t sak_rx[2] = {0};
    uint16_t sak_len = 0;
    err = st25r3916_nfca_transceive(sel_tx, sizeof(sel_tx), true, false, sak_rx, sizeof(sak_rx),
                                    &sak_len, 20);
    if (err != ESP_OK) return err;
    if (sak_len < 1) return ESP_ERR_INVALID_RESPONSE;
    last_sak = sak_rx[0];

    if (last_sak & NFCA_SAK_CASCADE) {
      /* UID incomplete: first byte was the cascade tag (0x88); keep the next 3. */
      if (*uid_len + 3 > 10) return ESP_ERR_INVALID_SIZE;
      memcpy(&uid[*uid_len], &ac_rx[1], 3);
      *uid_len += 3;
    } else {
      if (*uid_len + 4 > 10) return ESP_ERR_INVALID_SIZE;
      memcpy(&uid[*uid_len], &ac_rx[0], 4);
      *uid_len += 4;
      break;  // UID complete
    }
  }

  if (sak) *sak = last_sak;
  return ESP_OK;
}
