// meshcore_identity.c
// Ed25519 identity backed by NVS. See meshcore_identity.h.

#include "managers/meshcore_config.h"
#include "sdkconfig.h"

#ifdef CONFIG_HAS_MESHCORE

#include "managers/meshcore_identity.h"

#include <string.h>

#include "ed_25519.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

#define MC_NVS_NAMESPACE "meshcore"
#define MC_NVS_KEY_PRV   "ed_prv"
#define MC_NVS_KEY_PUB   "ed_pub"

static mc_identity_t s_id;

static bool nvs_open_id(nvs_open_mode_t mode, nvs_handle_t *out) {
    if (nvs_open(MC_NVS_NAMESPACE, mode, out) != ESP_OK) return false;
    return true;
}

static bool load_from_nvs(mc_identity_t *id) {
    nvs_handle_t h;
    if (!nvs_open_id(NVS_READONLY, &h)) return false;

    size_t prv_len = sizeof(id->prv_key);
    size_t pub_len = sizeof(id->pub_key);
    esp_err_t r1 = nvs_get_blob(h, MC_NVS_KEY_PRV, id->prv_key, &prv_len);
    esp_err_t r2 = nvs_get_blob(h, MC_NVS_KEY_PUB, id->pub_key, &pub_len);
    nvs_close(h);

    if (r1 != ESP_OK || r2 != ESP_OK) return false;
    if (prv_len != sizeof(id->prv_key) || pub_len != sizeof(id->pub_key)) return false;
    id->valid = true;
    return true;
}

static void save_to_nvs(const mc_identity_t *id) {
    nvs_handle_t h;
    if (!nvs_open_id(NVS_READWRITE, &h)) return;
    nvs_set_blob(h, MC_NVS_KEY_PRV, id->prv_key, sizeof(id->prv_key));
    nvs_set_blob(h, MC_NVS_KEY_PUB, id->pub_key, sizeof(id->pub_key));
    nvs_commit(h);
    nvs_close(h);
}

static void generate(mc_identity_t *id) {
    uint8_t seed[MC_SEED_SIZE];
    esp_fill_random(seed, sizeof(seed));
    ed25519_create_keypair(id->pub_key, id->prv_key, seed);
    memset(seed, 0, sizeof(seed));
    id->valid = true;
}

void mc_identity_init(void) {
    if (s_id.valid) return;
    if (load_from_nvs(&s_id)) return;
    generate(&s_id);
    save_to_nvs(&s_id);
}

bool mc_identity_ready(void) {
    return s_id.valid;
}

const mc_identity_t *mc_identity_get(void) {
    return &s_id;
}

bool mc_identity_regen(void) {
    generate(&s_id);
    save_to_nvs(&s_id);
    return true;
}

bool mc_identity_import(const uint8_t prv_key[64]) {
    if (!prv_key) return false;
    memcpy(s_id.prv_key, prv_key, MC_PRV_KEY_SIZE);
    ed25519_derive_pub(s_id.pub_key, s_id.prv_key);
    s_id.valid = true;
    save_to_nvs(&s_id);
    return true;
}

bool mc_identity_export(uint8_t prv_key[64]) {
    if (!s_id.valid || !prv_key) return false;
    memcpy(prv_key, s_id.prv_key, MC_PRV_KEY_SIZE);
    return true;
}

void mc_identity_sign(const uint8_t *msg, size_t msg_len, uint8_t sig[64]) {
    if (!s_id.valid) {
        memset(sig, 0, MC_SIGNATURE_SIZE);
        return;
    }
    ed25519_sign(sig, msg, msg_len, s_id.pub_key, s_id.prv_key);
}

bool mc_identity_verify(const uint8_t *pub_key, const uint8_t *msg, size_t msg_len,
                        const uint8_t sig[64]) {
    if (!pub_key || !sig) return false;
    return ed25519_verify(sig, msg, msg_len, pub_key) != 0;
}

void mc_identity_shared_secret(const uint8_t *peer_pub_key, uint8_t secret[32]) {
    if (!s_id.valid || !peer_pub_key) {
        memset(secret, 0, 32);
        return;
    }
    ed25519_key_exchange(secret, peer_pub_key, s_id.prv_key);
}

uint8_t mc_identity_node_hash(void) {
    return s_id.pub_key[0];
}

bool mc_identity_validate_private(const uint8_t prv_key[64]) {
    uint8_t pub[32];
    ed25519_derive_pub(pub, prv_key); // derive public key from given private key

    // Upstream disallows 0x00 or 0xFF prefixed public keys.
    if (pub[0] == 0x00 || pub[0] == 0xFF) return false;

    // Known-good upstream test client keypair.
    static const uint8_t test_client_prv[64] = {
        0x70, 0x65, 0xe1, 0x8f, 0xd9, 0xfa, 0xbb, 0x70,
        0xc1, 0xed, 0x90, 0xdc, 0xa1, 0x99, 0x07, 0xde,
        0x69, 0x8c, 0x88, 0xb7, 0x09, 0xea, 0x14, 0x6e,
        0xaf, 0xd9, 0x3d, 0x9b, 0x83, 0x0c, 0x7b, 0x60,
        0xc4, 0x68, 0x11, 0x93, 0xc7, 0x9b, 0xbc, 0x39,
        0x94, 0x5b, 0xa8, 0x06, 0x41, 0x04, 0xbb, 0x61,
        0x8f, 0x8f, 0xd7, 0xa8, 0x4a, 0x0a, 0xf6, 0xf5,
        0x70, 0x33, 0xd6, 0xe8, 0xdd, 0xcd, 0x64, 0x71
    };
    static const uint8_t test_client_pub[32] = {
        0x1e, 0xc7, 0x71, 0x75, 0xb0, 0x91, 0x8e, 0xd2,
        0x06, 0xf9, 0xae, 0x04, 0xec, 0x13, 0x6d, 0x6d,
        0x5d, 0x43, 0x15, 0xbb, 0x26, 0x30, 0x54, 0x27,
        0xf6, 0x45, 0xb4, 0x92, 0xe9, 0x35, 0x0c, 0x10
    };

    uint8_t ss1[32], ss2[32];
    ed25519_key_exchange(ss1, test_client_pub, prv_key);
    ed25519_key_exchange(ss2, pub, test_client_prv);

    if (memcmp(ss1, ss2, 32) != 0) return false;

    for (int i = 0; i < 32; ++i) {
        if (ss1[i] != 0) return true;
    }
    return false;
}

#else
typedef int meshcore_identity_stub_guard;
#endif // CONFIG_HAS_MESHCORE
