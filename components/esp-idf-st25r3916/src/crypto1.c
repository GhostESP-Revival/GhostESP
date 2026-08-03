// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
//
// Clean-room MIFARE Classic Crypto1 cipher (see crypto1.h). The 48-bit LFSR
// feedback polynomials, the 20-bit nonlinear filter and the 16-bit tag PRNG are
// the publicly documented Crypto1 algorithm.

#include "crypto1.h"

#define LF_POLY_ODD  0x29CE5Cu
#define LF_POLY_EVEN 0x870804u

#define BIT(x, n)   (((x) >> (n)) & 1)
#define BEBIT(x, n) BIT(x, (n) ^ 24)

static inline uint8_t even_parity32(uint32_t x) {
  x ^= x >> 16;
  x ^= x >> 8;
  x ^= x >> 4;
  x ^= x >> 2;
  x ^= x >> 1;
  return (uint8_t)(x & 1);
}

uint8_t crypto1_odd_parity8(uint8_t b) {
  /* Odd parity: parity bit set so the 9-bit group has an odd number of ones. */
  return (uint8_t)(even_parity32(b) ^ 1u);
}

void crypto1_reset(crypto1_t *c) {
  c->odd = 0;
  c->even = 0;
}

void crypto1_init(crypto1_t *c, uint64_t key) {
  c->odd = 0;
  c->even = 0;
  for (int8_t i = 47; i > 0; i -= 2) {
    c->odd = (c->odd << 1) | BIT(key, (i - 1) ^ 7);
    c->even = (c->even << 1) | BIT(key, i ^ 7);
  }
}

uint8_t crypto1_filter(uint32_t in) {
  uint32_t out = 0;
  out = 0xf22c0u >> (in & 0xf) & 16;
  out |= 0x6c9c0u >> (in >> 4 & 0xf) & 8;
  out |= 0x3c8b0u >> (in >> 8 & 0xf) & 4;
  out |= 0x1e458u >> (in >> 12 & 0xf) & 2;
  out |= 0x0d938u >> (in >> 16 & 0xf) & 1;
  return (uint8_t)BIT(0xEC57E80Au, out);
}

uint8_t crypto1_bit(crypto1_t *c, uint8_t in, int is_encrypted) {
  uint8_t out = crypto1_filter(c->odd);
  uint32_t feed = out & (uint32_t)(!!is_encrypted);
  feed ^= (uint32_t)(!!in);
  feed ^= LF_POLY_ODD & c->odd;
  feed ^= LF_POLY_EVEN & c->even;
  c->even = (c->even << 1) | even_parity32(feed);

  uint32_t t = c->odd;  // swap odd/even planes
  c->odd = c->even;
  c->even = t;
  return out;
}

uint8_t crypto1_byte(crypto1_t *c, uint8_t in, int is_encrypted) {
  uint8_t out = 0;
  for (uint8_t i = 0; i < 8; i++) {
    out |= (uint8_t)(crypto1_bit(c, (uint8_t)BIT(in, i), is_encrypted) << i);
  }
  return out;
}

uint32_t crypto1_word(crypto1_t *c, uint32_t in, int is_encrypted) {
  uint32_t out = 0;
  for (uint8_t i = 0; i < 32; i++) {
    out |= (uint32_t)crypto1_bit(c, (uint8_t)BEBIT(in, i), is_encrypted) << (24 ^ i);
  }
  return out;
}

uint32_t crypto1_prng_successor(uint32_t x, uint32_t n) {
  /* The tag PRNG is a 16-bit LFSR clocked on the byte-swapped nonce. */
  x = (x >> 8 & 0xff00ffu) | (x & 0xff00ffu) << 8;
  x = x >> 16 | x << 16;
  while (n--) {
    x = x >> 1 | (uint32_t)(x >> 16 ^ x >> 18 ^ x >> 19 ^ x >> 21) << 31;
  }
  x = (x >> 8 & 0xff00ffu) | (x & 0xff00ffu) << 8;
  return x >> 16 | x << 16;
}
