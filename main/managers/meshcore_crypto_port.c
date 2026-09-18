// meshcore_crypto_port.c
// Crypto primitives for the MeshCore composition.
//
// ESP-IDF v6.1 ships mbedTLS 4.x, where the legacy AES/SHA headers moved to
// mbedtls/private/ (the same headers lora_crypto.c / lora_pki.c use):
//   mbedtls/private/aes.h, mbedtls/private/sha256.h
// HMAC-SHA256 is composed from SHA-256 directly, avoiding the removed md API.

#include "managers/meshcore_config.h"
#include "sdkconfig.h"

#ifdef CONFIG_HAS_MESHCORE

#include "managers/meshcore_crypto.h"

#include <string.h>

// TF-PSA (mbedTLS 4.x) hides the private AES/SHA declarations unless this is
// defined before the headers; same pattern as lora_crypto.c / lora_pki.c.
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/private/aes.h"
#include "mbedtls/private/sha256.h"

static void mc_sha256_full(const uint8_t *msg, size_t msg_len, uint8_t out[32]) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    if (msg && msg_len) mbedtls_sha256_update(&ctx, msg, msg_len);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

void mc_sha256(uint8_t *out, size_t out_len, const uint8_t *msg, size_t msg_len) {
    uint8_t full[32];
    mc_sha256_full(msg, msg_len, full);
    if (out_len > sizeof(full)) out_len = sizeof(full);
    memcpy(out, full, out_len);
}

void mc_sha256_2(uint8_t *out, size_t out_len,
                 const uint8_t *frag1, size_t frag1_len,
                 const uint8_t *frag2, size_t frag2_len) {
    uint8_t full[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    if (frag1 && frag1_len) mbedtls_sha256_update(&ctx, frag1, frag1_len);
    if (frag2 && frag2_len) mbedtls_sha256_update(&ctx, frag2, frag2_len);
    mbedtls_sha256_finish(&ctx, full);
    mbedtls_sha256_free(&ctx);
    if (out_len > sizeof(full)) out_len = sizeof(full);
    memcpy(out, full, out_len);
}

void mc_hmac_sha256(uint8_t *out, size_t out_len,
                    const uint8_t *key, size_t key_len,
                    const uint8_t *msg, size_t msg_len) {
    uint8_t k[64];
    uint8_t ipad[64], opad[64];
    uint8_t inner[32];
    uint8_t full[32];

    memset(k, 0, sizeof(k));
    if (key_len > sizeof(k)) {
        mc_sha256_full(key, key_len, k); // long keys are hashed first
    } else if (key) {
        memcpy(k, key, key_len);
    }
    for (int i = 0; i < 64; ++i) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, ipad, sizeof(ipad));
    if (msg && msg_len) mbedtls_sha256_update(&ctx, msg, msg_len);
    mbedtls_sha256_finish(&ctx, inner);
    mbedtls_sha256_free(&ctx);

    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, opad, sizeof(opad));
    mbedtls_sha256_update(&ctx, inner, sizeof(inner));
    mbedtls_sha256_finish(&ctx, full);
    mbedtls_sha256_free(&ctx);

    if (out_len > sizeof(full)) out_len = sizeof(full);
    memcpy(out, full, out_len);
}

void mc_aes128_ecb_encrypt_block(const uint8_t key[16], uint8_t out[16], const uint8_t in[16]) {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, key, 128);
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, out);
    mbedtls_aes_free(&ctx);
}

void mc_aes128_ecb_decrypt_block(const uint8_t key[16], uint8_t out[16], const uint8_t in[16]) {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_dec(&ctx, key, 128);
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, in, out);
    mbedtls_aes_free(&ctx);
}

#else
typedef int meshcore_crypto_port_stub_guard;
#endif // CONFIG_HAS_MESHCORE
