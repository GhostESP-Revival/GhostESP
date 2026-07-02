// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
//
// Minimal standalone DES/3DES ECB implementation for iCLASS crypto.
// Used by loclass library when mbedtls DES is not available (CONFIG_MBEDTLS_DES_C=n).
//
// Based on public-domain DES reference code.
// This is ONLY used for iCLASS key diversification and PACS credential decryption.

#ifndef STANDALONE_DES_H
#define STANDALONE_DES_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sk[32]; /* DES subkeys */
} standalone_des_context;

typedef struct {
    standalone_des_context ctx_enc;
    standalone_des_context ctx_dec;
} standalone_des3_context;

void standalone_des_setkey_enc(standalone_des_context *ctx, const uint8_t key[8]);
void standalone_des_setkey_dec(standalone_des_context *ctx, const uint8_t key[8]);
void standalone_des_crypt_ecb(standalone_des_context *ctx, const uint8_t input[8], uint8_t output[8]);

void standalone_des3_set2key_enc(standalone_des3_context *ctx, const uint8_t key[16]);
void standalone_des3_set2key_dec(standalone_des3_context *ctx, const uint8_t key[16]);
void standalone_des3_crypt_ecb(standalone_des3_context *ctx, const uint8_t input[8], uint8_t output[8]);

#ifdef __cplusplus
}
#endif

#endif // STANDALONE_DES_H
