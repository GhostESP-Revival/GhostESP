// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
//
// EMV payment card reader. Ported from Flipper Zero / Momentum-Firmware
// lib/nfc/protocols/emv/ (GPL-3.0).
// https://github.com/Next-Flip/Momentum-Firmware
//
// Implements a synchronous EMV application selection and data read that mirrors
// Momentum's emv_poller state machine:
//   1. SELECT PPSE (2PAY.SYS.DDF01)  -> AID (0x4F), priority (0x87)
//   2. SELECT AID                    -> label (0x50), app name (0x9F12),
//                                       AIP (0x82), PDOL (0x9F38)
//   3. GET PROCESSING OPTIONS (GPO)  -> AIP (0x82), AFL (0x94)
//      PDOL is populated from a terminal-values table (emv_prepare_pdol), like
//      Momentum, instead of an empty PDOL.
//   4. READ RECORD across the AFL    -> PAN (0x5A), track 2 (0x57/0x9F6B),
//                                       cardholder name (0x5F20/0x9F0B),
//                                       expiry (0x5F24), effective (0x5F25),
//                                       country (0x5F28), currency (0x9F42),
//                                       ATC (0x9F36)
//      Falls back to brute-forcing SFI 1-4 records 1-3 if no AFL, then runs a
//      second pass over unread records to recover the cardholder name.
//   5. GET DATA (80 CA)              -> PIN try counter (0x9F17),
//                                       last online ATC (0x9F13)
//
// BER-TLV (ISO 8825) parsing handles 1- and 2-byte tags (e.g. 0x5F24, 0x9F1A)
// plus short / 0x81 / 0x82 length forms. All APDUs go through
// pn532_in_data_exchange which routes to the ISO14443-4A layer on ST25R3916
// (or PN532 hardware ISO14443-4 on PN532).

#include "managers/nfc/emv.h"
#include "managers/nfc/pn532_compat.h"
#include "managers/sd_card_manager.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"

static const char *TAG = "EMV";

/* Counts consecutive transport-level failures (CRC/timeout) across APDUs.
 * Any successful exchange resets it; only when it reaches the threshold do we
 * treat the card as gone and abort the optional read passes. This recovers
 * transient RF blips (a single timeout) while still bounding the hang when the
 * card is actually removed. emv_read_card() runs on a single scan task. */
#define EMV_CARD_GONE_THRESHOLD 3u
static unsigned s_transport_fails = 0;
#define emv_card_gone() (s_transport_fails >= EMV_CARD_GONE_THRESHOLD)

// ---- EMV tag constants (ISO 7816 / EMV Book 3) ----
#define EMV_TAG_AID                      0x4F
#define EMV_TAG_PRIORITY                 0x87
#define EMV_TAG_AIP                      0x82
#define EMV_TAG_PDOL                     0x9F38
#define EMV_TAG_APPL_LABEL               0x50
#define EMV_TAG_APPL_NAME                0x9F12
#define EMV_TAG_APPL_EFFECTIVE           0x5F25
#define EMV_TAG_PIN_TRY_COUNTER          0x9F17
#define EMV_TAG_LAST_ONLINE_ATC          0x9F13
#define EMV_TAG_ATC                      0x9F36
#define EMV_TAG_LOG_ENTRY                0x9F4D
#define EMV_TAG_LOG_FMT                  0x9F4F
#define EMV_TAG_LOG_AMOUNT               0x9F02
#define EMV_TAG_LOG_COUNTRY              0x9F1A
#define EMV_TAG_LOG_CURRENCY             0x5F2A
#define EMV_TAG_LOG_DATE                 0x9A
#define EMV_TAG_LOG_TIME                 0x9F21
#define EMV_TAG_TRACK_2_EQUIV            0x57
#define EMV_TAG_TRACK_2_DATA             0x9F6B
#define EMV_TAG_PAN                      0x5A
#define EMV_TAG_AFL                      0x94
#define EMV_TAG_EXP_DATE                 0x5F24
#define EMV_TAG_COUNTRY_CODE             0x5F28
#define EMV_TAG_CURRENCY_CODE            0x9F42
#define EMV_TAG_CARDHOLDER_NAME          0x5F20
#define EMV_TAG_CARDHOLDER_NAME_EXTENDED 0x9F0B
#define EMV_TAG_GPO_FMT1                 0x80
#define EMV_TAG_GPO_FMT2                 0x77
#define EMV_TAG_READ_RECORD              0x70

#define EMV_MAX_APDU_LEN 255

// ---- PDOL terminal values (mirrors Momentum's pdol_values[]) ----
// The card's PDOL lists (tag, length) pairs it wants from the terminal; we
// answer each known tag with a realistic value and zero-fill the rest.
typedef struct {
    uint16_t tag;
    uint8_t len;
    uint8_t data[20];
} emv_pdol_value_t;

static const emv_pdol_value_t pdol_values[] = {
    {0x9F59, 3, {0xC8, 0x80, 0x00}},                           // Terminal transaction information
    {0x9F5A, 1, {0x00}},                                       // Terminal transaction type
    {0x9F58, 1, {0x01}},                                       // Merchant type indicator
    {0x9F66, 4, {0x79, 0x00, 0x40, 0x80}},                     // Terminal transaction qualifiers
    {0x9F40, 4, {0x79, 0x00, 0x40, 0x80}},                     // Additional terminal qualifications
    {0x9F02, 6, {0x00, 0x00, 0x00, 0x10, 0x00, 0x00}},         // Amount, authorised
    {0x9F03, 6, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},         // Other amount
    {0x9F1A, 2, {0x01, 0x24}},                                 // Terminal country code
    {0x5F2A, 2, {0x01, 0x24}},                                 // Transaction currency code
    {0x95,   5, {0x00, 0x00, 0x00, 0x00, 0x00}},               // Terminal verification results
    {0x9A,   3, {0x19, 0x01, 0x01}},                           // Transaction date
    {0x9C,   1, {0x00}},                                       // Transaction type
    {0x98,  20, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}, // Transaction cert hash
    {0x9F37, 4, {0x82, 0x3D, 0xDE, 0x7A}},                     // Unpredictable number
};

