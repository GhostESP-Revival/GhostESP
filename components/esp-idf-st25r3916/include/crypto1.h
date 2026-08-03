// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
//
// Clean-room implementation of the MIFARE Classic "Crypto1" stream cipher from
// the publicly published algorithm (Garcia et al., "Dismantling MIFARE Classic";
// proxmark3 / crapto1 describe the same 48-bit LFSR, nonlinear filter and the
// 16-bit tag PRNG). Dependency-free so it can run on the ST25R3916 software
// MIFARE path. The cipher itself is NXP's; this is an independent reimpl, not a
// copy of any particular source file.

#ifndef ST25R3916_CRYPTO1_H
#define ST25R3916_CRYPTO1_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** @brief Crypto1 cipher state (48-bit LFSR split into odd/even bit planes). */
typedef struct {
  uint32_t odd;
  uint32_t even;
} crypto1_t;

/** @brief Load a 48-bit key (lower 48 bits of @p key) into the cipher. */
void crypto1_init(crypto1_t *c, uint64_t key);

/** @brief Reset the cipher state to zero. */
void crypto1_reset(crypto1_t *c);

/**
 * @brief Clock the cipher once.
 * @param in            Input bit fed into the LFSR (0/1).
 * @param is_encrypted  Non-zero to also feed back the keystream bit.
 * @return The keystream bit produced before clocking.
 */
uint8_t crypto1_bit(crypto1_t *c, uint8_t in, int is_encrypted);

/** @brief Clock 8 times, LSB first; returns 8 keystream bits. */
uint8_t crypto1_byte(crypto1_t *c, uint8_t in, int is_encrypted);

/** @brief Clock 32 times (big-endian bit order); returns 32 keystream bits. */
uint32_t crypto1_word(crypto1_t *c, uint32_t in, int is_encrypted);

/** @brief Current nonlinear-filter output for the cipher's odd plane (keystream bit). */
uint8_t crypto1_filter(uint32_t odd);

/** @brief n-th successor of the 16-bit MIFARE tag PRNG seeded by @p x. */
uint32_t crypto1_prng_successor(uint32_t x, uint32_t n);

/** @brief Odd parity bit of a byte (ISO14443-A parity: makes total #ones odd). */
uint8_t crypto1_odd_parity8(uint8_t b);

#ifdef __cplusplus
}
#endif

#endif // ST25R3916_CRYPTO1_H
