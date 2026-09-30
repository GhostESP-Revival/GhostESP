// meshcore_crypto.c
// MeshCore crypto composition. See meshcore_crypto.h and src/Utils.cpp.
//
// This file intentionally contains no platform crypto calls so that the
// composition can be validated natively against the upstream firmware.

#include "managers/meshcore_crypto.h"
#include "managers/meshcore_config.h"
#include "sdkconfig.h"

#include <string.h>

#ifdef CONFIG_HAS_MESHCORE

const uint8_t mc_public_channel_key[16] = {
    0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
    0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72,
};

int mc_encrypt(const uint8_t *shared_secret, uint8_t *dest, const uint8_t *src, int src_len) {
    if (!shared_secret || !dest || !src || src_len < 0) return 0;

    uint8_t *dp = dest;
    const uint8_t *sp = src;

    while (src_len >= MC_CIPHER_BLOCK_SIZE) {
        mc_aes128_ecb_encrypt_block(shared_secret, dp, sp);
        dp += MC_CIPHER_BLOCK_SIZE;
        sp += MC_CIPHER_BLOCK_SIZE;
        src_len -= MC_CIPHER_BLOCK_SIZE;
    }
    if (src_len > 0) {
        uint8_t tmp[MC_CIPHER_BLOCK_SIZE];
        memset(tmp, 0, sizeof(tmp));
        memcpy(tmp, sp, (size_t)src_len);
        mc_aes128_ecb_encrypt_block(shared_secret, dp, tmp);
        dp += MC_CIPHER_BLOCK_SIZE;
    }
    return (int)(dp - dest);
}

int mc_decrypt(const uint8_t *shared_secret, uint8_t *dest, const uint8_t *src, int src_len) {
    if (!shared_secret || !dest || !src || src_len <= 0) return 0;

    uint8_t *dp = dest;
    const uint8_t *sp = src;
    int remaining = src_len;

    while (remaining >= MC_CIPHER_BLOCK_SIZE) {
        mc_aes128_ecb_decrypt_block(shared_secret, dp, sp);
        dp += MC_CIPHER_BLOCK_SIZE;
        sp += MC_CIPHER_BLOCK_SIZE;
        remaining -= MC_CIPHER_BLOCK_SIZE;
    }
    return (int)(dp - dest);
}

int mc_encrypt_then_mac(const uint8_t *shared_secret, uint8_t *dest, const uint8_t *src, int src_len) {
    int enc_len = mc_encrypt(shared_secret, dest + MC_CIPHER_MAC_SIZE, src, src_len);
    // 2-byte truncated HMAC-SHA256 keyed with the full 32-byte shared secret,
    // computed over the ciphertext.
    mc_hmac_sha256(dest, MC_CIPHER_MAC_SIZE, shared_secret, MC_PUB_KEY_SIZE,
                   dest + MC_CIPHER_MAC_SIZE, (size_t)enc_len);
    return MC_CIPHER_MAC_SIZE + enc_len;
}

int mc_mac_then_decrypt(const uint8_t *shared_secret, uint8_t *dest, const uint8_t *src, int src_len) {
    if (src_len <= MC_CIPHER_MAC_SIZE) return 0;

    uint8_t hmac[MC_CIPHER_MAC_SIZE];
    mc_hmac_sha256(hmac, MC_CIPHER_MAC_SIZE, shared_secret, MC_PUB_KEY_SIZE,
                   src + MC_CIPHER_MAC_SIZE, (size_t)(src_len - MC_CIPHER_MAC_SIZE));
    if (memcmp(hmac, src, MC_CIPHER_MAC_SIZE) != 0) {
        return 0; // invalid HMAC
    }
    return mc_decrypt(shared_secret, dest, src + MC_CIPHER_MAC_SIZE, src_len - MC_CIPHER_MAC_SIZE);
}

void mc_calc_channel_hash(uint8_t *out, const uint8_t *key, size_t key_len) {
    if (!out) return;
    mc_sha256(out, 1, key, key_len);
}

bool mc_hashtag_channel_key(uint8_t out16[16], const char *name) {
    if (!name || name[0] != '#') return false;
    if (name[1] == '\0') return false;
    // Upstream derives the key from the channel name exactly as typed,
    // including the leading '#'. sha256(name) truncated to 16 bytes.
    mc_sha256(out16, 16, (const uint8_t *)name, strlen(name));
    return true;
}

void mc_to_hex(char *dest, const uint8_t *src, size_t len) {
    static const char hex_chars[] = "0123456789ABCDEF";
    if (!dest) return;
    for (size_t i = 0; i < len; ++i) {
        dest[i * 2] = hex_chars[src[i] >> 4];
        dest[i * 2 + 1] = hex_chars[src[i] & 0x0F];
    }
    dest[len * 2] = '\0';
}

static int mc_hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool mc_from_hex(uint8_t *dest, size_t dest_size, const char *src_hex) {
    if (!dest || !src_hex) return false;
    size_t len = strlen(src_hex);
    if (len != dest_size * 2) return false;
    for (size_t i = 0; i < dest_size; ++i) {
        int hi = mc_hex_val(src_hex[i * 2]);
        int lo = mc_hex_val(src_hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        dest[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

#else
typedef int meshcore_crypto_stub_guard;
#endif // CONFIG_HAS_MESHCORE
