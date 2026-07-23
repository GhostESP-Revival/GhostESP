/* emv_card.c - EMV payment card display parser.
 *
 * Ported from Flipper Zero / Momentum-Firmware
 * applications/main/nfc/plugins/supported_cards/emv.c (GPL-3.0).
 * Copyright 2023 Leptoptilos <leptoptilos@icloud.com>
 * Source: https://github.com/Next-Flip/Momentum-Firmware
 *
 * Adapted for GhostESP: includes changed to flipper_nfc_compat.h,
 * Storage/lookup table dependencies removed (raw codes shown instead of
 * looked-up country/currency names). Parser logic unchanged.
 */

#include "managers/nfc/flipper_nfc_compat.h"
#include "managers/nfc/emv.h"

/* ---- ISO 3166-1 (country) / ISO 4217 (currency) numeric-code lookup --------
 * EMV tags 5F28 (issuer country) and 9F42 (currency) are packed-BCD numeric
 * codes stored here as the raw 2-byte value (e.g. Australia/AUD = 0x0036).
 * Momentum looks these up from SD dictionary files; GhostESP can't rely on the
 * SD card, so the tables are compiled in. Codes below are the decimal ISO
 * numeric values; emv_bcd_to_dec() converts the on-card BCD before lookup. */
typedef struct {
    uint16_t code;      /* decimal ISO numeric code */
    const char* name;
} emv_code_entry_t;

static uint16_t emv_bcd_to_dec(uint16_t bcd) {
    return (uint16_t)(((bcd >> 12) & 0xF) * 1000 + ((bcd >> 8) & 0xF) * 100 +
                      ((bcd >> 4) & 0xF) * 10 + (bcd & 0xF));
}

static const char* emv_lookup(const emv_code_entry_t* tbl, uint16_t bcd) {
    const uint16_t dec = emv_bcd_to_dec(bcd);
    for(size_t i = 0; tbl[i].name != NULL; i++) {
        if(tbl[i].code == dec) return tbl[i].name;
    }
    return NULL;
}

