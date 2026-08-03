#ifndef SELF_OTA_MANAGER_H
#define SELF_OTA_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Banshee C5 self-OTA. This board cannot fit two full GhostESP app slots plus
// its fixed napps flash-XIP reservation, so it uses a tiny updater app instead:
// main firmware stores the pending manifest entry, boots the updater partition,
// and the updater rewrites app0 while running from its own partition.

typedef enum {
    SELF_OTA_STATE_IDLE = 0,
    SELF_OTA_STATE_CHECKING,
    SELF_OTA_STATE_UPDATE_AVAILABLE,
    SELF_OTA_STATE_DOWNLOADING,
    SELF_OTA_STATE_VERIFYING,
    SELF_OTA_STATE_FLASHING,   // updater handoff; keep powered until updater finishes
    SELF_OTA_STATE_FAILED,
} SelfOtaState;

typedef struct {
    SelfOtaState state;
    char latest_version[32];
    long latest_build_number;
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

// Manifest-only check for single-partition self-OTA boards. Spawns a task and
// reports SELF_OTA_STATE_UPDATE_AVAILABLE when a firmware image exists.
esp_err_t self_ota_manager_check_now(void);

// Explicit user-confirmed update: fetches this board's own manifest entry,
// stores the pending updater handoff in NVS, and reboots into the updater app.
// The running firmware is untouched until the updater is running from its own
// partition.
esp_err_t self_ota_manager_start_update(void);

#endif // SELF_OTA_MANAGER_H
