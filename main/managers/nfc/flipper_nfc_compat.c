/**
 * @file flipper_nfc_compat.c
 * @brief Implementation of Flipper Zero compatibility layer
 */

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

// --------------------------------------------------------------------------
// bit_lib Shim
// --------------------------------------------------------------------------

uint64_t bit_lib_bytes_to_num_le(const uint8_t* bytes, size_t len) {
    uint64_t res = 0;
    for (size_t i = 0; i < len; i++) {
        res |= ((uint64_t)bytes[i] << (8 * i));
    }
    return res;
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

// Registry array
static const NfcSupportedCardsPlugin* s_plugins[] = {
    &smartrider_plugin,
    NULL
};

char* flipper_nfc_try_parse_mfclassic_from_cache(const MfClassicData* data) {
    if (!data) return NULL;

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
