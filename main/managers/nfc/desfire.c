#include "managers/nfc/desfire.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
 #include "esp_log.h"

 static const char *TAG = "DESFIRE";

// Conservative heuristic: typical DESFire / Type 4 NXP tags use ATQA 0x0344
// and SAK 0x20. This intentionally avoids trying to classify every ISO14443-4
// tag as DESFire.
bool desfire_is_desfire_candidate(uint16_t atqa, uint8_t sak) {
    if (sak != 0x20) return false;
    if (atqa == 0x0344) return true;
    return false;
}

// SAK bit 5 (0x20) means "ISO14443-4 compliant" — covers DESFire, EMV, MIFARE Plus, etc.
bool desfire_sak_is_iso14443_4(uint8_t sak) {
    return (sak & 0x20) != 0;
}

static DESFIRE_MODEL desfire_model_from_size_byte(uint8_t size_byte,
                                                  uint32_t *out_bytes) {
    // AN10833: storage size calculation: 7 MSBs (n) encode size 2^n when
    // LSB is 0. Example values: 0x16 -> 2K, 0x18 -> 4K, 0x1A -> 8K.
    uint8_t n = (uint8_t)(size_byte >> 1);
    uint32_t bytes = 0;
    if (n < 31) {
        bytes = (uint32_t)1U << n;
    }
    if (out_bytes) *out_bytes = bytes;
    switch (bytes) {
        case 2048:  return DESFIRE_MODEL_2K;
        case 4096:  return DESFIRE_MODEL_4K;
        case 8192:  return DESFIRE_MODEL_8K;
        default:    return DESFIRE_MODEL_UNKNOWN;
    }
}

  #if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_ST25R3916)
   bool desfire_get_version(pn532_io_handle_t io, desfire_version_t *out) {
     if (!io || !out) return false;
     memset(out, 0, sizeof(*out));

     // DESFire GET_VERSION on some tags can take noticeably longer than
     // simple MIFARE Classic/NTAG operations, so allow a more generous
     // INDATAEXCHANGE wait window here. Classic code later re-tightens it anyway.
     pn532_set_indata_wait_timeout(500);

     uint8_t resp[32] = {0};
     uint8_t rlen = 0;
     size_t total = 0;
     uint8_t ins = 0x60;

     for (int frame = 0; frame < 3 && total < DESFIRE_PICC_VERSION_MAX; ++frame) {
         uint8_t apdu[5] = {0x90, ins, 0x00, 0x00, 0x00};
         rlen = (uint8_t)sizeof(resp);
         if (pn532_in_data_exchange(io, apdu, sizeof(apdu), resp, &rlen) != ESP_OK) {
             ESP_LOGI(TAG, "GET_VERSION frame %d: pn532_in_data_exchange failed", frame);
             return false;
         }
         if (rlen == 0) {
             ESP_LOGI(TAG, "GET_VERSION frame %d: empty response", frame);
             return false;
         }

         uint8_t status = resp[rlen - 1];
         uint8_t copy_len = rlen;

         bool has_af00 = (status == 0xAF || status == 0x00);
         if (has_af00) {
             if (rlen >= 2 && resp[rlen - 2] == 0x91) {
                 copy_len = (uint8_t)(rlen - 2);
             } else {
                 if (rlen < 2) {
                     ESP_LOGI(TAG, "GET_VERSION frame %d: short status frame (len=%u)", frame, (unsigned)rlen);
                     return false;
                 }
                 copy_len = (uint8_t)(rlen - 1);
             }
         } else {
             ESP_LOGI(TAG, "GET_VERSION frame %d: unexpected status 0x%02X (len=%u)",
                      frame, (unsigned)status, (unsigned)rlen);
             return false;
         }

         if (frame == 0 && copy_len < 7) {
             if (copy_len == 0 && status == 0xAF) {
                 ESP_LOGI(TAG, "GET_VERSION frame 0: status-only 0x91AF, continuing");
                 ins = 0xAF;
                 continue;
             }
             ESP_LOGI(TAG, "GET_VERSION frame 0: too short data len=%u", (unsigned)copy_len);
             return false;
         }

         if (copy_len > 0) {
             if ((size_t)copy_len > (DESFIRE_PICC_VERSION_MAX - total)) {
                 copy_len = (uint8_t)(DESFIRE_PICC_VERSION_MAX - total);
             }
             memcpy(out->picc_version + total, resp, copy_len);
             total += copy_len;
         }

         ESP_LOGI(TAG, "GET_VERSION frame %d: data_len=%u status=0x%02X total=%u",
                  frame, (unsigned)copy_len, (unsigned)status, (unsigned)total);

         if (status == 0x00) {
             break;
         }
         ins = 0xAF;
     }

     if (total < 7) {
         ESP_LOGI(TAG, "GET_VERSION: total data too short (%u)", (unsigned)total);
         return false;
     }

     out->picc_version_len = (uint8_t)total;

     uint8_t size_byte = out->picc_version[5];
     uint32_t bytes = 0;
     DESFIRE_MODEL model = desfire_model_from_size_byte(size_byte, &bytes);

     out->model = model;
     out->size_byte = size_byte;
     out->storage_bytes = bytes;
     return true;
 }
 #endif

// ---------------------------------------------------------------------------
// DESFire application/file tree reader
// ---------------------------------------------------------------------------
// Populates a read-only MfDesfireData tree by issuing native DESFire APDUs
// (NXP AN10927 / Momentum-Firmware poller-equivalent). All exchanges go
// through `pn532_in_data_exchange()`. On the PN532 backend the chip's own
// ISO14443-4 stack handles framing transparently. On the ST25R3916 backend
// the adapter routes these APDUs through the ISO14443-4A layer in
// components/esp-idf-st25r3916 (RATS/ATS + I-block framing), so the same
// code path works on both frontends.
//
// The struct layout mirrors Momentum-Firmware's MfDesfire types; the read
// logic here is a clean re-implementation. See `flipper_nfc_compat.h` for
// the upstream attribution.
//
// Only plaintext files are read; authenticated/enciphered files are skipped
// silently. This is enough for the supported-card parsers (e.g. myki, which
// lives entirely in a plaintext Standard file).

