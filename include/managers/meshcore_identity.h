// meshcore_identity.h
// Ed25519 node identity for GhostESP's MeshCore stack.
//
// MeshCore identity semantics (verified against firmware v1.17.1
// src/Identity.cpp): a 32-byte Ed25519 public key identifies a node, the
// "node hash" is pub_key[0], signatures are Ed25519 (64 bytes), and ECDH is
// ed25519_key_exchange (X25519 over the birationally equivalent curve) using
// the SHA-512-expanded private key.

#ifndef MESHCORE_IDENTITY_H
#define MESHCORE_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t pub_key[32];
    uint8_t prv_key[64]; // SHA-512(seed) with Ed25519 clamping applied
    bool valid;
} mc_identity_t;

// Load the persisted keypair, generating one from the hardware RNG the first
// time. Safe to call more than once.
void mc_identity_init(void);
bool mc_identity_ready(void);
const mc_identity_t *mc_identity_get(void);
// Regenerate and persist a fresh keypair.
bool mc_identity_regen(void);
// Restore a keypair from an expanded 64-byte private key (companion import).
bool mc_identity_import(const uint8_t prv_key[64]);
// Export the expanded 64-byte private key.
bool mc_identity_export(uint8_t prv_key[64]);

// Ed25519 sign/verify.
void mc_identity_sign(const uint8_t *msg, size_t msg_len, uint8_t sig[64]);
bool mc_identity_verify(const uint8_t *pub_key, const uint8_t *msg, size_t msg_len,
                        const uint8_t sig[64]);

// ECDH shared secret with a peer Ed25519 public key (32-byte output).
void mc_identity_shared_secret(const uint8_t *peer_pub_key, uint8_t secret[32]);

// First byte of our Ed25519 public key.
uint8_t mc_identity_node_hash(void);

// Validate an expanded private key the same way upstream does (rejects 0x00 /
// 0xFF public-key prefixes and all-zero shared secrets).
bool mc_identity_validate_private(const uint8_t prv_key[64]);

#ifdef __cplusplus
}
#endif

#endif // MESHCORE_IDENTITY_H