/* ISO 3166-1 numeric -> English short country name. */
static const emv_code_entry_t emv_country_codes[] = {
    {4, "Afghanistan"}, {8, "Albania"}, {10, "Antarctica"}, {12, "Algeria"},
    {16, "American Samoa"}, {20, "Andorra"}, {24, "Angola"},
    {28, "Antigua and Barbuda"}, {31, "Azerbaijan"}, {32, "Argentina"},
    {36, "Australia"}, {40, "Austria"}, {44, "Bahamas"}, {48, "Bahrain"},
    {50, "Bangladesh"}, {51, "Armenia"}, {52, "Barbados"}, {56, "Belgium"},
    {60, "Bermuda"}, {64, "Bhutan"}, {68, "Bolivia"},
    {70, "Bosnia and Herzegovina"}, {72, "Botswana"}, {74, "Bouvet Island"},
    {76, "Brazil"}, {84, "Belize"}, {86, "British Indian Ocean Territory"},
    {90, "Solomon Islands"}, {92, "British Virgin Islands"}, {96, "Brunei"},
    {100, "Bulgaria"}, {104, "Myanmar"}, {108, "Burundi"}, {112, "Belarus"},
    {116, "Cambodia"}, {120, "Cameroon"}, {124, "Canada"}, {132, "Cape Verde"},
    {136, "Cayman Islands"}, {140, "Central African Republic"},
    {144, "Sri Lanka"}, {148, "Chad"}, {152, "Chile"}, {156, "China"},
    {158, "Taiwan"}, {162, "Christmas Island"}, {166, "Cocos Islands"},
    {170, "Colombia"}, {174, "Comoros"}, {175, "Mayotte"},
    {178, "Republic of the Congo"}, {180, "DR Congo"}, {184, "Cook Islands"},
    {188, "Costa Rica"}, {191, "Croatia"}, {192, "Cuba"}, {196, "Cyprus"},
    {203, "Czechia"}, {204, "Benin"}, {208, "Denmark"}, {212, "Dominica"},
    {214, "Dominican Republic"}, {218, "Ecuador"}, {222, "El Salvador"},
    {226, "Equatorial Guinea"}, {231, "Ethiopia"}, {232, "Eritrea"},
    {233, "Estonia"}, {234, "Faroe Islands"}, {238, "Falkland Islands"},
    {242, "Fiji"}, {246, "Finland"}, {248, "Aland Islands"}, {250, "France"},
    {254, "French Guiana"}, {258, "French Polynesia"},
    {260, "French Southern Territories"}, {262, "Djibouti"}, {266, "Gabon"},
    {268, "Georgia"}, {270, "Gambia"}, {275, "Palestine"}, {276, "Germany"},
    {288, "Ghana"}, {292, "Gibraltar"}, {296, "Kiribati"}, {300, "Greece"},
    {304, "Greenland"}, {308, "Grenada"}, {312, "Guadeloupe"}, {316, "Guam"},
    {320, "Guatemala"}, {324, "Guinea"}, {328, "Guyana"}, {332, "Haiti"},
    {334, "Heard and McDonald Islands"}, {336, "Vatican City"},
    {340, "Honduras"}, {344, "Hong Kong"}, {348, "Hungary"}, {352, "Iceland"},
    {356, "India"}, {360, "Indonesia"}, {364, "Iran"}, {368, "Iraq"},
    {372, "Ireland"}, {376, "Israel"}, {380, "Italy"}, {384, "Ivory Coast"},
    {388, "Jamaica"}, {392, "Japan"}, {398, "Kazakhstan"}, {400, "Jordan"},
    {404, "Kenya"}, {408, "North Korea"}, {410, "South Korea"}, {414, "Kuwait"},
    {417, "Kyrgyzstan"}, {418, "Laos"}, {422, "Lebanon"}, {426, "Lesotho"},
    {428, "Latvia"}, {430, "Liberia"}, {434, "Libya"}, {438, "Liechtenstein"},
    {440, "Lithuania"}, {442, "Luxembourg"}, {446, "Macau"},
    {450, "Madagascar"}, {454, "Malawi"}, {458, "Malaysia"}, {462, "Maldives"},
    {466, "Mali"}, {470, "Malta"}, {474, "Martinique"}, {478, "Mauritania"},
    {480, "Mauritius"}, {484, "Mexico"}, {492, "Monaco"}, {496, "Mongolia"},
    {498, "Moldova"}, {499, "Montenegro"}, {500, "Montserrat"}, {504, "Morocco"},
    {508, "Mozambique"}, {512, "Oman"}, {516, "Namibia"}, {520, "Nauru"},
    {524, "Nepal"}, {528, "Netherlands"}, {531, "Curacao"}, {533, "Aruba"},
    {534, "Sint Maarten"}, {535, "Bonaire"}, {540, "New Caledonia"},
    {548, "Vanuatu"}, {554, "New Zealand"}, {558, "Nicaragua"}, {562, "Niger"},
    {566, "Nigeria"}, {570, "Niue"}, {574, "Norfolk Island"}, {578, "Norway"},
    {580, "Northern Mariana Islands"}, {581, "US Minor Outlying Islands"},
    {583, "Micronesia"}, {584, "Marshall Islands"}, {585, "Palau"},
    {586, "Pakistan"}, {591, "Panama"}, {598, "Papua New Guinea"},
    {600, "Paraguay"}, {604, "Peru"}, {608, "Philippines"},
    {612, "Pitcairn Islands"}, {616, "Poland"}, {620, "Portugal"},
    {624, "Guinea-Bissau"}, {626, "Timor-Leste"}, {630, "Puerto Rico"},
    {634, "Qatar"}, {638, "Reunion"}, {642, "Romania"}, {643, "Russia"},
    {646, "Rwanda"}, {652, "Saint Barthelemy"}, {654, "Saint Helena"},
    {659, "Saint Kitts and Nevis"}, {660, "Anguilla"}, {662, "Saint Lucia"},
    {663, "Saint Martin"}, {666, "Saint Pierre and Miquelon"},
    {670, "Saint Vincent and the Grenadines"}, {674, "San Marino"},
    {678, "Sao Tome and Principe"}, {682, "Saudi Arabia"}, {686, "Senegal"},
    {688, "Serbia"}, {690, "Seychelles"}, {694, "Sierra Leone"},
    {702, "Singapore"}, {703, "Slovakia"}, {704, "Vietnam"}, {705, "Slovenia"},
    {706, "Somalia"}, {710, "South Africa"}, {716, "Zimbabwe"}, {724, "Spain"},
    {728, "South Sudan"}, {729, "Sudan"}, {732, "Western Sahara"},
    {740, "Suriname"}, {744, "Svalbard and Jan Mayen"}, {748, "Eswatini"},
    {752, "Sweden"}, {756, "Switzerland"}, {760, "Syria"}, {762, "Tajikistan"},
    {764, "Thailand"}, {768, "Togo"}, {772, "Tokelau"}, {776, "Tonga"},
    {780, "Trinidad and Tobago"}, {784, "United Arab Emirates"},
    {788, "Tunisia"}, {792, "Turkey"}, {795, "Turkmenistan"},
    {796, "Turks and Caicos Islands"}, {798, "Tuvalu"}, {800, "Uganda"},
    {804, "Ukraine"}, {807, "North Macedonia"}, {818, "Egypt"},
    {826, "United Kingdom"}, {831, "Guernsey"}, {832, "Jersey"},
    {833, "Isle of Man"}, {834, "Tanzania"}, {840, "United States"},
    {850, "US Virgin Islands"}, {854, "Burkina Faso"}, {858, "Uruguay"},
    {860, "Uzbekistan"}, {862, "Venezuela"}, {876, "Wallis and Futuna"},
    {882, "Samoa"}, {887, "Yemen"}, {894, "Zambia"}, {0, NULL},
};