// ---- BER-TLV helpers (ISO 8825) ----

typedef struct {
    uint16_t tag;
    const uint8_t *value;
    size_t length;
} emv_tlv_t;

// Find a TLV object with the given tag in a BER-TLV buffer. Handles 1- and
// 2-byte tags (e.g. 0x5F24, 0x9F1A) and short-form / 0x81 / 0x82 lengths, and
// recurses into constructed objects (tag bit 5 set: 0x6F, 0xA5, 0x61, 0xBF0C..).
static bool emv_tlv_find(uint16_t tag, const uint8_t *buf, size_t len,
                         emv_tlv_t *out) {
    size_t i = 0;
    while (i < len) {
        if (i + 1 > len) break;
        uint8_t t0 = buf[i];
        // Parse tag (1 or 2 bytes; EMV card data never uses 3-byte tags)
        uint16_t tagval;
        size_t tag_len;
        if ((t0 & 0x1F) == 0x1F) {
            if (i + 2 > len) break;
            tagval = ((uint16_t)t0 << 8) | buf[i + 1];
            tag_len = 2;
        } else {
            tagval = t0;
            tag_len = 1;
        }
        size_t off = i + tag_len;
        if (off >= len) break;
        // Parse length: short form, or 0x81 / 0x82 long form
        size_t vlen;
        uint8_t l0 = buf[off];
        if (l0 < 0x80) {
            vlen = l0;
            off++;
        } else if (l0 == 0x81) {
            if (off + 2 > len) break;
            vlen = buf[off + 1];
            off += 2;
        } else if (l0 == 0x82) {
            if (off + 3 > len) break;
            vlen = (size_t)((buf[off + 1] << 8) | buf[off + 2]);
            off += 3;
        } else {
            break;
        }
        if (off + vlen > len) break;
        if (tagval == tag) {
            out->tag = tagval;
            out->value = buf + off;
            out->length = vlen;
            return true;
        }
        // Recurse into constructed objects.
        if (t0 & 0x20) {
            if (emv_tlv_find(tag, buf + off, vlen, out))
                return true;
        }
        i = off + vlen;
    }
    return false;
}

// APDU exchange helper. Builds a correct short-APDU depending on payload:
//  - data present  -> case-3/4: CLA INS P1 P2 Lc Data Le
//  - no data       -> case-2   : CLA INS P1 P2 Le   (no Lc field!)
// Emitting Lc=0 for case-2 commands (e.g. READ RECORD) is malformed and many
// EMV cards reject it with SW=6700 (wrong length). Requires SW==9000.
static bool emv_apdu(pn532_io_handle_t io, uint8_t cla, uint8_t ins,
                     uint8_t p1, uint8_t p2,
                     const uint8_t *data, uint8_t data_len,
                     uint8_t *resp, uint8_t *resp_len) {
    uint8_t apdu[128];
    uint8_t tx_len;
    if (data_len > 0) {
        if (data_len + 6u > sizeof(apdu)) return false;
        apdu[0] = cla; apdu[1] = ins; apdu[2] = p1; apdu[3] = p2;
        apdu[4] = data_len;
        memcpy(apdu + 5, data, data_len);
        apdu[5 + data_len] = 0x00; // Le
        tx_len = 6 + data_len;
    } else {
        apdu[0] = cla; apdu[1] = ins; apdu[2] = p1; apdu[3] = p2;
        apdu[4] = 0x00; // Le (case-2: no Lc)
        tx_len = 5;
    }

    uint8_t rlen = 255;
    esp_err_t err = pn532_in_data_exchange(io, apdu, tx_len, resp, &rlen);
    if (err != ESP_OK) {
        if (s_transport_fails < 255u) s_transport_fails++;
        ESP_LOGI(TAG, "APDU %02X%02X failed: %s", cla, ins, esp_err_to_name(err));
        return false;
    }
    if (rlen < 2) return false;
    uint16_t sw = (resp[rlen - 2] << 8) | resp[rlen - 1];
    if (sw != 0x9000) {
        ESP_LOGI(TAG, "APDU %02X%02X SW=%04X", cla, ins, sw);
        return false;
    }
    s_transport_fails = 0; // link is healthy
    *resp_len = rlen - 2; // strip status word
    return true;
}

// GET DATA (80 CA) exchange: case-2 (CLA INS P1 P2 Le), no command data.
static bool emv_get_data(pn532_io_handle_t io, uint16_t tag,
                         uint8_t *resp, uint8_t *resp_len) {
    uint8_t apdu[5] = { 0x80, 0xCA, (uint8_t)(tag >> 8), (uint8_t)(tag & 0xFF), 0x00 };
    uint8_t rlen = 255;
    esp_err_t err = pn532_in_data_exchange(io, apdu, sizeof(apdu), resp, &rlen);
    if (err != ESP_OK) {
        if (s_transport_fails < 255u) s_transport_fails++;
        ESP_LOGI(TAG, "GET DATA %04X failed: %s", tag, esp_err_to_name(err));
        return false;
    }
    if (rlen < 2) return false;
    uint16_t sw = (resp[rlen - 2] << 8) | resp[rlen - 1];
    if (sw != 0x9000) {
        ESP_LOGI(TAG, "GET DATA %04X SW=%04X", tag, sw);
        return false;
    }
    s_transport_fails = 0; // link is healthy
    *resp_len = rlen - 2;
    return true;
}

