// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
//
// Minimal standalone DES/3DES ECB implementation for iCLASS crypto.
// Used by loclass library when mbedtls DES is not available (CONFIG_MBEDTLS_DES_C=n).
//
// Based on public-domain DES reference code.

#include "managers/nfc/loclass/standalone_des.h"

/* Initial Permutation Table */
static const uint8_t IP[64] = {
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};

/* Final Permutation Table */
static const uint8_t FP[64] = {
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25};

/* Expansion Table */
static const uint8_t E[48] = {
    32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9,
    8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25,
    24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};

/* S-Boxes */
static const uint8_t S[8][64] = {{
    14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7,
    0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
    4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0,
    15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13
}, {
    15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10,
    3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
    0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15,
    13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9
}, {
    10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8,
    13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
    13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7,
    1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12
}, {
    7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15,
    13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
    10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4,
    3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14
}, {
    2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9,
    14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
    4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14,
    11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3
}, {
    12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11,
    10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
    9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6,
    4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13
}, {
    4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1,
    13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
    1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2,
    6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12
}, {
    13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7,
    1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 2, 0, 14, 9, 11,
    7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8,
    2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11
}};

/* Permutation Table */
static const uint8_t P[32] = {
    16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
    2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25};

/* Permuted Choice 1 (key schedule) */
static const uint8_t PC1[56] = {
    57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18,
    10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22,
    14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4};

/* Permuted Choice 2 (key schedule) */
static const uint8_t PC2[48] = {
    14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10,
    23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
    41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48,
    44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};

/* Number of left shifts per round */
static const uint8_t SHIFTS[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};

static uint32_t des_left_rotate_28(uint32_t val, uint8_t n) {
    return ((val << n) | (val >> (28 - n))) & 0x0FFFFFFF;
}

static void des_key_schedule(const uint8_t key[8], uint32_t subkeys[32]) {
    /* PC1: 64-bit key -> 56-bit */
    uint32_t C = 0, D = 0;
    for (int i = 0; i < 28; i++) {
        C |= ((key[(PC1[i] - 1) / 8] >> (7 - (PC1[i] - 1) % 8)) & 1) << (27 - i);
        D |= ((key[(PC1[i + 28] - 1) / 8] >> (7 - (PC1[i + 28] - 1) % 8)) & 1) << (27 - i);
    }

    for (int round = 0; round < 16; round++) {
        C = des_left_rotate_28(C, SHIFTS[round]);
        D = des_left_rotate_28(D, SHIFTS[round]);

        uint32_t CD = (C << 28) | D;
        uint32_t K = 0;
        for (int i = 0; i < 48; i++) {
            K |= ((CD >> (56 - PC2[i])) & 1) << (47 - i);
        }
        subkeys[round * 2] = (K >> 24) & 0x00FFFFFF;
        subkeys[round * 2 + 1] = K & 0x00FFFFFF;
    }
}

static uint32_t des_f(uint32_t R, uint32_t subkey0, uint32_t subkey1) {
    /* Expansion: 32 -> 48 bits */
    uint64_t expanded = 0;
    for (int i = 0; i < 48; i++) {
        expanded |= ((uint64_t)((R >> (32 - E[i])) & 1)) << (47 - i);
    }

    /* XOR with subkey */
    uint32_t sk0 = subkey0;
    uint32_t sk1 = subkey1;
    uint64_t subkey = ((uint64_t)sk0 << 24) | sk1;
    expanded ^= subkey;

    /* S-box substitution: 48 -> 32 bits */
    uint32_t out = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t six_bits = (expanded >> (42 - 6 * i)) & 0x3F;
        uint8_t row = ((six_bits & 0x20) >> 4) | (six_bits & 0x01);
        uint8_t col = (six_bits >> 1) & 0x0F;
        out |= (uint32_t)S[i][row * 16 + col] << (28 - 4 * i);
    }

    /* Permutation P */
    uint32_t result = 0;
    for (int i = 0; i < 32; i++) {
        result |= ((out >> (32 - P[i])) & 1) << (31 - i);
    }
    return result;
}

static void des_process_block(const uint32_t subkeys[32], const uint8_t input[8], uint8_t output[8]) {
    /* Initial Permutation */
    uint64_t block = 0;
    for (int i = 0; i < 64; i++) {
        block |= ((uint64_t)((input[(IP[i] - 1) / 8] >> (7 - (IP[i] - 1) % 8)) & 1)) << (63 - i);
    }

    uint32_t L = (uint32_t)(block >> 32);
    uint32_t R = (uint32_t)(block & 0xFFFFFFFF);

    /* 16 rounds */
    for (int round = 0; round < 16; round++) {
        uint32_t tmp = R;
        R = L ^ des_f(R, subkeys[round * 2], subkeys[round * 2 + 1]);
        L = tmp;
    }

    /* Pre-output: swap L and R */
    uint64_t preout = ((uint64_t)R << 32) | L;

    /* Final Permutation */
    uint64_t result = 0;
    for (int i = 0; i < 64; i++) {
        result |= ((preout >> (64 - FP[i])) & 1) << (63 - i);
    }

    for (int i = 0; i < 8; i++) {
        output[i] = (uint8_t)(result >> (56 - 8 * i));
    }
}

void standalone_des_setkey_enc(standalone_des_context *ctx, const uint8_t key[8]) {
    des_key_schedule(key, ctx->sk);
}

void standalone_des_setkey_dec(standalone_des_context *ctx, const uint8_t key[8]) {
    uint32_t tmp[32];
    des_key_schedule(key, tmp);
    /* Reverse the subkeys for decryption */
    for (int i = 0; i < 16; i++) {
        ctx->sk[i * 2] = tmp[(15 - i) * 2];
        ctx->sk[i * 2 + 1] = tmp[(15 - i) * 2 + 1];
    }
}

void standalone_des_crypt_ecb(standalone_des_context *ctx, const uint8_t input[8], uint8_t output[8]) {
    des_process_block(ctx->sk, input, output);
}

void standalone_des3_set2key_enc(standalone_des3_context *ctx, const uint8_t key[16]) {
    /* 2-key 3DES: Ek1(Dk2(Ek1(x))) */
    standalone_des_setkey_enc(&ctx->ctx_enc, key);
    standalone_des_setkey_dec(&ctx->ctx_dec, key + 8);
}

void standalone_des3_set2key_dec(standalone_des3_context *ctx, const uint8_t key[16]) {
    /* 2-key 3DES decrypt: Dk1(Ek2(Dk1(x))) */
    standalone_des_setkey_dec(&ctx->ctx_enc, key);
    standalone_des_setkey_enc(&ctx->ctx_dec, key + 8);
}

void standalone_des3_crypt_ecb(standalone_des3_context *ctx, const uint8_t input[8], uint8_t output[8]) {
    uint8_t tmp[8];
    standalone_des_crypt_ecb(&ctx->ctx_enc, input, tmp);
    standalone_des_crypt_ecb(&ctx->ctx_dec, tmp, tmp);
    standalone_des_crypt_ecb(&ctx->ctx_enc, tmp, output);
}