/* ISO 4217 numeric -> alpha-3 currency code. */
static const emv_code_entry_t emv_currency_codes[] = {
    {8, "ALL"}, {12, "DZD"}, {32, "ARS"}, {36, "AUD"}, {44, "BSD"},
    {48, "BHD"}, {50, "BDT"}, {51, "AMD"}, {52, "BBD"}, {60, "BMD"},
    {64, "BTN"}, {68, "BOB"}, {72, "BWP"}, {84, "BZD"}, {90, "SBD"},
    {96, "BND"}, {104, "MMK"}, {108, "BIF"}, {116, "KHR"}, {124, "CAD"},
    {132, "CVE"}, {136, "KYD"}, {144, "LKR"}, {152, "CLP"}, {156, "CNY"},
    {170, "COP"}, {174, "KMF"}, {188, "CRC"}, {192, "CUP"}, {203, "CZK"},
    {208, "DKK"}, {214, "DOP"}, {222, "SVC"}, {230, "ETB"}, {232, "ERN"},
    {238, "FKP"}, {242, "FJD"}, {262, "DJF"}, {270, "GMD"}, {292, "GIP"},
    {320, "GTQ"}, {324, "GNF"}, {328, "GYD"}, {332, "HTG"}, {340, "HNL"},
    {344, "HKD"}, {348, "HUF"}, {352, "ISK"}, {356, "INR"}, {360, "IDR"},
    {364, "IRR"}, {368, "IQD"}, {376, "ILS"}, {388, "JMD"}, {392, "JPY"},
    {398, "KZT"}, {400, "JOD"}, {404, "KES"}, {408, "KPW"}, {410, "KRW"},
    {414, "KWD"}, {417, "KGS"}, {418, "LAK"}, {422, "LBP"}, {426, "LSL"},
    {430, "LRD"}, {434, "LYD"}, {446, "MOP"}, {454, "MWK"}, {458, "MYR"},
    {462, "MVR"}, {480, "MUR"}, {484, "MXN"}, {496, "MNT"}, {498, "MDL"},
    {504, "MAD"}, {512, "OMR"}, {516, "NAD"}, {524, "NPR"}, {532, "ANG"},
    {533, "AWG"}, {548, "VUV"}, {554, "NZD"}, {558, "NIO"}, {566, "NGN"},
    {578, "NOK"}, {586, "PKR"}, {590, "PAB"}, {598, "PGK"}, {600, "PYG"},
    {604, "PEN"}, {608, "PHP"}, {634, "QAR"}, {643, "RUB"}, {646, "RWF"},
    {654, "SHP"}, {682, "SAR"}, {690, "SCR"}, {694, "SLL"}, {702, "SGD"},
    {704, "VND"}, {706, "SOS"}, {710, "ZAR"}, {728, "SSP"}, {748, "SZL"},
    {752, "SEK"}, {756, "CHF"}, {760, "SYP"}, {764, "THB"}, {776, "TOP"},
    {780, "TTD"}, {784, "AED"}, {788, "TND"}, {800, "UGX"}, {807, "MKD"},
    {818, "EGP"}, {826, "GBP"}, {834, "TZS"}, {840, "USD"}, {858, "UYU"},
    {860, "UZS"}, {882, "WST"}, {886, "YER"}, {901, "TWD"}, {925, "SLE"},
    {928, "VES"}, {929, "MRU"}, {930, "STN"}, {932, "ZWL"}, {933, "BYN"},
    {934, "TMT"}, {936, "GHS"}, {938, "SDG"}, {941, "RSD"}, {943, "MZN"},
    {944, "AZN"}, {946, "RON"}, {949, "TRY"}, {950, "XAF"}, {951, "XCD"},
    {952, "XOF"}, {953, "XPF"}, {967, "ZMW"}, {968, "SRD"}, {969, "MGA"},
    {971, "AFN"}, {972, "TJS"}, {973, "AOA"}, {975, "BGN"}, {976, "CDF"},
    {977, "BAM"}, {978, "EUR"}, {980, "UAH"}, {981, "GEL"}, {985, "PLN"},
    {986, "BRL"}, {0, NULL},
};