// ---- PDOL preparation (mirrors Momentum's emv_prepare_pdol) ----
// src is the raw PDOL (sequence of tag+length entries, no values). dest receives
// the concatenated terminal values to embed in the GPO command (tag 0x83 body).
static void emv_prepare_pdol(uint8_t *dest, size_t *dest_len, size_t dest_cap,
                             const uint8_t *src, size_t src_len) {
    size_t out = 0;
    size_t i = 0;
    while (i < src_len) {
        uint16_t tag;
        size_t tag_bytes;
        uint8_t t0 = src[i];
        if ((t0 & 0x1F) == 0x1F) {
            if (i + 1 >= src_len) break;
            tag = ((uint16_t)t0 << 8) | src[i + 1];
            tag_bytes = 2;
        } else {
            tag = t0;
            tag_bytes = 1;
        }
        if (i + tag_bytes >= src_len) break;
        uint8_t tlen = src[i + tag_bytes];
        i += tag_bytes + 1;

        bool found = false;
        for (size_t j = 0; j < sizeof(pdol_values) / sizeof(pdol_values[0]); j++) {
            if (pdol_values[j].tag == tag) {
                size_t copy = tlen < pdol_values[j].len ? tlen : pdol_values[j].len;
                if (out + copy > dest_cap) copy = dest_cap - out;
                memcpy(dest + out, pdol_values[j].data, copy);
                out += copy;
                // zero-pad remainder if the card asked for more than we carry
                for (size_t k = copy; k < tlen && out < dest_cap; k++, out++)
                    dest[out] = 0x00;
                found = true;
                break;
            }
        }
        if (!found) {
            for (size_t k = 0; k < tlen && out < dest_cap; k++, out++)
                dest[out] = 0x00;
        }
    }
    *dest_len = out;
}

// ---- Track 2 equivalent parsing (PAN + expiry, nibble-aligned) ----
// Format: PAN nibbles, 0xD separator nibble, YYMM expiry nibbles. We look for
// the byte whose high nibble is 0xD (even-nibble PAN case) and decode from there.
static void emv_parse_track2(const uint8_t *buf, size_t len, EmvApplication *app) {
    for (size_t j = 1; j < len; j++) {
        if (buf[j] > 0xD0) {
            if (app->pan_len == 0) {
                size_t copy = j; // PAN = buf[0..j-1]
                if (copy > EMV_PAN_LEN) copy = EMV_PAN_LEN;
                memcpy(app->pan, buf, copy);
                app->pan_len = copy;
            }
            if (j + 2 < len) {
                app->exp_year  = (uint8_t)((buf[j]     << 4) | (buf[j + 1] >> 4));
                app->exp_month = (uint8_t)((buf[j + 1] << 4) | (buf[j + 2] >> 4));
            }
            return;
        }
    }
}

// ---- Record decoding: extract every supported tag from one READ RECORD ----
static void emv_decode_record(const uint8_t *buf, size_t len, EmvApplication *app) {
    emv_tlv_t tlv;
    const uint8_t *d = buf;
    size_t dl = len;
    // READ RECORD responses are usually wrapped in tag 0x70
    if (emv_tlv_find(EMV_TAG_READ_RECORD, buf, len, &tlv)) {
        d = tlv.value;
        dl = tlv.length;
    }

    if (app->pan_len == 0 && emv_tlv_find(EMV_TAG_PAN, d, dl, &tlv)) {
        size_t copy = tlv.length > EMV_PAN_LEN ? EMV_PAN_LEN : tlv.length;
        memcpy(app->pan, tlv.value, copy);
        app->pan_len = copy;
    }
    // Track 2 equivalent / data — sets PAN (if still empty) and expiry
    if (emv_tlv_find(EMV_TAG_TRACK_2_EQUIV, d, dl, &tlv))
        emv_parse_track2(tlv.value, tlv.length, app);
    else if (emv_tlv_find(EMV_TAG_TRACK_2_DATA, d, dl, &tlv))
        emv_parse_track2(tlv.value, tlv.length, app);

    if (app->cardholder_name[0] == '\0' &&
        emv_tlv_find(EMV_TAG_CARDHOLDER_NAME, d, dl, &tlv) && tlv.length > 0) {
        size_t copy = tlv.length < EMV_CARDHOLDER_NAME_LEN ? tlv.length : EMV_CARDHOLDER_NAME_LEN - 1;
        memcpy(app->cardholder_name, tlv.value, copy);
        app->cardholder_name[copy] = '\0';
        // treat 0x20 (space) as terminator, like Momentum
        for (size_t k = 0; k < copy; k++)
            if (app->cardholder_name[k] == 0x20) { app->cardholder_name[k] = '\0'; break; }
    }
    if (app->cardholder_name[0] == '\0' &&
        emv_tlv_find(EMV_TAG_CARDHOLDER_NAME_EXTENDED, d, dl, &tlv) && tlv.length > 0) {
        size_t copy = tlv.length < EMV_CARDHOLDER_NAME_LEN ? tlv.length : EMV_CARDHOLDER_NAME_LEN - 1;
        memcpy(app->cardholder_name, tlv.value, copy);
        app->cardholder_name[copy] = '\0';
    }

    if (emv_tlv_find(EMV_TAG_EXP_DATE, d, dl, &tlv) && tlv.length >= 2) {
        app->exp_year = tlv.value[0];
        app->exp_month = tlv.value[1];
        if (tlv.length >= 3) app->exp_day = tlv.value[2];
    }
    if (emv_tlv_find(EMV_TAG_APPL_EFFECTIVE, d, dl, &tlv) && tlv.length >= 2) {
        app->effective_year = tlv.value[0];
        app->effective_month = tlv.value[1];
        if (tlv.length >= 3) app->effective_day = tlv.value[2];
    }
    if (emv_tlv_find(EMV_TAG_COUNTRY_CODE, d, dl, &tlv) && tlv.length >= 2)
        app->country_code = (tlv.value[0] << 8) | tlv.value[1];
    if (emv_tlv_find(EMV_TAG_CURRENCY_CODE, d, dl, &tlv) && tlv.length >= 2)
        app->currency_code = (tlv.value[0] << 8) | tlv.value[1];
    if (emv_tlv_find(EMV_TAG_ATC, d, dl, &tlv) && tlv.length >= 2)
        app->transaction_counter = (tlv.value[0] << 8) | tlv.value[1];
    if (emv_tlv_find(EMV_TAG_AIP, d, dl, &tlv) && tlv.length >= 2)
        memcpy(app->application_interchange_profile, tlv.value, 2);
    if (emv_tlv_find(EMV_TAG_LOG_ENTRY, d, dl, &tlv) && tlv.length >= 2) {
        app->log_sfi = tlv.value[0];
        app->log_records = tlv.value[1];
    }
}

