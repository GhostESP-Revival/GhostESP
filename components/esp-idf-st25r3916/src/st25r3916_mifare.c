// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
//
// MIFARE Classic reader auth + encrypted exchange for the ST25R3916.
// The three-pass handshake and keystream/parity construction follow the
// publicly documented Crypto1 protocol (proxmark3 / academic literature).
// Nested/hardnested nonce capture reference: Momentum-Firmware work by noproto,
// including MIFARE Classic key recovery improvements #3822.

#include "st25r3916_mifare.h"
#include "st25r3916_iso14443a.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "st25r3916_mfc";

#define MFC_SCRATCH 32

/* ISO14443-A CRC_A (DS-independent; ISO/IEC 14443-3 Annex B). */
static void crc_a(const uint8_t *data, uint16_t len, uint8_t out[2]) {
  uint32_t w = 0x6363;
  for (uint16_t i = 0; i < len; i++) {
    uint8_t b = data[i] ^ (uint8_t)(w & 0xFF);
    b ^= (uint8_t)(b << 4);
    w = (w >> 8) ^ ((uint32_t)b << 8) ^ ((uint32_t)b << 3) ^ ((uint32_t)b >> 4);
  }
  out[0] = (uint8_t)(w & 0xFF);
  out[1] = (uint8_t)((w >> 8) & 0xFF);
}

static inline uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static const uint8_t *cuid_bytes_from_uid(const uint8_t *uid, uint8_t uid_len) {
  if (!uid || uid_len < 4) return NULL;
  // cuid is the last 4 UID bytes for all lengths, matching Flipper/Momentum's
  // iso14443_3a_get_cuid(). Keep this identical to the host-side derivation.
  return uid + (uid_len - 4);
}

esp_err_t st25r3916_mifare_auth(crypto1_t *c, const uint8_t *uid, uint8_t uid_len, uint8_t block,
                                uint8_t key_type, const uint8_t key[6]) {
  if (!c || !uid || uid_len < 4 || !key) return ESP_ERR_INVALID_ARG;
  if (key_type != 0x60 && key_type != 0x61) return ESP_ERR_INVALID_ARG;

  uint64_t k = 0;
  for (int i = 0; i < 6; i++) k = (k << 8) | key[i];
  const uint8_t *cuid_bytes = cuid_bytes_from_uid(uid, uid_len);
  if (!cuid_bytes) return ESP_ERR_INVALID_ARG;
  uint32_t cuid = be32(cuid_bytes);

  /* Pass 1: send AUTH (cmd+block+CRC) and read the 4-byte tag nonce (plaintext,
   * no CRC on the response) via a raw transceive. */
  uint8_t cmd[4] = {key_type, block, 0, 0};
  crc_a(cmd, 2, &cmd[2]);
  uint8_t ntb[4] = {0};
  uint16_t rn = 0;
  esp_err_t err = st25r3916_nfca_transceive(cmd, sizeof(cmd), false, false, ntb, sizeof(ntb), &rn, 20);
  if (err != ESP_OK || rn < 4) return ESP_ERR_INVALID_RESPONSE;
  uint32_t nt = be32(ntb);

  /* Initialize cipher with key and feed (nt XOR cuid). */
  crypto1_init(c, k);
  crypto1_word(c, nt ^ cuid, 0);

  /* Pass 2: build {nR}{aR} (8 bytes) with encrypted parity.
   * nR is chosen as 0; aR = suc64(nt). */
  uint8_t nr[4] = {0, 0, 0, 0};
  uint8_t txd[8] = {0}, txp[8] = {0};
  for (int i = 0; i < 4; i++) {
    uint8_t ks = crypto1_byte(c, nr[i], 0);
    txd[i] = (uint8_t)(ks ^ nr[i]);
    txp[i] = (uint8_t)(crypto1_filter(c->odd) ^ crypto1_odd_parity8(nr[i]));
  }
  uint32_t ar = crypto1_prng_successor(nt, 64);
  for (int i = 4; i < 8; i++) {
    uint8_t arb = (uint8_t)(ar >> (24 - ((i - 4) * 8)));
    uint8_t ks = crypto1_byte(c, 0, 0);
    txd[i] = (uint8_t)(ks ^ arb);
    txp[i] = (uint8_t)(crypto1_filter(c->odd) ^ crypto1_odd_parity8(arb));
  }

  /* Pass 3: receive {aT} (4 bytes encrypted) and verify aT == suc96(nt). */
  uint8_t atd[8] = {0}, atp[8] = {0};
  uint16_t an = 0;
  err = st25r3916_nfca_transceive_bits(txd, txp, 8, atd, atp, sizeof(atd), &an, NULL, NULL, 30);
  if (err != ESP_OK || an < 4) return ESP_ERR_INVALID_RESPONSE;

  uint8_t at_plain[4];
  for (int i = 0; i < 4; i++) at_plain[i] = (uint8_t)(crypto1_byte(c, 0, 0) ^ atd[i]);
  uint32_t at = be32(at_plain);
  if (at != crypto1_prng_successor(nt, 96)) {
    ESP_LOGD(TAG, "auth verify failed (block %u)", block);
    return ESP_ERR_INVALID_RESPONSE;
  }
  return ESP_OK;  // cipher c is now armed for encrypted exchange
}

