// meshcore_manager.c
// MeshCore lifecycle, radio ownership, chat ring and self test.

#include "managers/meshcore_config.h"
#include "sdkconfig.h"

#ifdef CONFIG_HAS_MESHCORE

#include "managers/meshcore_manager.h"

#include <stdio.h>
#include <string.h>

#include "core/glog.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "managers/lora_manager.h"
#include "managers/lora_sx1262.h"
#include "managers/meshcore_ble.h"
#include "managers/meshcore_alloc.h"
#include "managers/meshcore_companion.h"
#include "managers/meshcore_crypto.h"
#include "managers/meshcore_identity.h"
#include "managers/meshcore_mesh.h"
#include "managers/meshcore_store.h"

static const char *TAG = "MeshCore";

static bool s_running;
static bool s_radio_present;
static const char *s_last_error = "none";

static mc_msg_t *s_msgs;      // MC_MSG_RING, allocated while running
static uint16_t s_msg_head; // next write index
static uint16_t s_msg_count;
static uint32_t s_msg_seq;

static void push_msg(const mc_msg_t *m) {
    if (!s_msgs) return;
    s_msgs[s_msg_head] = *m;
    s_msg_head = (uint16_t)((s_msg_head + 1) % MC_MSG_RING);
    if (s_msg_count < MC_MSG_RING) s_msg_count++;
    s_msg_seq++;
}

static void msg_index_to_slot(uint16_t index, uint16_t *slot) {
    uint16_t start = (uint16_t)((s_msg_head + MC_MSG_RING - s_msg_count) % MC_MSG_RING);
    *slot = (uint16_t)((start + index) % MC_MSG_RING);
}

uint16_t mc_manager_msg_count(void) { return s_msg_count; }

bool mc_manager_msg_at(uint16_t index, mc_msg_t *out) {
    if (!out || !s_msgs || index >= s_msg_count) return false;
    uint16_t slot;
    msg_index_to_slot(index, &slot);
    *out = s_msgs[slot];
    return true;
}

bool mc_manager_latest_message(mc_msg_t *out, uint32_t *out_seq) {
    if (!out || !s_msgs || s_msg_count == 0) {
        if (out_seq) *out_seq = 0;
        return false;
    }
    uint16_t slot;
    msg_index_to_slot((uint16_t)(s_msg_count - 1), &slot);
    *out = s_msgs[slot];
    if (out_seq) *out_seq = s_msg_seq;
    return true;
}

uint16_t mc_manager_msg_since(uint32_t *io_seq, mc_msg_t *out, uint16_t max) {
    if (!io_seq || !out || !s_msgs || max == 0) return 0;
    if (*io_seq == 0) *io_seq = s_msg_seq - s_msg_count;
    uint16_t wrote = 0;
    for (uint16_t i = 0; i < s_msg_count && wrote < max; ++i) {
        uint16_t slot;
        msg_index_to_slot(i, &slot);
        uint32_t seq = s_msg_seq - s_msg_count + i + 1;
        if (seq <= *io_seq) continue;
        out[wrote++] = s_msgs[slot];
        *io_seq = seq;
    }
    return wrote;
}

// ---------------------------------------------------------------------------
// Mesh callbacks
// ---------------------------------------------------------------------------

static void on_channel_message(uint8_t channel_idx, uint32_t timestamp,
                               const char *text, float snr, uint8_t path_len, bool is_flood) {
    mc_companion_on_channel_message(channel_idx, timestamp, text, snr, path_len, is_flood);
    mc_msg_t m;
    memset(&m, 0, sizeof(m));
    // Group text is "sender: message".
    const char *sep = strstr(text, ": ");
    if (sep) {
        size_t n = (size_t)(sep - text);
        if (n > sizeof(m.who) - 1) n = sizeof(m.who) - 1;
        memcpy(m.who, text, n);
        m.who[n] = 0;
        snprintf(m.text, sizeof(m.text), "%.159s", sep + 2);
    } else {
        snprintf(m.who, sizeof(m.who), "ch%u", (unsigned)channel_idx);
        snprintf(m.text, sizeof(m.text), "%.159s", text);
    }
    m.channel = channel_idx;
    m.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    m.direct = false;
    push_msg(&m);
    glog("[MC ch%u] %s: %s (snr %.1f, %u hops)\n", (unsigned)channel_idx, m.who, m.text,
         (double)snr, (unsigned)(is_flood ? (path_len & 63) : 0));
}

