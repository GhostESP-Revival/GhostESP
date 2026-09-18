// meshcore_mesh.h
// MeshCore mesh behaviour for a companion node (no repeater/relay).
//
// Mirrors the relevant parts of firmware v1.17.1 src/Mesh.cpp and
// src/helpers/BaseChatMesh.cpp: adverts, contact discovery, 8 group channels,
// group text/data, direct text with ACK + returned-path learning, and a
// packet-hash dedup ring. Packet forwarding is disabled (companion role).

#ifndef MESHCORE_MESH_H
#define MESHCORE_MESH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "managers/meshcore_config.h"
#include "managers/meshcore_identity.h"
#include "managers/meshcore_store.h"

#ifdef __cplusplus
extern "C" {
#endif

// Result codes for direct sends (upstream MSG_SEND_*).
#define MC_MSG_SEND_FAILED      0
#define MC_MSG_SEND_SENT_FLOOD  1
#define MC_MSG_SEND_SENT_DIRECT 2

typedef struct {
    void (*on_channel_message)(uint8_t channel_idx, uint32_t timestamp,
                               const char *text, float snr, uint8_t path_len, bool is_flood);
    void (*on_contact_message)(const mc_contact_t *from, uint32_t timestamp,
                               const char *text, uint8_t txt_type, float snr,
                               uint8_t path_len, bool is_flood);
    // Signed plain text (TXT_TYPE_SIGNED_PLAIN): `sender_prefix` is the 4-byte
    // peer prefix the app uses to verify the detached signature.
    void (*on_signed_message)(const mc_contact_t *from, uint32_t timestamp,
                              const uint8_t *sender_prefix, const char *text,
                              float snr, uint8_t path_len, bool is_flood);
    void (*on_channel_data)(uint8_t channel_idx, uint16_t data_type,
                            const uint8_t *data, uint8_t len, float snr,
                            uint8_t path_len, bool is_flood);
    void (*on_advert)(const mc_contact_t *contact, bool is_new);
    void (*on_ack)(const mc_contact_t *from, uint32_t ack);
    // Binary application response (PAYLOAD_TYPE_RESPONSE), with explicit length
    // (payload is binary and may contain NULs).
    void (*on_contact_response)(const mc_contact_t *from, const uint8_t *data,
                                uint8_t len, float snr, uint8_t path_len, bool is_flood);
    void (*on_path_updated)(const mc_contact_t *contact);
    void (*on_contact_deleted)(const uint8_t *pub_key);
} mc_mesh_callbacks_t;

typedef struct {
    uint32_t tx_ok;
    uint32_t tx_fail;
    uint32_t rx_ok;
    uint32_t rx_dup;
    uint32_t rx_bad;
    uint32_t rx_group;
    uint32_t rx_direct;
    uint32_t rx_advert;
    int16_t last_rssi;
    float last_snr;
    int node_count;
} mc_mesh_stats_t;

// TX sink supplied by the manager: serializes a frame to the radio.
typedef void (*mc_tx_fn)(const uint8_t *frame, uint8_t len, void *ctx);

// Allocate state tables and load prefs/contacts/channels. Returns false if the
// memory could not be obtained (nothing is left allocated in that case).
bool mc_mesh_init(void);
// Release all state tables. Safe to call when already deinitialised.
void mc_mesh_deinit(void);
void mc_mesh_set_tx(mc_tx_fn tx, void *ctx);
void mc_mesh_set_callbacks(const mc_mesh_callbacks_t *cb);
void mc_mesh_on_rx(const uint8_t *frame, uint8_t len, int16_t rssi, float snr);
void mc_mesh_get_stats(mc_mesh_stats_t *out);

// Clock (Unix epoch seconds). set_time anchors the software clock to the host
// time received over the companion protocol; now() returns the current value.
void mc_mesh_set_time(uint32_t epoch_secs);
uint32_t mc_mesh_now(void);
// Monotonic clock (strictly increasing within the same second), matching
// upstream RTCClock::getCurrentTimeUnique().
uint32_t mc_mesh_now_unique(void);
// Estimated on-air time for a frame of the given length, in milliseconds.
uint32_t mc_mesh_airtime_ms(int len_bytes);

const mc_identity_t *mc_mesh_self(void);
const char *mc_mesh_node_name(void);
bool mc_mesh_set_node_name(const char *name);

mc_prefs_t *mc_mesh_prefs(void);
bool mc_mesh_save_prefs(void);
bool mc_mesh_apply_radio(float freq_mhz, float bw_khz, uint8_t sf, uint8_t cr);
bool mc_mesh_set_tx_power(int8_t dbm);

// Region scoping (Flood scope transport codes).
// send_scope overrides the default scope for message floods; NULL/zero clears
// it. `unscoped` forces plain floods regardless of scope.
void mc_mesh_set_send_scope(const uint8_t *key16);
void mc_mesh_set_send_unscoped(bool on);
// Fills name31 (NUL-padded, 31 bytes) and key16; false when no scope is set.
bool mc_mesh_get_default_scope(char *name31, uint8_t key16[16]);
// name NULL/empty clears the default scope.
void mc_mesh_set_default_scope(const char *name, const uint8_t *key16);

// Advert TX.
bool mc_mesh_send_advert(bool flood);

// Group text/data TX. `path_len` == MC_OUT_PATH_UNKNOWN floods.
// The _len variants take an explicit text length: companion frames are binary
// and not NUL-terminated, so strlen() would read past the frame.
bool mc_mesh_send_group_text(uint8_t channel_idx, const char *text, uint32_t timestamp);
bool mc_mesh_send_group_text_len(uint8_t channel_idx, const char *text, int text_len,
                                 uint32_t timestamp);
bool mc_mesh_send_group_data(uint8_t channel_idx, uint8_t path_len, const uint8_t *path,
                             uint16_t data_type, const uint8_t *data, int data_len);

// Direct (contact) text TX. `txt_type` is MC_TXT_TYPE_PLAIN or
// MC_TXT_TYPE_CLI_DATA; `expected_ack` is only set for plain text.
int mc_mesh_send_direct_text(const mc_contact_t *to, uint32_t timestamp, uint8_t attempt,
                             uint8_t txt_type, const char *text, uint32_t *expected_ack,
                             uint32_t *est_timeout);
int mc_mesh_send_direct_text_len(const mc_contact_t *to, uint32_t timestamp, uint8_t attempt,
                                 uint8_t txt_type, const char *text, int text_len,
                                 uint32_t *expected_ack, uint32_t *est_timeout);

// Raw frame TX (companion CMD_SEND_RAW_PACKET).
bool mc_mesh_send_raw_frame(const uint8_t *frame, uint8_t len, uint8_t priority);

// Request TX (companion CMD_SEND_STATUS_REQ / _TELEMETRY_REQ / _BINARY_REQ).
// Payload is tag(4) + req_data. Returns MC_MSG_SEND_*, filling tag + timeout.
int mc_mesh_send_request(const mc_contact_t *to, const uint8_t *req_data, uint8_t len,
                         uint32_t *out_tag, uint32_t *out_timeout);
// Convenience for a bare request type (composes the 13-byte GET_STATUS/etc blob).
int mc_mesh_send_request_type(const mc_contact_t *to, uint8_t req_type,
                              uint32_t *out_tag, uint32_t *out_timeout);

// Contacts.
int mc_mesh_contact_count(void);                 // real contacts only
const mc_contact_t *mc_mesh_contact_at(int idx); // 0..count-1 (real contacts)
mc_contact_t *mc_mesh_find_contact_pubkey(const uint8_t *pub_key, int prefix_len);
mc_contact_t *mc_mesh_find_contact_name(const char *name);
bool mc_mesh_add_contact(const mc_contact_t *contact);
bool mc_mesh_remove_contact(const mc_contact_t *contact);
// Persist the current contact list to NVS.
bool mc_mesh_save_contacts(void);
// Raw advert blob cache (for contact share/export); RAM only.
bool mc_mesh_get_advert_blob(const uint8_t *pub_key, uint8_t *dest, uint8_t *out_len);

// Channels.
const mc_channel_t *mc_mesh_channel(uint8_t idx);
bool mc_mesh_set_channel(uint8_t idx, const mc_channel_t *ch);
int mc_mesh_channel_count(void);

// Export/import a contact as a raw advert packet blob.
uint8_t mc_mesh_export_contact(const mc_contact_t *contact, uint8_t *dest_buf);
// Export our own advert as a raw packet blob (header forced to flood).
uint8_t mc_mesh_export_self(uint8_t *dest_buf);
// Re-broadcast a contact's cached advert zero-hop (share).
bool mc_mesh_share_contact(const mc_contact_t *contact);
bool mc_mesh_import_contact(const uint8_t *src, uint8_t len);

// Ed25519 sign helper (companion CMD_SIGN_*).
void mc_mesh_sign(const uint8_t *data, size_t len, uint8_t sig[MC_SIGNATURE_SIZE]);

#ifdef __cplusplus
}
#endif

#endif // MESHCORE_MESH_H
