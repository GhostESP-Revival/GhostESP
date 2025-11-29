#include "managers/nfc/flipper_nfc_compat.h"

static const char* TAG = "FlipperCompat";

// --------------------------------------------------------------------------
// FuriString Implementation
// --------------------------------------------------------------------------

FuriString* furi_string_alloc(void) {
    FuriString* s = (FuriString*)malloc(sizeof(FuriString));
    if (s) {
        s->data = strdup("");
        s->len = 0;
        s->cap = 1;
    }
    return s;
}

void furi_string_free(FuriString* str) {
    if (!str) return;
    if (str->data) free(str->data);
    free(str);
}

void furi_string_reset(FuriString* str) {
    if (!str) return;
    if (str->data) free(str->data);
    str->data = strdup("");
    str->len = 0;
    str->cap = 1;
}

void furi_string_printf(FuriString* str, const char* fmt, ...) {
    if (!str) return;
    if (str->data) free(str->data);
    
    va_list args;
    va_start(args, fmt);
    int len = vasprintf(&str->data, fmt, args);
    va_end(args);

    if (len < 0) {
        // allocation failed
        str->data = strdup("");
        str->len = 0;
        str->cap = 1;
    } else {
        str->len = len;
        str->cap = len + 1;
    }
}

void furi_string_cat_printf(FuriString* str, const char* fmt, ...) {
    if (!str) return;
    
    va_list args;
    va_start(args, fmt);
    char* append_str = NULL;
    int append_len = vasprintf(&append_str, fmt, args);
    va_end(args);

    if (append_len > 0) {
        size_t new_len = str->len + append_len;
        char* new_data = (char*)realloc(str->data, new_len + 1);
        if (new_data) {
            str->data = new_data;
            memcpy(str->data + str->len, append_str, append_len);
            str->len = new_len;
            str->data[str->len] = '\0';
            str->cap = new_len + 1;
        }
    }
    if (append_str) free(append_str);
}

const char* furi_string_get_cstr(const FuriString* str) {
    return str ? str->data : "";
}

void furi_string_set_str(FuriString* str, const char* cstr) {
    if(!str) return;
    if(!cstr) cstr = "";
    if(str->data) free(str->data);
    size_t len = strlen(cstr);
    str->data = (char*)malloc(len + 1);
    if(!str->data) {
        str->len = 0;
        str->cap = 0;
        return;
    }
    memcpy(str->data, cstr, len + 1);
    str->len = len;
    str->cap = len + 1;
}

void furi_string_cat_str(FuriString* dst, const char* cstr) {
    if(!dst || !cstr) return;
    size_t add_len = strlen(cstr);
    if(add_len == 0) return;
    size_t new_len = dst->len + add_len;
    char* new_data = (char*)realloc(dst->data, new_len + 1);
    if(!new_data) return;
    memcpy(new_data + dst->len, cstr, add_len);
    new_data[new_len] = '\0';
    dst->data = new_data;
    dst->len = new_len;
    dst->cap = new_len + 1;
}

void furi_string_cat(FuriString* dst, const FuriString* src) {
    if(!dst || !src) return;
    furi_string_cat_str(dst, furi_string_get_cstr(src));
}

FuriString* furi_string_alloc_set_str(const char* cstr) {
    FuriString* str = furi_string_alloc();
    if (str && cstr) {
        furi_string_set_str(str, cstr);
    }
    return str;
}

void furi_string_push_back(FuriString* str, char c) {
    if (!str) return;
    if (str->len + 2 > str->cap) {
        size_t new_cap = (str->len + 2) * 2;
        char* new_data = realloc(str->data, new_cap);
        if (!new_data) return;
        str->data = new_data;
        str->cap = new_cap;
    }
    str->data[str->len++] = c;
    str->data[str->len] = '\0';
}

char furi_string_get_char(const FuriString* str, size_t index) {
    if (!str || index >= str->len) return '\0';
    return str->data[index];
}

// --------------------------------------------------------------------------
// bit_lib Shim
// --------------------------------------------------------------------------

uint64_t bit_lib_bytes_to_num_le(const uint8_t* bytes, size_t len) {
    uint64_t res = 0;
    if(!bytes) return 0;
    for(size_t i = 0; i < len; i++) {
        res |= ((uint64_t)bytes[i] << (8 * i));
    }
    return res;
}

uint64_t bit_lib_bytes_to_num_be(const uint8_t* bytes, size_t len) {
    uint64_t res = 0;
    if(!bytes) return 0;
    for(size_t i = 0; i < len; i++) {
        res = (res << 8) | bytes[i];
    }
    return res;
}