#if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_ST25R3916)

// Get a clean buffer for a single exchange, with up to 0xAF chained frames
// appended transparently. `out_size` must accommodate at least out_cap-2 bytes.
// Returns ESP_OK on success, follows the same status-byte convention as
// desfire_get_version (NXP 91 0x00 / 91 AF trailer).
static esp_err_t desfire_xch(pn532_io_handle_t io, uint8_t ins,
                             const uint8_t *payload, uint8_t payload_len,
                             uint8_t *out, size_t out_cap, size_t *out_size) {
    if (!io || !out || !out_size) return ESP_ERR_INVALID_ARG;
    *out_size = 0;
    pn532_set_indata_wait_timeout(500);

    uint8_t buf[256];
    if (payload_len + 6 > sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    buf[0] = 0x90; buf[1] = ins; buf[2] = 0x00; buf[3] = 0x00;
    uint8_t tx_len = 5;
    if (payload_len) {
        buf[4] = payload_len;
        memcpy(buf + 5, payload, payload_len);
        buf[5 + payload_len] = 0x00;  // ISO7816 Le
        tx_len = (uint8_t)(6 + payload_len);
    } else {
        buf[4] = 0x00;  // ISO7816 Le (no Lc/data)
    }

    uint8_t rlen = 255;  // pn532_in_data_exchange takes uint8_t*; 256 would wrap to 0
    esp_err_t err = pn532_in_data_exchange(io, buf, tx_len, buf, &rlen);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "xch INS=%02X transport failed: %s", ins, esp_err_to_name(err));
        return err;
    }
    if (rlen == 0) {
        ESP_LOGW(TAG, "xch INS=%02X empty response", ins);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Append chained frames until 91 0x00 is seen.
    for (;;) {
        uint8_t status = buf[rlen - 1];
        if (rlen < 2 || buf[rlen - 2] != 0x91) {
            // Non-NXP frame; pass through raw if it looks like data.
            if (status == 0x00) {
                size_t copy = rlen - 1;
                if (*out_size + copy > out_cap) copy = out_cap - *out_size;
                memcpy(out + *out_size, buf, copy);
                *out_size += copy;
                return ESP_OK;
            }
            ESP_LOGW(TAG, "xch INS=%02X malformed response len=%u tail=%02X %02X",
                     ins, (unsigned)rlen,
                     rlen >= 2 ? buf[rlen - 2] : 0,
                     buf[rlen - 1]);
            return ESP_ERR_INVALID_RESPONSE;
        }
        // NXP trailer 91 <status>
        size_t copy = rlen - 2;
        if (*out_size + copy > out_cap) copy = out_cap - *out_size;
        if (copy) memcpy(out + *out_size, buf, copy);
        *out_size += copy;

        if (status == 0x00) return ESP_OK;
        if (status != 0xAF) {
            ESP_LOGW(TAG, "xch INS=%02X DESFire status=%02X", ins, status);
            return ESP_ERR_INVALID_RESPONSE;
        }
        // Fetch more
        uint8_t more[5] = {0x90, 0xAF, 0x00, 0x00, 0x00};
        rlen = 255;
        err = pn532_in_data_exchange(io, more, sizeof(more), buf, &rlen);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "xch INS=%02X continuation failed: %s", ins, esp_err_to_name(err));
            return err;
        }
        if (rlen == 0) {
            ESP_LOGW(TAG, "xch INS=%02X continuation empty", ins);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
}

// SimpleArray helpers used only by the poller. Allocations are tracked and
// freed by desfire_tree_free().
static SimpleArray* simple_array_alloc(size_t elem_size, size_t initial_count) {
    SimpleArray* a = (SimpleArray*)calloc(1, sizeof(SimpleArray));
    if (!a) return NULL;
    a->elem_size = elem_size;
    a->count = initial_count;
    if (initial_count && elem_size) {
        a->data = calloc(initial_count, elem_size);
        if (!a->data) { free(a); return NULL; }
    }
    return a;
}

static void simple_array_free(SimpleArray* a) {
    if (!a) return;
    // Recursively free nested SimpleArrays owned by MfDesfireFileData /
    // MfDesfireApplication slots.
    if (a->elem_size == sizeof(MfDesfireFileData)) {
        MfDesfireFileData* slots = (MfDesfireFileData*)a->data;
        for (size_t i = 0; a->data && i < a->count && slots; ++i) {
            if (slots[i].data) {
                simple_array_free(slots[i].data);
                slots[i].data = NULL;
            }
        }
    } else if (a->elem_size == sizeof(MfDesfireApplication)) {
        MfDesfireApplication* slots = (MfDesfireApplication*)a->data;
        for (size_t i = 0; a->data && i < a->count && slots; ++i) {
            if (slots[i].file_ids)    simple_array_free(slots[i].file_ids);
            if (slots[i].file_settings) simple_array_free(slots[i].file_settings);
            if (slots[i].file_data)   simple_array_free(slots[i].file_data);
        }
    }
    free(a->data);
    free(a);
}

MfDesfireData* desfire_tree_alloc(void) {
    MfDesfireData* d = (MfDesfireData*)calloc(1, sizeof(MfDesfireData));
    if (!d) return NULL;
    d->application_ids = simple_array_alloc(sizeof(MfDesfireApplicationId), 0);
    d->applications    = simple_array_alloc(sizeof(MfDesfireApplication), 0);
    if (!d->application_ids || !d->applications) {
        desfire_tree_free(d);
        return NULL;
    }
    return d;
}

