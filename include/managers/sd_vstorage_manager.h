#ifndef SD_VSTORAGE_MANAGER_H
#define SD_VSTORAGE_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/*
 * Dynamic sizing for the "storage" FAT data partition used by boards that
 * have no physical SD card slot (S3TWatch, AtomS3R, and the generic
 * ESP32-S3 16MB board -- see CONFIG_IS_S3TWATCH / CONFIG_IS_ATOMS3R /
 * CONFIG_IS_GENERIC_ESP32S3_16MB and mount_virtual_storage() in
 * main/managers/sd_card_manager.c). Those boards mount a wear-levelled FAT
 * filesystem out of a "storage" data partition instead of an SD card, and
 * until now that partition's size was baked into a static
 * partitions_*.csv at build time. This module lets the user pick that size
 * at runtime instead, by rewriting the on-flash partition table directly.
 *
 * IMPORTANT -- reboot is required after any write:
 * ESP-IDF's esp_partition_* API reads and caches the partition table once,
 * lazily, on first use after boot (see components/esp_partition/partition.c
 * load_partitions() / ensure_partitions_loaded() in ESP-IDF), and never
 * re-reads it afterwards. Writing a new table here does NOT take effect
 * until the device reboots. Nothing in this file attempts to mount,
 * unmount, or remount storage, or to reinitialize the partition subsystem --
 * the CLI layer (main/core/commands/cmd_sd.c, "sd vstorage ...") is
 * responsible for telling the user a reboot is required.
 *
 * SAFETY MODEL (read before touching this file):
 * A new table is built entirely in RAM from the partitions ESP-IDF already
 * validated at boot (via esp_partition_find, not a hand-rolled raw-flash
 * parse), staged to a scratch flash sector, read back and compared
 * byte-for-byte, and only THEN erased+written to the live partition-table
 * sector at CONFIG_PARTITION_TABLE_OFFSET (0x8000 by default). This catches
 * a corrupt/garbled table (bad RAM, flash write glitch, bug in this code)
 * before it ever reaches the live sector.
 *
 * This does NOT make the live write itself atomic or power-loss-proof:
 * stock ESP-IDF has no concept of a backup/alternate partition-table slot
 * for this chip target, so if power is lost between the live-sector erase
 * and the completion of the live-sector write, the device will fail to
 * boot (the bootloader's own esp_partition_table_verify() will find no
 * valid entries), and recovery requires reflashing over the wire (e.g.
 * esptool) with a serial/USB connection. That is an inherent limitation of
 * stock ESP-IDF partition tables on this chip family, not something this
 * module can fully close -- the scratch-then-verify step only guarantees
 * we do not knowingly commit a *bad* table, not that the commit write
 * itself cannot be interrupted.
 */

typedef enum {
    SD_VSTORAGE_OK = 0,
    SD_VSTORAGE_ERR_NOT_SUPPORTED,  // board has no virtual-storage mechanism (see CONFIG_IS_*)
    SD_VSTORAGE_ERR_TOO_LARGE,      // requested size exceeds size_cap_bytes()
    SD_VSTORAGE_ERR_TOO_SMALL,      // requested size below SD_VSTORAGE_MIN_SIZE_BYTES
    SD_VSTORAGE_ERR_NO_CHANGE,      // resize requested but size already matches
    SD_VSTORAGE_ERR_NOT_FOUND,      // resize/delete requested but no "storage" partition exists
    SD_VSTORAGE_ERR_TABLE_FULL,     // no room left for one more partition-table entry
    SD_VSTORAGE_ERR_FLASH_IO,       // read/erase/write against flash failed
    SD_VSTORAGE_ERR_VERIFY_FAILED,  // scratch-write readback didn't match what was written
    SD_VSTORAGE_ERR_INVALID_ARG,    // bad argument (NULL out pointer, size 0, etc.)
    SD_VSTORAGE_ERR_INTERNAL,       // unexpected/should-not-happen state
} sd_vstorage_status_t;

// Minimum viable size for the "storage" FAT partition. Matches the 64KB
// floor mount_virtual_storage() already enforces in sd_card_manager.c.
#define SD_VSTORAGE_MIN_SIZE_BYTES (64u * 1024u)

