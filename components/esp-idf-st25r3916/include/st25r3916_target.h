// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
// Clean-room ST25R3916 NFC-A target support authored from ST datasheet DS12484 Rev 8.
// No vendor (ST SLA0044) or other third-party driver source is used.

#ifndef ST25R3916_TARGET_H
#define ST25R3916_TARGET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
  uint8_t irq_timer;
  uint8_t irq_pt;
  uint8_t state;
} st25r3916_target_status_t;

typedef struct {
  uint8_t main_irq;
  uint8_t err_irq;
  uint16_t fifo_bytes;
  uint8_t last_bits;
} st25r3916_target_rx_info_t;

esp_err_t st25r3916_target_nfca_start(const uint8_t *uid, size_t uid_len, uint16_t atqa,
                                      uint8_t sak);
esp_err_t st25r3916_target_nfca_status(st25r3916_target_status_t *status);
esp_err_t st25r3916_target_nfca_receive(uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len,
                                        int timeout_ms);
esp_err_t st25r3916_target_nfca_receive_ex(uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len,
                                           int timeout_ms,
                                           st25r3916_target_rx_info_t *info);
esp_err_t st25r3916_target_nfca_respond(const uint8_t *tx, uint16_t tx_len, bool with_crc);
esp_err_t st25r3916_target_nfca_respond_bits(uint8_t bits, uint8_t bit_len);
void st25r3916_target_stop(void);
const char *st25r3916_target_state_name(uint8_t state);

#ifdef __cplusplus
}
#endif

#endif // ST25R3916_TARGET_H