void bit_lib_num_to_bytes_be(uint64_t value, size_t len, uint8_t* out) {
    if(!out) return;
    for(size_t i = 0; i < len; i++) {
        size_t shift = (len - 1U - i) * 8U;
        out[i] = (uint8_t)((value >> shift) & 0xFFU);
    }
}

uint64_t bit_lib_bytes_to_num_bcd(const uint8_t* bytes, size_t len, bool* is_bcd) {
    uint64_t result = 0;
    bool valid = true;
    for (size_t i = 0; i < len && i < 8; i++) {
        uint8_t high = (bytes[i] >> 4) & 0x0F;
        uint8_t low = bytes[i] & 0x0F;
        if (high > 9 || low > 9) {
            valid = false;
        }
        result = result * 100 + high * 10 + low;
    }
    if (is_bcd) *is_bcd = valid;
    return result;
}

uint8_t bit_lib_get_bits(const uint8_t* data, size_t position, uint8_t length) {
    if (!data || length == 0 || length > 8) return 0;
    uint64_t result = bit_lib_get_bits_64(data, position, length);
    return (uint8_t)result;
}

uint32_t bit_lib_get_bits_32(const uint8_t* data, size_t position, uint8_t length) {
    if (!data || length == 0 || length > 32) return 0;
    uint64_t result = bit_lib_get_bits_64(data, position, length);
    return (uint32_t)result;
}

uint64_t bit_lib_get_bits_64(const uint8_t* data, size_t position, uint8_t length) {
    if (!data || length == 0 || length > 64) return 0;
    
    uint64_t result = 0;
    size_t byte_index = position / 8;
    uint8_t bit_offset = position % 8;
    
    for (uint8_t i = 0; i < length; i++) {
        size_t current_byte = byte_index + ((bit_offset + i) / 8);
        uint8_t current_bit = (bit_offset + i) % 8;
        
        if (data[current_byte] & (1 << current_bit)) {
            result |= (1ULL << i);
        }
    }
    
    return result;
}

// --------------------------------------------------------------------------
// Datetime / Locale shims
// --------------------------------------------------------------------------

static bool compat_is_leap_year(uint16_t year) {
    return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
}

uint32_t datetime_datetime_to_timestamp(const DateTime* dt) {
    if(!dt) return 0;
    uint16_t year = dt->year;
    if(year < 2000) year = 2000;
    uint32_t days = 0;
    for(uint16_t y = 2000; y < year; y++) {
        days += compat_is_leap_year(y) ? 366U : 365U;
    }
    static const uint8_t mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint8_t month = dt->month;
    if(month < 1) month = 1;
    if(month > 12) month = 12;
    for(uint8_t m = 1; m < month; m++) {
        days += mdays[m - 1];
        if(m == 2 && compat_is_leap_year(year)) days += 1;
    }
    uint8_t day = dt->day;
    if(day > 0) days += (uint32_t)(day - 1U);
    uint32_t seconds = days * 86400U;
    seconds += (uint32_t)dt->hour * 3600U;
    seconds += (uint32_t)dt->minute * 60U;
    seconds += (uint32_t)dt->second;
    return seconds;
}

void datetime_timestamp_to_datetime(uint32_t ts, DateTime* dt) {
    if(!dt) return;
    uint16_t year = 2000;
    uint32_t days = ts / 86400U;
    uint32_t rem = ts % 86400U;
    while(true) {
        uint32_t year_days = compat_is_leap_year(year) ? 366U : 365U;
        if(days < year_days) break;
        days -= year_days;
        year++;
    }
    static const uint8_t mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint8_t month = 1;
    while(month <= 12) {
        uint8_t dim = mdays[month - 1];
        if(month == 2 && compat_is_leap_year(year)) dim++;
        if(days < dim) break;
        days -= dim;
        month++;
    }
    dt->year = year;
    dt->month = month;
    dt->day = (uint8_t)(days + 1U);
    dt->hour = (uint8_t)(rem / 3600U);
    rem %= 3600U;
    dt->minute = (uint8_t)(rem / 60U);
    dt->second = (uint8_t)(rem % 60U);
}

LocaleDateFormat locale_get_date_format(void) {
    return LocaleDateFormatDMY;
}

LocaleTimeFormat locale_get_time_format(void) {
    return LocaleTimeFormat24h;
}

void locale_format_date(FuriString* out, const DateTime* dt, LocaleDateFormat fmt, const char* sep) {
    if(!out || !dt) return;
    if(!sep) sep = "/";
    uint16_t y = dt->year;
    uint8_t m = dt->month;
    uint8_t d = dt->day;
    switch(fmt) {
    case LocaleDateFormatMDY:
        furi_string_printf(out, "%02u%s%02u%s%04u", m, sep, d, sep, y);
        break;
    case LocaleDateFormatYMD:
        furi_string_printf(out, "%04u%s%02u%s%02u", y, sep, m, sep, d);
        break;
    case LocaleDateFormatDMY:
    default:
        furi_string_printf(out, "%02u%s%02u%s%04u", d, sep, m, sep, y);
        break;
    }
}

