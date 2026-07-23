/* opal.c - Parser for Opal card (Sydney, Australia).
 *
 * Originally authored for the Flipper Zero / Momentum-Firmware project by
 * Michael Farrell <micolous+git@gmail.com>.
 * Source: https://github.com/Next-Flip/Momentum-Firmware/blob/dev/applications/main/nfc/plugins/supported_cards/opal.c
 * Card format reference: https://github.com/metrodroid/metrodroid/wiki/Opal
 *
 * Adapted for GhostESP: includes changed to flipper_nfc_compat.h, FURI_PACKED
 * replaced with __attribute__((packed)). Parser logic unchanged.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */
#include "managers/nfc/flipper_nfc_compat.h"

#define FURI_PACKED __attribute__((packed))

static const MfDesfireApplicationId opal_app_id = {.data = {0x31, 0x45, 0x53}};

static const MfDesfireFileId opal_file_id = 0x07;

static const char* opal_modes[5] =
    {"Rail / Metro", "Ferry / Light Rail", "Bus", "Unknown mode", "Manly Ferry"};

static const char* opal_usages[14] = {
    "New / Unused",
    "Tap on: new journey",
    "Tap on: transfer from same mode",
    "Tap on: transfer from other mode",
    NULL, // Manly Ferry: new journey
    NULL, // Manly Ferry: transfer from ferry
    NULL, // Manly Ferry: transfer from other
    "Tap off: distance fare",
    "Tap off: flat fare",
    "Automated tap off: failed to tap off",
    "Tap off: end of trip without start",
    "Tap off: reversal",
    "Tap on: rejected",
    "Unknown usage",
};

// Opal file 0x7 structure. Assumes a little-endian CPU.
typedef struct FURI_PACKED {
    uint32_t serial         : 32;
    uint8_t check_digit     : 4;
    bool blocked            : 1;
    uint16_t txn_number     : 16;
    int32_t balance         : 21;
    uint16_t days           : 15;
    uint16_t minutes        : 11;
    uint8_t mode            : 3;
    uint16_t usage          : 4;
    bool auto_topup         : 1;
    uint8_t weekly_journeys : 4;
    uint16_t checksum       : 16;
} OpalFile;

_Static_assert(sizeof(OpalFile) == 16, "OpalFile must be 16 bytes");

// Converts an Opal timestamp to DateTime.
//
// Opal measures days since 1980-01-01 and minutes since midnight, and presumes
// all days are 1440 minutes.
static void opal_days_minutes_to_datetime(uint16_t days, uint16_t minutes, DateTime* out) {
    out->year = 1980;
    out->month = 1;
    // 1980-01-01 is a Tuesday
    out->weekday = (uint8_t)(((days + 1) % 7) + 1);
    out->hour = (uint8_t)(minutes / 60);
    out->minute = (uint8_t)(minutes % 60);
    out->second = 0;

    // What year is it?
    for(;;) {
        const uint16_t num_days_in_year = datetime_get_days_per_year(out->year);
        if(days < num_days_in_year) break;
        days -= num_days_in_year;
        out->year++;
    }

    // 1-index the day of the year
    days++;

    for(;;) {
        // What month is it?
        const bool is_leap = datetime_is_leap_year(out->year);
        const uint8_t num_days_in_month = datetime_get_days_per_month(is_leap, out->month);
        if(days <= num_days_in_month) break;
        days -= num_days_in_month;
        out->month++;
    }

    out->day = (uint8_t)days;
}

