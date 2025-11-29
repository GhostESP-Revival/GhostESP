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

 #ifdef CONFIG_NFC_PN532
 bool desfire_get_version(pn532_io_handle_t io, desfire_version_t *out) {
     if (!io || !out) return false;
     memset(out, 0, sizeof(*out));

     // DESFire GET_VERSION on some tags can take noticeably longer than
     // simple MIFARE Classic/NTAG operations, so allow a more generous
     // INDATAEXCHANGE wait window here. Classic code later re-tightens this.
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

    int len = snprintf(out, cap, "Card: %s\nUID:", label);
    if (len < 0 || (size_t)len >= cap) {
        free(out);
        return NULL;
    }

    for (uint8_t i = 0; i < uid_len && (size_t)len < cap - 4; ++i) {
        len += snprintf(out + len, cap - (size_t)len, " %02X", uid[i]);
        if (len < 0 || (size_t)len >= cap) break;
    }

    len += snprintf(out + len, cap - (size_t)len,
                    "\nATQA: %02X %02X | SAK: %02X\n",
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
                        "Storage: %u bytes (~%u KB)\n",
                        (unsigned)ver->storage_bytes,
                        kb);
    }

    return out;
}
