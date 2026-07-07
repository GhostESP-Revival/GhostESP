#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Compile-time OTA eligibility: only 8MB/16MB boards ship a dual-partition
// (A/B) table (see partitions_ota_*.csv). 4MB boards, and the flash-XIP C5
// boards that can't spare the napps reservation twice, stay manual-flash-only.
#define GHOSTESP_OTA_SUPPORTED (CONFIG_ESPTOOLPY_FLASHSIZE_8MB || CONFIG_ESPTOOLPY_FLASHSIZE_16MB)

typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_CHECKING,
    OTA_STATE_UPDATE_AVAILABLE,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_VERIFYING,
    OTA_STATE_READY_TO_REBOOT,
    OTA_STATE_FAILED,
} OtaState;

typedef struct {
    OtaState state;
    char latest_version[32];
    char latest_commit[16];
    long latest_build_number;
    size_t image_size;
    size_t bytes_downloaded;
    char error_msg[128];
} OtaStatus;

// A single board's entry from ota-manifest.json (see main/managers/ota_manager.c).
typedef struct {
    bool found;
    char version[32];
    char commit[16];
    long build_number;
    char sha256[65];
    size_t size;
    char download_url[256];
} OtaManifestEntry;

// True only when this board was built with an OTA-capable partition table
// (compile-time flash-size gate) AND the currently-flashed partition table
// actually has an inactive OTA slot to write into (runtime check) -- the
// latter distinguishes a board that still needs its one-time manual reflash
// migration onto the new partition table from one that's fully OTA-capable.
bool ota_manager_is_supported(void);

esp_err_t ota_manager_init(void);

// Manual check: fetches ota-manifest.json, populates status, and moves to
// OTA_STATE_UPDATE_AVAILABLE if a firmware image exists for this board.
// Spawns a background task; call ota_manager_get_status() to poll the result.
esp_err_t ota_manager_check_now(void);

// Low-frequency background check (call once per boot / once a day). Only
// ever performs the manifest fetch and sets the "update available" flag --
// never downloads or flashes anything.
esp_err_t ota_manager_background_check(void);

// Explicit user-confirmed download + flash into the inactive OTA partition.
// Spawns a SPIRAM-backed task; progress/result available via
// ota_manager_get_status().
esp_err_t ota_manager_start_update(void);

OtaStatus ota_manager_get_status(void);

// Offline alternative to the R2/HTTPS path: flashes /mnt/ghostesp/firmware_update.bin
// from the SD card (with an optional /mnt/ghostesp/firmware_update.sha256
// sidecar -- verified if present, otherwise flashed unverified) using the
// same raw-write API and rollback safety as the network and GhostLink
// peer-relay paths. Requires the SD card to be mounted. Spawns a background
// task; progress/result available via ota_manager_get_status().
esp_err_t ota_manager_start_update_from_sd(void);

// Fetches ota-manifest.json and extracts the entry for `board_key`. When
// channel == 1 (prerelease) it tries "<board_key>-prerelease" first and
// falls back to the stable key if no prerelease has been published. Pure
// fetch+parse -- does not compare against any locally-running version, does
// not touch OtaStatus, and does not download or flash anything. Used
// internally for this device's own self-check (board_key =
// CONFIG_BUILD_CONFIG_TEMPLATE) and by peer_ota_manager.c to look up a
// GhostLink peer's entry on its behalf.
esp_err_t ota_manager_fetch_manifest_entry(const char *board_key, uint8_t channel, OtaManifestEntry *out_entry);

// Call once, early in app_main after the critical boot sequence (display /
// core managers) has come up without crashing. Marks the running image
// valid and cancels the bootloader's pending-rollback state, if any. Safe
// to call unconditionally -- it's a no-op on boards without an OTA-capable
// partition table.
void ota_manager_confirm_boot_ok(void);

// --- Lower-level raw write API -------------------------------------------
// Wraps esp_ota_ops directly (independent of esp_https_ota) so a board can
// apply an update from any byte source -- used internally by the HTTPS path
// above, and reused by peer_ota_manager.c to apply an update relayed over
// GhostLink from a peer chip's own manifest entry.

// Begins a write session against this device's own inactive OTA partition.
// image_size is the total expected size, used only for progress reporting.
esp_err_t ota_manager_raw_write_begin(size_t image_size);

// Writes the next chunk of the image. May be called repeatedly as bytes
// arrive; updates internal SHA-256 accumulation and progress state.
esp_err_t ota_manager_raw_write_chunk(const uint8_t *data, size_t len);

// Finishes the write, verifies the accumulated SHA-256 against
// expected_sha256_hex (lowercase hex, as provided by the manifest), and -- if
// it matches -- commits the new partition as the next-boot target. Returns
// an error (leaving the running partition untouched) if the write session
// was never begun, is incomplete, or the checksum does not match.
esp_err_t ota_manager_raw_write_finish(const char *expected_sha256_hex);

// Aborts an in-progress raw write session without committing anything.
void ota_manager_raw_write_abort(void);

#endif // OTA_MANAGER_H