// Decode one transaction-log record. The card describes each log entry's layout
// with a format template (0x9F4F): a sequence of {tag, length} pairs. The record
// itself is NOT TLV — it is a flat concatenation of values, so we walk it
// positionally using the format (mirrors Momentum's emv_decode_tl).
static void emv_decode_log_entry(const uint8_t *buf, size_t len,
                                 const uint8_t *fmt, size_t fmt_len,
                                 EmvApplication *app) {
    if (app->trans_count >= EMV_TRANS_MAX) return;
    /* Some cards wrap the log entry in a Read Record template (tag 0x70); the
     * log format describes only the inner positional data, so strip it. */
    if (len >= 2 && buf[0] == EMV_TAG_READ_RECORD) {
        size_t hdr = 2;
        size_t inner = buf[1];
        if (buf[1] == 0x81 && len >= 3) { inner = buf[2]; hdr = 3; }
        if (hdr + inner <= len) { buf += hdr; len = inner; }
    }
    EmvTransaction *t = &app->trans[app->trans_count];

    size_t i = 0;   /* byte index into the record data */
    size_t f = 0;   /* byte index into the format template */
    while (f < fmt_len && i < len) {
        uint16_t tag;
        size_t tag_bytes;
        uint8_t t0 = fmt[f];
        if ((t0 & 0x1F) == 0x1F) {
            if (f + 1 >= fmt_len) break;
            tag = ((uint16_t)t0 << 8) | fmt[f + 1];
            tag_bytes = 2;
        } else {
            tag = t0;
            tag_bytes = 1;
        }
        if (f + tag_bytes >= fmt_len) break;
        uint8_t tlen = fmt[f + tag_bytes];
        f += tag_bytes + 1;
        if (i + tlen > len) break;

        switch (tag) {
        case EMV_TAG_ATC:               /* 0x9F36 */
            if (tlen >= 2) t->atc = (uint16_t)((buf[i] << 8) | buf[i + 1]);
            break;
        case EMV_TAG_LOG_AMOUNT: {      /* 0x9F02, BCD */
            uint64_t v = 0;
            for (size_t k = 0; k < tlen && k < 6; k++) {
                uint8_t b = buf[i + k];
                v = v * 100 + (uint64_t)((b >> 4) * 10 + (b & 0x0F));
            }
            t->amount = v;
            break;
        }
        case EMV_TAG_LOG_COUNTRY:        /* 0x9F1A */
            if (tlen >= 2) t->country = (uint16_t)((buf[i] << 8) | buf[i + 1]);
            break;
        case EMV_TAG_LOG_CURRENCY:       /* 0x5F2A */
            if (tlen >= 2) t->currency = (uint16_t)((buf[i] << 8) | buf[i + 1]);
            break;
        case EMV_TAG_LOG_DATE:          /* 0x9A, BCD YYMMDD */
            for (size_t k = 0; k < tlen && k < 3; k++) t->date[k] = buf[i + k];
            break;
        case EMV_TAG_LOG_TIME:          /* 0x9F21, BCD HHMMSS */
            for (size_t k = 0; k < tlen && k < 3; k++) t->time[k] = buf[i + k];
            break;
        default:
            break;
        }
        i += tlen;
    }
}

// READ RECORD for (sfi, record) and decode into app. Returns true on success.
static bool emv_read_record(pn532_io_handle_t io, uint8_t sfi, uint8_t record,
                            EmvApplication *app) {
    uint8_t p2 = (uint8_t)((sfi << 3) | 0x04);
    uint8_t resp[255];
    uint8_t rlen = 0;
    if (!emv_apdu(io, 0x00, 0xB2, record, p2, NULL, 0, resp, &rlen))
        return false;
    emv_decode_record(resp, rlen, app);
    ESP_LOGI(TAG, "READ RECORD SFI=%u rec=%u: %u bytes -> PAN=%u name=%u",
             sfi, record, (unsigned)rlen, app->pan_len,
             (unsigned)strlen(app->cardholder_name));
    return true;
}

// ---- EMV card reader ----