void desfire_tree_free(MfDesfireData* data) {
    if (!data) return;
    if (data->applications)    simple_array_free(data->applications);
    if (data->application_ids) simple_array_free(data->application_ids);
    free(data);
}

// Grow a SimpleArray by one zero-initialised slot and return a pointer to it.
static void* simple_array_push(SimpleArray* a) {
    if (!a || a->elem_size == 0) return NULL;
    void* nd = realloc(a->data, (a->count + 1) * a->elem_size);
    if (!nd) return NULL;
    a->data = nd;
    void* slot = (uint8_t*)a->data + a->count * a->elem_size;
    memset(slot, 0, a->elem_size);
    a->count++;
    return slot;
}

// Parse a FileSettings record (AN10927 §4.5/§4.6/§4.8). Returns the number
// of bytes consumed or 0 on malformed data.
//
// Only the bits needed to drive READ_DATA / READ_RECORDS are decoded here:
// file type, comm mode, and the file size hint (Standard/Backup) or record
// geometry (Linear/Cyclic). Access-rights bytes are stored verbatim into the
// first slot of access_rights[] so parsers can introspect them if needed.
static size_t parse_file_settings(const uint8_t* buf, size_t len, MfDesfireFileSettings* out) {
    if (len < 4) return 0;
    memset(out, 0, sizeof(*out));
    out->type = (MfDesfireFileType)buf[0];
    out->comm = (MfDesfireFileCommunicationSettings)(buf[1] & 0x03);
    // The common settings header is type, comm, and one little-endian
    // 16-bit access-rights word. Extra access rights (EV3) follow the
    // type-specific payload and are not needed by the current parsers.
    out->access_rights[0] = (uint16_t)(buf[2] | ((uint16_t)buf[3] << 8));
    out->access_rights_len = 1;

    size_t off = 4;
    switch (out->type) {
        case MfDesfireFileTypeStandard:
        case MfDesfireFileTypeBackup: {
            // FileSize is 3 bytes LE.
            if (off + 3 > len) return 0;
            uint32_t sz = ((uint32_t)buf[off] |
                           ((uint32_t)buf[off + 1] << 8) |
                           ((uint32_t)buf[off + 2] << 16));
            out->data.size = sz;
            return off + 3;
        }
        case MfDesfireFileTypeValue:
            if (off + 13 > len) return 0;
            out->value.lo_limit = ((uint32_t)buf[off] |
                                   ((uint32_t)buf[off + 1] << 8) |
                                   ((uint32_t)buf[off + 2] << 16) |
                                   ((uint32_t)buf[off + 3] << 24));
            out->value.hi_limit = ((uint32_t)buf[off + 4] |
                                   ((uint32_t)buf[off + 5] << 8) |
                                   ((uint32_t)buf[off + 6] << 16) |
                                   ((uint32_t)buf[off + 7] << 24));
            // LimitedCreditValue is 4 bytes LE, then Enabled flag (1 byte).
            out->value.limited_credit_value = (int32_t)(((uint32_t)buf[off + 8] |
                    ((uint32_t)buf[off + 9] << 8) |
                    ((uint32_t)buf[off + 10] << 16) |
                    ((uint32_t)buf[off + 11] << 24)));
            out->value.limited_credit_enabled = buf[off + 12] != 0;
            return off + 13;
        case MfDesfireFileTypeLinearRecord:
        case MfDesfireFileTypeCyclicRecord:
            // RecordSize, MaxRecords, CurrentRecords are each 3 bytes LE.
            if (off + 9 > len) return 0;
            out->record.size = ((uint32_t)buf[off] |
                                ((uint32_t)buf[off + 1] << 8) |
                                ((uint32_t)buf[off + 2] << 16));
            out->record.max  = ((uint32_t)buf[off + 3] |
                                ((uint32_t)buf[off + 4] << 8) |
                                ((uint32_t)buf[off + 5] << 16));
            out->record.cur  = ((uint32_t)buf[off + 6] |
                                ((uint32_t)buf[off + 7] << 8) |
                                ((uint32_t)buf[off + 8] << 16));
            return off + 9;
        default:
            return off;
    }
}

typedef struct {
    MfDesfireApplicationId aid;
    const uint8_t* file_ids;
    size_t file_count;
    const char* label;
} desfire_known_app_t;

static const uint8_t myki_files[] = {0x0F};
static const uint8_t myki_journal_files[] = {0x02, 0x0F};
static const uint8_t clipper_files[] = {0x02, 0x06, 0x08, 0x0E};
static const uint8_t itso_files[] = {0x0F};
static const uint8_t opal_files[] = {0x07};

// Some DESFire deployments protect GET_APPLICATION_IDS / GET_FILE_IDS while
// leaving specific public files readable. Supported-card parsers already know
// their AIDs and file IDs, so probe those directly when directory listing is
// denied (typically status 0x9D).
static const desfire_known_app_t known_apps[] = {
    {{{0x00, 0x11, 0xF2}}, myki_files, sizeof(myki_files), "myki"},
    {{{0xF0, 0x10, 0xF2}}, myki_journal_files, sizeof(myki_journal_files), "myki journal"},
    {{{0x90, 0x11, 0xF2}}, clipper_files, sizeof(clipper_files), "Clipper"},
    {{{0x91, 0x11, 0xF2}}, clipper_files, sizeof(clipper_files), "Clipper mobile"},
    {{{0x16, 0x02, 0xA0}}, itso_files, sizeof(itso_files), "ITSO"},
    {{{0x31, 0x45, 0x53}}, opal_files, sizeof(opal_files), "Opal"},
};

static const desfire_known_app_t* find_known_app(const MfDesfireApplicationId* aid) {
    if (!aid) return NULL;
    for (size_t i = 0; i < sizeof(known_apps) / sizeof(known_apps[0]); ++i) {
        if (memcmp(aid->data, known_apps[i].aid.data, MF_DESFIRE_APP_ID_SIZE) == 0) {
            return &known_apps[i];
        }
    }
    return NULL;
}

