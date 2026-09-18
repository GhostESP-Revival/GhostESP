// meshcore_companion.h
// MeshCore companion (phone) protocol frame layer.
//
// Binary command/response frames exactly as documented by the upstream MeshCore
// firmware v1.17.1 (examples/companion_radio/MyMesh.cpp). Frames are moved by
// the BLE transport in meshcore_ble.c, one frame per characteristic value.

#ifndef MESHCORE_COMPANION_H
#define MESHCORE_COMPANION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "managers/meshcore_store.h"

#ifdef __cplusplus
extern "C" {
#endif

// Allocate the offline queue and reset protocol state. Returns false if the
// queue could not be allocated.
bool mc_companion_init(void);
// Release the offline queue and any transient buffers.
void mc_companion_deinit(void);
void mc_companion_set_connected(bool connected);
bool mc_companion_is_connected(void);
bool mc_companion_has_data(void);
// Pump the contacts iterator / pending responses (called from the BLE worker).
void mc_companion_loop(void);

// Handle one complete frame written by the app to the RX characteristic.
void mc_companion_handle_frame(const uint8_t *frame, size_t len);

// Mesh event hooks (called by the manager's mesh callbacks).
void mc_companion_on_channel_message(uint8_t channel_idx, uint32_t timestamp,
                                     const char *text, float snr,
                                     uint8_t path_len, bool is_flood);
void mc_companion_on_contact_message(const mc_contact_t *from, uint32_t timestamp,
                                     const char *text, uint8_t txt_type, float snr,
                                     uint8_t path_len, bool is_flood);
// Binary application response, matched against pending requests and pushed to
// the app (PUSH_STATUS/TELEMETRY/BINARY_RESPONSE).
void mc_companion_on_contact_response(const mc_contact_t *from, const uint8_t *data, uint8_t len,
                                      float snr, uint8_t path_len, bool is_flood);
// Signed plain text: 4-byte sender_prefix is placed between the timestamp and
// text, matching upstream queueMessage(extra_len=4).
void mc_companion_on_signed_message(const mc_contact_t *from, uint32_t timestamp,
                                    const uint8_t *sender_prefix, const char *text,
                                    float snr, uint8_t path_len, bool is_flood);
void mc_companion_on_channel_data(uint8_t channel_idx, uint16_t data_type,
                                  const uint8_t *data, uint8_t len, float snr,
                                  uint8_t path_len, bool is_flood);
void mc_companion_on_advert(const mc_contact_t *contact, bool is_new);
void mc_companion_on_ack(const mc_contact_t *from, uint32_t ack);
void mc_companion_on_path_updated(const mc_contact_t *contact);
void mc_companion_on_contact_deleted(const uint8_t *pub_key);

#ifdef __cplusplus
}
#endif

#endif // MESHCORE_COMPANION_H
