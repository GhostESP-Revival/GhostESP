#ifndef SELF_OTA_MANAGER_H
#define SELF_OTA_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Single-partition self-overwrite OTA, for boards with no spare OTA slot at
// all (currently just "somethingsomething" / Banshee C5: its physical flash
// is 8MB, and the fixed 1MB napps flash-XIP reservation means a real A/B
// partition table doesn't fit its own binary size -- see the plan notes).
//
// Unlike every other OTA path in this project (ota_manager.c,
// peer_ota_manager.c), THIS path has NO rollback safety net: it erases and
// rewrites the currently-running app partition in place. If interrupted
// (power loss, bug) partway through, the device will very likely need a
// manual USB/serial reflash to recover -- the bootloader and partition
// table are never touched, so this is recoverable, just not automatic.
// Only ever use this where a real dual-partition table genuinely does not
// fit; prefer ota_manager.c wherever possible.

typedef enum {
    SELF_OTA_STATE_IDLE = 0,
    SELF_OTA_STATE_CHECKING,
    SELF_OTA_STATE_DOWNLOADING,
    SELF_OTA_STATE_VERIFYING,
    SELF_OTA_STATE_FLASHING,   // point of no return -- do not power off
    SELF_OTA_STATE_FAILED,
} SelfOtaState;

typedef struct {
    SelfOtaState state;
    char latest_version[32];
    size_t image_size;
    size_t bytes_written;
    char error_msg[128];
} SelfOtaStatus;

// True only on boards that use this mechanism (currently just
// "somethingsomething"). False everywhere else -- other boards either have
// a real OTA partition table (ota_manager.c) or no OTA path at all.
bool self_ota_manager_is_supported(void);

esp_err_t self_ota_manager_init(void);

SelfOtaStatus self_ota_manager_get_status(void);

// Explicit user-confirmed update: fetches this board's own manifest entry,
// downloads + verifies the full image completely in PSRAM (never touches
// flash until the image is confirmed correct), then overwrites the running
// partition in place and resets. Spawns a background task; the device
// reboots on success (there is nothing further to poll for -- if this
// returns and later self_ota_manager_get_status() shows SELF_OTA_STATE_FAILED,
// the update did not proceed and the running firmware is untouched).
esp_err_t self_ota_manager_start_update(void);

#endif // SELF_OTA_MANAGER_H
