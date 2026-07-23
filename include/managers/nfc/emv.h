// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
//
// EMV payment card types. Ported from Flipper Zero / Momentum-Firmware
// lib/nfc/protocols/emv/emv.h (GPL-3.0).
// https://github.com/Next-Flip/Momentum-Firmware

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "managers/nfc/pn532_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EMV_PAN_LEN         10
#define EMV_CARDHOLDER_NAME_LEN 64
#define EMV_AID_LEN         16
#define EMV_APP_NAME_LEN    16
#define EMV_APP_LABEL_LEN   16
#define EMV_TRANS_MAX       16

/* One entry of the on-card transaction log (EMV Book 3, GET DATA/log format). */
typedef struct {
    uint16_t atc;            /* 0x9F36 Application Transaction Counter */
    uint64_t amount;         /* 0x9F02 Amount authorised (numeric, minor units) */
    uint16_t country;        /* 0x9F1A */
    uint16_t currency;       /* 0x5F2A */
    uint8_t  date[3];        /* 0x9A, BCD YYMMDD */
    uint8_t  time[3];        /* 0x9F21, BCD HHMMSS */
} EmvTransaction;

typedef struct {
    uint8_t aid[EMV_AID_LEN];
    uint8_t aid_len;

    char application_label[EMV_APP_LABEL_LEN];
    char application_name[EMV_APP_NAME_LEN];

    uint8_t pan[EMV_PAN_LEN];
    uint8_t pan_len;

    char cardholder_name[EMV_CARDHOLDER_NAME_LEN];

    uint8_t exp_year;
    uint8_t exp_month;
    uint8_t exp_day;
    uint8_t effective_year;
    uint8_t effective_month;
    uint8_t effective_day;

    uint16_t country_code;
    uint16_t currency_code;
    uint8_t pin_try_counter;

    uint8_t application_interchange_profile[2];

    /* Closer parity with Momentum-Firmware's emv_poller. */
    uint8_t priority;              /* 0x87 Application Priority Indicator */
    uint16_t transaction_counter;  /* 0x9F36 Application Transaction Counter */
    uint16_t last_online_atc;      /* 0x9F13 Last Online ATC (via GET DATA) */

    /* Transaction log (0x9F4D entry + 0x9F4F format + positional READ RECORD). */
    uint8_t log_sfi;
    uint8_t log_records;
    EmvTransaction trans[EMV_TRANS_MAX];
    uint8_t trans_count;
} EmvApplication;

typedef struct {
    EmvApplication emv_application;
} EmvData;

#if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_ST25R3916)
// Read an EMV payment card: SELECT PPSE, SELECT AID, GPO, READ RECORD, GET DATA,
// and the transaction log. Returns true if any EMV data was extracted.
bool emv_read_card(pn532_io_handle_t io, EmvData *out);

// Build a malloc'd Flipper Zero ".nfc"/.shd-style text dump of the EMV card
// (NULL-terminated). Caller frees. Returns NULL on failure.
char *emv_build_flipper_text(const EmvData *data,
                             const uint8_t *uid, uint8_t uid_len,
                             uint16_t atqa, uint8_t sak);

// Save an EMV card as a Flipper-format file "<dir>/EMV_<UID>.nfc". If out_path
// is non-NULL the resulting path is copied there. Returns true on success.
bool emv_save_flipper_file(const EmvData *data, const char *dir,
                           const uint8_t *uid, uint8_t uid_len,
                           uint16_t atqa, uint8_t sak,
                           char *out_path, size_t out_path_len);
#endif

// Load an EMV card previously saved via emv_save_flipper_file back into EmvData
// (and, when non-NULL, uid/uid_len/atqa/sak). Inverse of emv_build_flipper_text;
// needs no reader hardware. Returns true when the file is a "Device type: EMV"
// Flipper .nfc and was parsed.
bool emv_load_flipper_file(const char *path, EmvData *out,
                           uint8_t *uid, uint8_t *uid_len,
                           uint16_t *atqa, uint8_t *sak);

#ifdef __cplusplus
}
#endif