bool emv_read_card(pn532_io_handle_t io, EmvData *out) {
    if (!io || !out) return false;
    memset(out, 0, sizeof(*out));
    out->emv_application.pin_try_counter = 0xFF; // "unknown" sentinel (Momentum)
    s_transport_fails = 0;

    EmvApplication *app = &out->emv_application;
    emv_tlv_t tlv;
    uint8_t resp[255];
    uint8_t rlen = 0;

    // 1) SELECT PPSE: "2PAY.SYS.DDF01"
    static const uint8_t ppse[] = {
        '2', 'P', 'A', 'Y', '.', 'S', 'Y', 'S', '.', 'D', 'D', 'F', '0', '1'
    };
    if (!emv_apdu(io, 0x00, 0xA4, 0x04, 0x00, ppse, sizeof(ppse), resp, &rlen)) {
        ESP_LOGI(TAG, "SELECT PPSE failed");
        return false;
    }
    ESP_LOGD(TAG, "PPSE response: %u bytes", rlen);

    // AID (0x4F) and application priority (0x87) from PPSE FCI
    uint8_t first_aid[EMV_AID_LEN];
    uint8_t first_aid_len = 0;
    if (emv_tlv_find(EMV_TAG_AID, resp, rlen, &tlv) && tlv.length > 0) {
        first_aid_len = tlv.length > EMV_AID_LEN ? EMV_AID_LEN : tlv.length;
        memcpy(first_aid, tlv.value, first_aid_len);
    }
    if (emv_tlv_find(EMV_TAG_PRIORITY, resp, rlen, &tlv) && tlv.length >= 1)
        app->priority = tlv.value[0];
    if (first_aid_len == 0) {
        ESP_LOGI(TAG, "No AID found in PPSE response");
        return false;
    }
    ESP_LOGI(TAG, "PPSE ok: AID len=%u priority=%02X", first_aid_len, app->priority);

    // 2) SELECT the AID -> label, application name, AIP, PDOL
    if (!emv_apdu(io, 0x00, 0xA4, 0x04, 0x00, first_aid, first_aid_len, resp, &rlen)) {
        ESP_LOGI(TAG, "SELECT AID failed");
        return false;
    }
    memcpy(app->aid, first_aid, first_aid_len);
    app->aid_len = first_aid_len;

    if (emv_tlv_find(EMV_TAG_APPL_LABEL, resp, rlen, &tlv) && tlv.length > 0) {
        size_t copy = tlv.length < EMV_APP_LABEL_LEN ? tlv.length : EMV_APP_LABEL_LEN - 1;
        memcpy(app->application_label, tlv.value, copy);
        app->application_label[copy] = '\0';
    }
    if (emv_tlv_find(EMV_TAG_APPL_NAME, resp, rlen, &tlv) && tlv.length > 0) {
        size_t copy = tlv.length < EMV_APP_NAME_LEN ? tlv.length : EMV_APP_NAME_LEN - 1;
        memcpy(app->application_name, tlv.value, copy);
        app->application_name[copy] = '\0';
    }
    if (emv_tlv_find(EMV_TAG_AIP, resp, rlen, &tlv) && tlv.length >= 2)
        memcpy(app->application_interchange_profile, tlv.value, 2);

    // PDOL (0x9F38) requested by the card
    uint8_t pdol[EMV_MAX_APDU_LEN];
    size_t pdol_len = 0;
    if (emv_tlv_find(EMV_TAG_PDOL, resp, rlen, &tlv) && tlv.length > 0) {
        size_t copy = tlv.length > sizeof(pdol) ? sizeof(pdol) : tlv.length;
        memcpy(pdol, tlv.value, copy);
        pdol_len = copy;
    }
    ESP_LOGI(TAG, "SELECT AID ok: label='%s' name='%s' PDOL=%u bytes",
             app->application_label, app->application_name, (unsigned)pdol_len);

    // 3) GET PROCESSING OPTIONS with a populated PDOL
    uint8_t pdol_term[EMV_MAX_APDU_LEN];
    size_t pdol_term_len = 0;
    emv_prepare_pdol(pdol_term, &pdol_term_len, sizeof(pdol_term), pdol, pdol_len);

    uint8_t gpo_payload[EMV_MAX_APDU_LEN];
    if (2u + pdol_term_len > sizeof(gpo_payload)) pdol_term_len = sizeof(gpo_payload) - 2;
    gpo_payload[0] = 0x83;
    gpo_payload[1] = (uint8_t)pdol_term_len;
    if (pdol_term_len) memcpy(gpo_payload + 2, pdol_term, pdol_term_len);

    bool gpo_ok = false;
    const uint8_t *afl = NULL;
    size_t afl_len = 0;

    if (emv_apdu(io, 0x80, 0xA8, 0x00, 0x00, gpo_payload, (uint8_t)(2 + pdol_term_len), resp, &rlen)) {
        ESP_LOGD(TAG, "GPO response: %u bytes", rlen);
        // Format 1: tag 0x80 <len> <AIP 2 bytes> <AFL...>
        if (emv_tlv_find(EMV_TAG_GPO_FMT1, resp, rlen, &tlv) && tlv.length >= 2) {
            memcpy(app->application_interchange_profile, tlv.value, 2);
            afl = tlv.value + 2;
            afl_len = tlv.length - 2;
            gpo_ok = true;
        }
        // Format 2: tag 0x77 constructed (AIP 0x82 + AFL 0x94)
        if (!gpo_ok && emv_tlv_find(EMV_TAG_GPO_FMT2, resp, rlen, &tlv)) {
            emv_tlv_t sub;
            if (emv_tlv_find(EMV_TAG_AIP, tlv.value, tlv.length, &sub) && sub.length >= 2)
                memcpy(app->application_interchange_profile, sub.value, 2);
            if (emv_tlv_find(EMV_TAG_AFL, tlv.value, tlv.length, &sub)) {
                afl = sub.value;
                afl_len = sub.length;
                gpo_ok = true;
            }
        }
    }
    if (gpo_ok)
        ESP_LOGI(TAG, "GPO ok: AFL=%u bytes", (unsigned)afl_len);
    else
        ESP_LOGI(TAG, "GPO failed or no AFL; trying common SFI/records");

    // 4) READ RECORD across the AFL (or brute-force common locations)
    uint32_t read_mask = 0; // bit (sfi-1)*8 + (rec-1) set once read
    if (gpo_ok && afl && afl_len >= 4) {
        for (size_t i = 0; i + 3 < afl_len && !emv_card_gone(); i += 4) {
            uint8_t sfi = (afl[i] >> 3) & 0x1F;
            uint8_t first_rec = afl[i + 1];
            uint8_t last_rec = afl[i + 2];
            for (uint8_t rec = first_rec; rec <= last_rec && rec > 0; rec++) {
                if (sfi >= 1 && sfi <= 4 && rec >= 1 && rec <= 8)
                    read_mask |= (1UL << ((sfi - 1) * 8 + (rec - 1)));
                (void)emv_read_record(io, sfi, rec, app); // SW errors here are non-fatal
                if (emv_card_gone()) break;
                if (rec == last_rec) break;
            }
        }
    } else {
        for (uint8_t sfi = 1; sfi <= 4 && !emv_card_gone(); sfi++) {
            for (uint8_t rec = 1; rec <= 3; rec++) {
                read_mask |= (1UL << ((sfi - 1) * 8 + (rec - 1)));
                (void)emv_read_record(io, sfi, rec, app);
                if (emv_card_gone()) break;
            }
        }
    }

    // Second pass: hunt cardholder name over unread SFI 1-4 records 1-5.
    // SW=6A82 (no such record) is normal and we keep scanning; a run of
    // transport errors (emv_card_gone()) bails out of this optional hunt.
    if (!emv_card_gone() && app->cardholder_name[0] == '\0') {
        for (uint8_t sfi = 1; sfi <= 4 && !emv_card_gone(); sfi++) {
            for (uint8_t rec = 1; rec <= 5; rec++) {
                uint32_t bit = (1UL << ((sfi - 1) * 8 + (rec - 1)));
                if (read_mask & bit) continue;
                if (!emv_read_record(io, sfi, rec, app)) {
                    if (emv_card_gone()) break; // link dead → stop hunting
                    continue;                    // 6A82 → try next record
                }
                read_mask |= bit;
                if (app->cardholder_name[0] != '\0') break;
            }
            if (app->cardholder_name[0] != '\0') break;
        }
    }

    // 5) GET DATA: PIN try counter (0x9F17) and last online ATC (0x9F13)
    if (!emv_card_gone()) {
        if (emv_get_data(io, EMV_TAG_PIN_TRY_COUNTER, resp, &rlen)) {
            emv_tlv_t ptc;
            if (emv_tlv_find(EMV_TAG_PIN_TRY_COUNTER, resp, rlen, &ptc) && ptc.length >= 1)
                app->pin_try_counter = ptc.value[0];
        }
        if (!emv_card_gone() && emv_get_data(io, EMV_TAG_LAST_ONLINE_ATC, resp, &rlen)) {
            emv_tlv_t atc;
            if (emv_tlv_find(EMV_TAG_LAST_ONLINE_ATC, resp, rlen, &atc) && atc.length >= 2)
                app->last_online_atc = (atc.value[0] << 8) | atc.value[1];
        }
    }

    // 6) Transaction log: fetch the log format (0x9F4F) via GET DATA, then read
    //    the log records positionally. The log SFI/record count come from the
    //    0x9F4D tag found during the READ RECORD pass above.
    if (!emv_card_gone() && app->log_sfi && app->log_records) {
        uint8_t logfmt[64];
        size_t logfmt_len = 0;
        if (emv_get_data(io, EMV_TAG_LOG_FMT, resp, &rlen)) {
            emv_tlv_t lf;
            if (emv_tlv_find(EMV_TAG_LOG_FMT, resp, rlen, &lf) && lf.length > 0) {
                size_t copy = lf.length > sizeof(logfmt) ? sizeof(logfmt) : lf.length;
                memcpy(logfmt, lf.value, copy);
                logfmt_len = copy;
            }
        }
        if (logfmt_len > 0) {
            for (uint8_t rec = 1; rec <= app->log_records; rec++) {
                if (app->trans_count >= EMV_TRANS_MAX) break;
                uint8_t p2 = (uint8_t)((app->log_sfi << 3) | 0x04);
                uint8_t lresp[255];
                uint8_t lrlen = 0;
                if (!emv_apdu(io, 0x00, 0xB2, rec, p2, NULL, 0, lresp, &lrlen)) break;
                emv_decode_log_entry(lresp, lrlen, logfmt, logfmt_len, app);
                app->trans_count++;
            }
        }
        ESP_LOGI(TAG, "Transaction log: %u entries (sfi=%u recs=%u)",
                 app->trans_count, app->log_sfi, app->log_records);
    }

    ESP_LOGI(TAG, "EMV read: PAN_len=%u name='%s' exp=%02x/%02x country=%04X currency=%04X atc=%u",
             app->pan_len, app->cardholder_name, app->exp_year, app->exp_month,
             app->country_code, app->currency_code, app->last_online_atc);

    return app->pan_len > 0 || app->aid_len > 0;
}