bool desfire_read_tree(pn532_io_handle_t io, MfDesfireData* out) {
    if (!io || !out) return false;

    // 1) GET_APPLICATION_IDS (0x6A)
    uint8_t aids[64];
    size_t aids_len = 0;
    esp_err_t list_err = desfire_xch(io, 0x6A, NULL, 0, aids, sizeof(aids), &aids_len);
    if (list_err == ESP_OK && aids_len >= 3) {
        size_t aid_count = aids_len / 3;  // AIDs are 3 bytes each.
        out->application_ids->data = realloc(out->application_ids->data,
                                             aid_count * out->application_ids->elem_size);
        if (!out->application_ids->data) return false;
        out->application_ids->count = aid_count;
        memcpy(out->application_ids->data, aids, aid_count * 3);

        out->applications->data = realloc(out->applications->data,
                aid_count * out->applications->elem_size);
        if (!out->applications->data) return false;
        out->applications->count = aid_count;
        memset(out->applications->data, 0, aid_count * out->applications->elem_size);
        ESP_LOGI(TAG, "read_tree: listed %u application(s)", (unsigned)aid_count);
    } else {
        ESP_LOGW(TAG, "read_tree: application listing unavailable; probing known AIDs");
        for (size_t i = 0; i < sizeof(known_apps) / sizeof(known_apps[0]); ++i) {
            size_t probe_len = 0;
            if (desfire_xch(io, 0x5A, known_apps[i].aid.data, 3,
                            aids, sizeof(aids), &probe_len) != ESP_OK) {
                continue;
            }
            MfDesfireApplicationId* aid_slot = simple_array_push(out->application_ids);
            MfDesfireApplication* app_slot = simple_array_push(out->applications);
            if (!aid_slot || !app_slot) return false;
            *aid_slot = known_apps[i].aid;
            ESP_LOGI(TAG, "read_tree: found known app %s (%02X%02X%02X)",
                     known_apps[i].label,
                     aid_slot->data[0], aid_slot->data[1], aid_slot->data[2]);
        }
        if (out->application_ids->count == 0) {
            ESP_LOGW(TAG, "read_tree: no readable applications found");
            return false;
        }
    }

    // 2) For each AID, SELECT_APPLICATION (0x5A), GET_FILE_IDS (0x6F),
    //    GET_FILE_SETTINGS (0xF5) and READ_DATA / READ_RECORDS where plaintext.
    for (size_t a = 0; a < out->application_ids->count; ++a) {
        MfDesfireApplicationId* aid =
            &((MfDesfireApplicationId*)out->application_ids->data)[a];
        MfDesfireApplication* app =
            &((MfDesfireApplication*)out->applications->data)[a];

        app->file_ids     = simple_array_alloc(sizeof(MfDesfireFileId), 0);
        app->file_settings = simple_array_alloc(sizeof(MfDesfireFileSettings), 0);
        app->file_data    = simple_array_alloc(sizeof(MfDesfireFileData), 0);
        if (!app->file_ids || !app->file_settings || !app->file_data) continue;

        if (desfire_xch(io, 0x5A, aid->data, 3, aids, sizeof(aids), &aids_len) != ESP_OK) {
            ESP_LOGD(TAG, "read_tree: SELECT_APPLICATION %02X%02X%02X failed",
                     aid->data[0], aid->data[1], aid->data[2]);
            continue;
        }
        // aids is ephemeral here; fetch file ids or use the parser-known set
        // when the directory is protected.
        uint8_t fids[64];
        size_t fids_len = 0;
        if (desfire_xch(io, 0x6F, NULL, 0, fids, sizeof(fids), &fids_len) != ESP_OK) {
            const desfire_known_app_t* known = find_known_app(aid);
            if (!known || known->file_count > sizeof(fids)) continue;
            memcpy(fids, known->file_ids, known->file_count);
            fids_len = known->file_count;
            ESP_LOGI(TAG, "read_tree: using known %s file map", known->label);
        }

        for (size_t fi = 0; fi < fids_len; ++fi) {
            MfDesfireFileId fid = fids[fi];
            void* id_slot = simple_array_push(app->file_ids);
            void* st_slot = simple_array_push(app->file_settings);
            void* fd_slot = simple_array_push(app->file_data);
            if (!id_slot || !st_slot || !fd_slot) break;
            *((MfDesfireFileId*)id_slot) = fid;

            // GET_FILE_SETTINGS (0xF5) with 1-byte payload = file id
            uint8_t sbuf[32];
            size_t s_len = 0;
            if (desfire_xch(io, 0xF5, &fid, 1, sbuf, sizeof(sbuf), &s_len) != ESP_OK) {
                continue;
            }
            MfDesfireFileSettings* fs = (MfDesfireFileSettings*)st_slot;
            if (parse_file_settings(sbuf, s_len, fs) == 0) {
                // Keep slot present but with default-zero settings.
                continue;
            }

            // Only attempt plaintext Standard / Backup / Linear / Cyclic reads.
            if (fs->comm != MfDesfireFileCommunicationSettingsPlaintext) continue;

            MfDesfireFileData* fdata = (MfDesfireFileData*)fd_slot;

            if (fs->type == MfDesfireFileTypeStandard ||
                fs->type == MfDesfireFileTypeBackup) {
                uint32_t want = fs->data.size;
                if (want == 0 || want > 4096) want = 4096;

                uint8_t* rbuf = (uint8_t*)malloc(want + 8);
                if (!rbuf) continue;
                size_t rlen = 0;
                // File ID + 3-byte LE offset + 3-byte LE byte count.
                uint8_t pld[7] = {fid, 0, 0, 0,
                                  (uint8_t)want,
                                  (uint8_t)(want >> 8),
                                  (uint8_t)(want >> 16)};
                if (desfire_xch(io, 0xBD, pld, sizeof(pld), rbuf, want, &rlen) != ESP_OK) {
                    free(rbuf);
                    continue;
                }
                SimpleArray* blob = simple_array_alloc(1, rlen);
                if (!blob) { free(rbuf); continue; }
                memcpy(blob->data, rbuf, rlen);
                free(rbuf);
                fdata->data = blob;
            } else if (fs->type == MfDesfireFileTypeLinearRecord ||
                       fs->type == MfDesfireFileTypeCyclicRecord) {
                uint32_t want = fs->record.size && fs->record.max
                                  ? (uint32_t)fs->record.size * (uint32_t)fs->record.max
                                  : 4096;
                if (want == 0 || want > 4096) want = 4096;
                uint8_t pld[7] = {fid, 0, 0, 0,
                                  (uint8_t)want,
                                  (uint8_t)(want >> 8),
                                  (uint8_t)(want >> 16)};
                uint8_t* rbuf = (uint8_t*)malloc(want + 8);
                if (!rbuf) continue;
                size_t rlen = 0;
                if (desfire_xch(io, 0xBB, pld, sizeof(pld), rbuf, want, &rlen) != ESP_OK) {
                    free(rbuf);
                    continue;
                }
                SimpleArray* blob = simple_array_alloc(1, rlen);
                if (!blob) { free(rbuf); continue; }
                memcpy(blob->data, rbuf, rlen);
                free(rbuf);
                fdata->data = blob;
            }
        }
    }

    // Re-select master application (AID 0x000000) so subsequent commands on
    // the PICC land at a known state. Soft-fail.
    uint8_t master[3] = {0, 0, 0};
    (void)desfire_xch(io, 0x5A, master, 3, aids, sizeof(aids), &aids_len);

    return true;
}

