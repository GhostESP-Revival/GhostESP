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
#include "managers/nfc/loclass/optimized_ikeys.h"
#include "managers/nfc/loclass/optimized_cipherutils.h"

#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include "managers/nfc/loclass/standalone_des.h"

static const uint8_t loclass_pi[35] = {
    0x0F, 0x17, 0x1B, 0x1D, 0x1E, 0x27, 0x2B, 0x2D, 0x2E,
    0x33, 0x35, 0x39, 0x36, 0x3A, 0x3C, 0x47, 0x4B, 0x4D,
    0x4E, 0x53, 0x55, 0x56, 0x59, 0x5A, 0x5C, 0x63, 0x65,
    0x66, 0x69, 0x6A, 0x6C, 0x71, 0x72, 0x74, 0x78};

static uint8_t loclass_getSixBitByte(uint64_t c, int n) {
    return (c >> (42 - 6 * n)) & 0x3F;
}

static void loclass_pushbackSixBitByte(uint64_t* c, uint8_t z, int n) {
    uint64_t masked = z & 0x3F;
    uint64_t eraser = 0x3F;
    masked <<= 42 - 6 * n;
    eraser <<= 42 - 6 * n;
    eraser = ~eraser;
    (*c) &= eraser;
    (*c) |= masked;
}

static uint64_t loclass_swapZvalues(uint64_t c) {
    uint64_t newz = 0;
    loclass_pushbackSixBitByte(&newz, loclass_getSixBitByte(c, 0), 7);
    loclass_pushbackSixBitByte(&newz, loclass_getSixBitByte(c, 1), 6);
    loclass_pushbackSixBitByte(&newz, loclass_getSixBitByte(c, 2), 5);
    loclass_pushbackSixBitByte(&newz, loclass_getSixBitByte(c, 3), 4);
    loclass_pushbackSixBitByte(&newz, loclass_getSixBitByte(c, 4), 3);
    loclass_pushbackSixBitByte(&newz, loclass_getSixBitByte(c, 5), 2);
    loclass_pushbackSixBitByte(&newz, loclass_getSixBitByte(c, 6), 1);
    loclass_pushbackSixBitByte(&newz, loclass_getSixBitByte(c, 7), 0);
    newz |= (c & 0xFFFF000000000000);
    return newz;
}

static uint64_t loclass_ck(int i, int j, uint64_t z) {
    if(i == 1 && j == -1) {
        return z;
    } else if(j == -1) {
        return loclass_ck(i - 1, i - 2, z);
    }

    if(loclass_getSixBitByte(z, i) == loclass_getSixBitByte(z, j)) {
        uint64_t newz = 0;
        int c;
        for(c = 0; c < 4; c++) {
            uint8_t val = loclass_getSixBitByte(z, c);
            if(c == i)
                loclass_pushbackSixBitByte(&newz, j, c);
            else
                loclass_pushbackSixBitByte(&newz, val, c);
        }
        return loclass_ck(i, j - 1, newz);
    } else {
        return loclass_ck(i, j - 1, z);
    }
}

static uint64_t loclass_check(uint64_t z) {
    uint64_t ck1 = loclass_ck(3, 2, z);
    uint64_t ck2 = loclass_ck(3, 2, z << 24);
    ck1 &= 0x00000000FFFFFF000000;
    ck2 &= 0x00000000FFFFFF000000;
    return ck1 | ck2 >> 24;
}

static void loclass_permute(
    LoclassBitstreamIn_t* p_in,
    uint64_t z,
    int l,
    int r,
    LoclassBitstreamOut_t* out) {
    if(loclass_bitsLeft(p_in) == 0) return;

    bool pn = loclass_tailBit(p_in);
    if(pn) {
        uint8_t zl = loclass_getSixBitByte(z, l);
        loclass_push6bits(out, zl + 1);
        loclass_permute(p_in, z, l + 1, r, out);
    } else {
        uint8_t zr = loclass_getSixBitByte(z, r);
        loclass_push6bits(out, zr);
        loclass_permute(p_in, z, l, r + 1, out);
    }
}

void loclass_hash0(uint64_t c, uint8_t k[8]) {
    c = loclass_swapZvalues(c);

    uint8_t x = (c & 0xFF00000000000000) >> 56;
    uint8_t y = (c & 0x00FF000000000000) >> 48;
    uint64_t zP = 0;

    for(int n = 0; n < 4; n++) {
        uint8_t zn = loclass_getSixBitByte(c, n);
        uint8_t zn4 = loclass_getSixBitByte(c, n + 4);
        uint8_t _zn = (zn % (63 - n)) + n;
        uint8_t _zn4 = (zn4 % (64 - n)) + n;
        loclass_pushbackSixBitByte(&zP, _zn, n);
        loclass_pushbackSixBitByte(&zP, _zn4, n + 4);
    }

    uint64_t zCaret = loclass_check(zP);
    uint8_t p = loclass_pi[x % 35];

    if(x & 1)
        p = ~p;

    LoclassBitstreamIn_t p_in = {&p, 8, 0};
    uint8_t outbuffer[] = {0, 0, 0, 0, 0, 0, 0, 0};
    LoclassBitstreamOut_t out = {outbuffer, 0, 0};
    loclass_permute(&p_in, zCaret, 0, 4, &out);

    uint64_t zTilde = loclass_x_bytes_to_num(outbuffer, sizeof(outbuffer));
    zTilde >>= 16;

    for(int i = 0; i < 8; i++) {
        k[i] = 0;
        k[i] |= (y << (7 - i)) & 0x80;

        uint8_t zTilde_i = loclass_getSixBitByte(zTilde, i);
        zTilde_i <<= 1;

        uint8_t p_i = p >> i & 0x1;

        if(k[i]) {
            k[i] |= ~zTilde_i & 0x7E;
            k[i] |= p_i & 1;
            k[i] += 1;
        } else {
            k[i] |= zTilde_i & 0x7E;
            k[i] |= (~p_i) & 1;
        }
    }
}

void loclass_diversifyKey(uint8_t* csn, const uint8_t* key, uint8_t* div_key) {
    standalone_des_context ctx;
    standalone_des_setkey_enc(&ctx, key);

    uint8_t crypted_csn[8] = {0};
    standalone_des_crypt_ecb(&ctx, csn, crypted_csn);

    uint64_t c_csn = loclass_x_bytes_to_num(crypted_csn, sizeof(crypted_csn));
    loclass_hash0(c_csn, div_key);
}