// ---- Flipper-format persistence (.nfc / .shd style) ------------------------

char *emv_build_flipper_text(const EmvData *data,
                             const uint8_t *uid, uint8_t uid_len,
                             uint16_t atqa, uint8_t sak) {
    if (!data) return NULL;
    const EmvApplication *app = &data->emv_application;

    int cap = 2048;
    char *buf = (char *)malloc((size_t)cap);
    if (!buf) return NULL;
    int pos = 0;
#define EMV_APPEND(...) do { \
        int _n = snprintf(buf + pos, (size_t)(cap - pos), __VA_ARGS__); \
        if (_n < 0 || _n >= cap - pos) { free(buf); return NULL; } \
        pos += _n; \
    } while (0)

    EMV_APPEND("Filetype: Flipper NFC device\n");
    EMV_APPEND("Version: 3\n");
    EMV_APPEND("# EMV information\n");
    EMV_APPEND("Device type: EMV\n");
    EMV_APPEND("UID:");
    for (uint8_t i = 0; i < uid_len; i++) EMV_APPEND(" %02X", uid[i]);
    EMV_APPEND("\n");
    EMV_APPEND("ATQA: %02X %02X\n", (atqa >> 8) & 0xFF, atqa & 0xFF);
    EMV_APPEND("SAK: %02X\n", sak);
    EMV_APPEND("# EMV specific data:\n");
    EMV_APPEND("Cardholder name: %s\n", app->cardholder_name);
    EMV_APPEND("Application name: %s\n", app->application_name);
    EMV_APPEND("Application label: %s\n", app->application_label);
    EMV_APPEND("PAN length: %u\n", app->pan_len);
    EMV_APPEND("PAN:");
    for (uint8_t i = 0; i < app->pan_len; i++) EMV_APPEND(" %02X", app->pan[i]);
    EMV_APPEND("\n");
    EMV_APPEND("AID length: %u\n", app->aid_len);
    EMV_APPEND("AID:");
    for (uint8_t i = 0; i < app->aid_len; i++) EMV_APPEND(" %02X", app->aid[i]);
    EMV_APPEND("\n");
    EMV_APPEND("Application interchange profile: %02X %02X\n",
               app->application_interchange_profile[0],
               app->application_interchange_profile[1]);
    EMV_APPEND("Country code: %02X %02X\n",
               (app->country_code >> 8) & 0xFF, app->country_code & 0xFF);
    EMV_APPEND("Currency code: %02X %02X\n",
               (app->currency_code >> 8) & 0xFF, app->currency_code & 0xFF);
    EMV_APPEND("Expiration year: %02X\n", app->exp_year);
    EMV_APPEND("Expiration month: %02X\n", app->exp_month);
    EMV_APPEND("Expiration day: %02X\n", app->exp_day);
    EMV_APPEND("Effective year: %02X\n", app->effective_year);
    EMV_APPEND("Effective month: %02X\n", app->effective_month);
    EMV_APPEND("Effective day: %02X\n", app->effective_day);
    EMV_APPEND("PIN try counter: %u\n", app->pin_try_counter);

    /* GhostESP extensions beyond the upstream EMV keys: ATC + transaction log. */
    if (app->last_online_atc || app->transaction_counter) {
        EMV_APPEND("# Transaction counters:\n");
        EMV_APPEND("Last online ATC: %u\n", app->last_online_atc);
        EMV_APPEND("Application transaction counter: %u\n", app->transaction_counter);
    }
    if (app->trans_count > 0) {
        EMV_APPEND("Transaction count: %u\n", app->trans_count);
        for (uint8_t i = 0; i < app->trans_count; i++) {
            const EmvTransaction *t = &app->trans[i];
            EMV_APPEND("Transaction %u: ATC=%u amount=%llu country=%04X currency=%04X "
                       "date=%02X%02X%02X time=%02X%02X%02X\n",
                       i + 1, t->atc, (unsigned long long)t->amount,
                       t->country, t->currency,
                       t->date[0], t->date[1], t->date[2],
                       t->time[0], t->time[1], t->time[2]);
        }
    }