static bool emv_parse(const NfcDevice* device, FuriString* parsed_data) {
    furi_assert(device);
    bool parsed = false;

    const EmvData* data = (const EmvData*)nfc_device_get_data(device, NfcProtocolEmv);
    const EmvApplication app = data->emv_application;

    do {
        if(strlen(app.application_label)) {
            furi_string_cat_printf(parsed_data, "\e#%s\n", app.application_label);
        } else if(app.aid_len) {
            furi_string_cat_str(parsed_data, "\e#EMV\n");
        } else {
            break;
        }

        if(app.aid_len) {
            furi_string_cat_str(parsed_data, "AID:");
            for(uint8_t i = 0; i < app.aid_len; i++)
                furi_string_cat_printf(parsed_data, " %02X", app.aid[i]);
            furi_string_cat_str(parsed_data, "\n");
            parsed = true;
        }

        if(app.pan_len) {
            for(uint8_t i = 0; i < app.pan_len; i += 2) {
                if(i + 1 < app.pan_len)
                    furi_string_cat_printf(parsed_data, "%02X%02X ", app.pan[i], app.pan[i + 1]);
                else
                    furi_string_cat_printf(parsed_data, "%02X", app.pan[i]);
            }
            furi_string_cat_str(parsed_data, "\n");
            parsed = true;
        }

        if(strlen(app.cardholder_name)) {
            furi_string_cat_printf(parsed_data, "Cardholder: %s\n", app.cardholder_name);
            parsed = true;
        }

        if(app.exp_month) {
            furi_string_cat_printf(parsed_data, "Expires: %02X/%02X\n",
                                   app.exp_month, app.exp_year);
            parsed = true;
        }

        if(app.effective_month) {
            furi_string_cat_printf(parsed_data, "Effective: %02X/%02X\n",
                                   app.effective_month, app.effective_year);
            parsed = true;
        }

        if(app.country_code) {
            const char* country = emv_lookup(emv_country_codes, app.country_code);
            if(country)
                furi_string_cat_printf(parsed_data, "Country: %s\n", country);
            else
                furi_string_cat_printf(parsed_data, "Country: %04X\n", app.country_code);
            parsed = true;
        }

        if(app.currency_code) {
            const char* currency = emv_lookup(emv_currency_codes, app.currency_code);
            if(currency)
                furi_string_cat_printf(parsed_data, "Currency: %s\n", currency);
            else
                furi_string_cat_printf(parsed_data, "Currency: %04X\n", app.currency_code);
            parsed = true;
        }

        if(app.pin_try_counter != 0xFF && app.pin_try_counter != 0) {
            furi_string_cat_printf(parsed_data, "PIN tries left: %d\n", app.pin_try_counter);
            parsed = true;
        }

        if(app.last_online_atc) {
            furi_string_cat_printf(parsed_data, "Last online ATC: %u\n", app.last_online_atc);
            parsed = true;
        }

        if(app.trans_count > 0) {
            furi_string_cat_printf(parsed_data, "Transactions: %u\n", app.trans_count);
            const EmvTransaction *t = &app.trans[0]; /* most recent entry */
            unsigned long long amt = t->amount;
            furi_string_cat_printf(parsed_data, "Last TX: ATC=%u amt=%llu.%02llu",
                                   t->atc, amt / 100ULL, amt % 100ULL);
            if(t->date[0] || t->date[1] || t->date[2])
                furi_string_cat_printf(parsed_data, " date=%02X%02X%02X",
                                       t->date[0], t->date[1], t->date[2]);
            furi_string_cat_str(parsed_data, "\n");
            parsed = true;
        }

        if(app.application_interchange_profile[0] || app.application_interchange_profile[1]) {
            furi_string_cat_printf(parsed_data, "AIP: %02X %02X\n",
                                   app.application_interchange_profile[0],
                                   app.application_interchange_profile[1]);
            parsed = true;
        }

        if((app.application_interchange_profile[1] >> 6) & 0b1) {
            furi_string_cat_str(parsed_data, "Mobile: yes\n");
            parsed = true;
        }

        parsed = true;
    } while(false);

    return parsed;
}

const NfcSupportedCardsPlugin emv_plugin = {
    .protocol = NfcProtocolEmv,
    .verify = NULL,
    .read = NULL,
    .parse = emv_parse,
};

static const FlipperAppPluginDescriptor emv_plugin_descriptor = {
    .appid = NFC_SUPPORTED_CARD_PLUGIN_APP_ID,
    .ep_api_version = NFC_SUPPORTED_CARD_PLUGIN_API_VERSION,
    .entry_point = &emv_plugin,
};

const FlipperAppPluginDescriptor* emv_plugin_ep(void) {
    return &emv_plugin_descriptor;
}