static void on_contact_message(const mc_contact_t *from, uint32_t timestamp,
                               const char *text, uint8_t txt_type, float snr,
                               uint8_t path_len, bool is_flood) {
    mc_companion_on_contact_message(from, timestamp, text, txt_type, snr, path_len, is_flood);
    if (txt_type == 0xFF) return; // raw response, not a chat line
    mc_msg_t m;
    memset(&m, 0, sizeof(m));
    snprintf(m.who, sizeof(m.who), "%.23s", from ? from->name : "?");
    snprintf(m.text, sizeof(m.text), "%s", text);
    m.channel = 0;
    m.node_hash = from ? from->pub_key[0] : 0;
    m.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    m.direct = true;
    push_msg(&m);
    glog("[MC dm] %s: %s (snr %.1f)\n", m.who, m.text, (double)snr);
}

static void on_signed_message(const mc_contact_t *from, uint32_t timestamp,
                              const uint8_t *sender_prefix, const char *text,
                              float snr, uint8_t path_len, bool is_flood) {
    (void)path_len; (void)is_flood;
    mc_companion_on_signed_message(from, timestamp, sender_prefix, text, snr, path_len, is_flood);
    mc_msg_t m;
    memset(&m, 0, sizeof(m));
    snprintf(m.who, sizeof(m.who), "%.23s", from ? from->name : "?");
    snprintf(m.text, sizeof(m.text), "%s", text);
    m.channel = 0;
    m.node_hash = from ? from->pub_key[0] : 0;
    m.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    m.direct = true;
    push_msg(&m);
    glog("[MC dm*] %s: %s (signed, snr %.1f)\n", m.who, m.text, (double)snr);
    // sync_since advanced: persist so signed-message ordering survives reboot.
    (void)mc_mesh_save_contacts();
}

static void on_channel_data(uint8_t channel_idx, uint16_t data_type,
                            const uint8_t *data, uint8_t len, float snr,
                            uint8_t path_len, bool is_flood) {
    mc_companion_on_channel_data(channel_idx, data_type, data, len, snr, path_len, is_flood);
    glog("[MC ch%u] data type=0x%04X len=%u snr=%.1f\n", (unsigned)channel_idx,
         (unsigned)data_type, (unsigned)len, (double)snr);
    (void)data;
}

static void on_advert(const mc_contact_t *contact, bool is_new) {
    mc_companion_on_advert(contact, is_new);
    glog("[MC] advert %s %s\n", is_new ? "new" : "update", contact ? contact->name : "?");
    if (is_new) mc_mesh_save_contacts();
}

static void on_ack(const mc_contact_t *from, uint32_t ack) {
    mc_companion_on_ack(from, ack);
    glog("[MC] ack from %s\n", from ? from->name : "?");
}

static void on_contact_response(const mc_contact_t *from, const uint8_t *data, uint8_t len,
                                float snr, uint8_t path_len, bool is_flood) {
    mc_companion_on_contact_response(from, data, len, snr, path_len, is_flood);
    glog("[MC] response from %s (%u bytes)\n", from ? from->name : "?", (unsigned)len);
}

static void on_path_updated(const mc_contact_t *contact) {
    mc_companion_on_path_updated(contact);
    glog("[MC] path updated for %s\n", contact ? contact->name : "?");
}

static void on_contact_deleted(const uint8_t *pub_key) {
    mc_companion_on_contact_deleted(pub_key);
    glog("[MC] contact overwritten (oldest non-favourite)\n");
}

// ---------------------------------------------------------------------------
// Radio plumbing
// ---------------------------------------------------------------------------

static void mc_tx(const uint8_t *frame, uint8_t len, void *ctx) {
    (void)ctx;
    if (!lora_radio_is_ready()) return;
    if (lora_radio_send(frame, len) != 0) {
        ESP_LOGW(TAG, "tx failed len=%u", (unsigned)len);
    }
}

static void mc_rx(const uint8_t *payload, uint8_t len, int16_t rssi, float snr, void *ctx) {
    (void)ctx;
    mc_mesh_on_rx(payload, len, rssi, snr);
}

static void install_callbacks(void) {
    mc_mesh_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.on_channel_message = on_channel_message;
    cb.on_contact_message = on_contact_message;
    cb.on_signed_message = on_signed_message;
    cb.on_channel_data = on_channel_data;
    cb.on_advert = on_advert;
    cb.on_ack = on_ack;
    cb.on_contact_response = on_contact_response;
    cb.on_path_updated = on_path_updated;
    cb.on_contact_deleted = on_contact_deleted;
    mc_mesh_set_callbacks(&cb);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void mc_manager_early_init(void) {
    mc_identity_init();
}

bool mc_manager_is_running(void) { return s_running; }

const char *mc_manager_last_error(void) { return s_last_error; }

// Preferred backend, persisted under NVS ns "mesh" key "mode" (same as the
// `mesh` CLI). 0 = Meshtastic, 1 = MeshCore.
#define MC_BACKEND_NVS_NS   "mesh"
#define MC_BACKEND_NVS_KEY  "mode"

bool mc_manager_default_backend_meshcore(void) {
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(MC_BACKEND_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, MC_BACKEND_NVS_KEY, &v);
        nvs_close(h);
    }
    return v != 0;
}

