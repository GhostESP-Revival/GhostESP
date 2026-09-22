// sd_vstorage_manager.c
//
// Dynamic sizing for the flash-backed "storage" FAT partition used by
// boards with no SD card slot (S3TWatch / AtomS3R / generic ESP32-S3 16MB --
// see mount_virtual_storage() in main/managers/sd_card_manager.c). See the
// long comment in include/managers/sd_vstorage_manager.h for the safety
// model before changing anything here -- this file writes raw bytes to the
// ESP-IDF partition-table flash region, which is not a mistake-tolerant
// place to improvise.
//
// Binary format used here (esp_partition_info_t, magic 0x50AA per entry,
// MD5 checksum entry with magic 0xEBEB, terminator implied by erased/0xFF
// padding) was cross-checked directly against the ESP-IDF "v6.1" tag --
// the same tag .github/workflows/compile_all.yml pins for this repo's CI
// builds -- specifically:
//   components/bootloader_support/include/esp_flash_partitions.h
//   components/bootloader_support/src/flash_partitions.c (esp_partition_table_verify)
//   components/esp_partition/partition.c (load_partitions, the app-side loader)
//   components/partition_table/gen_esp32part.py (the canonical CSV -> binary tool)
// not just recalled from general knowledge. See the NOTES file this patch
// ships with for the honest confidence assessment on this.

#include "managers/sd_vstorage_manager.h"

#include "esp_flash.h"
#include "esp_flash_partitions.h"
#include "esp_private/esp_flash_internal.h" // esp_flash_set_dangerous_write_protection()
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_md5.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "SD_VStorage";

// One slot of ESP_PARTITION_TABLE_MAX_ENTRIES (96 for the standard 0xC00
// table region) is always reserved for the MD5 checksum entry, so the
// largest number of *real* partitions we can ever write is one less.
#define VSTORAGE_MAX_ENTRIES (ESP_PARTITION_TABLE_MAX_ENTRIES - 1)

// ---------------------------------------------------------------------
// Small flash helpers
// ---------------------------------------------------------------------

static esp_err_t vstorage_get_chip_size(uint64_t *out_size) {
    uint32_t size32 = 0;
    esp_err_t err = esp_flash_get_size(NULL, &size32);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_flash_get_size failed: %s", esp_err_to_name(err));
        return err;
    }
    *out_size = (uint64_t)size32;
    return ESP_OK;
}

// Erases exactly one sector at `offset`, writes `len` bytes of `blob` into
// it, reads the same range back, and byte-compares. Used for BOTH the
// scratch staging write and the final live-table commit -- same function,
// same verification, just a different offset.
static sd_vstorage_status_t vstorage_write_sector_verified(uint32_t offset,
                                                             const uint8_t *blob,
                                                             size_t len) {
    esp_err_t err = esp_flash_erase_region(NULL, offset, SD_VSTORAGE_SECTOR_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase @0x%lx failed: %s", (unsigned long)offset, esp_err_to_name(err));
        return SD_VSTORAGE_ERR_FLASH_IO;
    }

    err = esp_flash_write(NULL, blob, offset, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write @0x%lx failed: %s", (unsigned long)offset, esp_err_to_name(err));
        return SD_VSTORAGE_ERR_FLASH_IO;
    }

    uint8_t *readback = malloc(len);
    if (!readback) {
        ESP_LOGE(TAG, "OOM allocating %zu-byte readback buffer", len);
        return SD_VSTORAGE_ERR_INTERNAL;
    }
    err = esp_flash_read(NULL, readback, offset, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "readback @0x%lx failed: %s", (unsigned long)offset, esp_err_to_name(err));
        free(readback);
        return SD_VSTORAGE_ERR_FLASH_IO;
    }

    bool match = (memcmp(readback, blob, len) == 0);
    free(readback);
    if (!match) {
        ESP_LOGE(TAG, "readback @0x%lx did not match what was written", (unsigned long)offset);
        return SD_VSTORAGE_ERR_VERIFY_FAILED;
    }
    return SD_VSTORAGE_OK;
}