/* ---------------------------------------------------------------------------
 * Saved-file loader: reconstruct the MfDesfire tree from a Flipper .nfc written
 * by desfire_build_flipper_text(). Inverse of the writer; lets the saved-tag
 * details path run the same supported-card parsers (Opal/myki/ITSO) as a live
 * scan. Reads no reader hardware, but lives inside the backend guard because it
 * reuses the static SimpleArray helpers above.
 * ------------------------------------------------------------------------- */

/* Parse a run of space-separated hex byte tokens into out[]; returns the count. */
static size_t desfire_hex_stream(const char *s, uint8_t *out, size_t max) {
    size_t n = 0;
    while (s && *s && n < max) {
        while (*s == ' ' || *s == '\t') s++;
        unsigned b = 0;
        int consumed = 0;
        if (sscanf(s, "%2x%n", &b, &consumed) == 1 && consumed > 0) {
            out[n++] = (uint8_t)b;
            s += consumed;
        } else {
            break;
        }
    }
    return n;
}

static int desfire_find_app_idx(const MfDesfireData *t, const uint8_t aid[MF_DESFIRE_APP_ID_SIZE]) {
    if (!t || !t->application_ids) return -1;
    const MfDesfireApplicationId *a = (const MfDesfireApplicationId *)t->application_ids->data;
    for (size_t i = 0; i < t->application_ids->count; ++i)
        if (memcmp(a[i].data, aid, MF_DESFIRE_APP_ID_SIZE) == 0) return (int)i;
    return -1;
}

static int desfire_find_file_idx(const MfDesfireApplication *app, uint8_t fid) {
    if (!app || !app->file_ids) return -1;
    const MfDesfireFileId *ids = (const MfDesfireFileId *)app->file_ids->data;
    for (size_t i = 0; i < app->file_ids->count; ++i)
        if (ids[i] == fid) return (int)i;
    return -1;
}