#undef EMV_APPEND
    return buf;
}

bool emv_save_flipper_file(const EmvData *data, const char *dir,
                           const uint8_t *uid, uint8_t uid_len,
                           uint16_t atqa, uint8_t sak,
                           char *out_path, size_t out_path_len) {
    if (!data || !dir || !uid || uid_len == 0) return false;

    char *text = emv_build_flipper_text(data, uid, uid_len, atqa, sak);
    if (!text) return false;

    sd_card_create_directory(dir);

    char uid_part[40] = {0};
    int up = 0;
    for (uint8_t i = 0; i < uid_len && up < (int)sizeof(uid_part) - 3; i++) {
        up += snprintf(uid_part + up, sizeof(uid_part) - up, "%02X", uid[i]);
        if (i + 1 < uid_len) up += snprintf(uid_part + up, sizeof(uid_part) - up, "-");
    }
    char path[192];
    snprintf(path, sizeof(path), "%s/EMV_%s.nfc", dir, uid_part);
    if (out_path && out_path_len) snprintf(out_path, out_path_len, "%s", path);

    size_t len = strlen(text);
    esp_err_t err = sd_card_write_file(path, text, len);
    free(text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "EMV save failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "EMV file saved (%u bytes): %s", (unsigned)len, path);
    return true;
}

/* ---- Saved-file loader (inverse of emv_build_flipper_text) ---------------- */

/* Return the tail of `line` past `prefix`, or NULL when it doesn't match. */
static const char *emv_after_prefix(const char *line, const char *prefix) {
    size_t n = strlen(prefix);
    return strncmp(line, prefix, n) == 0 ? line + n : NULL;
}

/* Parse a run of space-separated hex byte tokens into out[]; returns the count. */
static uint8_t emv_read_hex_bytes(const char *s, uint8_t *out, uint8_t max) {
    uint8_t n = 0;
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

static void emv_copy_str_field(char *dst, size_t dst_sz, const char *v) {
    while (*v == ' ') v++;
    strncpy(dst, v, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
}

bool emv_load_flipper_file(const char *path, EmvData *out,
                           uint8_t *uid, uint8_t *uid_len,
                           uint16_t *atqa, uint8_t *sak) {
    if (!path || !out) return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;

    memset(out, 0, sizeof(*out));
    if (uid_len) *uid_len = 0;
    if (atqa) *atqa = 0;
    if (sak) *sak = 0;
    EmvApplication *app = &out->emv_application;
    bool is_emv = false;
    char line[256];

    while (fgets(line, sizeof(line), f)) {
        size_t ll = strlen(line);
        while (ll && (line[ll - 1] == '\n' || line[ll - 1] == '\r')) line[--ll] = '\0';

        const char *v;
        uint8_t tmp[2];
        if ((v = emv_after_prefix(line, "Device type:"))) {
            if (strstr(v, "EMV")) is_emv = true;
        } else if ((v = emv_after_prefix(line, "UID:"))) {
            uint8_t u[10];
            uint8_t n = emv_read_hex_bytes(v, u, sizeof(u));
            if (uid && uid_len) { memcpy(uid, u, n); *uid_len = n; }
        } else if ((v = emv_after_prefix(line, "ATQA:"))) {
            if (emv_read_hex_bytes(v, tmp, 2) == 2 && atqa)
                *atqa = (uint16_t)((tmp[0] << 8) | tmp[1]);
        } else if ((v = emv_after_prefix(line, "SAK:"))) {
            uint8_t s1;
            if (emv_read_hex_bytes(v, &s1, 1) == 1 && sak) *sak = s1;
        } else if ((v = emv_after_prefix(line, "Cardholder name:"))) {
            emv_copy_str_field(app->cardholder_name, sizeof(app->cardholder_name), v);
        } else if ((v = emv_after_prefix(line, "Application label:"))) {
            emv_copy_str_field(app->application_label, sizeof(app->application_label), v);
        } else if ((v = emv_after_prefix(line, "Application name:"))) {
            emv_copy_str_field(app->application_name, sizeof(app->application_name), v);
        } else if ((v = emv_after_prefix(line, "Application interchange profile:"))) {
            if (emv_read_hex_bytes(v, tmp, 2) == 2) {
                app->application_interchange_profile[0] = tmp[0];
                app->application_interchange_profile[1] = tmp[1];
            }
        } else if ((v = emv_after_prefix(line, "Application transaction counter:"))) {
            app->transaction_counter = (uint16_t)strtoul(v, NULL, 10);
        } else if ((v = emv_after_prefix(line, "PAN length:"))) {
            /* Authoritative length comes from the PAN hex line below; ignore. */
        } else if ((v = emv_after_prefix(line, "PAN:"))) {
            app->pan_len = emv_read_hex_bytes(v, app->pan, EMV_PAN_LEN);
        } else if ((v = emv_after_prefix(line, "AID length:"))) {
            /* aid_len set from the AID hex line below. */
        } else if ((v = emv_after_prefix(line, "AID:"))) {
            app->aid_len = emv_read_hex_bytes(v, app->aid, EMV_AID_LEN);
        } else if ((v = emv_after_prefix(line, "Country code:"))) {
            if (emv_read_hex_bytes(v, tmp, 2) == 2)
                app->country_code = (uint16_t)((tmp[0] << 8) | tmp[1]);
        } else if ((v = emv_after_prefix(line, "Currency code:"))) {
            if (emv_read_hex_bytes(v, tmp, 2) == 2)
                app->currency_code = (uint16_t)((tmp[0] << 8) | tmp[1]);
        } else if ((v = emv_after_prefix(line, "Expiration year:"))) {
            emv_read_hex_bytes(v, &app->exp_year, 1);
        } else if ((v = emv_after_prefix(line, "Expiration month:"))) {
            emv_read_hex_bytes(v, &app->exp_month, 1);
        } else if ((v = emv_after_prefix(line, "Expiration day:"))) {
            emv_read_hex_bytes(v, &app->exp_day, 1);
        } else if ((v = emv_after_prefix(line, "Effective year:"))) {
            emv_read_hex_bytes(v, &app->effective_year, 1);
        } else if ((v = emv_after_prefix(line, "Effective month:"))) {
            emv_read_hex_bytes(v, &app->effective_month, 1);
        } else if ((v = emv_after_prefix(line, "Effective day:"))) {
            emv_read_hex_bytes(v, &app->effective_day, 1);
        } else if ((v = emv_after_prefix(line, "PIN try counter:"))) {
            app->pin_try_counter = (uint8_t)strtoul(v, NULL, 10);
        } else if ((v = emv_after_prefix(line, "Last online ATC:"))) {
            app->last_online_atc = (uint16_t)strtoul(v, NULL, 10);
        } else if ((v = emv_after_prefix(line, "Transaction count:"))) {
            unsigned n = (unsigned)strtoul(v, NULL, 10);
            app->trans_count = (uint8_t)(n > EMV_TRANS_MAX ? EMV_TRANS_MAX : n);
        } else if (strncmp(line, "Transaction ", 12) == 0) {
            unsigned idx = 0, atc = 0, country = 0, currency = 0;
            unsigned long long amount = 0;
            unsigned d0 = 0, d1 = 0, d2 = 0, t0 = 0, t1 = 0, t2 = 0;
            int got = sscanf(line,
                             "Transaction %u: ATC=%u amount=%llu country=%4x currency=%4x "
                             "date=%2x%2x%2x time=%2x%2x%2x",
                             &idx, &atc, &amount, &country, &currency,
                             &d0, &d1, &d2, &t0, &t1, &t2);
            if (got >= 2 && idx >= 1 && idx <= EMV_TRANS_MAX) {
                EmvTransaction *t = &app->trans[idx - 1];
                t->atc = (uint16_t)atc;
                t->amount = amount;
                t->country = (uint16_t)country;
                t->currency = (uint16_t)currency;
                t->date[0] = (uint8_t)d0; t->date[1] = (uint8_t)d1; t->date[2] = (uint8_t)d2;
                t->time[0] = (uint8_t)t0; t->time[1] = (uint8_t)t1; t->time[2] = (uint8_t)t2;
                if (idx > app->trans_count) app->trans_count = (uint8_t)idx;
            }
        }
    }
    fclose(f);
    return is_emv;
}