// ---------------------------------------------------------------------
// Reading the current table (via the already-boot-verified esp_partition
// API, not a hand-rolled raw flash parse -- see header comment for why)
// ---------------------------------------------------------------------

static sd_vstorage_status_t vstorage_read_current_entries(esp_partition_info_t *out,
                                                            size_t cap,
                                                            size_t *out_count) {
    if (!out || !out_count || cap == 0) return SD_VSTORAGE_ERR_INVALID_ARG;

    size_t n = 0;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        if (!p) continue;
        if (n >= cap) {
            esp_partition_iterator_release(it);
            ESP_LOGE(TAG, "Live partition table has more entries than this module can hold (%zu)", cap);
            return SD_VSTORAGE_ERR_TABLE_FULL;
        }
        esp_partition_info_t *e = &out[n];
        memset(e, 0, sizeof(*e));
        e->magic = ESP_PARTITION_MAGIC;
        e->type = (uint8_t)p->type;
        e->subtype = (uint8_t)p->subtype;
        e->pos.offset = p->address;
        e->pos.size = p->size;
        // p->label is a NUL-terminated string of at most 16 visible chars.
        // strncpy round-trips this correctly either way: if the name is
        // shorter than 16 bytes the remainder is NUL-padded (matches
        // gen_esp32part.py's struct.pack('16s', name) behavior); if it's
        // exactly 16 bytes there's simply no NUL byte in the field, which
        // the ESP-IDF/gen_esp32part.py binary format explicitly allows.
        strncpy((char *)e->label, p->label, sizeof(e->label));
        e->flags = 0;
        if (p->encrypted) e->flags |= PART_FLAG_ENCRYPTED;
        if (p->readonly) e->flags |= PART_FLAG_READONLY;
        n++;
    }
    esp_partition_iterator_release(it); // it == NULL here; documented safe no-op

    *out_count = n;
    return SD_VSTORAGE_OK;
}

static uint64_t vstorage_sum_entries_size(const esp_partition_info_t *entries, size_t count) {
    uint64_t sum = 0;
    for (size_t i = 0; i < count; i++) sum += entries[i].pos.size;
    return sum;
}

