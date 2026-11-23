// meshcore_manager.h
// MeshCore lifecycle + radio ownership for GhostESP.
//
// MeshCore and Meshtastic share one SX126x, so only one may own the radio at
// a time. mc_manager_start() stops Meshtastic first; the `lora` command stops
// MeshCore first.

#ifndef MESHCORE_MANAGER_H
#define MESHCORE_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "managers/meshcore_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool running;
    bool radio_present;
    float freq_mhz;
    float bw_khz;
    uint8_t sf;
    uint8_t cr;
    int8_t tx_dbm;
    uint32_t tx_ok;
    uint32_t tx_fail;
    uint32_t rx_ok;
    uint32_t rx_dup;
    uint32_t rx_bad;
    int16_t last_rssi;
    float last_snr;
    int node_count;
} mc_status_t;

typedef struct {
    char who[24];
    char text[160];
    uint8_t channel;
    uint8_t peer_key[MC_PUB_KEY_SIZE]; // full peer key; zero for group messages
    uint32_t node_hash; // display/CLI prefix; never use as a DM identity
    uint32_t timestamp_ms;
    uint32_t packet_id; // expected ACK tag for direct outgoing messages
    uint32_t delivery_deadline_ms;
    bool outgoing;
    bool direct;
    bool read;
    uint8_t delivery; // 0: sent/received, 1: awaiting ACK, 2: delivered, 3: failed
} mc_msg_t;

void mc_manager_early_init(void); // NVS/identity load only
bool mc_manager_start(void);
void mc_manager_stop(void);
// Re-tune the live radio from current prefs, keeping BLE/state intact. Returns
// false if the radio could not be restarted (radio left deinitialised).
bool mc_manager_reconfigure_radio(void);
bool mc_manager_is_running(void);
// Preferred mesh backend across reboots ("mesh"/"mode"), shared with the
// `mesh` CLI and the on-device protocol toggle.
bool mc_manager_default_backend_meshcore(void);
void mc_manager_set_default_backend_meshcore(bool meshcore);
const char *mc_manager_last_error(void);
void mc_manager_get_status(mc_status_t *out);

// CLI/UI helpers
bool mc_manager_send_text(const char *text);                    // channel 0
bool mc_manager_send_channel_text(uint8_t channel, const char *text);
bool mc_manager_send_dm(const char *peer, const char *text);    // by name or pubkey hex
// Legacy CLI helper. Prefixes are accepted for compatibility only; the UI
// uses the full public-key helper below.
bool mc_manager_send_dm_hash(uint8_t peer_hash, const char *text);
bool mc_manager_send_dm_key(const uint8_t peer_key[MC_PUB_KEY_SIZE], const char *text);
bool mc_manager_send_advert(bool flood);

uint16_t mc_manager_msg_count(void);
bool mc_manager_msg_at(uint16_t index, mc_msg_t *out);
bool mc_manager_latest_message(mc_msg_t *out, uint32_t *out_seq);
uint16_t mc_manager_msg_since(uint32_t *io_seq, mc_msg_t *out, uint16_t max);
// Mark a conversation read; 0 selects the group (channel) chat.
void mc_manager_chat_read(uint32_t peer_hash);
void mc_manager_chat_read_key(const uint8_t peer_key[MC_PUB_KEY_SIZE]);
void mc_manager_chat_read_channel(uint8_t channel);

// Run a known-answer self test over the crypto + packet core.
uint8_t mc_manager_selftest(void);

#ifdef __cplusplus
}
#endif

#endif // MESHCORE_MANAGER_H