static bool opal_parse(const NfcDevice* device, FuriString* parsed_data) {
    furi_assert(device);
    furi_assert(parsed_data);

    const MfDesfireData* data = nfc_device_get_data(device, NfcProtocolMfDesfire);

    bool parsed = false;

    do {
        const MfDesfireApplication* app = mf_desfire_get_application(data, &opal_app_id);
        if(app == NULL) break;

        const MfDesfireFileSettings* file_settings =
            mf_desfire_get_file_settings(app, &opal_file_id);
        if(file_settings == NULL || file_settings->type != MfDesfireFileTypeStandard ||
           file_settings->data.size != sizeof(OpalFile))
            break;

        const MfDesfireFileData* file_data = mf_desfire_get_file_data(app, &opal_file_id);
        if(file_data == NULL) break;

        const OpalFile* opal_file = simple_array_cget_data(file_data->data);

        const uint8_t serial2 = (uint8_t)(opal_file->serial / 10000000);
        const uint16_t serial3 = (uint16_t)((opal_file->serial / 1000) % 10000);
        const uint16_t serial4 = (uint16_t)(opal_file->serial % 1000);

        if(opal_file->check_digit > 9) break;

        // Negative balance. Make this a positive value again and record the
        // sign separately, because then we can handle balances of -99..-1
        // cents, as the "dollars" division below would result in a positive
        // zero value.
        const bool is_negative_balance = (opal_file->balance < 0);
        const char* sign = is_negative_balance ? "-" : "";
        const int32_t balance = is_negative_balance ? labs(opal_file->balance) :
                                                      opal_file->balance;
        const uint8_t balance_cents = (uint8_t)(balance % 100);
        const int32_t balance_dollars = balance / 100;

        DateTime timestamp;
        opal_days_minutes_to_datetime(opal_file->days, opal_file->minutes, &timestamp);

        // Usages 4..6 associated with the Manly Ferry, which correspond to
        // usages 1..3 for other modes.
        const bool is_manly_ferry = (opal_file->usage >= 4) && (opal_file->usage <= 6);

        // 3..7 are "reserved", but we use 4 to indicate the Manly Ferry.
        const uint8_t mode = is_manly_ferry ? 4 : opal_file->mode;
        const uint8_t usage = is_manly_ferry ? (uint8_t)(opal_file->usage - 3) : opal_file->usage;

        const char* mode_str = opal_modes[mode > 4 ? 3 : mode];
        const char* usage_str = opal_usages[usage > 12 ? 13 : usage];

        furi_string_printf(
            parsed_data,
            "\e#Opal: $%s%ld.%02u\nNo.: 3085 22%02u %04u %03u%01u\n%s, %s\n",
            sign,
            balance_dollars,
            balance_cents,
            serial2,
            serial3,
            serial4,
            opal_file->check_digit,
            mode_str,
            usage_str);

        FuriString* timestamp_str = furi_string_alloc();

        locale_format_date(timestamp_str, &timestamp, locale_get_date_format(), "-");
        furi_string_cat(parsed_data, timestamp_str);
        furi_string_cat_str(parsed_data, " at ");

        locale_format_time(timestamp_str, &timestamp, locale_get_time_format(), false);
        furi_string_cat(parsed_data, timestamp_str);

        furi_string_free(timestamp_str);

        furi_string_cat_printf(
            parsed_data,
            "\nWeekly journeys: %u, Txn #%u\n",
            opal_file->weekly_journeys,
            opal_file->txn_number);

        if(opal_file->auto_topup) {
            furi_string_cat_str(parsed_data, "Auto-topup enabled\n");
        }

        if(opal_file->blocked) {
            furi_string_cat_str(parsed_data, "Card blocked\n");
        }

        parsed = true;
    } while(false);

    return parsed;
}

const NfcSupportedCardsPlugin opal_plugin = {
    .protocol = NfcProtocolMfDesfire,
    .verify = NULL,
    .read = NULL,
    .parse = opal_parse,
};

static const FlipperAppPluginDescriptor opal_plugin_descriptor = {
    .appid = NFC_SUPPORTED_CARD_PLUGIN_APP_ID,
    .ep_api_version = NFC_SUPPORTED_CARD_PLUGIN_API_VERSION,
    .entry_point = &opal_plugin,
};

const FlipperAppPluginDescriptor* opal_plugin_ep(void) {
    return &opal_plugin_descriptor;
}
