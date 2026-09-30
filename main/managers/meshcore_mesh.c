// meshcore_mesh.c
// MeshCore companion mesh logic. See meshcore_mesh.h.
//
// Behaviour mirrors firmware v1.17.1 src/Mesh.cpp + src/helpers/BaseChatMesh.cpp
// for the companion (non-repeating) role.

#include "managers/meshcore_config.h"
#include "sdkconfig.h"

#ifdef CONFIG_HAS_MESHCORE

#include "managers/meshcore_mesh.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "esp_random.h"
#include "managers/meshcore_alloc.h"
#include "managers/meshcore_crypto.h"
#include "managers/meshcore_identity.h"
#include "managers/meshcore_packet.h"
#include "managers/meshcore_store.h"

#define MC_MAX_TEXT_LEN (10 * MC_CIPHER_BLOCK_SIZE) // 160
#define MC_SEND_TIMEOUT_BASE_MS        500
#define MC_FLOOD_SEND_TIMEOUT_FACTOR   16.0f
#define MC_DIRECT_SEND_PERHOP_FACTOR   6.0f
#define MC_DIRECT_SEND_PERHOP_EXTRA_MS 250

typedef struct {
    uint8_t pubkey_prefix[7];
    uint8_t path_len;
    char name[32];
    uint32_t recv_timestamp;
    uint8_t path[MC_MAX_PATH_SIZE];
} mc_advert_path_t;

#define MC_CONTACT_SLOTS     (MC_MAX_ANON_CONTACTS + MC_MAX_CONTACTS)
#define MC_ADVERT_PATH_SLOTS 16

// Large state tables are allocated on demand (mc_mesh_init/mc_mesh_deinit) and
// prefer PSRAM when the target has it, so a stopped stack costs no RAM and a
// PSRAM board keeps internal heap free for the radio/BLE buffers.
static mc_contact_t *s_contacts;   // MC_CONTACT_SLOTS
static int s_num_contacts;
static mc_channel_t *s_channels;   // MC_MAX_GROUP_CHANNELS
static mc_prefs_t s_prefs;
static bool s_prefs_loaded;
static mc_mesh_callbacks_t s_cb;
static mc_tx_fn s_tx;
static void *s_tx_ctx;
static mc_mesh_stats_t s_stats;
static bool s_inited;

static uint8_t *s_dedup;           // MC_DEDUP_HASHES * MC_MAX_HASH_SIZE
static int s_dedup_next;

// Small recent-advert blob cache (for contact share/export). Bounded to keep
// RAM low on no-PSRAM boards; older entries are evicted round-robin.
#define MC_ADVERT_BLOB_CACHE 8
typedef struct {
    uint8_t pub[MC_PUB_KEY_SIZE];
    uint8_t len;
    uint8_t blob[MC_MAX_TRANS_UNIT];
} mc_advert_blob_t;
static mc_advert_blob_t *s_blob_cache;  // MC_ADVERT_BLOB_CACHE
static int s_blob_next;

static void blob_cache_put(const uint8_t *pub, const uint8_t *blob, uint8_t len) {
    /* len is uint8_t and MC_MAX_TRANS_UNIT is 255, so it always fits. */
    if (!s_blob_cache || !pub || !blob || len == 0) return;
    int slot = -1;
    for (int i = 0; i < MC_ADVERT_BLOB_CACHE; ++i) {
        if (memcmp(s_blob_cache[i].pub, pub, MC_PUB_KEY_SIZE) == 0) { slot = i; break; }
    }
    if (slot < 0) {
        slot = s_blob_next;
        s_blob_next = (s_blob_next + 1) % MC_ADVERT_BLOB_CACHE;
    }
    // Skip the NVS write when nothing changed (busy networks re-advert often).
    if (s_blob_cache[slot].len == len &&
        memcmp(s_blob_cache[slot].pub, pub, MC_PUB_KEY_SIZE) == 0 &&
        memcmp(s_blob_cache[slot].blob, blob, len) == 0) {
        return;
    }
    memcpy(s_blob_cache[slot].pub, pub, MC_PUB_KEY_SIZE);
    memcpy(s_blob_cache[slot].blob, blob, len);
    s_blob_cache[slot].len = len;
    // Persist like upstream adv_blobs, so share/export survives a reboot.
    (void)mc_store_save_blob(MC_BLOB_KEY_ADVERT, s_blob_cache,
                             MC_ADVERT_BLOB_CACHE * sizeof(mc_advert_blob_t));
}

static const mc_advert_blob_t *blob_cache_get(const uint8_t *pub) {
    if (!s_blob_cache) return NULL;
    for (int i = 0; i < MC_ADVERT_BLOB_CACHE; ++i) {
        if (s_blob_cache[i].len && memcmp(s_blob_cache[i].pub, pub, MC_PUB_KEY_SIZE) == 0) {
            return &s_blob_cache[i];
        }
    }
    return NULL;
}

static mc_advert_path_t *s_advert_paths;  // MC_ADVERT_PATH_SLOTS
static int s_advert_path_next;

// True once mc_mesh_init() has allocated every state table.
static bool mesh_allocated(void) {
    return s_contacts && s_channels && s_dedup && s_blob_cache && s_advert_paths;
}

// True once the tables are allocated *and* loaded.
static bool mesh_ready(void) {
    return s_inited && mesh_allocated();
}

// Expected-ACK tags for direct messages we sent (upstream txt_send_timeout).
// Matched by onAckRecv to cancel waiting and, when the ACK arrives over flood
// while we already know a direct path, to send a reciprocal path.
#define MC_PENDING_ACKS 8
typedef struct {
    bool used;
    uint32_t ack;
    uint32_t expire_ms;
    uint8_t pub[MC_PUB_KEY_SIZE];
} mc_pending_ack_t;
static mc_pending_ack_t s_pending_acks[MC_PENDING_ACKS];
static int s_pending_ack_next;

static void pending_ack_add(uint32_t ack, const uint8_t *pub) {
    if (ack == 0 || !pub) return;
    mc_pending_ack_t *p = &s_pending_acks[s_pending_ack_next];
    p->used = true;
    p->ack = ack;
    p->expire_ms = (uint32_t)(esp_timer_get_time() / 1000) + MC_PENDING_ACK_TTL_MS;
    memcpy(p->pub, pub, MC_PUB_KEY_SIZE);
    s_pending_ack_next = (s_pending_ack_next + 1) % MC_PENDING_ACKS;
}

// Find and clear the pending entry for an ACK (expired entries are dropped).
// Returns the matching contact, or NULL.
static mc_contact_t *pending_ack_take(uint32_t ack) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    for (int i = 0; i < MC_PENDING_ACKS; ++i) {
        mc_pending_ack_t *p = &s_pending_acks[i];
        if (!p->used) continue;
        if ((int32_t)(now - p->expire_ms) >= 0) { p->used = false; continue; }
        if (p->ack == ack) {
            p->used = false;
            return mc_mesh_find_contact_pubkey(p->pub, MC_PUB_KEY_SIZE);
        }
    }
    return NULL;
}

static uint32_t s_time_base; // epoch seconds captured at mc_mesh_set_time()
static int64_t s_time_base_us;

// Region scoping overrides set by the app (upstream send_scope/send_unscoped).
static uint8_t s_send_scope[16];
static bool s_send_scope_set;
static bool s_send_unscoped;

// Opt-in forwarding (upstream Mesh::allowPacketForward when repeat is enabled).
// One pending retransmit slot serviced by an esp_timer, so no extra task and no
// delayed work in the RX task. If the slot is busy the packet is simply not
// forwarded (conservative, matches "not a repeater by default").
static esp_timer_handle_t s_fwd_timer;
static uint8_t s_fwd_frame[MC_MAX_TRANS_UNIT];
static volatile uint8_t s_fwd_len;
static volatile bool s_fwd_pending;

