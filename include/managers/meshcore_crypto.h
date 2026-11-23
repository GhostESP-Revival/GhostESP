// meshcore_crypto.h
// MeshCore crypto composition (AES-128-ECB + truncated HMAC-SHA256 + SHA-256).
//
// Verified against meshcore/firmware v1.17.1 src/Utils.cpp:
//   encryptThenMAC: AES128(key=shared_secret[0:16], zero-padded) then
//                   HMAC-SHA256(key=shared_secret[0:32], ciphertext)[0:2] prepended
//   MACThenDecrypt: recompute HMAC over ciphertext, compare, then decrypt
//   sha256:         SHA-256 truncated to the requested output length
//
// The low-level primitives (SHA-256, HMAC-SHA256, AES-128-ECB block) live in
// meshcore_crypto_port.c so the composition can be unit-tested natively.

#ifndef MESHCORE_CRYPTO_H
#define MESHCORE_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Primitives (implemented by meshcore_crypto_port.c) ----
// SHA-256 of msg, truncated into out[0..out_len).
void mc_sha256(uint8_t *out, size_t out_len, const uint8_t *msg, size_t msg_len);
// SHA-256 of frag1 || frag2, truncated into out[0..out_len).
void mc_sha256_2(uint8_t *out, size_t out_len,
                 const uint8_t *frag1, size_t frag1_len,
                 const uint8_t *frag2, size_t frag2_len);
// HMAC-SHA256 of msg with the given key, truncated into out[0..out_len).
void mc_hmac_sha256(uint8_t *out, size_t out_len,
                    const uint8_t *key, size_t key_len,
                    const uint8_t *msg, size_t msg_len);
// Single AES-128-ECB block (16 bytes in, 16 bytes out).
void mc_aes128_ecb_encrypt_block(const uint8_t key[16], uint8_t out[16], const uint8_t in[16]);
void mc_aes128_ecb_decrypt_block(const uint8_t key[16], uint8_t out[16], const uint8_t in[16]);

// ---- Composition (meshcore_crypto.c) ----

// AES-128-ECB over src, zero-padding the final partial block.
// Returns the number of bytes written to dest (a multiple of 16).
int mc_encrypt(const uint8_t *shared_secret, uint8_t *dest, const uint8_t *src, int src_len);

// AES-128-ECB over src (src_len must be a multiple of 16).
int mc_decrypt(const uint8_t *shared_secret, uint8_t *dest, const uint8_t *src, int src_len);

// encrypt then prepend MAC. Returns MC_CIPHER_MAC_SIZE + encrypted length.
int mc_encrypt_then_mac(const uint8_t *shared_secret, uint8_t *dest, const uint8_t *src, int src_len);

// Verify the leading MAC then decrypt the remainder.
// Returns 0 when the MAC is invalid, else the decrypted length.
int mc_mac_then_decrypt(const uint8_t *shared_secret, uint8_t *dest, const uint8_t *src, int src_len);

// Hash a channel key: out[0] = SHA256(key)[0]. key_len is normally 16 (128-bit
// keys) or 32 (legacy 256-bit keys).
void mc_calc_channel_hash(uint8_t *out, const uint8_t *key, size_t key_len);

// Hashtag channel key: first 16 bytes of SHA256("#name"). Returns false when
// the name is empty or does not start with '#'.
bool mc_hashtag_channel_key(uint8_t out16[16], const char *name);

// The publicly-known default channel key.
extern const uint8_t mc_public_channel_key[16];

// Hex helpers (NUL-terminated output; dest must be len*2+1).
void mc_to_hex(char *dest, const uint8_t *src, size_t len);
bool mc_from_hex(uint8_t *dest, size_t dest_size, const char *src_hex);

#ifdef __cplusplus
}
#endif

#endif // MESHCORE_CRYPTO_H
