// meshcore_store.h
// Persistent MeshCore state: prefs, contacts and channels.
//
// Contacts/channels are plain fixed-size structs so they can be written to a
// single NVS blob. The first MC_MAX_ANON_CONTACTS slots are runtime-only
// anonymous entries and are not persisted.

#ifndef MESHCORE_STORE_H
#define MESHCORE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "managers/meshcore_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t pub_key[MC_PUB_KEY_SIZE];
    char name[32];
    uint8_t type;  // MC_ADV_TYPE_*
    uint8_t flags;
    uint8_t out_path_len;
    uint8_t out_path[MC_MAX_PATH_SIZE];
    uint32_t last_advert_timestamp; // by their clock
    uint32_t lastmod;               // by our clock
    int32_t gps_lat, gps_lon;       // 6 decimal places
    uint32_t sync_since;
    uint8_t shared_secret[MC_PUB_KEY_SIZE];
    bool shared_secret_valid;
} mc_contact_t;

typedef struct {
    char name[32];
    uint8_t secret[MC_PUB_KEY_SIZE];
    uint8_t hash;
} mc_channel_t;

typedef struct {
    float freq_mhz;
    float bw_khz;
    uint8_t sf;
    uint8_t cr;
    int8_t tx_dbm;
    char node_name[32];
    int32_t node_lat, node_lon; // 6 decimal places
    uint8_t advert_loc_policy;
    uint8_t multi_acks;
    uint8_t manual_add_contacts;
    uint8_t autoadd_config;
    uint8_t autoadd_max_hops;
    uint8_t path_hash_mode;
    uint32_t ble_pin;
    bool radio_configured;
    // Region scoping (upstream NodePrefs::default_scope_name/default_scope_key).
    // Appended so older stored blobs upgrade via the prefix copy in load_prefs.
    char default_scope_name[31];
    uint8_t default_scope_key[16];
    // Opt-in packet forwarding (upstream NodePrefs::repeat). Companion default 0.
    uint8_t repeat;
} mc_prefs_t;

void mc_store_defaults(mc_prefs_t *prefs);
bool mc_store_load_prefs(mc_prefs_t *prefs);
bool mc_store_save_prefs(const mc_prefs_t *prefs);

// Contacts: only the real (non-anon) contacts are persisted.
bool mc_store_load_contacts(mc_contact_t *arr, int *count, int max);
bool mc_store_save_contacts(const mc_contact_t *arr, int count);
void mc_store_erase_contacts(void);

// Channels: exactly MC_MAX_GROUP_CHANNELS slots.
bool mc_store_load_channels(mc_channel_t *arr);
bool mc_store_save_channels(const mc_channel_t *arr);

// Raw auxiliary NVS blobs (e.g. the advert blob cache). The caller owns the
// key namespace and layout.
bool mc_store_save_blob(const char *key, const void *data, size_t len);
bool mc_store_load_blob(const char *key, void *out, size_t max, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif // MESHCORE_STORE_H
