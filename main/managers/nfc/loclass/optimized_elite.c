//-----------------------------------------------------------------------------
// Borrowed initially from https://github.com/holiman/loclass
// Copyright (C) 2014 Martin Holst Swende
// Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// See LICENSE.txt for the text of the license.
//-----------------------------------------------------------------------------
// Adapted for GhostESP from Momentum-Firmware's picopass application.
// Momentum-Firmware: https://github.com/Next-Flip/Momentum-Firmware
// Original picopass app by Eric Betts (bettse):
//   https://github.com/bettse/picopass
//-----------------------------------------------------------------------------
#include "managers/nfc/loclass/optimized_elite.h"
#include "managers/nfc/loclass/optimized_ikeys.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "managers/nfc/loclass/standalone_des.h"

void loclass_permutekey(const uint8_t key[8], uint8_t dest[8]) {
    int i;
    for(i = 0; i < 8; i++) {
        dest[i] = (((key[7] & (0x80 >> i)) >> (7 - i)) << 7) |
                  (((key[6] & (0x80 >> i)) >> (7 - i)) << 6) |
                  (((key[5] & (0x80 >> i)) >> (7 - i)) << 5) |
                  (((key[4] & (0x80 >> i)) >> (7 - i)) << 4) |
                  (((key[3] & (0x80 >> i)) >> (7 - i)) << 3) |
                  (((key[2] & (0x80 >> i)) >> (7 - i)) << 2) |
                  (((key[1] & (0x80 >> i)) >> (7 - i)) << 1) |
                  (((key[0] & (0x80 >> i)) >> (7 - i)) << 0);
    }
}

void loclass_permutekey_rev(const uint8_t key[8], uint8_t dest[8]) {
    int i;
    for(i = 0; i < 8; i++) {
        dest[7 - i] = (((key[0] & (0x80 >> i)) >> (7 - i)) << 7) |
                      (((key[1] & (0x80 >> i)) >> (7 - i)) << 6) |
                      (((key[2] & (0x80 >> i)) >> (7 - i)) << 5) |
                      (((key[3] & (0x80 >> i)) >> (7 - i)) << 4) |
                      (((key[4] & (0x80 >> i)) >> (7 - i)) << 3) |
                      (((key[5] & (0x80 >> i)) >> (7 - i)) << 2) |
                      (((key[6] & (0x80 >> i)) >> (7 - i)) << 1) |
                      (((key[7] & (0x80 >> i)) >> (7 - i)) << 0);
    }
}

static uint8_t loclass_rr(uint8_t val) {
    return val >> 1 | ((val & 1) << 7);
}

static uint8_t loclass_rl(uint8_t val) {
    return val << 1 | ((val & 0x80) >> 7);
}

static uint8_t loclass_swap(uint8_t val) {
    return ((val >> 4) & 0xFF) | ((val & 0xFF) << 4);
}

void loclass_hash1(const uint8_t csn[], uint8_t k[]) {
    k[0] = csn[0] ^ csn[1] ^ csn[2] ^ csn[3] ^ csn[4] ^ csn[5] ^ csn[6] ^ csn[7];
    k[1] = csn[0] + csn[1] + csn[2] + csn[3] + csn[4] + csn[5] + csn[6] + csn[7];
    k[2] = loclass_rr(loclass_swap(csn[2] + k[1]));
    k[3] = loclass_rl(loclass_swap(csn[3] + k[0]));
    k[4] = ~loclass_rr(csn[4] + k[2]) + 1;
    k[5] = ~loclass_rl(csn[5] + k[3]) + 1;
    k[6] = loclass_rr(csn[6] + (k[4] ^ 0x3c));
    k[7] = loclass_rl(csn[7] + (k[5] ^ 0xc3));

    k[7] &= 0x7F;
    k[6] &= 0x7F;
    k[5] &= 0x7F;
    k[4] &= 0x7F;
    k[3] &= 0x7F;
    k[2] &= 0x7F;
    k[1] &= 0x7F;
    k[0] &= 0x7F;
}

static void loclass_rk(const uint8_t* key, uint8_t n, uint8_t* outp_key) {
    memcpy(outp_key, key, 8);
    uint8_t j;
    while(n-- > 0) {
        for(j = 0; j < 8; j++) outp_key[j] = loclass_rl(outp_key[j]);
    }
    return;
}

static void loclass_desdecrypt_iclass(uint8_t* iclass_key, uint8_t* input, uint8_t* output) {
    uint8_t key_std_format[8] = {0};
    loclass_permutekey_rev(iclass_key, key_std_format);
    standalone_des_context ctx;
    standalone_des_setkey_dec(&ctx, key_std_format);
    standalone_des_crypt_ecb(&ctx, input, output);
}

static void loclass_desencrypt_iclass(const uint8_t* iclass_key, uint8_t* input, uint8_t* output) {
    uint8_t key_std_format[8] = {0};
    loclass_permutekey_rev(iclass_key, key_std_format);
    standalone_des_context ctx;
    standalone_des_setkey_enc(&ctx, key_std_format);
    standalone_des_crypt_ecb(&ctx, input, output);
}

void loclass_hash2(const uint8_t* key64, uint8_t* outp_keytable) {
    uint8_t key64_negated[8] = {0};
    uint8_t z[8][8] = {{0}, {0}};
    uint8_t temp_output[8] = {0};

    int i;
    for(i = 0; i < 8; i++) key64_negated[i] = ~key64[i];

    loclass_desencrypt_iclass(key64, key64_negated, z[0]);

    uint8_t y[8][8] = {{0}, {0}};

    loclass_desdecrypt_iclass(z[0], key64_negated, y[0]);

    for(i = 1; i < 8; i++) {
        loclass_rk(key64, i, temp_output);
        loclass_desdecrypt_iclass(temp_output, z[i - 1], z[i]);
        loclass_desencrypt_iclass(temp_output, y[i - 1], y[i]);
    }

    if(outp_keytable != NULL) {
        for(i = 0; i < 8; i++) {
            memcpy(outp_keytable + i * 16, y[i], 8);
            memcpy(outp_keytable + 8 + i * 16, z[i], 8);
        }
    }
}
