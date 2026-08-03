#ifndef PEER_OTA_MANAGER_H
#define PEER_OTA_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
    PEER_OTA_STATE_IDLE = 0,
    PEER_OTA_STATE_CHECKING,
    PEER_OTA_STATE_UPDATE_AVAILABLE,
    PEER_OTA_STATE_SENDING,
    PEER_OTA_STATE_WAITING_PEER,
    PEER_OTA_STATE_FAILED,
    PEER_OTA_STATE_DONE,
} PeerOtaState;

typedef struct {
    PeerOtaState state;
    bool peer_connected;
    char peer_version[32];
    long peer_build_number;
    long peer_current_build_number;
    size_t bytes_sent;
    size_t total_bytes;
    char error_msg[128];
} PeerOtaStatus;

// True only on the GhostLink primary that owns a peer-flash pairing
// (currently just "somethingsomething" / Banshee C5, whose peer is
// "somethingsomething2" / Banshee S3). False everywhere else, including on
// the peer itself -- the peer only ever reacts to the "otarecv" command, it
// never initiates anything.
bool peer_ota_manager_is_supported(void);

esp_err_t peer_ota_manager_init(void);

// Low-frequency background check (call once per boot / periodically). Only
// proceeds while GhostLink reports a connected peer session; only ever
// fetches the peer's manifest entry and updates status -- never downloads
// or flashes anything.
void peer_ota_manager_background_check(void);

// User-initiated manual check (e.g. a "Check for Updates" button) -- same as
// the background check but bypasses its once-a-day throttle. Spawns a
// background task; poll peer_ota_manager_get_status() for the result.
esp_err_t peer_ota_manager_check_now(void);

// Explicit user-confirmed relay: fetch the peer's firmware from R2, stage +
// verify it locally, then stream it to the peer over GhostLink. Requires an
// active GhostLink connection. Spawns a background task; poll
// peer_ota_manager_get_status() for progress/result.
esp_err_t peer_ota_manager_start_update(void);

PeerOtaStatus peer_ota_manager_get_status(void);

// --- Peer-side entry points --------------------------------------------
// Called (on somethingsomething2) when the "otarecv" remote command arrives
// from its primary. argv is {"otarecv", "<size>", "<sha256>"}.
void peer_ota_manager_handle_otarecv_cmd(int argc, char **argv);

// Called (on somethingsomething2) when the primary polls "otastatus" for the
// final result of the write it just streamed. Responds "PENDING", "DONE", or
// "ERROR:...".
void peer_ota_manager_handle_otastatus_cmd(int argc, char **argv);

// Called (on somethingsomething2) when the primary's own download failed
// partway through an in-progress receive -- aborts the write session
// without committing anything. A receive-timeout watchdog covers the case
// where this command itself never arrives (e.g. GhostLink dropped).
void peer_ota_manager_handle_otaabort_cmd(int argc, char **argv);

// Returns the peer's currently-running build/version to its primary.
void peer_ota_manager_handle_otainfo_cmd(int argc, char **argv);

#endif // PEER_OTA_MANAGER_H