MfDesfireData *desfire_load_flipper_file(const char *path,
                                         desfire_version_t *ver_out,
                                         bool *have_ver_out) {
    if (ver_out) memset(ver_out, 0, sizeof(*ver_out));
    if (have_ver_out) *have_ver_out = false;
    if (!path) return NULL;

    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    /* Data lines can hold a whole file (writer caps reads at 4096 bytes -> up to
     * ~12 KB of "XX " text), so the line buffer must be large. */
    const size_t LINE_CAP = 16384;
    char *line = (char *)malloc(LINE_CAP);
    uint8_t *hexbuf = (uint8_t *)malloc(4096);
    MfDesfireData *tree = desfire_tree_alloc();
    if (!line || !hexbuf || !tree) {
        free(line);
        free(hexbuf);
        if (tree) desfire_tree_free(tree);
        fclose(f);
        return NULL;
    }

    while (fgets(line, (int)LINE_CAP, f)) {
        size_t ll = strlen(line);
        while (ll && (line[ll - 1] == '\n' || line[ll - 1] == '\r')) line[--ll] = '\0';

        /* PICC Version bytes -> reconstruct desfire_version_t. */
        if (strncmp(line, "PICC Version:", 13) == 0) {
            uint8_t vb[DESFIRE_PICC_VERSION_MAX];
            size_t n = desfire_hex_stream(line + 13, vb, sizeof(vb));
            if (n > 0 && ver_out) {
                memcpy(ver_out->picc_version, vb, n);
                ver_out->picc_version_len = (uint8_t)n;
                if (n >= 6) {
                    ver_out->size_byte = vb[5];
                    ver_out->model = desfire_model_from_size_byte(vb[5], &ver_out->storage_bytes);
                }
                if (have_ver_out) *have_ver_out = true;
            }
            continue;
        }

        /* "Application IDs: aa aa aa bb bb bb" -> allocate app slots. */
        if (strncmp(line, "Application IDs:", 16) == 0) {
            size_t n = desfire_hex_stream(line + 16, hexbuf, 4096);
            for (size_t i = 0; i + MF_DESFIRE_APP_ID_SIZE <= n; i += MF_DESFIRE_APP_ID_SIZE) {
                MfDesfireApplicationId *aid_slot = simple_array_push(tree->application_ids);
                MfDesfireApplication *app_slot = simple_array_push(tree->applications);
                if (!aid_slot || !app_slot) break;
                memcpy(aid_slot->data, &hexbuf[i], MF_DESFIRE_APP_ID_SIZE);
                app_slot->file_ids = simple_array_alloc(sizeof(MfDesfireFileId), 0);
                app_slot->file_settings = simple_array_alloc(sizeof(MfDesfireFileSettings), 0);
                app_slot->file_data = simple_array_alloc(sizeof(MfDesfireFileData), 0);
            }
            continue;
        }

        /* Per-application lines: "Application aabbcc <key>" (6 lowercase hex AID). */
        if (strncmp(line, "Application ", 12) == 0 && ll >= 19 && line[18] == ' ') {
            uint8_t aid[MF_DESFIRE_APP_ID_SIZE];
            char aidhex[7];
            memcpy(aidhex, line + 12, 6);
            aidhex[6] = '\0';
            if (desfire_hex_stream(aidhex, aid, MF_DESFIRE_APP_ID_SIZE) != MF_DESFIRE_APP_ID_SIZE)
                continue; /* not an AID block (e.g. "Application Count:") */
            int ai = desfire_find_app_idx(tree, aid);
            if (ai < 0) continue;
            MfDesfireApplication *app =
                &((MfDesfireApplication *)tree->applications->data)[ai];
            const char *key = line + 19; /* past "Application aabbcc " */

            if (strncmp(key, "File IDs:", 9) == 0) {
                size_t n = desfire_hex_stream(key + 9, hexbuf, 256);
                for (size_t i = 0; i < n; ++i) {
                    MfDesfireFileId *id = (MfDesfireFileId *)simple_array_push(app->file_ids);
                    simple_array_push(app->file_settings);
                    simple_array_push(app->file_data);
                    if (id) *id = hexbuf[i];
                }
                continue;
            }

            if (strncmp(key, "File ", 5) != 0) continue;
            const char *fk = key + 5; /* "1 Type: ..", "1: AA BB", "12 Size: N" */
            char *endp = NULL;
            unsigned long fid = strtoul(fk, &endp, 10);
            if (endp == fk || fid > 0xFF) continue;
            int fx = desfire_find_file_idx(app, (uint8_t)fid);
            if (fx < 0) continue;
            /* file_ids/file_settings/file_data are pushed in lockstep; guard in
             * case an OOM left them unequal so we never index out of bounds. */
            if ((size_t)fx >= app->file_settings->count ||
                (size_t)fx >= app->file_data->count)
                continue;
            MfDesfireFileSettings *fs =
                &((MfDesfireFileSettings *)app->file_settings->data)[fx];
            MfDesfireFileData *fd = &((MfDesfireFileData *)app->file_data->data)[fx];

            if (*endp == ':') {
                /* File data line: "File N: AA BB ..". */
                size_t n = desfire_hex_stream(endp + 1, hexbuf, 4096);
                if (n > 0) {
                    SimpleArray *blob = simple_array_alloc(1, n);
                    if (blob) {
                        memcpy(blob->data, hexbuf, n);
                        if (fd->data) simple_array_free(fd->data);
                        fd->data = blob;
                    }
                }
                continue;
            }
            if (*endp != ' ') continue;
            const char *sub = endp + 1; /* "Type: 00", etc. */
            uint8_t one;
            if (strncmp(sub, "Type:", 5) == 0) {
                if (desfire_hex_stream(sub + 5, &one, 1) == 1) fs->type = (MfDesfireFileType)one;
            } else if (strncmp(sub, "Communication Settings:", 23) == 0) {
                if (desfire_hex_stream(sub + 23, &one, 1) == 1)
                    fs->comm = (MfDesfireFileCommunicationSettings)one;
            } else if (strncmp(sub, "Access Rights:", 14) == 0) {
                uint8_t ar[MF_DESFIRE_MAX_KEYS * 2];
                size_t n = desfire_hex_stream(sub + 14, ar, sizeof(ar));
                uint8_t r = 0;
                for (size_t i = 0; i + 1 < n && r < MF_DESFIRE_MAX_KEYS; i += 2)
                    fs->access_rights[r++] = (uint16_t)(ar[i] | (ar[i + 1] << 8));
                fs->access_rights_len = r;
            } else if (strncmp(sub, "Size:", 5) == 0) {
                unsigned long sz = strtoul(sub + 5, NULL, 10);
                if (fs->type == MfDesfireFileTypeLinearRecord ||
                    fs->type == MfDesfireFileTypeCyclicRecord)
                    fs->record.size = (uint32_t)sz;
                else
                    fs->data.size = (uint32_t)sz;
            } else if (strncmp(sub, "Max:", 4) == 0) {
                fs->record.max = (uint32_t)strtoul(sub + 4, NULL, 10);
            } else if (strncmp(sub, "Cur:", 4) == 0) {
                fs->record.cur = (uint32_t)strtoul(sub + 4, NULL, 10);
            } else if (strncmp(sub, "Hi Limit:", 9) == 0) {
                fs->value.hi_limit = (uint32_t)strtoul(sub + 9, NULL, 10);
            } else if (strncmp(sub, "Lo Limit:", 9) == 0) {
                fs->value.lo_limit = (uint32_t)strtoul(sub + 9, NULL, 10);
            } else if (strncmp(sub, "Limited Credit Value:", 21) == 0) {
                fs->value.limited_credit_value = (int32_t)strtol(sub + 21, NULL, 10);
            } else if (strncmp(sub, "Limited Credit Enabled:", 23) == 0) {
                const char *p = sub + 23;
                while (*p == ' ') p++;
                fs->value.limited_credit_enabled = (strncmp(p, "true", 4) == 0);
            }
            continue;
        }
    }

    free(line);
    free(hexbuf);
    fclose(f);

    if (tree->application_ids->count == 0) {
        desfire_tree_free(tree);
        return NULL;
    }
    return tree;
}
#endif