esp_err_t st25r3916_mifare_xfer(crypto1_t *c, const uint8_t *plain, uint16_t plen, uint8_t *out,
                                uint16_t out_cap, uint16_t *out_len) {
  if (!c || !plain || plen + 2 > MFC_SCRATCH) return ESP_ERR_INVALID_ARG;
  if (out_len) *out_len = 0;

  /* Append CRC_A to the plaintext, then encrypt each byte and its parity. */
  uint8_t buf[MFC_SCRATCH];
  memcpy(buf, plain, plen);
  crc_a(plain, plen, &buf[plen]);
  uint16_t n = (uint16_t)(plen + 2);

  uint8_t ed[MFC_SCRATCH], ep[MFC_SCRATCH];
  for (uint16_t i = 0; i < n; i++) {
    uint8_t ks = crypto1_byte(c, 0, 0);
    ed[i] = (uint8_t)(ks ^ buf[i]);
    ep[i] = (uint8_t)(crypto1_filter(c->odd) ^ crypto1_odd_parity8(buf[i]));
  }

  uint8_t rd[MFC_SCRATCH], rp[MFC_SCRATCH];
  uint16_t rn = 0;
  uint8_t res_bits = 0, res_val = 0;
  esp_err_t err = st25r3916_nfca_transceive_bits(ed, ep, n, rd, rp, sizeof(rd), &rn, &res_bits,
                                                 &res_val, 60);
  if (err != ESP_OK) return err;

  /* 4-bit ACK/NAK: decrypt the nibble with 4 keystream bits. */
  if (rn == 0 && res_bits > 0) {
    uint8_t ack = 0;
    for (int b = 0; b < 4; b++) {
      uint8_t enc_bit = (uint8_t)((res_val >> b) & 1u);
      ack |= (uint8_t)((crypto1_bit(c, 0, 0) ^ enc_bit) << b);
    }
    if (out && out_cap >= 1) {
      out[0] = ack;
      if (out_len) *out_len = 1;
    }
    return ESP_OK;
  }

  /* Decrypt response bytes (keystream continues across the exchange). */
  uint8_t dec[MFC_SCRATCH];
  for (uint16_t i = 0; i < rn && i < MFC_SCRATCH; i++) {
    dec[i] = (uint8_t)(crypto1_byte(c, 0, 0) ^ rd[i]);
  }

  /* Strip the 2 trailing CRC bytes; the caller wants the payload only. */
  uint16_t payload = (rn >= 2) ? (uint16_t)(rn - 2) : rn;
  uint16_t copy = (payload > out_cap) ? out_cap : payload;
  if (out) memcpy(out, dec, copy);
  if (out_len) *out_len = copy;
  return ESP_OK;
}

esp_err_t st25r3916_mifare_nested_auth_raw(crypto1_t *c, uint8_t key_type, uint8_t block,
                                            uint8_t nt_enc[4], uint8_t nt_par[4]) {
  if (!c || !nt_enc || !nt_par) return ESP_ERR_INVALID_ARG;
  if (key_type != 0x60 && key_type != 0x61) return ESP_ERR_INVALID_ARG;

  uint8_t plain[4] = {key_type, block, 0, 0};
  crc_a(plain, 2, &plain[2]);

  uint8_t ed[4], ep[4];
  for (uint16_t i = 0; i < sizeof(plain); i++) {
    uint8_t ks = crypto1_byte(c, 0, 0);
    ed[i] = (uint8_t)(ks ^ plain[i]);
    ep[i] = (uint8_t)(crypto1_filter(c->odd) ^ crypto1_odd_parity8(plain[i]));
  }

  uint16_t rn = 0;
  uint8_t residual_bits = 0, residual = 0;
  esp_err_t err = st25r3916_nfca_transceive_bits(ed, ep, sizeof(ed), nt_enc, nt_par, 4, &rn,
                                                 &residual_bits, &residual, 30);
  (void)residual_bits;
  (void)residual;
  if (err != ESP_OK) return err;
  return (rn == 4) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