void locale_format_time(FuriString* out, const DateTime* dt, LocaleTimeFormat fmt, bool with_seconds) {
    if(!out || !dt) return;
    uint8_t h = dt->hour;
    uint8_t m = dt->minute;
    uint8_t s = dt->second;
    if(fmt == LocaleTimeFormat12h) {
        const char* ampm = "AM";
        uint8_t hour12 = h;
        if(hour12 == 0) {
            hour12 = 12;
        } else if(hour12 >= 12) {
            if(hour12 > 12) hour12 -= 12;
            ampm = "PM";
        }
        if(with_seconds) {
            furi_string_printf(out, "%02u:%02u:%02u %s", hour12, m, s, ampm);
        } else {
            furi_string_printf(out, "%02u:%02u %s", hour12, m, ampm);
        }
    } else {
        if(with_seconds) {
            furi_string_printf(out, "%02u:%02u:%02u", h, m, s);
        } else {
            furi_string_printf(out, "%02u:%02u", h, m);
        }
    }
}

// --------------------------------------------------------------------------
// NfcDevice / MfClassic Helpers
// --------------------------------------------------------------------------

struct NfcDevice {
    NfcProtocol protocol;
    void* data;
};

void nfc_device_copy_data(const NfcDevice* dev, NfcProtocol protocol, void* dst) {
    if (!dev || !dst) return;
    if (dev->protocol == protocol && protocol == NfcProtocolMfClassic) {
        // In Flipper, this deep copies. We will do a shallow memcpy for now
        // as our MfClassicData is a flat struct.
        memcpy(dst, dev->data, sizeof(MfClassicData));
    }
}

void nfc_device_set_data(NfcDevice* dev, NfcProtocol protocol, void* src) {
    if (!dev) return;
    // We are replacing the pointer or content. 
    // For this shim, we assume src is a managed pointer or stack object we copy from?
    // Actually, SmartRider uses copy_data to get FROM device, and set_data to put back.
    // Since we are only PARSING, we don't expect the plugin to modify device state meaningfully.
    // But we'll update the protocol.
    dev->protocol = protocol;
    // If we owned data, we'd free it. Here we just point to the new data or copy it?
    // For safety in this limited scope:
    if (dev->data && src) {
        memcpy(dev->data, src, sizeof(MfClassicData));
    }
}

const void* nfc_device_get_data(const NfcDevice* dev, NfcProtocol protocol) {
    if (!dev || dev->protocol != protocol) return NULL;
    return dev->data;
}

// --------------------------------------------------------------------------
// MIFARE Classic Logic
// --------------------------------------------------------------------------

MfClassicData* mf_classic_alloc(void) {
    return (MfClassicData*)calloc(1, sizeof(MfClassicData));
}

void mf_classic_free(MfClassicData* data) {
    free(data);
}

size_t mf_classic_get_total_sectors_num(MfClassicType type) {
    switch(type) {
        case MfClassicTypeMini: return 5;
        case MfClassicType1k: return 16;
        case MfClassicType4k: return 40;
        default: return 16;
    }
}

uint8_t mf_classic_get_first_block_num_of_sector(uint8_t sector) {
    if (sector < 32) return sector * 4;
    return 32 * 4 + (sector - 32) * 16;
}

bool mf_classic_is_block_read(const MfClassicData* data, uint8_t block) {
    if (!data) return false;
    // check bitmask
    return (data->block_read_mask[block / 8] & (1 << (block % 8))) != 0;
}

const MfClassicSectorTrailer* mf_classic_get_sector_trailer_by_sector(const MfClassicData* data, uint8_t sector) {
    if (!data) return NULL;
    // Calculate trailer block
    uint8_t first = mf_classic_get_first_block_num_of_sector(sector);
    uint8_t count = (sector < 32) ? 4 : 16;
    uint8_t trailer = first + count - 1;
    
    // We return a pointer to the block data, cast as trailer.
    // NOTE: Flipper's MfClassicData structure stores trailers differently/separately sometimes,
    // but here we are mapping flat blocks.
    // However, SmartRider expects to see `key_a` in the trailer struct. 
    // GhostESP cache stores keys separately.
    // So we must ensure that when we built MfClassicData, we put the keys into the trailer block.
    // The shim `mfc_build_flipper_mfclassic_view` must handle this.
    
    return (const MfClassicSectorTrailer*)&data->block[trailer];
}

