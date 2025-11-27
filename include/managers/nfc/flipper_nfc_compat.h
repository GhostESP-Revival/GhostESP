/**
 * @file flipper_nfc_compat.h
 * @brief Compatibility layer to run Flipper Zero NFC parsers on GhostESP
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

// --------------------------------------------------------------------------
// Furi Core Shims
// --------------------------------------------------------------------------

#define FURI_LOG_D(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
#define FURI_LOG_I(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define FURI_LOG_W(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define FURI_LOG_E(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)

#define furi_assert(x) do { if(!(x)) { ESP_LOGE("COMPAT", "ASSERT FAILED: %s", #x); abort(); } } while(0)

#ifndef COUNT_OF
#define COUNT_OF(x) ((sizeof(x)/sizeof(0[x])) / ((size_t)(!(sizeof(x) % sizeof(0[x])))))
#endif

#define FURI_BIT_SET(p, n) ((p) |= (1UL << (n)))
#define FURI_BIT_CLEAR(p, n) ((p) &= ~(1UL << (n)))

// FuriString (Minimal Implementation)
typedef struct {
    char* data;
    size_t len;
    size_t cap;
} FuriString;

FuriString* furi_string_alloc(void);
void furi_string_free(FuriString* str);
void furi_string_reset(FuriString* str);
void furi_string_printf(FuriString* str, const char* fmt, ...);
void furi_string_cat_printf(FuriString* str, const char* fmt, ...);
const char* furi_string_get_cstr(const FuriString* str);
void furi_string_set_str(FuriString* str, const char* cstr);
void furi_string_cat(FuriString* dst, const FuriString* src);
void furi_string_cat_str(FuriString* dst, const char* cstr);
FuriString* furi_string_alloc_set_str(const char* cstr);
void furi_string_push_back(FuriString* str, char c);
char furi_string_get_char(const FuriString* str, size_t index);

// bit_lib shim
uint64_t bit_lib_bytes_to_num_le(const uint8_t* bytes, size_t len);
uint64_t bit_lib_bytes_to_num_be(const uint8_t* bytes, size_t len);
void bit_lib_num_to_bytes_be(uint64_t value, size_t len, uint8_t* out);
uint64_t bit_lib_bytes_to_num_bcd(const uint8_t* bytes, size_t len, bool* is_bcd);
uint8_t bit_lib_get_bits(const uint8_t* data, size_t position, uint8_t length);
uint32_t bit_lib_get_bits_32(const uint8_t* data, size_t position, uint8_t length);
uint64_t bit_lib_get_bits_64(const uint8_t* data, size_t position, uint8_t length);

#define REVERSE_BYTES_U32(x) __builtin_bswap32(x)

// --------------------------------------------------------------------------
// Datetime / Locale shims
// --------------------------------------------------------------------------

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} DateTime;

uint32_t datetime_datetime_to_timestamp(const DateTime* dt);
void datetime_timestamp_to_datetime(uint32_t ts, DateTime* dt);

typedef enum {
    LocaleDateFormatDMY = 0,
    LocaleDateFormatMDY,
    LocaleDateFormatYMD,
} LocaleDateFormat;

typedef enum {
    LocaleTimeFormat24h = 0,
    LocaleTimeFormat12h,
} LocaleTimeFormat;

LocaleDateFormat locale_get_date_format(void);
LocaleTimeFormat locale_get_time_format(void);
void locale_format_date(FuriString* out, const DateTime* dt, LocaleDateFormat fmt, const char* sep);
void locale_format_time(FuriString* out, const DateTime* dt, LocaleTimeFormat fmt, bool with_seconds);

// --------------------------------------------------------------------------
// NFC / Device Shims
// --------------------------------------------------------------------------

// Opaque types from Flipper
typedef struct Nfc Nfc; 

typedef enum {
    NfcProtocolUnknown = 0,
    NfcProtocolMfClassic,
    // Add others if needed
} NfcProtocol;

typedef struct NfcDevice NfcDevice;

void nfc_device_copy_data(const NfcDevice* dev, NfcProtocol protocol, void* dst);
void nfc_device_set_data(NfcDevice* dev, NfcProtocol protocol, void* src);
const void* nfc_device_get_data(const NfcDevice* dev, NfcProtocol protocol);

// --------------------------------------------------------------------------
// MIFARE Classic Shims
// --------------------------------------------------------------------------

typedef enum {
    MfClassicType1k = 0,
    MfClassicType4k,
    MfClassicTypeMini,
    // simplified
} MfClassicType;

typedef enum {
    MfClassicKeyTypeA = 0,
    MfClassicKeyTypeB = 1,
} MfClassicKeyType;

typedef enum {
    MfClassicErrorNone = 0,
    MfClassicErrorNotPresent,
    MfClassicErrorProtocol,
    MfClassicErrorAuth,
    MfClassicErrorPartialRead,
} MfClassicError;

typedef struct {
    uint8_t data[6];
} MfClassicKey;

typedef struct {
    uint8_t data[16];
} MfClassicBlock;

typedef struct {
    MfClassicKey key_a;
    uint8_t access_bits[4];
    MfClassicKey key_b;
} MfClassicSectorTrailer;

typedef struct {
    MfClassicKey key_a[40]; // Sufficient for 4K (40 sectors)
    MfClassicKey key_b[40];
    uint64_t key_a_mask;    // Simple mask for first 64 sectors
    uint64_t key_b_mask;
} MfClassicDeviceKeys;

// Internal representation of data for the plugin
typedef struct {
    MfClassicType type;
    MfClassicBlock block[256]; // Max blocks for 4K
    uint8_t block_read_mask[32]; // 256 bits indicating valid blocks
    // Helper to track known keys for sector trailer emulation if needed
    MfClassicDeviceKeys keys; 
    uint8_t uid[10];
    uint8_t uid_len;
} MfClassicData;

// Minimal auth context placeholder used by Flipper poller APIs
typedef struct {
    uint8_t reserved[16];
} MfClassicAuthContext;

MfClassicData* mf_classic_alloc(void);
void mf_classic_free(MfClassicData* data);

size_t mf_classic_get_total_sectors_num(MfClassicType type);
uint8_t mf_classic_get_first_block_num_of_sector(uint8_t sector);
bool mf_classic_is_block_read(const MfClassicData* data, uint8_t block);
const MfClassicSectorTrailer* mf_classic_get_sector_trailer_by_sector(const MfClassicData* data, uint8_t sector);
const uint8_t* mf_classic_get_uid(const MfClassicData* data, size_t* uid_len);
bool mf_classic_is_card_read(const MfClassicData* data);

// Poller stubs (GhostESP does not use these for parsing, but plugins reference them)
MfClassicError mf_classic_poller_sync_detect_type(Nfc* nfc, MfClassicType* type);
MfClassicError mf_classic_poller_sync_read(Nfc* nfc, const MfClassicDeviceKeys* keys, MfClassicData* data);
MfClassicError mf_classic_poller_sync_auth(Nfc* nfc, uint8_t block, const MfClassicKey* key, MfClassicKeyType type, void* dict_ctx);
MfClassicError mf_classic_poller_sync_read_block(Nfc* nfc, uint8_t block, const MfClassicKey* key, MfClassicKeyType type, MfClassicBlock* data);

// --------------------------------------------------------------------------
// Plugin Interface
// --------------------------------------------------------------------------

#define NFC_SUPPORTED_CARD_PLUGIN_APP_ID "NfcSupportedCardPlugin"
#define NFC_SUPPORTED_CARD_PLUGIN_API_VERSION 1

typedef bool (*NfcSupportedCardPluginVerify)(Nfc* nfc);
typedef bool (*NfcSupportedCardPluginRead)(Nfc* nfc, NfcDevice* device);
typedef bool (*NfcSupportedCardPluginParse)(const NfcDevice* device, FuriString* parsed_data);

typedef struct {
    NfcProtocol protocol;
    NfcSupportedCardPluginVerify verify;
    NfcSupportedCardPluginRead read;
    NfcSupportedCardPluginParse parse;
} NfcSupportedCardsPlugin;

typedef struct {
    const char* appid;
    uint32_t ep_api_version;
    const NfcSupportedCardsPlugin* entry_point;
} FlipperAppPluginDescriptor;

// --------------------------------------------------------------------------
// GhostESP Dispatcher
// --------------------------------------------------------------------------

/**
 * @brief Try to parse MIFARE Classic data using registered Flipper plugins.
 * @param data The filled MfClassicData structure (converted from GhostESP cache)
 * @return Heap-allocated string with description, or NULL if no plugin matched. Caller frees.
 */
char* flipper_nfc_try_parse_mfclassic_from_cache(const MfClassicData* data);

#ifdef __cplusplus
}
#endif