// ---------------------------------------------------------------------------
// Flipper-format .nfc file builder
// ---------------------------------------------------------------------------
// Produces a text dump in Flipper Zero's FlipperFormat key-value style so the
// saved file is byte-compatible with what a Flipper creates when scanning a
// MIFARE DESFire card. Applications without key settings (we don't read them)
// get sensible defaults; file data is written when available.
//
// Format reference: Momentum-Firmware lib/nfc/protocols/mf_desfire/mf_desfire_i.c

char *desfire_build_flipper_text(const MfDesfireData *tree,
                                 const uint8_t *uid, uint8_t uid_len,
                                 uint16_t atqa, uint8_t sak,
                                 const desfire_version_t *ver) {
    if (!tree) return NULL;

    size_t cap = 4096;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    size_t len = 0;

#define DES_APPEND(fmt, ...) do { \
        int _n = snprintf(buf + len, cap - len, fmt, ##__VA_ARGS__); \
        if (_n < 0 || (size_t)_n >= cap - len) { \
            while ((size_t)_n >= cap - len) { \
                cap *= 2; \
                char *_nb = (char *)realloc(buf, cap); \
                if (!_nb) { free(buf); return NULL; } \
                buf = _nb; \
                _n = snprintf(buf + len, cap - len, fmt, ##__VA_ARGS__); \
            } \
        } \
        len += (size_t)_n; \
    } while (0)

    /* ---- Flipper NFC header (iso14443_4a_save equivalent) ---- */
    DES_APPEND("Filetype: Flipper NFC device\n");
    DES_APPEND("Version: 4\n");
    DES_APPEND("# ISO14443-4A specific data\n");
    DES_APPEND("UID:");
    for (uint8_t i = 0; i < uid_len; ++i) DES_APPEND(" %02X", uid[i]);
    DES_APPEND("\n");
    DES_APPEND("ATQA: %02X %02X\n", (atqa >> 8) & 0xFF, atqa & 0xFF);
    DES_APPEND("SAK: %02X\n", sak);

    /* ---- DESFire PICC section ---- */
    DES_APPEND("# Mifare DESFire specific data\n");
    if (ver && ver->picc_version_len > 0) {
        DES_APPEND("PICC Version:");
        for (uint8_t i = 0; i < ver->picc_version_len; ++i)
            DES_APPEND(" %02X", ver->picc_version[i]);
        DES_APPEND("\n");
    }

    /* ---- Master key settings (defaults — we don't read them) ---- */
    DES_APPEND("PICC Change Key ID: 00\n");
    DES_APPEND("PICC Config Changeable: true\n");
    DES_APPEND("PICC Free Create Delete: true\n");
    DES_APPEND("PICC Free Directory List: true\n");
    DES_APPEND("PICC Key Changeable: true\n");
    DES_APPEND("PICC Flags: 00\n");
    DES_APPEND("PICC Max Keys: 01\n");
    DES_APPEND("PICC Key 0 Version: 00\n");

    /* ---- Application section ---- */
    size_t app_count = simple_array_get_count(tree->application_ids);
    DES_APPEND("Application Count: %u\n", (unsigned)app_count);

    if (app_count > 0) {
        /* All AIDs on one line, hex bytes */
        DES_APPEND("Application IDs:");
        for (size_t i = 0; i < app_count; ++i) {
            const MfDesfireApplicationId *aid =
                (const MfDesfireApplicationId *)simple_array_cget(tree->application_ids, i);
            DES_APPEND(" %02X %02X %02X", aid->data[0], aid->data[1], aid->data[2]);
        }
        DES_APPEND("\n");

        /* Per-application data */
        for (size_t a = 0; a < app_count; ++a) {
            const MfDesfireApplicationId *aid =
                (const MfDesfireApplicationId *)simple_array_cget(tree->application_ids, a);
            const MfDesfireApplication *app =
                (const MfDesfireApplication *)simple_array_cget(tree->applications, a);

            /* Flipper uses lowercase hex for the AID prefix */
            char pfx[32];
            snprintf(pfx, sizeof(pfx), "Application %02x%02x%02x",
                     aid->data[0], aid->data[1], aid->data[2]);

            /* Key settings (defaults since we don't authenticate) */
            DES_APPEND("%s Change Key ID: 00\n", pfx);
            DES_APPEND("%s Config Changeable: true\n", pfx);
            DES_APPEND("%s Free Create Delete: true\n", pfx);
            DES_APPEND("%s Free Directory List: true\n", pfx);
            DES_APPEND("%s Key Changeable: true\n", pfx);
            DES_APPEND("%s Flags: 00\n", pfx);
            DES_APPEND("%s Max Keys: 01\n", pfx);
            DES_APPEND("%s Key 0 Version: 00\n", pfx);

            if (!app || !app->file_ids) continue;

            size_t file_count = simple_array_get_count(app->file_ids);
            if (file_count > 0) {
                /* File IDs as hex bytes */
                DES_APPEND("%s File IDs:", pfx);
                for (size_t f = 0; f < file_count; ++f) {
                    const MfDesfireFileId *fid =
                        (const MfDesfireFileId *)simple_array_cget(app->file_ids, f);
                    DES_APPEND(" %02X", *fid);
                }
                DES_APPEND("\n");

                /* Per-file settings + data */
                for (size_t f = 0; f < file_count; ++f) {
                    const MfDesfireFileId *fid =
                        (const MfDesfireFileId *)simple_array_cget(app->file_ids, f);
                    const MfDesfireFileSettings *fs =
                        (const MfDesfireFileSettings *)simple_array_cget(app->file_settings, f);
                    const MfDesfireFileData *fd =
                        (const MfDesfireFileData *)simple_array_cget(app->file_data, f);

                    /* Flipper uses decimal file ID in the per-file prefix */
                    unsigned fdec = *fid;

                    if (fs) {
                        DES_APPEND("%s File %u Type: %02X\n", pfx, fdec, (unsigned)fs->type);
                        DES_APPEND("%s File %u Communication Settings: %02X\n",
                                   pfx, fdec, (unsigned)fs->comm);

                        /* Access rights: raw LE bytes (matches Flipper's
                         * flipper_format_write_hex of the uint16_t array) */
                        DES_APPEND("%s File %u Access Rights:", pfx, fdec);
                        for (uint8_t r = 0; r < fs->access_rights_len; ++r) {
                            DES_APPEND(" %02X %02X",
                                       fs->access_rights[r] & 0xFF,
                                       (fs->access_rights[r] >> 8) & 0xFF);
                        }
                        DES_APPEND("\n");

                        /* Type-specific size/limit fields (decimal uint32) */
                        if (fs->type == MfDesfireFileTypeStandard ||
                            fs->type == MfDesfireFileTypeBackup) {
                            DES_APPEND("%s File %u Size: %u\n", pfx, fdec,
                                       (unsigned)fs->data.size);
                        } else if (fs->type == MfDesfireFileTypeValue) {
                            DES_APPEND("%s File %u Hi Limit: %u\n", pfx, fdec,
                                       (unsigned)fs->value.hi_limit);
                            DES_APPEND("%s File %u Lo Limit: %u\n", pfx, fdec,
                                       (unsigned)fs->value.lo_limit);
                            DES_APPEND("%s File %u Limited Credit Value: %d\n", pfx, fdec,
                                       (int)fs->value.limited_credit_value);
                            DES_APPEND("%s File %u Limited Credit Enabled: %s\n", pfx, fdec,
                                       fs->value.limited_credit_enabled ? "true" : "false");
                        } else if (fs->type == MfDesfireFileTypeLinearRecord ||
                                   fs->type == MfDesfireFileTypeCyclicRecord) {
                            DES_APPEND("%s File %u Size: %u\n", pfx, fdec,
                                       (unsigned)fs->record.size);
                            DES_APPEND("%s File %u Max: %u\n", pfx, fdec,
                                       (unsigned)fs->record.max);
                            DES_APPEND("%s File %u Cur: %u\n", pfx, fdec,
                                       (unsigned)fs->record.cur);
                        }
                    }

                    /* File data: hex bytes (key is just the prefix) */
                    if (fd && fd->data) {
                        size_t dlen = simple_array_get_count(fd->data);
                        if (dlen > 0) {
                            const uint8_t *d = (const uint8_t *)simple_array_cget_data(fd->data);
                            DES_APPEND("%s File %u:", pfx, fdec);
                            for (size_t b = 0; b < dlen; ++b)
                                DES_APPEND(" %02X", d[b]);
                            DES_APPEND("\n");
                        }
                    }
                }
            }
        }
    }

#undef DES_APPEND
    return buf;
}

const char *desfire_model_str(DESFIRE_MODEL m) {
    switch (m) {
        case DESFIRE_MODEL_2K:  return "MIFARE DESFire 2K";
        case DESFIRE_MODEL_4K:  return "MIFARE DESFire 4K";
        case DESFIRE_MODEL_8K:  return "MIFARE DESFire 8K";
        default:                return "MIFARE DESFire";
    }
}

bool desfire_build_picc_version_line(const desfire_version_t *ver,
                                     char *out,
                                     size_t out_cap) {
    if (!out || out_cap == 0) return false;
    if (!ver || ver->picc_version_len == 0) {
        out[0] = '\0';
        return false;
    }

    int len = snprintf(out, out_cap, "PICC Version:");
    if (len < 0 || (size_t)len >= out_cap) {
        if (out_cap) out[out_cap - 1] = '\0';
        return false;
    }

    for (uint8_t i = 0; i < ver->picc_version_len; ++i) {
        if ((size_t)len >= out_cap - 4) break;
        len += snprintf(out + len, out_cap - (size_t)len, " %02X", ver->picc_version[i]);
        if (len < 0) {
            out[0] = '\0';
            return false;
        }
    }

    return true;
}

char *desfire_build_details_summary(const desfire_version_t *ver,
                                    const uint8_t *uid,
                                    uint8_t uid_len,
                                    uint16_t atqa,
                                    uint8_t sak) {
    const char *label = ver ? desfire_model_str(ver->model) : "MIFARE DESFire";

    size_t cap = 256;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;

    int len = snprintf(out, cap, "Type: %s\nUID:", label);
    if (len < 0 || (size_t)len >= cap) {
        free(out);
        return NULL;
    }

    for (uint8_t i = 0; i < uid_len && (size_t)len < cap - 4; ++i) {
        len += snprintf(out + len, cap - (size_t)len, " %02X", uid[i]);
        if (len < 0 || (size_t)len >= cap) break;
    }

    len += snprintf(out + len, cap - (size_t)len,
                    "\nATQA: %02X %02X  SAK: %02X\n",
                    (atqa >> 8) & 0xFF,
                    atqa & 0xFF,
                    sak);
    if (len < 0 || (size_t)len >= cap) {
        out[cap - 1] = '\0';
        return out;
    }

    if (ver && ver->storage_bytes) {
        unsigned kb = (unsigned)(ver->storage_bytes / 1024U);
        len += snprintf(out + len, cap - (size_t)len,
                        "Mem: %u B (~%u KB)\n",
                        (unsigned)ver->storage_bytes,
                        kb);
    }

    return out;
}