const uint8_t* mf_classic_get_uid(const MfClassicData* data, size_t* uid_len) {
    if(!data || data->uid_len == 0) {
        if(uid_len) *uid_len = 0;
        return NULL;
    }
    if(uid_len) *uid_len = data->uid_len;
    return data->uid;
}

bool mf_classic_is_card_read(const MfClassicData* data) {
    if(!data) return false;
    for(size_t i = 0; i < sizeof(data->block_read_mask); i++) {
        if(data->block_read_mask[i] != 0) return true;
    }
    return false;
}

// Stubs
MfClassicError mf_classic_poller_sync_detect_type(Nfc* nfc, MfClassicType* type) { return MfClassicErrorProtocol; }
MfClassicError mf_classic_poller_sync_read(Nfc* nfc, const MfClassicDeviceKeys* keys, MfClassicData* data) { return MfClassicErrorProtocol; }
MfClassicError mf_classic_poller_sync_auth(Nfc* nfc, uint8_t block, const MfClassicKey* key, MfClassicKeyType type, void* dict_ctx) { return MfClassicErrorProtocol; }
MfClassicError mf_classic_poller_sync_read_block(Nfc* nfc, uint8_t block, const MfClassicKey* key, MfClassicKeyType type, MfClassicBlock* data) { return MfClassicErrorProtocol; }

// --------------------------------------------------------------------------
// Registry & Dispatcher
// --------------------------------------------------------------------------

// Declare plugins extern
extern const NfcSupportedCardsPlugin smartrider_plugin;
extern const NfcSupportedCardsPlugin aime_plugin;
extern const NfcSupportedCardsPlugin csc_plugin;
extern const NfcSupportedCardsPlugin washcity_plugin;
extern const NfcSupportedCardsPlugin metromoney_plugin;
extern const NfcSupportedCardsPlugin bip_plugin;
extern const NfcSupportedCardsPlugin charliecard_plugin;
extern const NfcSupportedCardsPlugin disney_infinity_plugin;
extern const NfcSupportedCardsPlugin hi_plugin;
extern const NfcSupportedCardsPlugin hid_plugin;
extern const NfcSupportedCardsPlugin hworld_plugin;
extern const NfcSupportedCardsPlugin kazan_plugin;
extern const NfcSupportedCardsPlugin microel_plugin;
extern const NfcSupportedCardsPlugin mizip_plugin;
extern const NfcSupportedCardsPlugin plantain_plugin;
extern const NfcSupportedCardsPlugin saflok_plugin;
extern const NfcSupportedCardsPlugin skylanders_plugin;
extern const NfcSupportedCardsPlugin social_moscow_plugin;
extern const NfcSupportedCardsPlugin troika_plugin;
extern const NfcSupportedCardsPlugin two_cities_plugin;
extern const NfcSupportedCardsPlugin umarsh_plugin;
extern const NfcSupportedCardsPlugin zolotaya_korona_plugin;
extern const NfcSupportedCardsPlugin zolotaya_korona_online_plugin;

// Registry array
static const NfcSupportedCardsPlugin* s_plugins[] = {
    &smartrider_plugin,
    &aime_plugin,
    &csc_plugin,
    &washcity_plugin,
    &metromoney_plugin,
    &bip_plugin,
    &charliecard_plugin,
    &disney_infinity_plugin,
    &hi_plugin,
    &hid_plugin,
    &hworld_plugin,
    &kazan_plugin,
    &microel_plugin,
    &mizip_plugin,
    &plantain_plugin,
    &saflok_plugin,
    &skylanders_plugin,
    &social_moscow_plugin,
    &troika_plugin,
    &two_cities_plugin,
    &umarsh_plugin,
    &zolotaya_korona_plugin,
    &zolotaya_korona_online_plugin,
    NULL
};

char* flipper_nfc_try_parse_mfclassic_from_cache(const MfClassicData* data) {
    if (!data) return NULL;
    if (data->uid_len == 0) return NULL;

    // Setup a temporary device wrapper
    NfcDevice dev;
    dev.protocol = NfcProtocolMfClassic;
    dev.data = (void*)data; // We cast away const, but pure parsers shouldn't mutate

    for (int i = 0; s_plugins[i] != NULL; i++) {
        const NfcSupportedCardsPlugin* p = s_plugins[i];
        if (p->protocol == NfcProtocolMfClassic && p->parse) {
            FuriString* out = furi_string_alloc();
            if (p->parse(&dev, out)) {
                // Success
                const char* res = furi_string_get_cstr(out);
                char* result_copy = NULL;
                if (res && strlen(res) > 0) {
                    result_copy = strdup(res);
                }
                furi_string_free(out);
                return result_copy;
            }
            furi_string_free(out);
        }
    }
    return NULL;
}