static bool all_zero16(const uint8_t *p) {
    for (int i = 0; i < 16; ++i) if (p[i]) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Clock + airtime
// ---------------------------------------------------------------------------

uint32_t mc_mesh_now(void) {
    if (s_time_base_us == 0) {
        return (uint32_t)(esp_timer_get_time() / 1000000);
    }
    int64_t delta = (esp_timer_get_time() - s_time_base_us) / 1000000;
    return s_time_base + (uint32_t)delta;
}

uint32_t mc_mesh_now_unique(void) {
    static uint32_t last_unique;
    uint32_t t = mc_mesh_now();
    if (t <= last_unique) return ++last_unique;
    return last_unique = t;
}

void mc_mesh_set_time(uint32_t epoch_secs) {
    s_time_base = epoch_secs;
    s_time_base_us = esp_timer_get_time();
}

uint32_t mc_mesh_airtime_ms(int len_bytes) {
    float bw = s_prefs.bw_khz > 0 ? s_prefs.bw_khz : 62.5f;
    int sf = s_prefs.sf >= 5 ? s_prefs.sf : 7;
    int cr = s_prefs.cr >= 5 ? s_prefs.cr : 5;
    int preamble = sf <= 8 ? MC_PREAMBLE_SF_LE8 : MC_PREAMBLE_SF_GT8;
    uint32_t symbol_us = (uint32_t)((10000ULL << sf) / (uint32_t)(bw * 10.0f));
    uint8_t sf_coeff1_x4 = (sf == 5 || sf == 6) ? 25 : 17;
    uint8_t sf_coeff2 = (sf == 5 || sf == 6) ? 0 : 8;
    uint8_t divisor = 4 * sf;
    uint32_t sym_ms = (uint32_t)((10000ULL * (1u << sf)) / (uint32_t)(bw * 10.0f * 1000));
    if (sym_ms > 16) divisor = 4 * (sf - 2); // LDRO
    int bit_count = 8 * len_bytes + 16 - 4 * sf + sf_coeff2 + 20;
    if (bit_count < 0) bit_count = 0;
    uint32_t n_pre = (uint32_t)((bit_count + divisor - 1) / divisor);
    uint32_t n_symbol_x4 = (uint32_t)(preamble + 8) * 4 + sf_coeff1_x4 + n_pre * cr * 4;
    uint32_t us = (uint32_t)(((uint64_t)symbol_us * n_symbol_x4) / 4);
    uint32_t ms = (us + 999) / 1000;
    return ms ? ms : 1;
}

// ---------------------------------------------------------------------------
// Dedup ring
// ---------------------------------------------------------------------------

static bool dedup_seen(const mc_packet_t *pkt) {
    if (!s_dedup) return false;
    uint8_t h[MC_MAX_HASH_SIZE];
    mc_packet_calculate_hash(pkt, h);
    const uint8_t *sp = s_dedup;
    for (int i = 0; i < MC_DEDUP_HASHES; ++i, sp += MC_MAX_HASH_SIZE) {
        if (memcmp(h, sp, MC_MAX_HASH_SIZE) == 0) return true;
    }
    return false;
}

static void dedup_mark(const mc_packet_t *pkt) {
    if (!s_dedup) return;
    uint8_t h[MC_MAX_HASH_SIZE];
    mc_packet_calculate_hash(pkt, h);
    memcpy(&s_dedup[s_dedup_next * MC_MAX_HASH_SIZE], h, MC_MAX_HASH_SIZE);
    s_dedup_next = (s_dedup_next + 1) % MC_DEDUP_HASHES;
}

// ---------------------------------------------------------------------------
// Contacts + channels
// ---------------------------------------------------------------------------

int mc_mesh_contact_count(void) {
    if (!s_contacts) return 0;
    int n = s_num_contacts - MC_MAX_ANON_CONTACTS;
    return n > 0 ? n : 0;
}

const mc_contact_t *mc_mesh_contact_at(int idx) {
    if (!s_contacts) return NULL;
    int real = idx + MC_MAX_ANON_CONTACTS;
    if (real < 0 || real >= s_num_contacts) return NULL;
    return &s_contacts[real];
}

mc_contact_t *mc_mesh_find_contact_pubkey(const uint8_t *pub_key, int prefix_len) {
    if (!s_contacts || !pub_key || prefix_len <= 0 || prefix_len > MC_PUB_KEY_SIZE) return NULL;
    for (int i = 0; i < s_num_contacts; ++i) {
        if (memcmp(s_contacts[i].pub_key, pub_key, prefix_len) == 0) return &s_contacts[i];
    }
    return NULL;
}

mc_contact_t *mc_mesh_find_contact_name(const char *name) {
    if (!s_contacts || !name) return NULL;
    size_t n = strlen(name);
    for (int i = 0; i < s_num_contacts; ++i) {
        if (strncmp(s_contacts[i].name, name, n) == 0) return &s_contacts[i];
    }
    return NULL;
}

static int contact_slot_index(const mc_contact_t *c) {
    if (!s_contacts || !c) return -1;
    return (int)(c - s_contacts);
}

static const uint8_t *contact_secret(mc_contact_t *c) {
    if (!c) return NULL;
    if (!c->shared_secret_valid) {
        mc_identity_shared_secret(c->pub_key, c->shared_secret);
        c->shared_secret_valid = true;
    }
    return c->shared_secret;
}

static mc_contact_t *alloc_contact_slot(bool transient_only) {
    if (!s_contacts) return NULL;
    if (transient_only) {
        for (int i = 0; i < MC_MAX_ANON_CONTACTS; ++i) {
            if (s_contacts[i].type == MC_ADV_TYPE_NONE) return &s_contacts[i];
        }
        return NULL;
    }
    if (s_num_contacts < MC_MAX_ANON_CONTACTS + MC_MAX_CONTACTS) {
        return &s_contacts[s_num_contacts++];
    }
    return NULL;
}

bool mc_mesh_add_contact(const mc_contact_t *contact) {
    if (!contact) return false;
    mc_contact_t *slot = alloc_contact_slot(contact->type == MC_ADV_TYPE_NONE);
    if (!slot) return false;
    *slot = *contact;
    slot->shared_secret_valid = false;
    return true;
}

bool mc_mesh_remove_contact(const mc_contact_t *contact) {
    int idx = contact_slot_index(contact);
    if (idx < 0 || idx >= s_num_contacts) return false;
    s_num_contacts--;
    while (idx < s_num_contacts) {
        s_contacts[idx] = s_contacts[idx + 1];
        idx++;
    }
    return true;
}

bool mc_mesh_save_contacts(void) {
    if (!s_contacts) return false;
    int n = mc_mesh_contact_count();
    return mc_store_save_contacts(&s_contacts[MC_MAX_ANON_CONTACTS], n);
}

bool mc_mesh_get_advert_blob(const uint8_t *pub_key, uint8_t *dest, uint8_t *out_len) {
    const mc_advert_blob_t *e = blob_cache_get(pub_key);
    if (!e) return false;
    memcpy(dest, e->blob, e->len);
    if (out_len) *out_len = e->len;
    return true;
}

const mc_channel_t *mc_mesh_channel(uint8_t idx) {
    if (!s_channels || idx >= MC_MAX_GROUP_CHANNELS) return NULL;
    return &s_channels[idx];
}

int mc_mesh_channel_count(void) {
    if (!s_channels) return 0;
    int n = 0;
    for (int i = 0; i < MC_MAX_GROUP_CHANNELS; ++i) {
        if (s_channels[i].secret[0] || s_channels[i].secret[15]) n++;
    }
    return n;
}

bool mc_mesh_set_channel(uint8_t idx, const mc_channel_t *ch) {
    if (!s_channels || idx >= MC_MAX_GROUP_CHANNELS || !ch) return false;
    static const uint8_t zeroes[16] = {0};
    s_channels[idx] = *ch;
    size_t key_len = (memcmp(&ch->secret[16], zeroes, 16) == 0) ? 16 : 32;
    mc_calc_channel_hash(&s_channels[idx].hash, ch->secret, key_len);
    mc_store_save_channels(s_channels);
    return true;
}

const mc_identity_t *mc_mesh_self(void) {
    return mc_identity_get();
}

const char *mc_mesh_node_name(void) {
    return s_prefs.node_name;
}

bool mc_mesh_set_node_name(const char *name) {
    if (!name) return false;
    snprintf(s_prefs.node_name, sizeof(s_prefs.node_name), "%.31s", name);
    return mc_store_save_prefs(&s_prefs);
}

mc_prefs_t *mc_mesh_prefs(void) {
    if (!s_prefs_loaded) {
        (void)mc_store_load_prefs(&s_prefs);
        s_prefs_loaded = true;
    }
    return &s_prefs;
}

bool mc_mesh_save_prefs(void) {
    return mc_store_save_prefs(&s_prefs);
}

bool mc_mesh_apply_radio(float freq_mhz, float bw_khz, uint8_t sf, uint8_t cr) {
    if (freq_mhz < 150.0f || freq_mhz > 2500.0f) return false;
    if (sf < 5 || sf > 12) return false;
    if (cr < 5 || cr > 8) return false;
    if (bw_khz < 7.0f || bw_khz > 500.0f) return false;
    s_prefs.freq_mhz = freq_mhz;
    s_prefs.bw_khz = bw_khz;
    s_prefs.sf = sf;
    s_prefs.cr = cr;
    s_prefs.radio_configured = true;
    return mc_store_save_prefs(&s_prefs);
}

bool mc_mesh_set_tx_power(int8_t dbm) {
    if (dbm < -9 || dbm > 22) return false;
    s_prefs.tx_dbm = dbm;
    return mc_store_save_prefs(&s_prefs);
}

// ---------------------------------------------------------------------------
// Packet construction + TX
// ---------------------------------------------------------------------------

static void tx_frame(const uint8_t *frame, uint8_t len) {
    if (!s_tx) return;
    s_tx(frame, len, s_tx_ctx);
}

static uint8_t tx_hash_size(void) {
    uint8_t mode = s_prefs.path_hash_mode;
    if (mode > 2) mode = 0;
    return (uint8_t)(mode + 1);
}

static void send_flood(mc_packet_t *pkt) {
    pkt->header &= ~MC_PH_ROUTE_MASK;
    pkt->header |= MC_ROUTE_TYPE_FLOOD;
    mc_packet_set_path_hash_size_and_count(pkt, tx_hash_size(), 0);
    dedup_mark(pkt); // don't process our own broadcast when it loops back
    uint8_t raw[MC_MAX_TRANS_UNIT];
    uint8_t len = mc_packet_write_to(pkt, raw);
    tx_frame(raw, len);
    s_stats.tx_ok++;
}

static void send_direct(mc_packet_t *pkt, const uint8_t *path, uint8_t path_len) {
    pkt->header &= ~MC_PH_ROUTE_MASK;
    pkt->header |= MC_ROUTE_TYPE_DIRECT;
    pkt->path_len = mc_packet_copy_path(pkt->path, path, path_len);
    dedup_mark(pkt);
    uint8_t raw[MC_MAX_TRANS_UNIT];
    uint8_t len = mc_packet_write_to(pkt, raw);
    tx_frame(raw, len);
    s_stats.tx_ok++;
}

static void send_zero_hop(mc_packet_t *pkt, uint16_t codes0, uint16_t codes1) {
    pkt->header &= ~MC_PH_ROUTE_MASK;
    if (codes0 || codes1) {
        pkt->header |= MC_ROUTE_TYPE_TRANSPORT_DIRECT;
        pkt->transport_codes[0] = codes0;
        pkt->transport_codes[1] = codes1;
    } else {
        pkt->header |= MC_ROUTE_TYPE_DIRECT;
    }
    pkt->path_len = 0;
    dedup_mark(pkt);
    uint8_t raw[MC_MAX_TRANS_UNIT];
    uint8_t len = mc_packet_write_to(pkt, raw);
    tx_frame(raw, len);
    s_stats.tx_ok++;
}

// Upstream TransportKey::calcTransportCode: HMAC-SHA256(key[16], type||payload)
// truncated to 2 bytes, reserving 0x0000 and 0xFFFF.
static uint16_t calc_transport_code(const uint8_t key[16], const mc_packet_t *pkt) {
    uint8_t msg[1 + MC_MAX_PACKET_PAYLOAD];
    msg[0] = mc_packet_payload_type(pkt);
    if (pkt->payload_len > MC_MAX_PACKET_PAYLOAD) return 0;
    memcpy(&msg[1], pkt->payload, pkt->payload_len);
    uint8_t out[2];
    mc_hmac_sha256(out, 2, key, 16, msg, (size_t)1 + pkt->payload_len);
    uint16_t code = (uint16_t)(out[0] | ((uint16_t)out[1] << 8));
    if (code == 0) code = 1;
    else if (code == 0xFFFF) code = 0xFFFE;
    return code;
}

// Flood a packet with a specific scope key. A null key means unscoped.
static void send_flood_scoped_key(mc_packet_t *pkt, const uint8_t key[16]) {
    if (!key || all_zero16(key)) { send_flood(pkt); return; }
    uint16_t code = calc_transport_code(key, pkt);
    pkt->header &= ~MC_PH_ROUTE_MASK;
    pkt->header |= MC_ROUTE_TYPE_TRANSPORT_FLOOD;
    pkt->transport_codes[0] = code;
    pkt->transport_codes[1] = 0;
    mc_packet_set_path_hash_size_and_count(pkt, tx_hash_size(), 0);
    dedup_mark(pkt);
    uint8_t raw[MC_MAX_TRANS_UNIT];
    uint8_t len = mc_packet_write_to(pkt, raw);
    tx_frame(raw, len);
    s_stats.tx_ok++;
}

// Message floods use the app's send-scope override, else the default scope.
static void send_flood_scoped(mc_packet_t *pkt) {
    if (s_send_unscoped) { send_flood(pkt); return; }
    if (s_send_scope_set) { send_flood_scoped_key(pkt, s_send_scope); return; }
    send_flood_scoped_key(pkt, s_prefs.default_scope_key);
}

// Advert floods always use the default scope (upstream CMD_SEND_SELF_ADVERT).
static void send_advert_flood(mc_packet_t *pkt) {
    send_flood_scoped_key(pkt, s_prefs.default_scope_key);
}

void mc_mesh_set_send_scope(const uint8_t *key16) {
    if (!key16 || all_zero16(key16)) {
        memset(s_send_scope, 0, sizeof(s_send_scope));
        s_send_scope_set = false;
    } else {
        memcpy(s_send_scope, key16, sizeof(s_send_scope));
        s_send_scope_set = true;
    }
    s_send_unscoped = false;
}

void mc_mesh_set_send_unscoped(bool on) { s_send_unscoped = on; }

bool mc_mesh_get_default_scope(char *name31, uint8_t key16[16]) {
    bool named = s_prefs.default_scope_name[0] != 0;
    if (name31) {
        memset(name31, 0, 31);
        if (named) memcpy(name31, s_prefs.default_scope_name, 30);
    }
    if (key16) memcpy(key16, s_prefs.default_scope_key, 16);
    return named || !all_zero16(s_prefs.default_scope_key);
}

void mc_mesh_set_default_scope(const char *name, const uint8_t *key16) {
    if (!name || !name[0] || !key16) {
        memset(s_prefs.default_scope_name, 0, sizeof(s_prefs.default_scope_name));
        memset(s_prefs.default_scope_key, 0, sizeof(s_prefs.default_scope_key));
    } else {
        memset(s_prefs.default_scope_name, 0, sizeof(s_prefs.default_scope_name));
        snprintf(s_prefs.default_scope_name, sizeof(s_prefs.default_scope_name), "%.30s", name);
        memcpy(s_prefs.default_scope_key, key16, 16);
    }
    (void)mc_store_save_prefs(&s_prefs);
}

// ---- Opt-in forwarding ----------------------------------------------------

static void fwd_timer_cb(void *arg) {
    (void)arg;
    if (s_fwd_pending) {
        s_fwd_pending = false;
        tx_frame(s_fwd_frame, s_fwd_len);
        s_stats.tx_ok++;
    }
}

// Called for every accepted flood packet. Only forwards group text/data and
// adverts, appends our path hash, and schedules a randomized retransmit.
static void maybe_forward(mc_packet_t *pkt) {
    if (!s_prefs.repeat || !s_fwd_timer || s_fwd_pending) return;
    if (!mc_packet_is_route_flood(pkt)) return;
    if (mc_packet_is_marked_do_not_retransmit(pkt)) return;
    uint8_t type = mc_packet_payload_type(pkt);
    if (type != MC_PAYLOAD_TYPE_GRP_TXT && type != MC_PAYLOAD_TYPE_GRP_DATA &&
        type != MC_PAYLOAD_TYPE_ADVERT) {
        return;
    }
    const mc_identity_t *id = mc_identity_get();
    if (!id || !id->valid) return;

    uint8_t sz = mc_packet_path_hash_size(pkt);
    uint8_t n = mc_packet_path_hash_count(pkt);
    if (sz == 0 || sz > 3) return;
    if ((n + 1) * sz > MC_MAX_PATH_SIZE) return; // no room for another hop

    // Skip if our hash is already in the path (would be a loop).
    for (uint8_t i = 0; i < n; ++i) {
        if (memcmp(&pkt->path[i * sz], id->pub_key, sz) == 0) return;
    }
    memcpy(&pkt->path[n * sz], id->pub_key, sz);
    mc_packet_set_path_hash_count(pkt, (uint8_t)(n + 1));

    uint8_t len = mc_packet_write_to(pkt, s_fwd_frame);
    /* len is uint8_t and s_fwd_frame is MC_MAX_TRANS_UNIT (255) bytes. */
    if (len == 0) return;
    s_fwd_len = len;
    s_fwd_pending = true;

    // Upstream getRetransmitDelay: RNG(0..5) * (airtime * 52/50) / 2.
    uint32_t air = mc_mesh_airtime_ms(mc_packet_raw_length(pkt));
    uint32_t t_us = ((air * 52u) / 50u) * 500u; // /2 * 1000 (ms -> us)
    uint32_t ticks = (esp_random() % 6u) * t_us;
    if (ticks < 1000) ticks = 1000;
    if (esp_timer_start_once(s_fwd_timer, ticks) != ESP_OK) {
        s_fwd_pending = false; // timer busy/unavailable: skip this forward
    }
}

bool mc_mesh_send_raw_frame(const uint8_t *frame, uint8_t len, uint8_t priority) {
    (void)priority;
    if (!mesh_ready() || !frame || len < 3) return false;
    mc_packet_t pkt;
    mc_packet_init(&pkt);
    if (!mc_packet_read_from(&pkt, frame, len)) return false;
    dedup_mark(&pkt);
    tx_frame(frame, len);
    s_stats.tx_ok++;
    return true;
}

static void build_advert_appdata(uint8_t *app_data, uint8_t *out_len) {
    const mc_prefs_t *p = &s_prefs;
    int i = 1;
    app_data[0] = MC_ADV_TYPE_CHAT;
    if (p->advert_loc_policy != 0 && (p->node_lat || p->node_lon)) {
        app_data[0] |= MC_ADV_LATLON_MASK;
        memcpy(&app_data[i], &p->node_lat, 4); i += 4;
        memcpy(&app_data[i], &p->node_lon, 4); i += 4;
    }
    size_t name_len = strlen(p->node_name);
    if (name_len > (size_t)(MC_MAX_ADVERT_DATA_SIZE - i - 1)) {
        name_len = (size_t)(MC_MAX_ADVERT_DATA_SIZE - i - 1);
    }
    if (name_len > 0) {
        app_data[0] |= MC_ADV_NAME_MASK;
        memcpy(&app_data[i], p->node_name, name_len);
        i += (int)name_len;
    }
    *out_len = (uint8_t)i;
}

static bool create_advert(mc_packet_t *pkt) {
    const mc_identity_t *id = mc_identity_get();
    if (!id || !id->valid) return false;

    uint8_t app_data[MC_MAX_ADVERT_DATA_SIZE];
    uint8_t app_len = 0;
    build_advert_appdata(app_data, &app_len);

    mc_packet_init(pkt);
    pkt->header = (uint8_t)(MC_PAYLOAD_TYPE_ADVERT << MC_PH_TYPE_SHIFT);

    int i = 0;
    memcpy(&pkt->payload[i], id->pub_key, MC_PUB_KEY_SIZE); i += MC_PUB_KEY_SIZE;
    uint32_t ts = mc_mesh_now();
    memcpy(&pkt->payload[i], &ts, 4); i += 4;
    uint8_t *sig = &pkt->payload[i]; i += MC_SIGNATURE_SIZE;
    memcpy(&pkt->payload[i], app_data, app_len); i += app_len;
    pkt->payload_len = (uint16_t)i;

    uint8_t message[MC_PUB_KEY_SIZE + 4 + MC_MAX_ADVERT_DATA_SIZE];
    int mlen = 0;
    memcpy(&message[mlen], id->pub_key, MC_PUB_KEY_SIZE); mlen += MC_PUB_KEY_SIZE;
    memcpy(&message[mlen], &ts, 4); mlen += 4;
    memcpy(&message[mlen], app_data, app_len); mlen += app_len;
    mc_identity_sign(message, (size_t)mlen, sig);
    return true;
}

bool mc_mesh_send_advert(bool flood) {
    if (!mesh_ready()) return false;
    mc_packet_t pkt;
    if (!create_advert(&pkt)) return false;
    if (flood) send_advert_flood(&pkt);
    else send_zero_hop(&pkt, 0, 0);
    return true;
}

static bool create_direct_datagram(mc_packet_t *pkt, uint8_t type, const uint8_t *dest_pub,
                                   const uint8_t *secret, const uint8_t *data, int data_len) {
    if (type != MC_PAYLOAD_TYPE_TXT_MSG && type != MC_PAYLOAD_TYPE_REQ &&
        type != MC_PAYLOAD_TYPE_RESPONSE) {
        return false;
    }
    if (data_len + MC_CIPHER_MAC_SIZE + MC_CIPHER_BLOCK_SIZE - 1 > MC_MAX_PACKET_PAYLOAD) return false;
    mc_packet_init(pkt);
    pkt->header = (uint8_t)(type << MC_PH_TYPE_SHIFT);
    int i = 0;
    memcpy(&pkt->payload[i], dest_pub, MC_PATH_HASH_SIZE); i += MC_PATH_HASH_SIZE; // dest hash
    memcpy(&pkt->payload[i], mc_identity_get()->pub_key, MC_PATH_HASH_SIZE); i += MC_PATH_HASH_SIZE; // src hash
    i += mc_encrypt_then_mac(secret, &pkt->payload[i], data, data_len);
    pkt->payload_len = (uint16_t)i;
    return true;
}

static bool create_group_datagram(mc_packet_t *pkt, uint8_t type, const mc_channel_t *ch,
                                  const uint8_t *data, int data_len) {
    if (type != MC_PAYLOAD_TYPE_GRP_TXT && type != MC_PAYLOAD_TYPE_GRP_DATA) return false;
    if (data_len + 1 + MC_CIPHER_BLOCK_SIZE - 1 > MC_MAX_PACKET_PAYLOAD) return false;
    mc_packet_init(pkt);
    pkt->header = (uint8_t)(type << MC_PH_TYPE_SHIFT);
    int i = 0;
    pkt->payload[i++] = ch->hash;
    i += mc_encrypt_then_mac(ch->secret, &pkt->payload[i], data, data_len);
    pkt->payload_len = (uint16_t)i;
    return true;
}

static bool create_ack_packet(mc_packet_t *pkt, const uint8_t *ack, uint8_t ack_len) {
    mc_packet_init(pkt);
    pkt->header = (uint8_t)(MC_PAYLOAD_TYPE_ACK << MC_PH_TYPE_SHIFT);
    memcpy(pkt->payload, ack, ack_len);
    pkt->payload_len = ack_len;
    return true;
}

static bool create_path_return(mc_packet_t *pkt, const uint8_t *dest_pub, const uint8_t *secret,
                               const uint8_t *path, uint8_t path_len, uint8_t extra_type,
                               const uint8_t *extra, size_t extra_len) {
    uint8_t path_hash_size = (uint8_t)((path_len >> 6) + 1);
    uint8_t path_hash_count = (uint8_t)(path_len & 63);
    if ((size_t)path_hash_count * path_hash_size + extra_len + 5 > MC_MAX_PACKET_PAYLOAD - 2 - MC_CIPHER_BLOCK_SIZE) {
        return false;
    }
    mc_packet_init(pkt);
    pkt->header = (uint8_t)(MC_PAYLOAD_TYPE_PATH << MC_PH_TYPE_SHIFT);
    pkt->payload[0] = dest_pub[0]; // dest hash
    pkt->payload[1] = mc_identity_get()->pub_key[0]; // src hash
    int len = 2;

    uint8_t data[MC_MAX_PACKET_PAYLOAD];
    int dlen = 0;
    data[dlen++] = path_len;
    memcpy(&data[dlen], path, (size_t)path_hash_count * path_hash_size);
    dlen += path_hash_count * path_hash_size;
    if (extra_len > 0) {
        data[dlen++] = extra_type;
        memcpy(&data[dlen], extra, extra_len);
        dlen += (int)extra_len;
    } else {
        data[dlen++] = 0xFF;
        uint32_t r = (uint32_t)esp_timer_get_time();
        memcpy(&data[dlen], &r, 4);
        dlen += 4;
    }
    len += mc_encrypt_then_mac(secret, &pkt->payload[len], data, dlen);
    pkt->payload_len = (uint16_t)len;
    return true;
}

// Upstream handleReturnPathRetry: when a peer's ACK/response still arrives over
// flood even though we hold a direct path for them, re-send a reciprocal return
// path directly so they learn the route back to us.
static void return_path_retry(mc_contact_t *c, const uint8_t *path, uint8_t path_len) {
    if (!c || c->out_path_len == MC_OUT_PATH_UNKNOWN) return;
    const uint8_t *secret = contact_secret(c);
    if (!secret) return;
    mc_packet_t rp;
    if (create_path_return(&rp, c->pub_key, secret, path, path_len, 0, NULL, 0)) {
        send_direct(&rp, c->out_path, c->out_path_len);
    }
}

bool mc_mesh_send_group_text_len(uint8_t channel_idx, const char *text, int text_len,
                                 uint32_t timestamp) {
    if (!mesh_ready()) return false;
    const mc_channel_t *ch = mc_mesh_channel(channel_idx);
    if (!ch || (!ch->secret[0] && !ch->secret[15])) return false;
    if (text_len < 0) text_len = 0;

    uint8_t temp[5 + MC_MAX_TEXT_LEN + 32];
    memcpy(temp, &timestamp, 4);
    temp[4] = 0; // TXT_TYPE_PLAIN, attempt 0
    int prefix = snprintf((char *)&temp[5], sizeof(temp) - 5, "%s: ", s_prefs.node_name);
    if (prefix < 0) prefix = 0;
    if (prefix + text_len > MC_MAX_TEXT_LEN) text_len = MC_MAX_TEXT_LEN - prefix;
    if (text_len < 0) text_len = 0;
    memcpy(&temp[5 + prefix], text, (size_t)text_len);

    mc_packet_t pkt;
    if (!create_group_datagram(&pkt, MC_PAYLOAD_TYPE_GRP_TXT, ch, temp, 5 + prefix + text_len)) {
        return false;
    }
    send_flood_scoped(&pkt);
    return true;
}

bool mc_mesh_send_group_text(uint8_t channel_idx, const char *text, uint32_t timestamp) {
    if (!text) return false;
    return mc_mesh_send_group_text_len(channel_idx, text, (int)strlen(text), timestamp);
}

bool mc_mesh_send_group_data(uint8_t channel_idx, uint8_t path_len, const uint8_t *path,
                             uint16_t data_type, const uint8_t *data, int data_len) {
    if (!mesh_ready()) return false;
    const mc_channel_t *ch = mc_mesh_channel(channel_idx);
    if (!ch || data_len < 0 || data_len > MC_MAX_GROUP_DATA_LENGTH) return false;
    uint8_t temp[3 + MC_MAX_GROUP_DATA_LENGTH];
    temp[0] = (uint8_t)(data_type & 0xFF);
    temp[1] = (uint8_t)(data_type >> 8);
    temp[2] = (uint8_t)data_len;
    if (data_len > 0) memcpy(&temp[3], data, (size_t)data_len);

    mc_packet_t pkt;
    if (!create_group_datagram(&pkt, MC_PAYLOAD_TYPE_GRP_DATA, ch, temp, 3 + data_len)) return false;
    if (path_len == MC_OUT_PATH_UNKNOWN) send_flood_scoped(&pkt);
    else send_direct(&pkt, path, path_len);
    return true;
}

int mc_mesh_send_direct_text_len(const mc_contact_t *to, uint32_t timestamp, uint8_t attempt,
                                 uint8_t txt_type, const char *text, int text_len,
                                 uint32_t *expected_ack, uint32_t *est_timeout) {
    if (!to || text_len < 0 || text_len > MC_MAX_TEXT_LEN) return MC_MSG_SEND_FAILED;

    mc_contact_t *c = mc_mesh_find_contact_pubkey(to->pub_key, MC_PUB_KEY_SIZE);
    if (!c) return MC_MSG_SEND_FAILED;
    const uint8_t *secret = contact_secret(c);

    uint8_t temp[5 + MC_MAX_TEXT_LEN + 2];
    memcpy(temp, &timestamp, 4);
    temp[4] = (uint8_t)((attempt & 3) | (txt_type << 2));
    memcpy(&temp[5], text, (size_t)text_len);
    temp[5 + text_len] = 0;
    int len = 5 + text_len;

    uint32_t ack = 0;
    if (txt_type == MC_TXT_TYPE_PLAIN) {
        // Expected ACK tag (upstream composeMsgPacket): truncated SHA-256 over
        // timestamp + flags + text + OUR public key.
        mc_sha256_2((uint8_t *)&ack, 4, temp, (size_t)(5 + text_len),
                    mc_identity_get()->pub_key, MC_PUB_KEY_SIZE);
    }
    if (attempt > 3) {
        temp[len++] = 0;
        temp[len++] = attempt;
    }

    mc_packet_t pkt;
    if (!create_direct_datagram(&pkt, MC_PAYLOAD_TYPE_TXT_MSG, c->pub_key, secret, temp, len)) {
        return MC_MSG_SEND_FAILED;
    }
    if (expected_ack) *expected_ack = ack;
    if (txt_type == MC_TXT_TYPE_PLAIN && ack != 0) pending_ack_add(ack, c->pub_key);

    uint32_t air = mc_mesh_airtime_ms(mc_packet_raw_length(&pkt));
    if (c->out_path_len == MC_OUT_PATH_UNKNOWN) {
        send_flood_scoped(&pkt);
        if (est_timeout) *est_timeout = MC_SEND_TIMEOUT_BASE_MS + (uint32_t)(MC_FLOOD_SEND_TIMEOUT_FACTOR * air);
        return MC_MSG_SEND_SENT_FLOOD;
    }
    send_direct(&pkt, c->out_path, c->out_path_len);
    uint8_t hops = (uint8_t)(c->out_path_len & 63);
    if (est_timeout) {
        *est_timeout = MC_SEND_TIMEOUT_BASE_MS +
                       (uint32_t)((air * MC_DIRECT_SEND_PERHOP_FACTOR + MC_DIRECT_SEND_PERHOP_EXTRA_MS) * (hops + 1));
    }
    return MC_MSG_SEND_SENT_DIRECT;
}

int mc_mesh_send_direct_text(const mc_contact_t *to, uint32_t timestamp, uint8_t attempt,
                             uint8_t txt_type, const char *text, uint32_t *expected_ack,
                             uint32_t *est_timeout) {
    if (!text) return MC_MSG_SEND_FAILED;
    return mc_mesh_send_direct_text_len(to, timestamp, attempt, txt_type, text, (int)strlen(text),
                                        expected_ack, est_timeout);
}

int mc_mesh_send_request(const mc_contact_t *to, const uint8_t *req_data, uint8_t data_len,
                         uint32_t *out_tag, uint32_t *out_timeout) {
    if (!mesh_ready() || !to || (!req_data && data_len)) return MC_MSG_SEND_FAILED;
    if (data_len > MC_MAX_PACKET_PAYLOAD - 16) return MC_MSG_SEND_FAILED;
    mc_contact_t *c = mc_mesh_find_contact_pubkey(to->pub_key, MC_PUB_KEY_SIZE);
    if (!c) return MC_MSG_SEND_FAILED;
    const uint8_t *secret = contact_secret(c);
    if (!secret) return MC_MSG_SEND_FAILED;

    uint8_t temp[MC_MAX_PACKET_PAYLOAD];
    uint32_t tag = mc_mesh_now_unique();
    memcpy(temp, &tag, 4);
    if (data_len) memcpy(&temp[4], req_data, data_len);

    mc_packet_t pkt;
    if (!create_direct_datagram(&pkt, MC_PAYLOAD_TYPE_REQ, c->pub_key, secret, temp, 4 + data_len)) {
        return MC_MSG_SEND_FAILED;
    }
    if (out_tag) *out_tag = tag;

    uint32_t air = mc_mesh_airtime_ms(mc_packet_raw_length(&pkt));
    if (c->out_path_len == MC_OUT_PATH_UNKNOWN) {
        send_flood_scoped(&pkt);
        if (out_timeout) *out_timeout = MC_SEND_TIMEOUT_BASE_MS + (uint32_t)(MC_FLOOD_SEND_TIMEOUT_FACTOR * air);
        return MC_MSG_SEND_SENT_FLOOD;
    }
    send_direct(&pkt, c->out_path, c->out_path_len);
    uint8_t hops = (uint8_t)(c->out_path_len & 63);
    if (out_timeout) {
        *out_timeout = MC_SEND_TIMEOUT_BASE_MS +
                       (uint32_t)((air * MC_DIRECT_SEND_PERHOP_FACTOR + MC_DIRECT_SEND_PERHOP_EXTRA_MS) * (hops + 1));
    }
    return MC_MSG_SEND_SENT_DIRECT;
}

int mc_mesh_send_request_type(const mc_contact_t *to, uint8_t req_type,
                              uint32_t *out_tag, uint32_t *out_timeout) {
    // Upstream composes tag(4) + type(1) + reserved(4) + random(4) = 13 bytes.
    uint8_t blob[9];
    blob[0] = req_type;
    memset(&blob[1], 0, 4);
    uint32_t r = (uint32_t)esp_timer_get_time();
    memcpy(&blob[5], &r, 4);
    return mc_mesh_send_request(to, blob, sizeof(blob), out_tag, out_timeout);
}

// ---------------------------------------------------------------------------
// RX
// ---------------------------------------------------------------------------

static void handle_advert(mc_packet_t *pkt) {
    if (pkt->payload_len < MC_PUB_KEY_SIZE + 4 + MC_SIGNATURE_SIZE) return;
    const uint8_t *pub = &pkt->payload[0];
    uint32_t ts;
    memcpy(&ts, &pkt->payload[MC_PUB_KEY_SIZE], 4);
    const uint8_t *sig = &pkt->payload[MC_PUB_KEY_SIZE + 4];
    const uint8_t *app_data = &pkt->payload[MC_PUB_KEY_SIZE + 4 + MC_SIGNATURE_SIZE];
    int app_len = (int)pkt->payload_len - (MC_PUB_KEY_SIZE + 4 + MC_SIGNATURE_SIZE);
    if (app_len > MC_MAX_ADVERT_DATA_SIZE) app_len = MC_MAX_ADVERT_DATA_SIZE;

    const mc_identity_t *self = mc_identity_get();
    if (self && memcmp(self->pub_key, pub, MC_PUB_KEY_SIZE) == 0) return;

    uint8_t message[MC_PUB_KEY_SIZE + 4 + MC_MAX_ADVERT_DATA_SIZE];
    int mlen = 0;
    memcpy(&message[mlen], pub, MC_PUB_KEY_SIZE); mlen += MC_PUB_KEY_SIZE;
    memcpy(&message[mlen], &ts, 4); mlen += 4;
    memcpy(&message[mlen], app_data, app_len); mlen += app_len;
    if (!mc_identity_verify(pub, message, (size_t)mlen, sig)) return;

    if (app_len < 1) return;
    uint8_t flags = app_data[0];
    const char *name = NULL;
    char name_buf[32];
    name_buf[0] = 0;
    int off = 1;
    int32_t lat = 0, lon = 0;
    if (flags & MC_ADV_LATLON_MASK) {
        if (off + 8 <= app_len) {
            memcpy(&lat, &app_data[off], 4); off += 4;
            memcpy(&lon, &app_data[off], 4); off += 4;
        }
    }
    if (flags & MC_ADV_FEAT1_MASK) off += 2;
    if (flags & MC_ADV_FEAT2_MASK) off += 2;
    if ((flags & MC_ADV_NAME_MASK) && off < app_len) {
        int n = app_len - off;
        if (n > (int)sizeof(name_buf) - 1) n = (int)sizeof(name_buf) - 1;
        memcpy(name_buf, &app_data[off], (size_t)n);
        name_buf[n] = 0;
        name = name_buf;
    }
    if (!name || !name[0]) return;

    mc_contact_t *c = mc_mesh_find_contact_pubkey(pub, MC_PUB_KEY_SIZE);
    bool is_new = false;
    if (!c) {
        // Auto-add policy (upstream MyMesh::shouldAutoAddContactType /
        // getAutoAddMaxHops / shouldOverwriteWhenFull).
        uint8_t type = (uint8_t)(flags & 0x0F);
        bool allow = true;
        if (s_prefs.manual_add_contacts & 1) {
            allow = false;
            uint8_t bit = 0;
            switch (type) {
                case MC_ADV_TYPE_CHAT:     bit = 0x02; break; // AUTO_ADD_CHAT
                case MC_ADV_TYPE_REPEATER: bit = 0x04; break; // AUTO_ADD_REPEATER
                case MC_ADV_TYPE_ROOM:     bit = 0x08; break; // AUTO_ADD_ROOM_SERVER
                case MC_ADV_TYPE_SENSOR:   bit = 0x10; break; // AUTO_ADD_SENSOR
                default: break;
            }
            if (bit && (s_prefs.autoadd_config & bit)) allow = true;
        }
        if (allow && s_prefs.autoadd_max_hops > 0 &&
            mc_packet_path_hash_count(pkt) >= s_prefs.autoadd_max_hops) {
            allow = false;
        }
        if (!allow) {
            // Surface as a discovered (not added) contact so apps still see it.
            if (s_cb.on_advert) {
                mc_contact_t tmp;
                memset(&tmp, 0, sizeof(tmp));
                memcpy(tmp.pub_key, pub, MC_PUB_KEY_SIZE);
                snprintf(tmp.name, sizeof(tmp.name), "%.31s", name);
                tmp.type = type;
                tmp.out_path_len = MC_OUT_PATH_UNKNOWN;
                tmp.last_advert_timestamp = ts;
                tmp.lastmod = mc_mesh_now();
                s_cb.on_advert(&tmp, true);
            }
            return;
        }
        c = alloc_contact_slot(false);
        if (!c && (s_prefs.autoadd_config & 0x01)) { // AUTO_ADD_OVERWRITE_OLDEST
            int idx = -1;
            uint32_t oldest = 0xFFFFFFFF;
            for (int i = MC_MAX_ANON_CONTACTS; i < s_num_contacts; ++i) {
                if ((s_contacts[i].flags & 0x01) == 0 && s_contacts[i].lastmod < oldest) {
                    oldest = s_contacts[i].lastmod;
                    idx = i;
                }
            }
            if (idx >= 0) {
                if (s_cb.on_contact_deleted) s_cb.on_contact_deleted(s_contacts[idx].pub_key);
                c = &s_contacts[idx];
            }
        }
        if (!c) return;
        memset(c, 0, sizeof(*c));
        c->out_path_len = MC_OUT_PATH_UNKNOWN;
        is_new = true;
    } else if (ts <= c->last_advert_timestamp) {
        return; // replay protection
    }

    memcpy(c->pub_key, pub, MC_PUB_KEY_SIZE);
    snprintf(c->name, sizeof(c->name), "%.31s", name);
    c->type = (uint8_t)(flags & 0x0F);
    c->gps_lat = lat;
    c->gps_lon = lon;
    c->last_advert_timestamp = ts;
    c->lastmod = mc_mesh_now();
    c->shared_secret_valid = false;
    if (is_new) c->sync_since = 0;

    {
        uint8_t raw[MC_MAX_TRANS_UNIT];
        uint8_t save = pkt->header;
        pkt->header = (uint8_t)((pkt->header & ~MC_PH_ROUTE_MASK) | MC_ROUTE_TYPE_FLOOD);
        uint8_t n = mc_packet_write_to(pkt, raw);
        pkt->header = save;
        blob_cache_put(pub, raw, n);
    }

    // Record advert path for the companion.
    mc_advert_path_t *ap = &s_advert_paths[s_advert_path_next];
    memcpy(ap->pubkey_prefix, pub, 7);
    ap->path_len = (uint8_t)pkt->path_len;
    snprintf(ap->name, sizeof(ap->name), "%.31s", name);
    ap->recv_timestamp = mc_mesh_now();
    memcpy(ap->path, pkt->path, sizeof(ap->path));
    s_advert_path_next = (s_advert_path_next + 1) % 16;

    s_stats.rx_advert++;
    if (s_cb.on_advert) s_cb.on_advert(c, is_new);
}

static int find_channel_index_by_hash(uint8_t hash, uint8_t *out_idx, int max) {
    int n = 0;
    for (int i = 0; i < MC_MAX_GROUP_CHANNELS && n < max; ++i) {
        if (s_channels[i].hash == hash && (s_channels[i].secret[0] || s_channels[i].secret[15])) {
            out_idx[n++] = (uint8_t)i;
        }
    }
    return n;
}

static void handle_group(mc_packet_t *pkt) {
    if ((uint8_t)pkt->payload_len < 1 + MC_CIPHER_MAC_SIZE + 1) return;
    uint8_t chash = pkt->payload[0];
    const uint8_t *mac_and_data = &pkt->payload[1];
    int n = (int)pkt->payload_len - 1;

    uint8_t idxs[4];
    int num = find_channel_index_by_hash(chash, idxs, 4);
    for (int j = 0; j < num; ++j) {
        mc_channel_t *ch = &s_channels[idxs[j]];
        uint8_t data[MC_MAX_PACKET_PAYLOAD + 1];
        memset(data, 0, sizeof(data));
        int len = mc_mac_then_decrypt(ch->secret, data, mac_and_data, n);
        if (len <= 0) continue;
        data[len] = 0;

        uint8_t ptype = mc_packet_payload_type(pkt);
        if (ptype == MC_PAYLOAD_TYPE_GRP_TXT) {
            if (len < 5) break;
            if ((data[4] >> 2) != 0) break; // only plain group text
            uint32_t ts;
            memcpy(&ts, data, 4);
            s_stats.rx_group++;
            if (s_cb.on_channel_message) {
                s_cb.on_channel_message((uint8_t)idxs[j], ts, (const char *)&data[5], pkt->snr / 4.0f,
                                        (uint8_t)pkt->path_len, mc_packet_is_route_flood(pkt));
            }
        } else if (ptype == MC_PAYLOAD_TYPE_GRP_DATA) {
            if (len < 3) break;
            uint16_t dtype = (uint16_t)(data[0] | (data[1] << 8));
            uint8_t dlen = data[2];
            if ((size_t)dlen + 3 > (size_t)len) break;
            s_stats.rx_group++;
            if (s_cb.on_channel_data) {
                s_cb.on_channel_data((uint8_t)idxs[j], dtype, &data[3], dlen, pkt->snr / 4.0f,
                                     (uint8_t)pkt->path_len, mc_packet_is_route_flood(pkt));
            }
        }
        break; // first decrypting channel wins
    }
}

static void handle_direct(mc_packet_t *pkt, uint8_t ptype) {
    if ((uint8_t)pkt->payload_len < 2 + MC_CIPHER_MAC_SIZE + 1) return;
    uint8_t dest_hash = pkt->payload[0];
    uint8_t src_hash = pkt->payload[1];
    const uint8_t *mac_and_data = &pkt->payload[2];
    int n = (int)pkt->payload_len - 2;

    if (!mc_identity_ready()) return;
    if (mc_identity_node_hash() != dest_hash) return; // not for us (and we don't relay)

    // Find candidate peers by source hash.
    for (int i = 0; i < s_num_contacts; ++i) {
        if (s_contacts[i].pub_key[0] != src_hash) continue;
        mc_contact_t *from = &s_contacts[i];
        const uint8_t *secret = contact_secret(from);
        uint8_t data[MC_MAX_PACKET_PAYLOAD + 1];
        memset(data, 0, sizeof(data));
        int len = mc_mac_then_decrypt(secret, data, mac_and_data, n);
        if (len <= 0) continue;
        data[len] = 0;

        if (ptype == MC_PAYLOAD_TYPE_TXT_MSG && len > 5) {
            uint32_t ts;
            memcpy(&ts, data, 4);
            uint8_t flags = (uint8_t)(data[4] >> 2);
            from->lastmod = mc_mesh_now();
            s_stats.rx_direct++;

            if (flags == MC_TXT_TYPE_PLAIN) {
                int text_len = (int)strlen((char *)&data[5]);
                if (s_cb.on_contact_message) {
                    s_cb.on_contact_message(from, ts, (const char *)&data[5], MC_TXT_TYPE_PLAIN,
                                            pkt->snr / 4.0f, (uint8_t)pkt->path_len,
                                            mc_packet_is_route_flood(pkt));
                }
                // ACK: truncated hash of timestamp+text+our pubkey, plus extended attempt.
                uint8_t ack_hash[6];
                mc_sha256_2(ack_hash, 4, data, (size_t)(5 + text_len),
                            mc_identity_get()->pub_key, MC_PUB_KEY_SIZE);
                ack_hash[4] = data[5 + text_len + 1];
                ack_hash[5] = (uint8_t)(esp_timer_get_time() & 0xFF);

                if (mc_packet_is_route_flood(pkt)) {
                    mc_packet_t path;
                    if (create_path_return(&path, from->pub_key, secret, pkt->path, (uint8_t)pkt->path_len,
                                           MC_PAYLOAD_TYPE_ACK, ack_hash, 6)) {
                        send_flood_scoped(&path);
                    }
                } else {
                    mc_packet_t ack;
                    if (create_ack_packet(&ack, ack_hash, 6)) send_direct(&ack, from->out_path, from->out_path_len);
                }
            } else if (flags == MC_TXT_TYPE_SIGNED_PLAIN && len > 9) {
                // Signed plain text: ts(4) flags(1) sender_prefix(4) text. The
                // signature is verified by the app (it resolves the prefix to
                // the contact's pubkey); the device forwards prefix+text and
                // ACKs like a plain message but over 9+len bytes.
                if (ts > from->sync_since) from->sync_since = ts;
                const uint8_t *prefix = &data[5];
                const char *text = (const char *)&data[9];
                if (s_cb.on_signed_message) {
                    s_cb.on_signed_message(from, ts, prefix, text, pkt->snr / 4.0f,
                                           (uint8_t)pkt->path_len, mc_packet_is_route_flood(pkt));
                }
                int text_len = (int)strlen(text);
                uint8_t ack_hash[4];
                mc_sha256_2(ack_hash, 4, data, (size_t)(9 + text_len),
                            mc_identity_get()->pub_key, MC_PUB_KEY_SIZE);
                if (mc_packet_is_route_flood(pkt)) {
                    mc_packet_t path;
                    if (create_path_return(&path, from->pub_key, secret, pkt->path, (uint8_t)pkt->path_len,
                                           MC_PAYLOAD_TYPE_ACK, ack_hash, 4)) {
                        send_flood_scoped(&path);
                    }
                } else {
                    mc_packet_t ack;
                    if (create_ack_packet(&ack, ack_hash, 4)) send_direct(&ack, from->out_path, from->out_path_len);
                }
            } else if (s_cb.on_contact_message) {
                s_cb.on_contact_message(from, ts, (const char *)&data[5], flags, pkt->snr / 4.0f,
                                        (uint8_t)pkt->path_len, mc_packet_is_route_flood(pkt));
            }
        } else if (ptype == MC_PAYLOAD_TYPE_PATH) {
            int k = 0;
            uint8_t path_len = data[k++];
            if (!mc_packet_is_valid_path_len(path_len)) break;
            uint8_t hs = (uint8_t)((path_len >> 6) + 1);
            uint8_t hc = (uint8_t)(path_len & 63);
            const uint8_t *path = &data[k];
            k += hs * hc;
            uint8_t extra_type = (uint8_t)(data[k++] & 0x0F);
            const uint8_t *extra = &data[k];
            int extra_len = len - k;

            from->out_path_len = mc_packet_copy_path(from->out_path, path, path_len);
            from->lastmod = mc_mesh_now();
            if (s_cb.on_path_updated) s_cb.on_path_updated(from);

            // Send a reciprocal returned path so the peer learns the route
            // back to us (upstream onPeerPathRecv -> createPathReturn).
            if (mc_packet_is_route_flood(pkt)) {
                mc_packet_t rpath;
                if (create_path_return(&rpath, from->pub_key, secret, pkt->path,
                                       (uint8_t)pkt->path_len, 0, NULL, 0)) {
                    send_direct(&rpath, path, path_len);
                }
            }

            if (extra_type == MC_PAYLOAD_TYPE_ACK && extra_len >= 4) {
                uint32_t ack;
                memcpy(&ack, extra, 4);
                mc_contact_t *pc = pending_ack_take(ack);
                if (s_cb.on_ack) s_cb.on_ack(pc ? pc : from, ack);
            } else if (extra_type == MC_PAYLOAD_TYPE_RESPONSE && extra_len > 0) {
                // Application response carried inside a path return.
                if (s_cb.on_contact_response) {
                    s_cb.on_contact_response(from, extra, (uint8_t)extra_len, pkt->snr / 4.0f,
                                             (uint8_t)pkt->path_len, mc_packet_is_route_flood(pkt));
                }
            }
        } else if (ptype == MC_PAYLOAD_TYPE_REQ && len > 4) {
            // Companion does not answer requests; ignore.
        } else if (ptype == MC_PAYLOAD_TYPE_RESPONSE && len > 0) {
            // Binary application response (tag + payload); the companion layer
            // matches it against pending requests and pushes to the app.
            if (s_cb.on_contact_response) {
                s_cb.on_contact_response(from, data, (uint8_t)len, pkt->snr / 4.0f,
                                         (uint8_t)pkt->path_len, mc_packet_is_route_flood(pkt));
            }
            // Peer still flooded a response though we hold a direct path.
            if (mc_packet_is_route_flood(pkt)) {
                return_path_retry(from, pkt->path, (uint8_t)pkt->path_len);
            }
        }
        break;
    }
}

static void handle_ack(mc_packet_t *pkt) {
    if (pkt->payload_len < 4) return;
    uint32_t ack;
    memcpy(&ack, pkt->payload, 4);
    mc_contact_t *c = pending_ack_take(ack);
    if (s_cb.on_ack) s_cb.on_ack(c, ack);
    // ACK arrived over flood but we already have a direct path: refresh it.
    if (c && mc_packet_is_route_flood(pkt)) {
        return_path_retry(c, pkt->path, (uint8_t)pkt->path_len);
    }
}

void mc_mesh_on_rx(const uint8_t *frame, uint8_t len, int16_t rssi, float snr) {
    if (!s_inited || !frame || len == 0) return;
    mc_packet_t pkt;
    mc_packet_init(&pkt);
    if (!mc_packet_read_from(&pkt, frame, len)) {
        s_stats.rx_bad++;
        return;
    }
    pkt.snr = (int8_t)(snr * 4.0f);
    s_stats.last_rssi = rssi;
    s_stats.last_snr = snr;

    if (dedup_seen(&pkt)) {
        s_stats.rx_dup++;
        return;
    }
    dedup_mark(&pkt);

    s_stats.rx_ok++;
    switch (mc_packet_payload_type(&pkt)) {
        case MC_PAYLOAD_TYPE_ADVERT:
            handle_advert(&pkt);
            break;
        case MC_PAYLOAD_TYPE_GRP_TXT:
        case MC_PAYLOAD_TYPE_GRP_DATA:
            handle_group(&pkt);
            break;
        case MC_PAYLOAD_TYPE_TXT_MSG:
        case MC_PAYLOAD_TYPE_REQ:
        case MC_PAYLOAD_TYPE_RESPONSE:
        case MC_PAYLOAD_TYPE_PATH:
            handle_direct(&pkt, mc_packet_payload_type(&pkt));
            break;
        case MC_PAYLOAD_TYPE_ACK:
            handle_ack(&pkt);
            break;
        default:
            break;
    }

    // Route after local handling, like upstream (routeRecvPacket last), so the
    // appended hop does not leak into the handler's view of the path.
    maybe_forward(&pkt);
}

// ---------------------------------------------------------------------------
// Import/export + misc
// ---------------------------------------------------------------------------

bool mc_mesh_import_contact(const uint8_t *src, uint8_t len) {
    mc_packet_t pkt;
    mc_packet_init(&pkt);
    if (!mc_packet_read_from(&pkt, src, len)) return false;
    if (mc_packet_payload_type(&pkt) != MC_PAYLOAD_TYPE_ADVERT) return false;
    pkt.header = (uint8_t)((pkt.header & ~MC_PH_ROUTE_MASK) | MC_ROUTE_TYPE_FLOOD);
    handle_advert(&pkt);
    return true;
}

uint8_t mc_mesh_export_contact(const mc_contact_t *contact, uint8_t *dest_buf) {
    if (!contact) return 0;
    uint8_t n = 0;
    if (!mc_mesh_get_advert_blob(contact->pub_key, dest_buf, &n)) return 0;
    return n;
}

uint8_t mc_mesh_export_self(uint8_t *dest_buf) {
    if (!dest_buf) return 0;
    mc_packet_t pkt;
    if (!create_advert(&pkt)) return 0;
    pkt.header = (uint8_t)((pkt.header & ~MC_PH_ROUTE_MASK) | MC_ROUTE_TYPE_FLOOD);
    return mc_packet_write_to(&pkt, dest_buf);
}

bool mc_mesh_share_contact(const mc_contact_t *contact) {
    if (!contact) return false;
    uint8_t raw[MC_MAX_TRANS_UNIT];
    uint8_t n = 0;
    if (!mc_mesh_get_advert_blob(contact->pub_key, raw, &n)) return false;
    mc_packet_t pkt;
    mc_packet_init(&pkt);
    if (!mc_packet_read_from(&pkt, raw, n)) return false;
    send_zero_hop(&pkt, 0, 0);
    return true;
}

void mc_mesh_sign(const uint8_t *data, size_t len, uint8_t sig[MC_SIGNATURE_SIZE]) {
    mc_identity_sign(data, len, sig);
}

void mc_mesh_get_stats(mc_mesh_stats_t *out) {
    if (!out) return;
    *out = s_stats;
    out->node_count = mc_mesh_contact_count();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void mc_mesh_set_tx(mc_tx_fn tx, void *ctx) {
    s_tx = tx;
    s_tx_ctx = ctx;
}

void mc_mesh_set_callbacks(const mc_mesh_callbacks_t *cb) {
    if (cb) s_cb = *cb;
    else memset(&s_cb, 0, sizeof(s_cb));
}

bool mc_mesh_init(void) {
    if (s_inited) return true;
    if (!s_contacts) {
        s_contacts = mc_alloc(MC_CONTACT_SLOTS * sizeof(mc_contact_t));
        s_channels = mc_alloc(MC_MAX_GROUP_CHANNELS * sizeof(mc_channel_t));
        s_dedup = mc_alloc(MC_DEDUP_HASHES * MC_MAX_HASH_SIZE);
        s_blob_cache = mc_alloc(MC_ADVERT_BLOB_CACHE * sizeof(mc_advert_blob_t));
        s_advert_paths = mc_alloc(MC_ADVERT_PATH_SLOTS * sizeof(mc_advert_path_t));
        if (!mesh_allocated()) {
            mc_mesh_deinit();
            return false;
        }
    }
    // mc_alloc() zeroes, so the tables are already empty.
    memset(&s_stats, 0, sizeof(s_stats));
    memset(s_pending_acks, 0, sizeof(s_pending_acks));
    s_pending_ack_next = 0;
    s_blob_next = 0;
    s_dedup_next = 0;
    s_advert_path_next = 0;

    // Restore the persisted advert blob cache (share/export survives reboot).
    size_t blen = 0;
    if (s_blob_cache) {
        (void)mc_store_load_blob(MC_BLOB_KEY_ADVERT, s_blob_cache,
                                 MC_ADVERT_BLOB_CACHE * sizeof(mc_advert_blob_t), &blen);
    }

    // One retransmit slot for opt-in forwarding; failure just disables repeat.
    if (!s_fwd_timer) {
        esp_timer_create_args_t fa = {.callback = fwd_timer_cb, .name = "mc_fwd"};
        (void)esp_timer_create(&fa, &s_fwd_timer);
    }
    s_fwd_pending = false;

    mc_identity_init();

    mc_store_load_prefs(&s_prefs);
    s_prefs_loaded = true;

    s_num_contacts = MC_MAX_ANON_CONTACTS;
    int loaded = 0;
    if (mc_store_load_contacts(&s_contacts[MC_MAX_ANON_CONTACTS], &loaded, MC_MAX_CONTACTS)) {
        s_num_contacts += loaded;
    }

    // Stock MyMesh::begin() pre-configures channel 0 as "Public" with the
    // well-known key, then loads stored channels over it.
    snprintf(s_channels[0].name, sizeof(s_channels[0].name), "Public");
    memcpy(s_channels[0].secret, mc_public_channel_key, sizeof(mc_public_channel_key));
    mc_calc_channel_hash(&s_channels[0].hash, s_channels[0].secret, 16);
    mc_store_load_channels(s_channels);

    s_inited = true;
    return true;
}

void mc_mesh_deinit(void) {
    if (s_fwd_timer) { esp_timer_delete(s_fwd_timer); s_fwd_timer = NULL; }
    s_fwd_pending = false;
    mc_free(s_contacts);      s_contacts = NULL;
    mc_free(s_channels);      s_channels = NULL;
    mc_free(s_dedup);         s_dedup = NULL;
    mc_free(s_blob_cache);    s_blob_cache = NULL;
    mc_free(s_advert_paths);  s_advert_paths = NULL;
    s_num_contacts = 0;
    s_dedup_next = 0;
    s_blob_next = 0;
    s_advert_path_next = 0;
    s_inited = false;
}

#else
typedef int meshcore_mesh_stub_guard;
#endif // CONFIG_HAS_MESHCORE