// One SPI flash erase sector. The partition table region and this module's
// scratch staging region are each exactly one sector.
#define SD_VSTORAGE_SECTOR_SIZE 0x1000u

// Label used for the virtual-storage data partition. Must match the label
// mount_virtual_storage() looks up in sd_card_manager.c ("storage").
#define SD_VSTORAGE_PARTITION_LABEL "storage"

// Numbers reported by sd_vstorage_get_info() / "sd vstorage info".
typedef struct {
    uint64_t chip_total_bytes;    // esp_flash_get_size()
    uint64_t used_bytes;          // sum of all other partitions + table sector + scratch sector
    uint64_t free_bytes;          // chip_total_bytes - used_bytes (0 if that would underflow)
    uint64_t cap_bytes;           // size_cap_bytes(): 80% of free_bytes, integer floor
    bool storage_exists;
    uint32_t storage_offset;      // valid only if storage_exists
    uint32_t storage_size_bytes;  // valid only if storage_exists
} sd_vstorage_info_t;

/*
 * Sums the size of every partition currently in the live table, plus the
 * fixed-offset partition-table sector itself and this module's reserved
 * scratch sector, subtracts that total from esp_flash_get_size(), and
 * returns the remainder via *out_free_bytes.
 *
 * This is generic: nothing here is hardcoded to a particular chip size or
 * to a particular existing partition layout, so it works on any GhostESP
 * board, not just a 16MB-flash one.
 */
sd_vstorage_status_t calc_free_flash_space(uint64_t *out_free_bytes);

// 80% of calc_free_flash_space(), rounded down (integer floor via
// (free_bytes * 4) / 5 -- deliberately not floating point).
sd_vstorage_status_t size_cap_bytes(uint64_t *out_cap_bytes);

// Fills *out_info with the numbers above plus the current "storage"
// partition's location/size, if one exists.
sd_vstorage_status_t sd_vstorage_get_info(sd_vstorage_info_t *out_info);

/*
 * Core of "sd vstorage create" / "sd vstorage resize". Validates
 * size_bytes against size_cap_bytes() and SD_VSTORAGE_MIN_SIZE_BYTES,
 * then builds a new partition table with a "storage"
 * (ESP_PARTITION_TYPE_DATA / ESP_PARTITION_SUBTYPE_DATA_FAT) entry of
 * exactly size_bytes:
 *   - if no "storage" partition exists yet, appends one immediately after
 *     the last entry in the current table, sector-aligned (this is what
 *     the CLI exposes as "create");
 *   - if one already exists, resizes it IN PLACE at its existing offset
 *     (this is "resize"). A resize destroys the partition's prior FAT
 *     contents -- the old filesystem's size/geometry no longer matches the
 *     partition bounds, so the caller MUST treat this as destructive and
 *     have already obtained explicit user confirmation before calling this
 *     for an existing partition. This function itself does not prompt or
 *     ask; see the confirm-flag handling in cmd_sd.c's "sd vstorage
 *     resize" handler.
 *
 * Never mounts, unmounts, or formats anything, and never touches the live
 * partition-table sector until the scratch-staged copy has been written
 * back and verified byte-for-byte (see the safety-model note above).
 *
 * On SD_VSTORAGE_OK, *out_reboot_required is always set true: the new
 * table is now committed to flash, but ESP-IDF will not see it until the
 * device reboots.
 */
sd_vstorage_status_t create_or_resize_storage_partition(uint32_t size_bytes,
                                                          bool *out_reboot_required);

/*
 * Removes the "storage" partition from the table entirely, freeing that
 * space back to calc_free_flash_space(). Returns SD_VSTORAGE_ERR_NOT_FOUND
 * if no "storage" partition currently exists. Uses the same
 * scratch-then-verify-then-commit write path and reboot-required contract
 * as create_or_resize_storage_partition().
 */
sd_vstorage_status_t delete_storage_partition(bool *out_reboot_required);

// Human-readable string for a sd_vstorage_status_t, for CLI/log output.
const char *sd_vstorage_status_str(sd_vstorage_status_t status);

#endif // SD_VSTORAGE_MANAGER_H