static int vstorage_find_storage_index(const esp_partition_info_t *entries, size_t count) {
    const size_t label_len = strlen(SD_VSTORAGE_PARTITION_LABEL);
    for (size_t i = 0; i < count; i++) {
        if (entries[i].type == ESP_PARTITION_TYPE_DATA &&
            entries[i].subtype == ESP_PARTITION_SUBTYPE_DATA_FAT &&
            memcmp(entries[i].label, SD_VSTORAGE_PARTITION_LABEL, label_len + 1) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool vstorage_ranges_overlap(uint32_t a_off, uint32_t a_size, uint32_t b_off, uint32_t b_size) {
    uint64_t a_end = (uint64_t)a_off + a_size;
    uint64_t b_end = (uint64_t)b_off + b_size;
    return ((uint64_t)a_off < b_end) && ((uint64_t)b_off < a_end);
}

// skip_index: index to ignore during the check (the entry being resized),
// or (size_t)-1 to check against every entry (used for a fresh create).
static sd_vstorage_status_t vstorage_check_no_overlap(const esp_partition_info_t *entries,
                                                       size_t count, size_t skip_index,
                                                       uint32_t offset, uint32_t size) {
    for (size_t i = 0; i < count; i++) {
        if (i == skip_index) continue;
        if (vstorage_ranges_overlap(offset, size, entries[i].pos.offset, entries[i].pos.size)) {
            ESP_LOGE(TAG, "Proposed storage range 0x%lx+0x%lx overlaps existing partition '%.16s' at 0x%lx+0x%lx",
                      (unsigned long)offset, (unsigned long)size, (const char *)entries[i].label,
                      (unsigned long)entries[i].pos.offset, (unsigned long)entries[i].pos.size);
            return SD_VSTORAGE_ERR_INTERNAL;
        }
    }
    return SD_VSTORAGE_OK;
}

// ---------------------------------------------------------------------
// Public sizing queries
// ---------------------------------------------------------------------

sd_vstorage_status_t calc_free_flash_space(uint64_t *out_free_bytes) {
    if (!out_free_bytes) return SD_VSTORAGE_ERR_INVALID_ARG;

    uint64_t chip_size = 0;
    if (vstorage_get_chip_size(&chip_size) != ESP_OK) return SD_VSTORAGE_ERR_FLASH_IO;

    esp_partition_info_t entries[VSTORAGE_MAX_ENTRIES];
    size_t count = 0;
    sd_vstorage_status_t st = vstorage_read_current_entries(entries, VSTORAGE_MAX_ENTRIES, &count);
    if (st != SD_VSTORAGE_OK) return st;

    uint64_t used = vstorage_sum_entries_size(entries, count);
    used += SD_VSTORAGE_SECTOR_SIZE; // the partition-table sector itself (CONFIG_PARTITION_TABLE_OFFSET)
    used += SD_VSTORAGE_SECTOR_SIZE; // this module's own reserved scratch sector (see header)

    *out_free_bytes = (chip_size > used) ? (chip_size - used) : 0;
    return SD_VSTORAGE_OK;
}

sd_vstorage_status_t size_cap_bytes(uint64_t *out_cap_bytes) {
    if (!out_cap_bytes) return SD_VSTORAGE_ERR_INVALID_ARG;
    uint64_t free_bytes = 0;
    sd_vstorage_status_t st = calc_free_flash_space(&free_bytes);
    if (st != SD_VSTORAGE_OK) return st;
    // 80% cap, integer floor. Deliberately not floating point: (free*4)/5
    // is exact, deterministic, and matches "free space * 0.8" for every
    // free_bytes value without any rounding-mode ambiguity.
    *out_cap_bytes = (free_bytes / 5) * 4;
    return SD_VSTORAGE_OK;
}

sd_vstorage_status_t sd_vstorage_get_info(sd_vstorage_info_t *out_info) {
    if (!out_info) return SD_VSTORAGE_ERR_INVALID_ARG;
    memset(out_info, 0, sizeof(*out_info));

    uint64_t chip_size = 0;
    if (vstorage_get_chip_size(&chip_size) != ESP_OK) return SD_VSTORAGE_ERR_FLASH_IO;
    out_info->chip_total_bytes = chip_size;

    uint64_t free_bytes = 0;
    sd_vstorage_status_t st = calc_free_flash_space(&free_bytes);
    if (st != SD_VSTORAGE_OK) return st;
    out_info->free_bytes = free_bytes;
    out_info->used_bytes = (chip_size > free_bytes) ? (chip_size - free_bytes) : 0;
    out_info->cap_bytes = (free_bytes / 5) * 4;

    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, SD_VSTORAGE_PARTITION_LABEL);
    if (p) {
        out_info->storage_exists = true;
        out_info->storage_offset = p->address;
        out_info->storage_size_bytes = p->size;
    }
    return SD_VSTORAGE_OK;
}

// ---------------------------------------------------------------------
// Table serialization (the risky binary-format part)
// ---------------------------------------------------------------------

// out_blob must be at least ESP_PARTITION_TABLE_MAX_LEN (0xC00) bytes.
static sd_vstorage_status_t vstorage_build_table_blob(const esp_partition_info_t *entries,
                                                        size_t count, uint8_t *out_blob) {
    if (count > VSTORAGE_MAX_ENTRIES) return SD_VSTORAGE_ERR_TABLE_FULL;

    // Fill with 0xFF (erased-flash value) first. Bytes we never explicitly
    // write -- everything after the MD5 entry, up through the end of the
    // 0xC00 region -- stay 0xFF, which is exactly what
    // esp_partition_table_verify() / load_partitions() treat as "no more
    // entries" (they stop at the first non-magic, non-MD5-magic record,
    // and an erased/0xFF record's magic field reads as 0xFFFF). This
    // mirrors gen_esp32part.py's PartitionTable.to_binary(), which pads
    // the same way.
    memset(out_blob, 0xFF, ESP_PARTITION_TABLE_MAX_LEN);

    for (size_t i = 0; i < count; i++) {
        memcpy(out_blob + i * sizeof(esp_partition_info_t), &entries[i], sizeof(esp_partition_info_t));
    }

    size_t entries_len = count * sizeof(esp_partition_info_t);

    // MD5 entry: magic 0xEBEB (bytes 0xEB,0xEB either byte order, so no LE/BE
    // ambiguity), 14 bytes of 0xFF padding, then the 16-byte MD5 digest of
    // every real entry's raw bytes (NOT including this entry itself). This
    // exact layout -- and the requirement that CONFIG_PARTITION_TABLE_MD5
    // builds refuse to load a table with no MD5 entry, or one whose digest
    // doesn't match -- was confirmed against
    // components/esp_partition/partition.c's load_partitions() and
    // components/bootloader_support/src/flash_partitions.c's
    // esp_partition_table_verify() on the esp-idf "v6.1" tag. Getting this
    // wrong means the device cannot find ANY partitions after reboot.
    // ESP-IDF v6.1's mbedtls component (mbedtls 4.x / TF-PSA-Crypto) no
    // longer ships the legacy public mbedtls/md5.h header, so this uses
    // ESP-IDF's own ROM MD5 API instead -- the same one
    // components/bootloader_support/src/flash_partitions.c uses to verify
    // this exact MD5 entry type, so the digest algorithm matches what the
    // bootloader/esp_partition loader will check on boot.
    unsigned char digest[ESP_ROM_MD5_DIGEST_LEN];
    md5_context_t md5_ctx;
    esp_rom_md5_init(&md5_ctx);
    esp_rom_md5_update(&md5_ctx, out_blob, entries_len);
    esp_rom_md5_final(digest, &md5_ctx);

    uint8_t md5_entry[sizeof(esp_partition_info_t)];
    memset(md5_entry, 0xFF, sizeof(md5_entry));
    md5_entry[0] = (uint8_t)(ESP_PARTITION_MAGIC_MD5 & 0xFF);
    md5_entry[1] = (uint8_t)((ESP_PARTITION_MAGIC_MD5 >> 8) & 0xFF);
    memcpy(md5_entry + ESP_PARTITION_MD5_OFFSET, digest, sizeof(digest));
    memcpy(out_blob + entries_len, md5_entry, sizeof(md5_entry));

    return SD_VSTORAGE_OK;
}

// Shared commit path for create/resize/delete: build the blob, stage it to
// a scratch sector and verify, and ONLY THEN commit it to the live
// partition-table sector (also verified). See the header's safety-model
// comment for exactly what this does and does not protect against.
static sd_vstorage_status_t vstorage_commit_new_table(const esp_partition_info_t *entries,
                                                        size_t count, bool *out_reboot_required) {
    *out_reboot_required = false;

    uint8_t *blob = malloc(ESP_PARTITION_TABLE_MAX_LEN);
    if (!blob) return SD_VSTORAGE_ERR_INTERNAL;

    sd_vstorage_status_t st = vstorage_build_table_blob(entries, count, blob);
    if (st != SD_VSTORAGE_OK) {
        free(blob);
        return st;
    }

    uint64_t chip_size = 0;
    if (vstorage_get_chip_size(&chip_size) != ESP_OK) {
        free(blob);
        return SD_VSTORAGE_ERR_FLASH_IO;
    }
    if (chip_size < SD_VSTORAGE_SECTOR_SIZE) {
        free(blob);
        return SD_VSTORAGE_ERR_INTERNAL;
    }
    uint32_t scratch_offset = (uint32_t)(chip_size - SD_VSTORAGE_SECTOR_SIZE);

    ESP_LOGI(TAG, "Staging new partition table (%zu entries) to scratch sector 0x%lx",
             count, (unsigned long)scratch_offset);
    st = vstorage_write_sector_verified(scratch_offset, blob, ESP_PARTITION_TABLE_MAX_LEN);
    if (st != SD_VSTORAGE_OK) {
        ESP_LOGE(TAG, "Scratch write/verify failed (%s) -- live partition table was NOT touched",
                 sd_vstorage_status_str(st));
        free(blob);
        return st;
    }

    ESP_LOGW(TAG, "Scratch copy verified OK. Committing to live partition table at 0x%x -- do not power off now.",
              ESP_PARTITION_TABLE_OFFSET);
    // The live partition table sits in esp_flash's protected region (bootloader,
    // partition table, running app) - esp_flash_erase_region()/esp_flash_write()
    // abort() on it by default (CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS=y in this
    // build). Bracket just this one write with the same protection toggle
    // components/app_update/esp_ota_ops.c uses for the same class of legitimate
    // protected-region write; re-enabled immediately after regardless of outcome.
    esp_flash_set_dangerous_write_protection(esp_flash_default_chip, false);
    st = vstorage_write_sector_verified(ESP_PARTITION_TABLE_OFFSET, blob, ESP_PARTITION_TABLE_MAX_LEN);
    esp_flash_set_dangerous_write_protection(esp_flash_default_chip, true);
    free(blob);
    if (st != SD_VSTORAGE_OK) {
        ESP_LOGE(TAG, "Live partition table write/verify FAILED (%s). The device's partition table may now be "
                      "inconsistent; if it will not boot, reflash over serial (esptool) to recover.",
                  sd_vstorage_status_str(st));
        return st;
    }

    ESP_LOGW(TAG, "Live partition table updated. A reboot is required for it to take effect.");
    *out_reboot_required = true;
    return SD_VSTORAGE_OK;
}

// ---------------------------------------------------------------------
// Public mutation API
// ---------------------------------------------------------------------

sd_vstorage_status_t create_or_resize_storage_partition(uint32_t size_bytes, bool *out_reboot_required) {
    if (!out_reboot_required) return SD_VSTORAGE_ERR_INVALID_ARG;
    *out_reboot_required = false;

    if (size_bytes < SD_VSTORAGE_MIN_SIZE_BYTES) return SD_VSTORAGE_ERR_TOO_SMALL;
    if (size_bytes % SD_VSTORAGE_SECTOR_SIZE != 0) return SD_VSTORAGE_ERR_INVALID_ARG;

    uint64_t cap = 0;
    sd_vstorage_status_t st = size_cap_bytes(&cap);
    if (st != SD_VSTORAGE_OK) return st;
    if ((uint64_t)size_bytes > cap) return SD_VSTORAGE_ERR_TOO_LARGE;

    esp_partition_info_t entries[VSTORAGE_MAX_ENTRIES];
    size_t count = 0;
    st = vstorage_read_current_entries(entries, VSTORAGE_MAX_ENTRIES, &count);
    if (st != SD_VSTORAGE_OK) return st;

    uint64_t chip_size = 0;
    if (vstorage_get_chip_size(&chip_size) != ESP_OK) return SD_VSTORAGE_ERR_FLASH_IO;
    if (chip_size < SD_VSTORAGE_SECTOR_SIZE) return SD_VSTORAGE_ERR_INTERNAL;
    // Never let the storage partition reach into this module's reserved
    // scratch sector at the very top of the chip.
    uint32_t reach_limit = (uint32_t)(chip_size - SD_VSTORAGE_SECTOR_SIZE);

    int idx = vstorage_find_storage_index(entries, count);
    if (idx >= 0) {
        // Resize in place. DESTRUCTIVE: the existing FAT filesystem's
        // geometry no longer matches the new partition bounds. The caller
        // (cmd_sd.c) is responsible for having already obtained explicit
        // user confirmation -- this function does not ask.
        uint32_t offset = entries[idx].pos.offset;
        if (size_bytes == entries[idx].pos.size) return SD_VSTORAGE_ERR_NO_CHANGE;
        if ((uint64_t)offset + size_bytes > reach_limit) return SD_VSTORAGE_ERR_TOO_LARGE;
        st = vstorage_check_no_overlap(entries, count, (size_t)idx, offset, size_bytes);
        if (st != SD_VSTORAGE_OK) return st;

        ESP_LOGW(TAG, "Resizing existing 'storage' partition at 0x%lx: %lu -> %lu bytes (contents destroyed)",
                  (unsigned long)offset, (unsigned long)entries[idx].pos.size, (unsigned long)size_bytes);
        entries[idx].pos.size = size_bytes;
    } else {
        // Create: append after the highest end-offset currently in the table.
        if (count >= VSTORAGE_MAX_ENTRIES) return SD_VSTORAGE_ERR_TABLE_FULL;

        uint64_t highest_end = 0;
        for (size_t i = 0; i < count; i++) {
            uint64_t end = (uint64_t)entries[i].pos.offset + entries[i].pos.size;
            if (end > highest_end) highest_end = end;
        }
        uint64_t new_offset64 =
            (highest_end + (SD_VSTORAGE_SECTOR_SIZE - 1)) & ~((uint64_t)SD_VSTORAGE_SECTOR_SIZE - 1);
        if (new_offset64 + size_bytes > reach_limit) return SD_VSTORAGE_ERR_TOO_LARGE;
        uint32_t new_offset = (uint32_t)new_offset64;

        st = vstorage_check_no_overlap(entries, count, (size_t)-1, new_offset, size_bytes);
        if (st != SD_VSTORAGE_OK) return st;

        esp_partition_info_t *e = &entries[count];
        memset(e, 0, sizeof(*e));
        e->magic = ESP_PARTITION_MAGIC;
        e->type = ESP_PARTITION_TYPE_DATA;
        e->subtype = ESP_PARTITION_SUBTYPE_DATA_FAT;
        e->pos.offset = new_offset;
        e->pos.size = size_bytes;
        strncpy((char *)e->label, SD_VSTORAGE_PARTITION_LABEL, sizeof(e->label));
        e->flags = 0;
        count++;

        ESP_LOGI(TAG, "Creating new 'storage' partition at 0x%lx, size %lu bytes",
                 (unsigned long)new_offset, (unsigned long)size_bytes);
    }

    return vstorage_commit_new_table(entries, count, out_reboot_required);
}

sd_vstorage_status_t delete_storage_partition(bool *out_reboot_required) {
    if (!out_reboot_required) return SD_VSTORAGE_ERR_INVALID_ARG;
    *out_reboot_required = false;

    esp_partition_info_t entries[VSTORAGE_MAX_ENTRIES];
    size_t count = 0;
    sd_vstorage_status_t st = vstorage_read_current_entries(entries, VSTORAGE_MAX_ENTRIES, &count);
    if (st != SD_VSTORAGE_OK) return st;

    int idx = vstorage_find_storage_index(entries, count);
    if (idx < 0) return SD_VSTORAGE_ERR_NOT_FOUND;

    ESP_LOGW(TAG, "Deleting 'storage' partition at 0x%lx (%lu bytes)",
              (unsigned long)entries[idx].pos.offset, (unsigned long)entries[idx].pos.size);

    for (size_t i = (size_t)idx; i + 1 < count; i++) {
        entries[i] = entries[i + 1];
    }
    count--;

    return vstorage_commit_new_table(entries, count, out_reboot_required);
}

const char *sd_vstorage_status_str(sd_vstorage_status_t status) {
    switch (status) {
        case SD_VSTORAGE_OK: return "ok";
        case SD_VSTORAGE_ERR_NOT_SUPPORTED: return "not supported on this board";
        case SD_VSTORAGE_ERR_TOO_LARGE: return "requested size exceeds the 80% cap";
        case SD_VSTORAGE_ERR_TOO_SMALL: return "requested size is below the minimum";
        case SD_VSTORAGE_ERR_NO_CHANGE: return "requested size matches the current size";
        case SD_VSTORAGE_ERR_NOT_FOUND: return "no storage partition exists";
        case SD_VSTORAGE_ERR_TABLE_FULL: return "partition table has no room for another entry";
        case SD_VSTORAGE_ERR_FLASH_IO: return "flash read/erase/write failed";
        case SD_VSTORAGE_ERR_VERIFY_FAILED: return "readback verification failed";
        case SD_VSTORAGE_ERR_INVALID_ARG: return "invalid argument";
        case SD_VSTORAGE_ERR_INTERNAL: return "internal error";
        default: return "unknown";
    }
}