void mc_manager_set_default_backend_meshcore(bool meshcore) {
    nvs_handle_t h;
    if (nvs_open(MC_BACKEND_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_u8(h, MC_BACKEND_NVS_KEY, meshcore ? 1 : 0);
    (void)nvs_commit(h);
    nvs_close(h);
}

// Per-run state: mesh tables, companion queue and the chat ring. Allocated
// when the stack starts and released on stop so an idle (or Meshtastic-running)
// board pays nothing for it. The mesh tables are PSRAM-backed where available.
static bool alloc_state(void) {
    if (s_msgs) return true;
    if (!mc_mesh_init()) {
        s_last_error = "out of memory (mesh state)";
        return false;
    }
    if (!mc_companion_init()) {
        mc_mesh_deinit();
        s_last_error = "out of memory (companion state)";
        return false;
    }
    s_msgs = mc_alloc(MC_MSG_RING * sizeof(mc_msg_t));
    if (!s_msgs) {
        mc_companion_deinit();
        mc_mesh_deinit();
        s_last_error = "out of memory (chat ring)";
        return false;
    }
    s_msg_head = 0;
    s_msg_count = 0;
    s_msg_seq = 0;
    return true;
}

static void free_state(void) {
    mc_free(s_msgs);
    s_msgs = NULL;
    s_msg_head = 0;
    s_msg_count = 0;
    s_msg_seq = 0;
    mc_companion_deinit();
    mc_mesh_deinit();
}

// Build the SX126x profile from the current prefs, clamped to the board's TX
// limit. Shared by start and the in-place re-tune.
static bool build_profile(lora_hw_t *hw, lora_radio_profile_t *prof) {
    if (!lora_manager_get_hw(hw)) {
        s_last_error = "no board config";
        return false;
    }
    mc_prefs_t *p = mc_mesh_prefs();
    int tx = p->tx_dbm;
    if (tx > hw->max_tx_dbm) tx = hw->max_tx_dbm;
    if (tx > 22) tx = 22;
    if (tx < -9) tx = -9;
    *prof = (lora_radio_profile_t){
        .freq_hz = (uint32_t)(p->freq_mhz * 1000000.0f + 0.5f),
        .sf = p->sf,
        .bw_khz_x10 = (int)(p->bw_khz * 10.0f + 0.5f),
        .cr_denom = p->cr,
        .tx_dbm = tx,
        .sync_reg0 = MC_SYNC_WORD_REG_0,
        .sync_reg1 = MC_SYNC_WORD_REG_1,
        .preamble_len = (p->sf <= 8) ? MC_PREAMBLE_SF_LE8 : MC_PREAMBLE_SF_GT8,
        .ldro = -1,
    };
    return true;
}

bool mc_manager_start(void) {
    if (s_running) return true;
    s_last_error = "none";

    // One radio: make sure Meshtastic has released it.
    if (lora_manager_is_running()) {
        lora_manager_stop();
    }

    if (!alloc_state()) {
        ESP_LOGE(TAG, "state allocation failed: %s (free internal heap %u)",
                 s_last_error, (unsigned)xPortGetFreeHeapSize());
        return false;
    }
    install_callbacks();
    mc_mesh_set_tx(mc_tx, NULL);

    lora_hw_t hw;
    lora_radio_profile_t prof;
    if (!build_profile(&hw, &prof)) {
        free_state();
        return false;
    }
    if (lora_radio_init_profile(&hw, &prof) != 0) {
        s_last_error = lora_radio_step();
        free_state();
        return false;
    }
    s_radio_present = true;
    if (lora_radio_start_rx(mc_rx, NULL) != 0) {
        s_last_error = lora_radio_step();
        ESP_LOGE(TAG, "RX start failed at stage '%s' (free internal heap %u)",
                 lora_radio_step(), (unsigned)xPortGetFreeHeapSize());
        lora_radio_deinit();
        s_radio_present = false;
        free_state();
        return false;
    }
    s_running = true;
    // Advertise the MeshCore companion service (best-effort; the radio works
    // headless without it).
    if (mc_ble_start()) {
        ESP_LOGI(TAG, "BLE companion advertising");
    } else {
        ESP_LOGW(TAG, "BLE companion unavailable (radio-only mode)");
    }
    ESP_LOGI(TAG, "MeshCore started %.3fMHz BW%.1f SF%u CR4/%u",
             (double)prof.freq_hz / 1000000.0, (double)prof.bw_khz_x10 / 10.0,
             (unsigned)prof.sf, (unsigned)prof.cr_denom);
    return true;
}

// Re-tune the live radio from the current prefs without touching BLE, the
// companion state or the chat ring. Used by SET_RADIO_PARAMS so a region change
// no longer tears down the BLE link (and the Wi-Fi AP restore that follows it).
bool mc_manager_reconfigure_radio(void) {
    if (!s_running) return false;
    lora_radio_stop();
    lora_radio_deinit();
    s_radio_present = false;

    lora_hw_t hw;
    lora_radio_profile_t prof;
    if (!build_profile(&hw, &prof)) return false;
    if (lora_radio_init_profile(&hw, &prof) != 0) {
        s_last_error = lora_radio_step();
        return false;
    }
    s_radio_present = true;
    if (lora_radio_start_rx(mc_rx, NULL) != 0) {
        s_last_error = lora_radio_step();
        ESP_LOGE(TAG, "radio re-tune RX start failed at stage '%s'", lora_radio_step());
        lora_radio_deinit();
        s_radio_present = false;
        return false;
    }
    ESP_LOGI(TAG, "radio re-tuned %.3fMHz BW%.1f SF%u CR4/%u",
             (double)prof.freq_hz / 1000000.0, (double)prof.bw_khz_x10 / 10.0,
             (unsigned)prof.sf, (unsigned)prof.cr_denom);
    return true;
}

void mc_manager_stop(void) {
    if (!s_running) return;
    mc_ble_stop();
    lora_radio_stop();
    lora_radio_deinit();
    s_running = false;
    s_radio_present = false;
    free_state();
}

void mc_manager_get_status(mc_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->running = s_running;
    out->radio_present = s_radio_present;
    const mc_prefs_t *p = mc_mesh_prefs();
    out->freq_mhz = p->freq_mhz;
    out->bw_khz = p->bw_khz;
    out->sf = p->sf;
    out->cr = p->cr;
    out->tx_dbm = p->tx_dbm;
    mc_mesh_stats_t st;
    mc_mesh_get_stats(&st);
    out->tx_ok = st.tx_ok;
    out->tx_fail = st.tx_fail;
    out->rx_ok = st.rx_ok;
    out->rx_dup = st.rx_dup;
    out->rx_bad = st.rx_bad;
    out->last_rssi = st.last_rssi;
    out->last_snr = st.last_snr;
    out->node_count = st.node_count;
}

// ---------------------------------------------------------------------------
// CLI helpers
// ---------------------------------------------------------------------------

bool mc_manager_send_text(const char *text) {
    return mc_manager_send_channel_text(0, text);
}

bool mc_manager_send_channel_text(uint8_t channel, const char *text) {
    if (!s_running) { s_last_error = "not running"; return false; }
    bool ok = mc_mesh_send_group_text(channel, text, mc_mesh_now());
    if (!ok) {
        s_last_error = "channel not set";
        return false;
    }
    // Mirror the sent line into the chat ring so the on-device view shows our
    // own channel messages (Meshtastic's ring does this).
    mc_msg_t m;
    memset(&m, 0, sizeof(m));
    snprintf(m.text, sizeof(m.text), "%s", text ? text : "");
    m.channel = channel;
    m.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    m.outgoing = true;
    m.direct = false;
    m.read = true;
    push_msg(&m);
    return true;
}

static bool parse_pubkey_prefix(const char *s, uint8_t *out, int *out_len) {
    if (!s) return false;
    if (s[0] == '!') s++;
    size_t n = strlen(s);
    if (n < 8 || n > 64) return false;
    int bytes = (int)(n / 2);
    for (int i = 0; i < bytes; ++i) {
        int hi, lo;
        char a = s[i * 2], b = s[i * 2 + 1];
        hi = (a >= '0' && a <= '9') ? a - '0' : (a >= 'a' && a <= 'f') ? a - 'a' + 10 :
             (a >= 'A' && a <= 'F') ? a - 'A' + 10 : -1;
        lo = (b >= '0' && b <= '9') ? b - '0' : (b >= 'a' && b <= 'f') ? b - 'a' + 10 :
             (b >= 'A' && b <= 'F') ? b - 'A' + 10 : -1;
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = bytes;
    return true;
}

static bool send_dm_to_contact(const mc_contact_t *c, const char *text) {
    uint32_t ack = 0, timeout = 0;
    int rc = mc_mesh_send_direct_text(c, mc_mesh_now(), 0, MC_TXT_TYPE_PLAIN, text, &ack, &timeout);
    if (rc == MC_MSG_SEND_FAILED) { s_last_error = "send failed"; return false; }
    mc_msg_t m;
    memset(&m, 0, sizeof(m));
    snprintf(m.who, sizeof(m.who), "%.23s", c->name);
    snprintf(m.text, sizeof(m.text), "%s", text);
    m.node_hash = c->pub_key[0];
    m.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    m.direct = true;
    m.outgoing = true;
    m.read = true;
    push_msg(&m);
    return true;
}

bool mc_manager_send_dm(const char *peer, const char *text) {
    if (!s_running) { s_last_error = "not running"; return false; }
    const mc_contact_t *c = NULL;
    uint8_t pk[32];
    int pklen = 0;
    if (parse_pubkey_prefix(peer, pk, &pklen)) {
        c = mc_mesh_find_contact_pubkey(pk, pklen);
    }
    if (!c) {
        mc_contact_t *found = mc_mesh_find_contact_name(peer);
        c = found;
    }
    if (!c) { s_last_error = "contact not found"; return false; }
    return send_dm_to_contact(c, text);
}

bool mc_manager_send_dm_hash(uint8_t peer_hash, const char *text) {
    if (!s_running) { s_last_error = "not running"; return false; }
    uint8_t prefix = peer_hash;
    const mc_contact_t *c = mc_mesh_find_contact_pubkey(&prefix, 1);
    if (!c) { s_last_error = "contact not found"; return false; }
    return send_dm_to_contact(c, text);
}

void mc_manager_chat_read(uint32_t peer_hash) {
    if (!s_msgs) return;
    for (uint16_t i = 0; i < s_msg_count; ++i) {
        uint16_t slot;
        msg_index_to_slot(i, &slot);
        mc_msg_t *m = &s_msgs[slot];
        bool in_conversation = peer_hash ? (m->direct && m->node_hash == peer_hash) : !m->direct;
        if (in_conversation) m->read = true;
    }
}

bool mc_manager_send_advert(bool flood) {
    if (!s_running) { s_last_error = "not running"; return false; }
    return mc_mesh_send_advert(flood);
}

// ---------------------------------------------------------------------------
// Self test (known-answer vectors; mirrors the host test harness)
// ---------------------------------------------------------------------------

static int hexeq(const uint8_t *got, size_t n, const char *want) {
    char buf[160];
    mc_to_hex(buf, got, n);
    return strcmp(buf, want) == 0;
}

uint8_t mc_manager_selftest(void) {
    uint8_t fail = 0;

    // SHA-256("abc")
    uint8_t h[32];
    mc_sha256(h, 32, (const uint8_t *)"abc", 3);
    if (!hexeq(h, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")) fail |= 0x01;

    // HMAC-SHA256 RFC 4231 case 1
    uint8_t key[20];
    memset(key, 0x0b, sizeof(key));
    mc_hmac_sha256(h, 32, key, sizeof(key), (const uint8_t *)"Hi There", 8);
    if (!hexeq(h, 32, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7")) fail |= 0x02;

    // AES-128 FIPS-197 + EncryptThenMAC round trip
    uint8_t secret[32];
    for (int i = 0; i < 32; ++i) secret[i] = (uint8_t)i;
    uint8_t src[16];
    for (int i = 0; i < 16; ++i) src[i] = (uint8_t)i;
    uint8_t ct[64], back[64];
    int n = mc_encrypt_then_mac(secret, ct, src, sizeof(src));
    int m = mc_mac_then_decrypt(secret, back, ct, n);
    if (n != 2 + 16 || m < 16 || memcmp(back, src, 16) != 0) fail |= 0x04;

    // Channel hash + hashtag derivation
    uint8_t chash;
    mc_calc_channel_hash(&chash, mc_public_channel_key, 16);
    if (chash != 0x11) fail |= 0x08;
    uint8_t hk[16];
    mc_hashtag_channel_key(hk, "#test");
    if (!hexeq(hk, 16, "9cd8fcf22a47333b591d96a2b848b73f")) fail |= 0x10;

    // Ed25519 RFC 8032 test 1 public key is covered by the host harness; the
    // firmware self-test focuses on the on-device library set.

    return fail;
}

#else
typedef int meshcore_manager_stub_guard;
#endif // CONFIG_HAS_MESHCORE
