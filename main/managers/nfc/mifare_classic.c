// MIFARE Classic nested/hardnested capture and `.nested.log` workflow reference:
// Momentum-Firmware work by noproto, including key recovery improvements #3822.

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "managers/nfc/mifare_classic.h"
#include "managers/nfc/mifare_attack.h"
#include "managers/sd_card_manager.h"
#include "esp_heap_caps.h"
#include "gui/toast.h"
#include "esp_log.h"
#include "esp_timer.h"
#if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_ST25R3916)
#include "pn532.h"
#endif
#if defined(CONFIG_NFC_ST25R3916)
#include "crypto1.h"
#include <math.h>
#endif
#include "managers/fuel_gauge_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "managers/nfc/ndef.h"
#include "managers/nfc/ntag_t2.h"
#include "managers/nfc/flipper_nfc_compat.h"

char* ndef_build_details_from_tlv(const uint8_t* tlv_area,
                                  size_t tlv_len,
                                  const uint8_t* uid,
                                  uint8_t uid_len,
                                  const char* card_label);

#define MFC_TAG "MFC"
#if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_ST25R3916)
static void crc_a_calc(const uint8_t *data, size_t len, uint8_t out[2]);
#endif

static const mfc_attack_hooks_t *g_attack_hooks = NULL;

static inline void mfc_call_on_phase(int sector, int first_block, bool key_b, int total_keys) {
    if (g_attack_hooks && g_attack_hooks->on_phase) g_attack_hooks->on_phase(sector, first_block, key_b, total_keys);
}
static inline void mfc_call_on_cache_mode(bool on) {
    if (g_attack_hooks && g_attack_hooks->on_cache_mode) g_attack_hooks->on_cache_mode(on);
}
static inline void mfc_call_on_paused(bool on) {
    if (g_attack_hooks && g_attack_hooks->on_paused) g_attack_hooks->on_paused(on);
}
static inline bool mfc_call_should_cancel(void) {
    return (g_attack_hooks && g_attack_hooks->should_cancel) ? g_attack_hooks->should_cancel() : false;
}
static inline bool mfc_call_should_skip_dict(void) {
    return (g_attack_hooks && g_attack_hooks->should_skip_dict) ? g_attack_hooks->should_skip_dict() : false;
}

extern void mfc_ui_set_phase(int sector, int first_block, bool key_b, int total_keys) __attribute__((weak));
static void mfc_enable_debug_once(void) {
    static bool done = false;
    if (done) return;
    esp_log_level_set("PN532", ESP_LOG_DEBUG);
    esp_log_level_set("MFC", ESP_LOG_DEBUG);
    done = true;
}

 
static int mfc_sector_of_block(MFC_TYPE t, int abs_block){
    int sectors = mfc_sector_count(t); if (sectors == 0) sectors = 16;
    for (int s = 0; s < sectors; ++s) {
        int first = mfc_first_block_of_sector(t, s);
        int blocks = mfc_blocks_in_sector(t, s);
        if (abs_block >= first && abs_block < first + blocks) return s;
    }
    return -1;
}
// User dictionary parser: accept real 6-byte key tokens, not arbitrary hex
// characters from labelled solver/log lines such as "Sec 1 key A cuid ...".
static int hexn_u(char c){ if(c>='0'&&c<='9')return c-'0'; c|=0x20; if(c>='a'&&c<='f')return 10+(c-'a'); return -1; }
static bool parse_12_hex_token_u(const char* s, const char* e, uint8_t out[6]){
    char hex[12];
    int n = 0;
    for (const char* p = s; p < e; ++p) {
        int v = hexn_u(*p);
        if (v >= 0) {
            if (n >= 12) return false;
            hex[n++] = *p;
        } else if (*p == ':' || *p == '-') {
            continue;
        } else {
            return false;
        }
    }
    if (n != 12) return false;
    for (int i = 0; i < 6; ++i) {
        int hi = hexn_u(hex[i * 2]);
        int lo = hexn_u(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}
static bool parse_key_line_u(const char* s,const char* e,uint8_t out[6]){
    while (s < e && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) s++;
    if (s >= e || *s == '#') return false;

    // First accept a standalone token: A0A1A2A3A4A5, A0:A1:..., or A0-A1-...
    for (const char* p = s; p < e;) {
        while (p < e && !((hexn_u(*p) >= 0) || *p == ':' || *p == '-')) p++;
        const char* ts = p;
        while (p < e && ((hexn_u(*p) >= 0) || *p == ':' || *p == '-')) p++;
        if (ts < p && parse_12_hex_token_u(ts, p, out)) return true;
    }

    // Also accept six byte tokens separated by whitespace: A0 A1 A2 A3 A4 A5.
    uint8_t bytes[6];
    int count = 0;
    for (const char* p = s; p < e;) {
        while (p < e && (*p == ' ' || *p == '\t' || *p == ',')) p++;
        const char* ts = p;
        while (p < e && hexn_u(*p) >= 0) p++;
        if (p - ts == 2) {
            int hi = hexn_u(ts[0]);
            int lo = hexn_u(ts[1]);
            bytes[count++] = (uint8_t)((hi << 4) | lo);
            if (count == 6) {
                memcpy(out, bytes, 6);
                return true;
            }
        } else if (p > ts) {
            count = 0;
        }
        while (p < e && *p != ' ' && *p != '\t' && *p != ',') p++;
    }
    return false;
}

#define BITSET_SET(arr, idx)   ((arr)[(idx) >> 3] |= (uint8_t)(1u << ((idx) & 7)))
#define BITSET_TEST(arr, idx)  (((arr)[(idx) >> 3] & (uint8_t)(1u << ((idx) & 7))) != 0)
// Simple cache of last scanned Classic card to allow saving without the card present
static uint8_t *g_mfc_cache = NULL;          // blocks*16
static uint8_t *g_mfc_known = NULL;          // bitset for known blocks
static int g_mfc_blocks = 0;
static MFC_TYPE g_mfc_type = MFC_UNKNOWN;
static uint8_t g_mfc_uid[10] = {0};
static uint8_t g_mfc_uid_len = 0;
static uint16_t g_mfc_atqa = 0;
static uint8_t g_mfc_sak = 0;
// Discovered per-sector keys
static uint8_t *g_sector_key_a = NULL;       // sectors * 6
static uint8_t *g_sector_key_b = NULL;       // sectors * 6
static uint8_t *g_sector_key_a_valid = NULL; // bitset per sector
static uint8_t *g_sector_key_b_valid = NULL; // bitset per sector

// Forward declaration for session reset function
static void mfc_session_reset(void);

#if !defined(CONFIG_NFC_PN532) && !defined(CONFIG_NFC_ST25R3916)
static void mfc_session_reset(void) {
}
#endif

static void mfc_cache_reset(void){
    if(g_mfc_cache){ free(g_mfc_cache); g_mfc_cache=NULL; }
    if(g_mfc_known){ free(g_mfc_known); g_mfc_known=NULL; }
    if(g_sector_key_a){ free(g_sector_key_a); g_sector_key_a=NULL; }
    if(g_sector_key_b){ free(g_sector_key_b); g_sector_key_b=NULL; }
    if(g_sector_key_a_valid){ free(g_sector_key_a_valid); g_sector_key_a_valid=NULL; }
    if(g_sector_key_b_valid){ free(g_sector_key_b_valid); g_sector_key_b_valid=NULL; }
    g_mfc_blocks=0; g_mfc_type=MFC_UNKNOWN; g_mfc_uid_len=0; g_mfc_atqa=0; g_mfc_sak=0;
}
static void mfc_cache_begin(MFC_TYPE t, const uint8_t* uid, uint8_t uid_len, uint16_t atqa, uint8_t sak){
    mfc_cache_reset();
    // Reset session state for new scan to ensure clean state
    mfc_session_reset();
    g_mfc_type = t; g_mfc_uid_len = uid_len; if(uid && uid_len){ memcpy(g_mfc_uid, uid, uid_len); }
    g_mfc_atqa = atqa; g_mfc_sak = sak;
    int sectors = mfc_sector_count(t); if (sectors == 0) sectors = 16;
    int blocks = 0;
    for (int s=0; s<sectors; ++s) blocks += mfc_blocks_in_sector(t, s);
    g_mfc_blocks = blocks;
    g_mfc_cache = (uint8_t*)calloc((size_t)blocks, 16);
    int known_bytes = (blocks + 7) >> 3;
    g_mfc_known = (uint8_t*)calloc((size_t)known_bytes, 1);
    // per-sector keys
    g_sector_key_a = (uint8_t*)calloc((size_t)sectors, 6);
    g_sector_key_b = (uint8_t*)calloc((size_t)sectors, 6);
    int sec_bits = (sectors + 7) >> 3;
    g_sector_key_a_valid = (uint8_t*)calloc((size_t)sec_bits, 1);
    g_sector_key_b_valid = (uint8_t*)calloc((size_t)sec_bits, 1);
    if (!g_mfc_cache || !g_mfc_known ||
        !g_sector_key_a || !g_sector_key_b ||
        !g_sector_key_a_valid || !g_sector_key_b_valid) {
        mfc_cache_reset();
    }
}
static void mfc_cache_store_block(int abs_block, const uint8_t data[16]){
    if (!g_mfc_cache || !g_mfc_known) return;
    if (abs_block < 0 || abs_block >= g_mfc_blocks) return;
    memcpy(&g_mfc_cache[abs_block * 16], data, 16);
    BITSET_SET(g_mfc_known, abs_block);
}
// Check if all blocks in a sector are already present in the cache
static bool mfc_sector_all_blocks_known(MFC_TYPE t, int sector) {
    if (!g_mfc_known || sector < 0) return false;
    int first = mfc_first_block_of_sector(t, sector);
    int blocks = mfc_blocks_in_sector(t, sector);
    for (int b = 0; b < blocks; ++b) {
        int idx = first + b;
        if (idx < 0 || idx >= g_mfc_blocks) return false;
        if (!BITSET_TEST(g_mfc_known, idx)) return false;
    }
    return true;
}
static bool mfc_cache_matches(const uint8_t* uid, uint8_t uid_len){
    if (!g_mfc_cache || !g_mfc_known || g_mfc_uid_len != uid_len) return false;
    return (uid_len == 0) ? false : (memcmp(g_mfc_uid, uid, uid_len) == 0);
}
static bool mfc_cache_all_blocks_known(void) {
    if (!g_mfc_known || g_mfc_blocks <= 0) return false;
    for (int b = 0; b < g_mfc_blocks; ++b) {
        if (!BITSET_TEST(g_mfc_known, b)) return false;
    }
    return true;
}

bool mfc_has_unread_blocks(void) {
    return !mfc_cache_all_blocks_known();
}

static inline void mfc_cache_record_sector_key(int sector, bool usedB, const uint8_t key[6]){
    if (!g_sector_key_a || !g_sector_key_b || !g_sector_key_a_valid || !g_sector_key_b_valid) return;
    if (sector < 0) return;
    if (usedB) {
        memcpy(&g_sector_key_b[sector*6], key, 6);
        BITSET_SET(g_sector_key_b_valid, sector);
    } else {
        memcpy(&g_sector_key_a[sector*6], key, 6);
        BITSET_SET(g_sector_key_a_valid, sector);
    }
}

#if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_ST25R3916)
#define MFC_NESTED_ANALYZE_NT_COUNT 5
#define MFC_NESTED_NT_HARD_MINIMUM 3

static const uint8_t *mfc_cuid_bytes_from_uid(const uint8_t *uid, uint8_t uid_len) {
    if (!uid || uid_len < 4) return NULL;
    // cuid is the last 4 UID bytes for all lengths, matching Flipper/Momentum's
    // iso14443_3a_get_cuid() (uid[uid_len-4..]). Crypto1 auth and the .nested.log
    // must agree with the external solver, so do not special-case UID length.
    return &uid[uid_len - 4];
}

static uint32_t mfc_cuid_from_uid(const uint8_t *uid, uint8_t uid_len) {
    const uint8_t *u = mfc_cuid_bytes_from_uid(uid, uid_len);
    if (!u) return 0;
    return ((uint32_t)u[0] << 24) | ((uint32_t)u[1] << 16) | ((uint32_t)u[2] << 8) | u[3];
}

static bool mfc_is_weak_prng_nonce(uint32_t nonce) {
    if (nonce == 0) return false;
    uint16_t x = (uint16_t)(nonce >> 16);
    x = (uint16_t)((x & 0xffu) << 8) | (uint16_t)(x >> 8);
    for (uint8_t i = 0; i < 16; ++i) {
        x = (uint16_t)((x >> 1) | (uint16_t)(x ^ (x >> 2) ^ (x >> 3) ^ (x >> 5)) << 15);
    }
    x = (uint16_t)((x & 0xffu) << 8) | (uint16_t)(x >> 8);
    return x == (uint16_t)(nonce & 0xffffu);
}

// PRNG analysis mirrors Momentum's weak/hard split: collect a few plaintext tag
// nonces and count how many do not fit the weak 16-bit PRNG relation.
static bool g_prng_checked = false;
static bool g_prng_weak = false;
static bool mfc_get_nonce(pn532_io_handle_t io, uint8_t block, bool key_b, uint8_t nt[4]){
    uint8_t cmd[4] = { key_b ? 0x61 : 0x60, block, 0, 0 };
    uint8_t crc[2];
    crc_a_calc(cmd, 2, crc);
    cmd[2]=crc[0]; cmd[3]=crc[1];
    uint8_t resp[8]; uint8_t rlen = sizeof(resp);
    if (pn532_in_communicate_thru(io, cmd, sizeof(cmd), resp, &rlen) != ESP_OK) return false;
    if (rlen < 4) return false;
    memcpy(nt, resp, 4);
    return true;
}
static void mfc_analyze_prng_once(pn532_io_handle_t io){
    if (g_prng_checked) {
        return;
    }
    g_prng_checked = true;
    if (!io) return;
    int n = 0;
    int hard = 0;
    for (int i = 0; i < MFC_NESTED_ANALYZE_NT_COUNT; i++){
        uint8_t nt[4];
        if (mfc_get_nonce(io, 0, false, nt)){
            uint32_t nt32 = ((uint32_t)nt[0] << 24) | ((uint32_t)nt[1] << 16) |
                            ((uint32_t)nt[2] << 8) | nt[3];
            n++;
            if (!mfc_is_weak_prng_nonce(nt32)) hard++;
        } else {
            break;
        }
    }
    g_prng_weak = (n > 0) && (hard < MFC_NESTED_NT_HARD_MINIMUM);
    ESP_LOGI(MFC_TAG, "PRNG analysis: samples=%d hard=%d weak=%d", n, hard, (int)g_prng_weak);
    // Raw 0x60/0x61 via InCommunicateThru can disturb selection; reselect to restore state
    (void)pn532_in_list_passive_target(io);
}

static const uint8_t DEFAULT_KEYS[][6] = {
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
    {0xA0,0xA1,0xA2,0xA3,0xA4,0xA5},
    {0x00,0x00,0x00,0x00,0x00,0x00},
    {0xD3,0xF7,0xD3,0xF7,0xD3,0xF7},
    {0x4D,0x3A,0x99,0xC3,0x51,0xDD},
    {0x1A,0x98,0x2C,0x7E,0x45,0x9A},
    {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF},
    {0xB0,0xB1,0xB2,0xB3,0xB4,0xB5},
    {0xC0,0xC1,0xC2,0xC3,0xC4,0xC5},
    {0xD0,0xD1,0xD2,0xD3,0xD4,0xD5},
    {0x00,0x11,0x22,0x33,0x44,0x55},
    {0x12,0x34,0x56,0x78,0x9A,0xBC},
};
// Forward declarations needed before dictionary helpers use them
static esp_err_t mfc_auth_block(pn532_io_handle_t io, uint8_t block, bool use_key_b,
                                const uint8_t key[6], const uint8_t *uid, uint8_t uid_len);
static esp_err_t mfc_read_block(pn532_io_handle_t io, uint8_t block, uint8_t out16[16]);
static void mfc_record_working_key(const uint8_t key[6], bool use_key_b);
static bool mfc_auth_with_known(pn532_io_handle_t io, uint8_t block, bool use_key_b,
                                const uint8_t *uid, uint8_t uid_len);
// Safe auth target selector (prototype) used by summary/save flows
static uint8_t mfc_auth_target_block(MFC_TYPE t, int sector);
// Fast sweep using a found key to unlock other sectors quickly
static void mfc_key_reuse_sweep(pn532_io_handle_t io, MFC_TYPE t, const uint8_t *uid, uint8_t uid_len,
                                bool use_key_b, const uint8_t key[6], int current_sector);
// Verify and harvest keys from a readable trailer block
static void mfc_harvest_trailer_keys(pn532_io_handle_t io, MFC_TYPE t, int sector,
                                     const uint8_t *uid, uint8_t uid_len, const uint8_t trailer[16]);
// After unlocking with A or B, try to find the complementary key using user dict only (lightweight)
static void mfc_try_find_complementary_user_key(pn532_io_handle_t io, MFC_TYPE t, int sector, uint8_t auth_blk,
                                                const uint8_t *uid, uint8_t uid_len, bool usedB_cur);
static bool mfc_try_nested_dict_recovery(pn532_io_handle_t io, MFC_TYPE t, int sector,
                                         bool target_key_b, const uint8_t *uid, uint8_t uid_len,
                                         uint8_t out_key[6]);
// Card presence detection functions
static bool mfc_tag_is_present(pn532_io_handle_t io, const uint8_t* uid, uint8_t uid_len);
static bool mfc_wait_for_tag_return(pn532_io_handle_t io, const uint8_t* uid, uint8_t uid_len);
extern bool nfc_is_scan_cancelled(void) __attribute__((weak));
extern bool nfc_is_dict_skip_requested(void) __attribute__((weak));
 
static mfc_progress_cb_t g_prog_cb = NULL;
static void* g_prog_user = NULL;
void mfc_set_progress_callback(mfc_progress_cb_t cb, void* user) { g_prog_cb = cb; g_prog_user = user; }

void mfc_set_attack_hooks(const mfc_attack_hooks_t *hooks) {
    g_attack_hooks = hooks;
}

static inline void crc_a_calc(const uint8_t *data, size_t len, uint8_t out[2]){
    uint16_t crc = 0x6363;
    for (size_t i=0;i<len;i++){
        uint8_t d = data[i];
        for (int b=0;b<8;b++){
            uint8_t mix = (uint8_t)((crc ^ d) & 0x0001);
            crc >>= 1;
            if (mix) crc ^= 0x8408;
            d >>= 1;
        }
    }
    out[0] = (uint8_t)(crc & 0xFF);
    out[1] = (uint8_t)((crc >> 8) & 0xFF);
}
static bool g_backdoor_checked = false;
static bool g_backdoor_enabled = false;
static void mfc_try_backdoor_once(pn532_io_handle_t io){
    if (g_backdoor_checked) {
        return;
    }
    g_backdoor_checked = true;
    if (!io) return;
    uint8_t hlta[4] = {0x50,0x00,0x00,0x00};
    uint8_t crc[2]; crc_a_calc(hlta, 2, crc); hlta[2]=crc[0]; hlta[3]=crc[1];
    uint8_t rb[8]; uint8_t rbl;
    rbl = sizeof(rb);
    (void)pn532_in_communicate_thru(io, hlta, sizeof(hlta), rb, &rbl);
    bool ok40 = false, ok43 = false;
    uint8_t cmd;
    rbl = sizeof(rb); cmd = 0x40; ok40 = (pn532_in_communicate_thru(io, &cmd, 1, rb, &rbl) == ESP_OK);
    rbl = sizeof(rb); cmd = 0x43; ok43 = (pn532_in_communicate_thru(io, &cmd, 1, rb, &rbl) == ESP_OK);
    if (ok40 && ok43) {
        g_backdoor_enabled = true;
        ESP_LOGI(MFC_TAG, "Magic backdoor likely enabled");
    }
    // Important: HLTA halts normal cards; also magic cards typically need WUPA/Select again.
    // Reselect target so subsequent AUTH/READ via InDataExchange works.
    (void)pn532_in_list_passive_target(io);
}

#if defined(CONFIG_HAS_NFC) && defined(CONFIG_MFC_DICT_EMBEDDED)
// Primary embedded blob symbols (strong). We're inside CONFIG_MFC_DICT_EMBEDDED section, so they must exist.
extern const uint8_t _binary_mf_classic_dict_nfc_start[] asm("_binary_mf_classic_dict_nfc_start");
extern const uint8_t _binary_mf_classic_dict_nfc_end[]   asm("_binary_mf_classic_dict_nfc_end");
static int hexn(char c){ if(c>='0'&&c<='9')return c-'0'; c|=0x20; if(c>='a'&&c<='f')return 10+(c-'a'); return -1; }
static bool parse_key_line(const char* s,const char* e,uint8_t out[6]){
    uint8_t b[6]; int bi=0; int hi=-1; for(const char* p=s;p<e && bi<6; ++p){ int v=hexn(*p); if(v<0){ if(*p=='#') return false; else continue; } if(hi<0){ hi=v; } else { b[bi++]=(uint8_t)((hi<<4)|v); hi=-1; } }
    if(bi==6){ for(int i=0;i<6;i++) out[i]=b[i]; return true; } return false;
}
// Cache parsed dictionary keys to avoid reparsing for every sector/key attempt
static uint8_t *g_dict_keys = NULL;   // flat array of 6-byte keys, length = 6 * g_dict_key_count
static int g_dict_key_count = 0;
static uint8_t g_last_key_a[6]; static bool g_last_key_a_valid = false;
static uint8_t g_last_key_b[6]; static bool g_last_key_b_valid = false;
// Optional UI hook (weak). If defined by UI layer, it will be called to indicate the
// current dictionary attempt phase (sector/first-block/key type) and total keys.
extern void mfc_ui_set_phase(int sector, int first_block, bool key_b, int total_keys) __attribute__((weak));
// UI-layer cancel hook (weak). When true, long operations should abort early.
extern bool nfc_is_scan_cancelled(void) __attribute__((weak));
// UI-layer dict-skip hook (weak). When true, skip only dictionary attempts but keep reading flow.
extern bool nfc_is_dict_skip_requested(void) __attribute__((weak));
// Resolve embedded dictionary start/end regardless of exact symbol variant
static inline void dict_blob_bounds(const char **out_start, const char **out_end){
    const char *s = (const char*)_binary_mf_classic_dict_nfc_start;
    const char *e = (const char*)_binary_mf_classic_dict_nfc_end;
    if (e > s) { *out_start = s; *out_end = e; } else { *out_start = NULL; *out_end = NULL; }
}
static int dict_total_keys_cached = -1;
static int mfc_dict_total_keys(void){
    if (dict_total_keys_cached >= 0) return dict_total_keys_cached;
    const char *s, *e; dict_blob_bounds(&s, &e); if(!s||!e||e<=s) { dict_total_keys_cached = 0; return 0; }
    int cnt=0; const char* p=s; uint8_t tmp[6];
    while(p<e){ const char* nl=memchr(p,'\n',(size_t)(e-p)); const char* ln_end = nl? nl : e; if(parse_key_line(p,ln_end,tmp)) cnt++; p = nl? nl+1 : e; }
    dict_total_keys_cached = cnt;
    ESP_LOGI("MFC", "Dict: embedded ok s=%p e=%p total=%d", (void*)s, (void*)e, cnt);
    return cnt;
}
// Load dictionary keys into RAM once (after we have parser and bounds defined)
static void mfc_dict_ensure_loaded(void){
    if (g_dict_keys) return;
    int total = mfc_dict_total_keys();
    if (total <= 0) return;
    const char *s, *e; dict_blob_bounds(&s, &e);
    if (!s || !e || e <= s) return;
    g_dict_keys = (uint8_t*)malloc((size_t)total * 6);
    if (!g_dict_keys) { g_dict_key_count = 0; return; }
    int idx = 0; const char *p = s; uint8_t key[6];
    while (p < e && idx < total) {
        const char* nl = memchr(p,'\n',(size_t)(e-p)); const char* ln_end = nl? nl : e;
        if (parse_key_line(p, ln_end, key)) {
            memcpy(&g_dict_keys[idx*6], key, 6);
            idx++;
        }
        p = nl? nl+1 : e;
    }
    g_dict_key_count = idx;
}
static bool mfc_auth_with_dict(pn532_io_handle_t io,uint8_t block,bool use_key_b,const uint8_t* uid,uint8_t uid_len){
    if (&nfc_is_dict_skip_requested && nfc_is_dict_skip_requested()) {
        ESP_LOGI("MFC", "Dict: skip requested (blk=%u keyB=%d)", (unsigned)block, (int)use_key_b);
        return false;
    }
    const char *s, *e; dict_blob_bounds(&s, &e); if(!s||!e||e<=s){
        ESP_LOGI("MFC", "Dict: blob missing, trying SD file (blk=%u keyB=%d)", (unsigned)block, (int)use_key_b);
        // Fallback: try SD card dictionary at /mnt/ghostesp/nfc/mf_classic_dict.nfc
        FILE *f = fopen("/mnt/ghostesp/nfc/mf_classic_dict.nfc", "r");
        if (!f) { ESP_LOGI("MFC", "Dict syms: s=%p e=%p", (void*)_binary_mf_classic_dict_nfc_start, (void*)_binary_mf_classic_dict_nfc_end); return false; }
        char line[96];
        uint8_t key[6];
        int idx = 0;
        while (fgets(line, sizeof(line), f)) {
            if (mfc_call_should_cancel() || mfc_call_should_skip_dict()) {
                ESP_LOGW("MFC", "Dict(SD): abort at idx=%d blk=%u keyB=%d", idx, (unsigned)block, (int)use_key_b);
                fclose(f);
                return false;
            }
            const char *ls = line; const char *le = line + strlen(line);
            if (parse_key_line(ls, le, key)) {
                idx++;
                if (g_prog_cb) g_prog_cb(idx, 0, g_prog_user);
                if (mfc_auth_block(io, block, use_key_b, key, uid, uid_len) == ESP_OK) {
                    ESP_LOGI("MFC", "Dict(SD): success blk=%u keyB=%d idx=%d key=%02X%02X%02X%02X%02X%02X",
                            (unsigned)block, (int)use_key_b, idx, key[0],key[1],key[2],key[3],key[4],key[5]);
                    if (use_key_b) { memcpy(g_last_key_b, key, 6); g_last_key_b_valid = true; }
                    else { memcpy(g_last_key_a, key, 6); g_last_key_a_valid = true; }
                    mfc_record_working_key(key, use_key_b);
                    fclose(f);
                    return true;
                }
            }
        }
        fclose(f);
        ESP_LOGI("MFC", "Dict(SD): failed blk=%u keyB=%d", (unsigned)block, (int)use_key_b);
        return false;
    }
    // Try last successful key of this type first (fast path)
    if ((&nfc_is_dict_skip_requested && nfc_is_dict_skip_requested())) {
        ESP_LOGI("MFC", "Dict: skip requested before fast-path (blk=%u)", (unsigned)block);
        return false;
    }
    if (!use_key_b && g_last_key_a_valid) {
        if (mfc_auth_block(io, block, false, g_last_key_a, uid, uid_len) == ESP_OK) {
            ESP_LOGI("MFC", "Dict: reused last A key for blk=%u", (unsigned)block);
            mfc_record_working_key(g_last_key_a, false);
            return true;
        }
    } else if (use_key_b && g_last_key_b_valid) {
        if (mfc_auth_block(io, block, true, g_last_key_b, uid, uid_len) == ESP_OK) {
            ESP_LOGI("MFC", "Dict: reused last B key for blk=%u", (unsigned)block);
            mfc_record_working_key(g_last_key_b, true);
            return true;
        }
    }
    // Iterate embedded blob directly without caching into RAM
    {
        const int total = mfc_dict_total_keys();
        ESP_LOGI("MFC", "Dict: start auth blk=%u keyB=%d total=%d", (unsigned)block, (int)use_key_b, total);
        if (g_prog_cb) g_prog_cb(0, total, g_prog_user);
        int idx = 0; int last_cb = 0;
        const char *p = s; uint8_t key[6];
        while (p < e) {
            if (mfc_call_should_cancel() || mfc_call_should_skip_dict()) { ESP_LOGW("MFC", "Dict: cancelled/skip blk=%u keyB=%d at idx=%d", (unsigned)block, (int)use_key_b, idx); return false; }
            const char *nl = memchr(p,'\n',(size_t)(e - p));
            const char *ln_end = nl ? nl : e;
            if (parse_key_line(p, ln_end, key)) {
                if (mfc_auth_block(io, block, use_key_b, key, uid, uid_len) == ESP_OK) {
                    ESP_LOGI("MFC", "Dict: success blk=%u keyB=%d idx=%d key=%02X%02X%02X%02X%02X%02X",
                            (unsigned)block, (int)use_key_b, idx+1, key[0],key[1],key[2],key[3],key[4],key[5]);
                    if (use_key_b) { memcpy(g_last_key_b, key, 6); g_last_key_b_valid = true; }
                    else { memcpy(g_last_key_a, key, 6); g_last_key_a_valid = true; }
                    mfc_record_working_key(key, use_key_b);
                    return true;
                }
                idx++;
                if (g_prog_cb) {
                    int pct = (total > 0) ? ((idx * 100) / total) : 0;
                    if (pct > last_cb) { g_prog_cb(idx, total, g_prog_user); last_cb = pct; }
                }
            }
            p = nl ? (nl + 1) : e;
        }
        ESP_LOGI("MFC", "Dict: failed blk=%u keyB=%d", (unsigned)block, (int)use_key_b);
        return false;
    }
}

 

// Quickly try a just-found key across sectors to snowball coverage.
// Momentum tries a found key as both A and B; doing the same avoids missing
// cards that reuse one value under different key types across sectors.
static void mfc_key_reuse_sweep(pn532_io_handle_t io, MFC_TYPE t, const uint8_t *uid, uint8_t uid_len,
                                bool use_key_b, const uint8_t key[6], int current_sector) {
    if (!io || !uid || !key) return;
    if (mfc_call_should_skip_dict()) return;
    int sectors = mfc_sector_count(t); if (sectors == 0) sectors = 16;
    for (int s = 0; s < sectors; ++s) {
        if (mfc_call_should_skip_dict()) break;
        if (mfc_call_should_cancel()) break;
        for (int pass = 0; pass < 2; ++pass) {
            bool try_key_b = (pass == 0) ? use_key_b : !use_key_b;
            if (s == current_sector && try_key_b == use_key_b) continue;

            // Skip sectors we already have a valid key recorded for this key type.
            if (!try_key_b) {
                if (g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s)) continue;
            } else {
                if (g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s)) continue;
            }

            uint8_t blk = mfc_auth_target_block(t, s);
            if (mfc_auth_block(io, blk, try_key_b, key, uid, uid_len) == ESP_OK) {
                ESP_LOGI("MFC", "Reuse: sector=%d unlocked via %c", s, try_key_b ? 'B' : 'A');
                mfc_cache_record_sector_key(s, try_key_b, key);

                if (!mfc_sector_all_blocks_known(t, s)) {
                    int first = mfc_first_block_of_sector(t, s);
                    int blocks = mfc_blocks_in_sector(t, s);
                    for (int b = 0; b < blocks; ++b) {
                        uint8_t data[16];
                        if (mfc_read_block(io, (uint8_t)(first + b), data) == ESP_OK) {
                            mfc_cache_store_block(first + b, data);
                            if (b == blocks - 1) mfc_harvest_trailer_keys(io, t, s, uid, uid_len, data);
                        }
                    }
                }
            }
        }
        // Sector completed; bump progress
        if (g_prog_cb) g_prog_cb(s + 1, sectors, g_prog_user);
    }
}

// If trailer bytes expose keys (e.g., backdoor or permissive access), verify and record them
static void mfc_harvest_trailer_keys(pn532_io_handle_t io, MFC_TYPE t, int sector,
                                     const uint8_t *uid, uint8_t uid_len, const uint8_t trailer[16]) {
    if (!io || !uid || !trailer) return;
    // Prepare candidate keys from trailer
    uint8_t key_a[6];
    uint8_t key_b[6];
    memcpy(key_a, &trailer[0], 6);
    memcpy(key_b, &trailer[10], 6);
    uint8_t blk = mfc_auth_target_block(t, sector);
    // Try Key A
    if (mfc_auth_block(io, blk, false, key_a, uid, uid_len) == ESP_OK) {
        mfc_record_working_key(key_a, false);
        mfc_cache_record_sector_key(sector, false, key_a);
        ESP_LOGI("MFC", "Harvest: sector=%d Key A verified from trailer", sector);
    }
    // Try Key B
    if (mfc_auth_block(io, blk, true, key_b, uid, uid_len) == ESP_OK) {
        mfc_record_working_key(key_b, true);
        mfc_cache_record_sector_key(sector, true, key_b);
        ESP_LOGI("MFC", "Harvest: sector=%d Key B verified from trailer", sector);
    }
}
// Interleaved A/B dict attempts: A[idx], then B[idx], until success or end
static bool mfc_auth_with_dict_interleaved(pn532_io_handle_t io,
                                           uint8_t block,
                                           const uint8_t* uid,
                                           uint8_t uid_len,
                                           bool* out_used_key_b){
    if (mfc_call_should_skip_dict()) return false;
    mfc_dict_ensure_loaded();
    const int total = g_dict_key_count;
    if (g_prog_cb) g_prog_cb(0, total, g_prog_user);
    for (int idx = 0; idx < total; ++idx) {
        if (mfc_call_should_cancel() || mfc_call_should_skip_dict()) return false;
        const uint8_t *key = &g_dict_keys[idx*6];
        if (mfc_auth_block(io, block, false, key, uid, uid_len) == ESP_OK) {
            if (out_used_key_b) *out_used_key_b = false;
            mfc_record_working_key(key, false);
            // cache last dict key for A
            memcpy(g_last_key_a, key, 6); g_last_key_a_valid = true;
            return true;
        }
        if (mfc_call_should_cancel() || mfc_call_should_skip_dict()) return false;
        if (mfc_auth_block(io, block, true, key, uid, uid_len) == ESP_OK) {
            if (out_used_key_b) *out_used_key_b = true;
            mfc_record_working_key(key, true);
            // cache last dict key for B
            memcpy(g_last_key_b, key, 6); g_last_key_b_valid = true;
            return true;
        }
        if (g_prog_cb) g_prog_cb(idx+1, total, g_prog_user);
    }
    return false;
}
#endif

// ----------------------------
// Session/User dictionary cache
// ----------------------------
static uint8_t *g_user_keys = NULL; static int g_user_key_count = 0; static bool g_user_loaded = false;
static uint8_t *g_sess_a_keys = NULL; static int g_sess_a_count = 0;
static uint8_t *g_sess_b_keys = NULL; static int g_sess_b_count = 0;
static uint8_t s_last_key_a[6]; static bool s_last_key_a_valid = false;
static uint8_t s_last_key_b[6]; static bool s_last_key_b_valid = false;
// Flag to track if user dict is cached for this scan session
static bool g_user_dict_cached_for_scan = false;

// Forward declaration for cleanup function
static void mfc_user_dict_cleanup(void);

static bool key_eq_u(const uint8_t *a, const uint8_t *b){ return memcmp(a,b,6)==0; }
static bool list_contains_u(const uint8_t *list, int count, const uint8_t key[6]){
    for (int i=0;i<count;i++){ if (key_eq_u(&list[i*6], key)) return true; } return false;
}
static bool list_append_unique_u(uint8_t **list, int *count, const uint8_t key[6]){
    if (list_contains_u(*list, *count, key)) return false;
    uint8_t *n = (uint8_t*)realloc(*list, (size_t)(*count + 1) * 6);
    if (!n) return false;
    *list = n; memcpy(&(*list)[(*count)*6], key, 6); (*count)++; return true;
}
// Cleanup user dictionary cache (should be called when module is deinitialized)
static void mfc_user_dict_cleanup(void) {
    ESP_LOGI("MFC", "User dict: cleanup - freeing cached dictionary");
    if (g_user_keys) { free(g_user_keys); g_user_keys = NULL; }
    if (g_sess_a_keys) { free(g_sess_a_keys); g_sess_a_keys = NULL; }
    if (g_sess_b_keys) { free(g_sess_b_keys); g_sess_b_keys = NULL; }
    g_user_key_count = 0;
    g_sess_a_count = 0;
    g_sess_b_count = 0;
    g_user_loaded = false;
    g_user_dict_cached_for_scan = false;
    s_last_key_a_valid = false;
    s_last_key_b_valid = false;
}

// Reset session state and force user dictionary reload for new scan
static void mfc_session_reset(void) {
    ESP_LOGI("MFC", "Session reset: clearing session state for new scan");
    
    // Clear session keys
    s_last_key_a_valid = false;
    s_last_key_b_valid = false;
    memset(s_last_key_a, 0, 6);
    memset(s_last_key_b, 0, 6);
    
    // Clear session key lists
    if (g_sess_a_keys) { free(g_sess_a_keys); g_sess_a_keys = NULL; }
    if (g_sess_b_keys) { free(g_sess_b_keys); g_sess_b_keys = NULL; }
    g_sess_a_count = 0;
    g_sess_b_count = 0;
    
    // Mark user dict as not cached for this scan (will force reload)
    g_user_dict_cached_for_scan = false;
    g_user_loaded = false;
}

static void mfc_user_dict_ensure_loaded(void){
    // Only reload if not already cached for this scan session
    if (g_user_dict_cached_for_scan && g_user_loaded && g_user_keys) {
        ESP_LOGD("MFC", "User dict: using cached version (%d keys)", g_user_key_count);
        return;
    }

    bool mounted_here = false;
    bool display_was_suspended = false;
    bool needs_jit_mount = false;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        needs_jit_mount = true;
    }
#endif
    if (needs_jit_mount && !sd_card_manager.is_initialized) {
        if (sd_card_mount_for_flush(&display_was_suspended) == ESP_OK) {
            mounted_here = true;
        } else {
            ESP_LOGW("MFC", "User dict: failed to mount storage; using empty dictionary this scan");
            g_user_loaded = true;
            g_user_dict_cached_for_scan = true;
            return;
        }
    }

    // Clear existing cache
    if (g_user_keys) { free(g_user_keys); g_user_keys = NULL; }
    g_user_key_count = 0;

    ESP_LOGI("MFC", "User dict: loading from SD card");
    if (sd_card_manager.is_initialized) {
        (void)sd_card_create_directory("/mnt/ghostesp/nfc");
    }

    FILE *f = fopen("/mnt/ghostesp/nfc/mfc_user_dict.nfc", "r");
    if (!f) {
        ESP_LOGW("MFC", "User dict: file not found, creating empty dictionary");
        if (sd_card_manager.is_initialized) {
            FILE *cf = fopen("/mnt/ghostesp/nfc/mfc_user_dict.nfc", "a");
            if (cf) fclose(cf);
        }
        g_user_loaded = true;
        g_user_dict_cached_for_scan = true;
        if (mounted_here) sd_card_unmount_after_flush(display_was_suspended);
        return;
    }

    char line[96]; uint8_t key[6];
    int raw_lines = 0;
    int parsed_lines = 0;
    int duplicate_lines = 0;
    int rejected_lines = 0;
    while (fgets(line, sizeof(line), f)){
        raw_lines++;
        const char *ls = line; const char *le = line + strlen(line);
        if (parse_key_line_u(ls, le, key)) {
            parsed_lines++;
            if (list_contains_u(g_user_keys, g_user_key_count, key)) {
                duplicate_lines++;
                continue;
            }
            if (!list_append_unique_u(&g_user_keys, &g_user_key_count, key)) {
                ESP_LOGW("MFC", "User dict: failed to add key (OOM?)");
                break;
            }
        } else {
            const char *p = line;
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
            if (*p && *p != '#') {
                rejected_lines++;
                ESP_LOGW("MFC", "User dict: rejected line %d: %.80s", raw_lines, line);
            }
        }
    }
    fclose(f);

    g_user_loaded = true;
    g_user_dict_cached_for_scan = true;
    ESP_LOGI(
        "MFC",
        "User dict: loaded %d unique keys from %d lines (parsed=%d duplicate=%d rejected=%d)",
        g_user_key_count,
        raw_lines,
        parsed_lines,
        duplicate_lines,
        rejected_lines);
    if (mounted_here) sd_card_unmount_after_flush(display_was_suspended);
}

// Force reload user dictionary (for external changes detection)
static void mfc_user_dict_force_reload(void) {
    ESP_LOGI("MFC", "User dict: forcing fresh reload from SD card");
    // Free existing cache first
    if (g_user_keys) { free(g_user_keys); g_user_keys = NULL; }
    g_user_key_count = 0;
    g_user_dict_cached_for_scan = false;
    g_user_loaded = false;
    mfc_user_dict_ensure_loaded();
}
// Batched user-dict persistence. A brute-force finds many working keys; writing
// each one immediately costs a full JIT mount/unmount (display suspend + SPI bus
// reinit) per key on bus-sharing boards. In batch mode new keys are only cached
// in RAM (g_user_keys), and mfc_user_dict_end_batch() persists them all in a
// single mount.
static bool s_user_dict_batch = false;
static int s_user_dict_batch_start = 0;

void mfc_user_dict_begin_batch(void) {
    mfc_user_dict_ensure_loaded();
    s_user_dict_batch = true;
    s_user_dict_batch_start = g_user_key_count;  // keys beyond this are new
}

void mfc_user_dict_end_batch(void) {
    if (!s_user_dict_batch) return;
    s_user_dict_batch = false;
    if (g_user_key_count <= s_user_dict_batch_start) return;  // nothing new

    bool susp = false;
    if (!sd_card_jit_begin(&susp, true)) {
        ESP_LOGW("MFC", "User dict: batch flush failed to mount storage");
        return;
    }
    FILE *f = fopen("/mnt/ghostesp/nfc/mfc_user_dict.nfc", "a");
    if (f) {
        for (int i = s_user_dict_batch_start; i < g_user_key_count; i++) {
            const uint8_t *k = &g_user_keys[i * 6];
            fprintf(f, "%02X%02X%02X%02X%02X%02X\n", k[0], k[1], k[2], k[3], k[4], k[5]);
        }
        fclose(f);
        ESP_LOGI("MFC", "User dict: batch-persisted %d new keys",
                 g_user_key_count - s_user_dict_batch_start);
    } else {
        ESP_LOGW("MFC", "User dict: batch flush failed to open file");
    }
    sd_card_jit_end(susp);
}

static void mfc_user_dict_append_unique(const uint8_t key[6]){
    mfc_user_dict_ensure_loaded();
    if (list_contains_u(g_user_keys, g_user_key_count, key)) return;

    if (s_user_dict_batch) {
        /* Defer the SD write; end_batch() persists all new keys in one mount. */
        if (!list_append_unique_u(&g_user_keys, &g_user_key_count, key)) {
            ESP_LOGW("MFC", "User dict: failed to add new key to cache (OOM?)");
        }
        return;
    }

    bool mounted_here = false;
    bool display_was_suspended = false;
    bool needs_jit_mount = false;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        needs_jit_mount = true;
    }
#endif
    if (needs_jit_mount && !sd_card_manager.is_initialized) {
        if (sd_card_mount_for_flush(&display_was_suspended) == ESP_OK) {
            mounted_here = true;
        } else {
            ESP_LOGW("MFC", "User dict: append skipped, failed to mount storage");
            return;
        }
    }

    // Append to SD card file
    if (sd_card_manager.is_initialized) {
        sd_card_create_directory("/mnt/ghostesp/nfc");
    }
    FILE *f = fopen("/mnt/ghostesp/nfc/mfc_user_dict.nfc", "a");
    if (!f) {
        ESP_LOGW("MFC", "User dict: failed to open file for append");
        if (mounted_here) sd_card_unmount_after_flush(display_was_suspended);
        return;
    }
    fprintf(f, "%02X%02X%02X%02X%02X%02X\n", key[0],key[1],key[2],key[3],key[4],key[5]);
    fclose(f);

    // Add to cached dictionary for this session
    if (!list_append_unique_u(&g_user_keys, &g_user_key_count, key)) {
        ESP_LOGW("MFC", "User dict: failed to add new key to cache (OOM?)");
    } else {
        ESP_LOGI("MFC", "User dict: added new key to cache (now %d keys)", g_user_key_count);
    }

    if (mounted_here) sd_card_unmount_after_flush(display_was_suspended);
}

// Public: add a manually-entered key to the user dictionary. Accepts a 12
// hex-digit string (spaces and ':' separators tolerated), e.g. "A0A1A2A3A4A5"
// or "A0 A1 A2 A3 A4 A5". Persists to the user dict file and this session's
// cache. Returns false if the string isn't exactly 6 valid bytes.
bool mfc_add_user_key_hex(const char *hex) {
    if (!hex) return false;
    uint8_t key[6];
    int nib = 0;
    uint8_t cur = 0;
    for (const char *p = hex; *p; ++p) {
        if (*p == ' ' || *p == ':') continue;  // tolerate separators
        int v;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        else return false;             // invalid hex character
        if (nib >= 12) return false;   // more than 6 bytes
        if ((nib & 1) == 0) cur = (uint8_t)(v << 4);
        else key[nib / 2] = (uint8_t)(cur | v);
        nib++;
    }
    if (nib != 12) return false;       // need exactly 6 bytes
    mfc_user_dict_append_unique(key);
    return true;
}

static void session_add_key(bool use_key_b, const uint8_t key[6]){
    if (use_key_b) list_append_unique_u(&g_sess_b_keys, &g_sess_b_count, key);
    else list_append_unique_u(&g_sess_a_keys, &g_sess_a_count, key);
}
static void mfc_record_working_key(const uint8_t key[6], bool use_key_b){
    // update last-known and session caches
    if (use_key_b) { memcpy(s_last_key_b, key, 6); s_last_key_b_valid = true; }
    else { memcpy(s_last_key_a, key, 6); s_last_key_a_valid = true; }
    session_add_key(use_key_b, key);
    mfc_user_dict_append_unique(key);
}
static bool try_list_keys_u(pn532_io_handle_t io, uint8_t block, bool use_key_b,
                            const uint8_t *uid, uint8_t uid_len,
                            const uint8_t *keys, int count, const uint8_t **ok_key){
    for (int i=0;i<count;i++){
        if (mfc_call_should_cancel() || mfc_call_should_skip_dict()) {
            return false;
        }
        const uint8_t *k = &keys[i*6];
        if (mfc_auth_block(io, block, use_key_b, k, uid, uid_len) == ESP_OK){ if (ok_key) *ok_key = k; return true; }
        if (g_prog_cb) g_prog_cb(i+1, count, g_prog_user);
    }
    return false;
}
static bool mfc_auth_with_known(pn532_io_handle_t io, uint8_t block, bool use_key_b,
                                const uint8_t *uid, uint8_t uid_len){
    const uint8_t *ok = NULL;
    // Last successful of this type first
    if (!use_key_b && s_last_key_a_valid){ if (mfc_auth_block(io, block, false, s_last_key_a, uid, uid_len) == ESP_OK){ mfc_record_working_key(s_last_key_a, false); return true; } }
    if (use_key_b && s_last_key_b_valid){ if (mfc_auth_block(io, block, true, s_last_key_b, uid, uid_len) == ESP_OK){ mfc_record_working_key(s_last_key_b, true); return true; } }
    // Session keys of this type
    if (!use_key_b){ if (try_list_keys_u(io, block, false, uid, uid_len, g_sess_a_keys, g_sess_a_count, &ok)){ mfc_record_working_key(ok, false); return true; } }
    else { if (try_list_keys_u(io, block, true, uid, uid_len, g_sess_b_keys, g_sess_b_count, &ok)){ mfc_record_working_key(ok, true); return true; } }
    return false;
}

static bool mfc_auth_with_user_interleaved(pn532_io_handle_t io, uint8_t block,
                                           const uint8_t *uid, uint8_t uid_len,
                                           bool *out_used_key_b, const uint8_t **out_key){
    mfc_user_dict_ensure_loaded();
    const int total = (g_user_key_count > 0) ? (g_user_key_count * 2) : 1;
    int step = 0;
    if (g_prog_cb) g_prog_cb(0, total, g_prog_user);
    bool have_key_a = false;
    bool have_key_b = false;
    bool found_any = false;
    bool first_used_b = false;
    const uint8_t *first_key = NULL;
    // Debug: entering user dict for this block
    ESP_LOGI("MFC", "User dict: entering for blk=%u with %d keys (cached=%d)", 
             (unsigned)block, g_user_key_count, g_user_dict_cached_for_scan ? 1 : 0);
    for (int i = 0; i < g_user_key_count; ++i) {
        if (mfc_call_should_cancel() || mfc_call_should_skip_dict()) break;
        const uint8_t *k = &g_user_keys[i*6];
        // Attempt Key A unless we already have one
        if (!have_key_a) {
            if (block == 1) {
                ESP_LOGD("MFC", "User dict: try blk=%u A idx=%d key=%02X%02X%02X%02X%02X%02X",
                        (unsigned)block, i+1, k[0],k[1],k[2],k[3],k[4],k[5]);
            }
            esp_err_t erra = mfc_auth_block(io, block, false, k, uid, uid_len);
            if (erra == ESP_OK) {
                ESP_LOGI("MFC", "User dict: success blk=%u key=A idx=%d key=%02X%02X%02X%02X%02X%02X",
                        (unsigned)block, i+1, k[0],k[1],k[2],k[3],k[4],k[5]);
                have_key_a = true;
                if (!found_any) {
                    found_any = true;
                    first_used_b = false;
                    first_key = k;
                }
                mfc_record_working_key(k, false);
                if (out_used_key_b) *out_used_key_b = false;
                if (out_key) *out_key = k;
                return true;
            } else if (block == 1) {
                ESP_LOGD("MFC", "User dict: A fail blk=%u idx=%d err=%d", (unsigned)block, i+1, (int)erra);
            }
        }
        if (g_prog_cb) g_prog_cb(++step, total, g_prog_user);
        if (mfc_call_should_cancel() || mfc_call_should_skip_dict()) break;
        // Attempt Key B unless we already have one
        if (!have_key_b) {
            if (block == 1) {
                ESP_LOGD("MFC", "User dict: try blk=%u B idx=%d key=%02X%02X%02X%02X%02X%02X",
                        (unsigned)block, i+1, k[0],k[1],k[2],k[3],k[4],k[5]);
            }
            esp_err_t errb = mfc_auth_block(io, block, true, k, uid, uid_len);
            if (errb == ESP_OK) {
                ESP_LOGI("MFC", "User dict: success blk=%u key=B idx=%d key=%02X%02X%02X%02X%02X%02X",
                        (unsigned)block, i+1, k[0],k[1],k[2],k[3],k[4],k[5]);
                have_key_b = true;
                if (!found_any) {
                    found_any = true;
                    first_used_b = true;
                    first_key = k;
                }
                mfc_record_working_key(k, true);
                if (out_used_key_b) *out_used_key_b = true;
                if (out_key) *out_key = k;
                return true;
            } else if (block == 1) {
                ESP_LOGD("MFC", "User dict: B fail blk=%u idx=%d err=%d", (unsigned)block, i+1, (int)errb);
            }
        }
        if (g_prog_cb) g_prog_cb(++step, total, g_prog_user);
    }
    ESP_LOGI("MFC", "User dict: no match blk=%u after %d keys", (unsigned)block, g_user_key_count);
    if (found_any) {
        if (out_used_key_b) *out_used_key_b = first_used_b;
        if (out_key) *out_key = first_key;
    }
    return found_any;
}

// Attempt to discover the complementary key type for this sector using only the user dictionary
static void mfc_try_find_complementary_user_key(pn532_io_handle_t io, MFC_TYPE t, int sector, uint8_t auth_blk,
                                                const uint8_t *uid, uint8_t uid_len, bool usedB_cur) {
    (void)t;
    if (mfc_call_should_skip_dict()) return;
    int first = mfc_first_block_of_sector(t, sector);
    int blocks = mfc_blocks_in_sector(t, sector);
    uint8_t trailer_blk = (uint8_t)(first + blocks - 1);
    // 1) quick: try session keys of complementary type
    if (!usedB_cur) {
        // have A, want B
        if (s_last_key_b_valid && (mfc_auth_block(io, auth_blk, true, s_last_key_b, uid, uid_len) == ESP_OK
            || mfc_auth_block(io, trailer_blk, true, s_last_key_b, uid, uid_len) == ESP_OK)) {
            mfc_record_working_key(s_last_key_b, true);
            mfc_cache_record_sector_key(sector, true, s_last_key_b);
            mfc_key_reuse_sweep(io, t, uid, uid_len, true, s_last_key_b, sector);
            ESP_LOGI(MFC_TAG, "CompKey: sector=%d found B via session-last", sector);
            return;
        }
        const uint8_t *okb = NULL;
        if (g_sess_b_count > 0 && (try_list_keys_u(io, auth_blk, true, uid, uid_len, g_sess_b_keys, g_sess_b_count, &okb)
            || try_list_keys_u(io, trailer_blk, true, uid, uid_len, g_sess_b_keys, g_sess_b_count, &okb))) {
            mfc_record_working_key(okb, true);
            mfc_cache_record_sector_key(sector, true, okb);
            mfc_key_reuse_sweep(io, t, uid, uid_len, true, okb, sector);
            ESP_LOGI(MFC_TAG, "CompKey: sector=%d found B via session list", sector);
            return;
        }
    } else {
        // have B, want A
        if (s_last_key_a_valid && (mfc_auth_block(io, auth_blk, false, s_last_key_a, uid, uid_len) == ESP_OK
            || mfc_auth_block(io, trailer_blk, false, s_last_key_a, uid, uid_len) == ESP_OK)) {
            mfc_record_working_key(s_last_key_a, false);
            mfc_cache_record_sector_key(sector, false, s_last_key_a);
            mfc_key_reuse_sweep(io, t, uid, uid_len, false, s_last_key_a, sector);
            ESP_LOGI(MFC_TAG, "CompKey: sector=%d found A via session-last", sector);
            return;
        }
        const uint8_t *oka = NULL;
        if (g_sess_a_count > 0 && (try_list_keys_u(io, auth_blk, false, uid, uid_len, g_sess_a_keys, g_sess_a_count, &oka)
            || try_list_keys_u(io, trailer_blk, false, uid, uid_len, g_sess_a_keys, g_sess_a_count, &oka))) {
            mfc_record_working_key(oka, false);
            mfc_cache_record_sector_key(sector, false, oka);
            mfc_key_reuse_sweep(io, t, uid, uid_len, false, oka, sector);
            ESP_LOGI(MFC_TAG, "CompKey: sector=%d found A via session list", sector);
            return;
        }
    }

    // 2) user dictionary (respects skip flag)
    if (!mfc_call_should_skip_dict() && g_user_key_count > 0 && g_user_keys) {
        for (int i = 0; i < g_user_key_count; ++i) {
            const uint8_t *k = &g_user_keys[i * 6];
            if (!usedB_cur) {
                if (mfc_auth_block(io, auth_blk, true, k, uid, uid_len) == ESP_OK
                    || mfc_auth_block(io, trailer_blk, true, k, uid, uid_len) == ESP_OK) {
                    mfc_record_working_key(k, true);
                    mfc_cache_record_sector_key(sector, true, k);
                    mfc_key_reuse_sweep(io, t, uid, uid_len, true, k, sector);
                    ESP_LOGI(MFC_TAG, "CompKey: sector=%d found B via user dict idx=%d", sector, i + 1);
                    return;
                }
            } else {
                if (mfc_auth_block(io, auth_blk, false, k, uid, uid_len) == ESP_OK
                    || mfc_auth_block(io, trailer_blk, false, k, uid, uid_len) == ESP_OK) {
                    mfc_record_working_key(k, false);
                    mfc_cache_record_sector_key(sector, false, k);
                    mfc_key_reuse_sweep(io, t, uid, uid_len, false, k, sector);
                    ESP_LOGI(MFC_TAG, "CompKey: sector=%d found A via user dict idx=%d", sector, i + 1);
                    return;
                }
            }
        }
    }

    // 3) defaults fallback for complementary type
    for (int k = 0; k < (int)(sizeof(DEFAULT_KEYS) / 6); ++k) {
        const uint8_t *dk = DEFAULT_KEYS[k];
        if (!usedB_cur) {
            if (mfc_auth_block(io, auth_blk, true, dk, uid, uid_len) == ESP_OK
                || mfc_auth_block(io, trailer_blk, true, dk, uid, uid_len) == ESP_OK) {
                mfc_record_working_key(dk, true);
                mfc_cache_record_sector_key(sector, true, dk);
                mfc_key_reuse_sweep(io, t, uid, uid_len, true, dk, sector);
                ESP_LOGI(MFC_TAG, "CompKey: sector=%d found B via defaults idx=%d", sector, k);
                return;
            }
        } else {
            if (mfc_auth_block(io, auth_blk, false, dk, uid, uid_len) == ESP_OK
                || mfc_auth_block(io, trailer_blk, false, dk, uid, uid_len) == ESP_OK) {
                mfc_record_working_key(dk, false);
                mfc_cache_record_sector_key(sector, false, dk);
                mfc_key_reuse_sweep(io, t, uid, uid_len, false, dk, sector);
                ESP_LOGI(MFC_TAG, "CompKey: sector=%d found A via defaults idx=%d", sector, k);
                return;
            }
        }
    }
}

#if defined(CONFIG_NFC_ST25R3916)
#define MFC_NESTED_LOCAL_SAMPLES 8
#define MFC_NESTED_LOCAL_WEAK_SAMPLES 2

typedef struct {
    uint32_t nt_enc;
    uint8_t par;
} mfc_nested_sample_t;

static inline uint8_t mfc_even_parity32(uint32_t x) {
    x ^= x >> 16;
    x ^= x >> 8;
    x ^= x >> 4;
    x ^= x >> 2;
    x ^= x >> 1;
    return (uint8_t)(x & 1u);
}

static inline uint8_t mfc_even_parity8(uint8_t x) {
    return mfc_even_parity32(x);
}

static uint8_t mfc_crypto1_rollback_bit(crypto1_t *c, uint32_t in, int encrypted) {
    uint32_t t;
    uint32_t out;
    uint8_t ret;

    c->odd &= 0xffffffu;
    t = c->odd;
    c->odd = c->even;
    c->even = t;

    out = c->even & 1u;
    out ^= 0x870804u & (c->even >>= 1);
    out ^= 0x29CE5Cu & c->odd;
    out ^= !!in;
    ret = crypto1_filter(c->odd);
    out ^= ret & (uint8_t)(!!encrypted);

    c->even |= (uint32_t)mfc_even_parity32(out) << 23;
    return ret;
}

static uint32_t mfc_crypto1_rollback_word(crypto1_t *c, uint32_t in, int encrypted) {
    uint32_t ret = 0;
    for (int i = 31; i >= 0; --i) {
        ret |= (uint32_t)mfc_crypto1_rollback_bit(c, (in >> (i ^ 24)) & 1u, encrypted) << (24 ^ i);
    }
    return ret;
}

static uint64_t mfc_key_to_u64(const uint8_t key[6]) {
    uint64_t out = 0;
    for (int i = 0; i < 6; ++i) out = (out << 8) | key[i];
    return out;
}

static uint32_t mfc_nested_decrypt_nt_enc(uint32_t cuid, uint32_t nt_enc, const uint8_t key[6]) {
    crypto1_t c;
    crypto1_init(&c, mfc_key_to_u64(key));
    crypto1_word(&c, nt_enc ^ cuid, 1);
    return nt_enc ^ mfc_crypto1_rollback_word(&c, nt_enc ^ cuid, 1);
}

static bool mfc_nested_nonce_matches_parity(uint32_t nt, uint32_t ks, uint8_t par) {
    return (mfc_even_parity8((uint8_t)(nt >> 24)) == (((par >> 3) & 1u) ^ ((ks >> 16) & 1u))) &&
           (mfc_even_parity8((uint8_t)(nt >> 16)) == (((par >> 2) & 1u) ^ ((ks >> 8) & 1u))) &&
           (mfc_even_parity8((uint8_t)(nt >> 8)) == (((par >> 1) & 1u) ^ (ks & 1u)));
}

static bool mfc_nested_candidate_matches(const mfc_nested_sample_t *samples, int sample_count,
                                         uint32_t cuid, const uint8_t key[6], bool require_weak_prng) {
    if (!samples || sample_count <= 0 || !key) return false;
    for (int i = 0; i < sample_count; ++i) {
        uint32_t nt = mfc_nested_decrypt_nt_enc(cuid, samples[i].nt_enc, key);
        uint32_t ks = nt ^ samples[i].nt_enc;
        if (require_weak_prng && !mfc_is_weak_prng_nonce(nt)) return false;
        if (!mfc_nested_nonce_matches_parity(nt, ks, samples[i].par)) return false;
    }
    return true;
}

static bool mfc_nested_search_key_list(pn532_io_handle_t io, uint8_t target_block, bool target_key_b,
                                       const uint8_t *uid, uint8_t uid_len,
                                       const mfc_nested_sample_t *samples, int sample_count,
                                       uint32_t cuid, bool require_weak_prng,
                                       const uint8_t *keys, int key_count, uint8_t out_key[6]) {
    if (!keys || key_count <= 0) return false;
    for (int i = 0; i < key_count; ++i) {
        if (mfc_call_should_cancel() || mfc_call_should_skip_dict()) return false;
        const uint8_t *key = &keys[i * 6];
        if (mfc_nested_candidate_matches(samples, sample_count, cuid, key, require_weak_prng)) {
            if (mfc_auth_block(io, target_block, target_key_b, key, uid, uid_len) == ESP_OK) {
                memcpy(out_key, key, 6);
                return true;
            }
            ESP_LOGD(MFC_TAG, "Nested: local candidate failed RF verify idx=%d", i + 1);
        }
        if (g_prog_cb) g_prog_cb(i + 1, key_count, g_prog_user);
    }
    return false;
}

static bool mfc_nested_find_anchor(MFC_TYPE t, uint8_t *known_block, bool *known_key_b, uint8_t known_key[6]) {
    int sectors = mfc_sector_count(t);
    if (sectors <= 0) return false;
    for (int s = 0; s < sectors; ++s) {
        if (g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s)) {
            *known_block = mfc_auth_target_block(t, s);
            *known_key_b = false;
            memcpy(known_key, &g_sector_key_a[s * 6], 6);
            return true;
        }
        if (g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s)) {
            *known_block = mfc_auth_target_block(t, s);
            *known_key_b = true;
            memcpy(known_key, &g_sector_key_b[s * 6], 6);
            return true;
        }
    }
    return false;
}

static int mfc_nested_collect_samples(pn532_io_handle_t io, const uint8_t *uid, uint8_t uid_len,
                                      uint8_t known_block, bool known_key_b, const uint8_t known_key[6],
                                      uint8_t target_block, bool target_key_b,
                                      int wanted_samples,
                                      mfc_nested_sample_t samples[MFC_NESTED_LOCAL_SAMPLES]) {
    int got = 0;
    if (wanted_samples <= 0 || wanted_samples > MFC_NESTED_LOCAL_SAMPLES) wanted_samples = MFC_NESTED_LOCAL_SAMPLES;
    for (int i = 0; i < wanted_samples; ++i) {
        if (mfc_call_should_cancel() || mfc_call_should_skip_dict()) break;
        (void)pn532_in_list_passive_target(io);
        if (mfc_auth_block(io, known_block, known_key_b, known_key, uid, uid_len) != ESP_OK) {
            continue;
        }

        uint8_t cmd[2] = { target_key_b ? MIFARE_CMD_AUTH_B : MIFARE_CMD_AUTH_A, target_block };
        uint8_t resp[16] = {0};
        uint8_t resp_len = sizeof(resp);
        esp_err_t err = pn532_in_data_exchange(io, cmd, sizeof(cmd), resp, &resp_len);
        if (err != ESP_OK || resp_len < 8) {
            ESP_LOGD(MFC_TAG, "Nested: sample failed idx=%d err=%d len=%u", i, (int)err, (unsigned)resp_len);
            continue;
        }
        samples[got].nt_enc = ((uint32_t)resp[0] << 24) | ((uint32_t)resp[1] << 16) |
                              ((uint32_t)resp[2] << 8) | resp[3];
        uint8_t raw_par = (uint8_t)(((resp[4] & 1) << 3) | ((resp[5] & 1) << 2) |
                                    ((resp[6] & 1) << 1) | (resp[7] & 1));
        samples[got].par = (uint8_t)(raw_par ^ 0x0F);  // Momentum dict matching uses encrypted parity bits.
        got++;
        if (g_prog_cb) g_prog_cb(got, wanted_samples, g_prog_user);
    }
    (void)pn532_in_list_passive_target(io);
    return got;
}

static bool mfc_try_nested_dict_recovery(pn532_io_handle_t io, MFC_TYPE t, int sector,
                                         bool target_key_b, const uint8_t *uid, uint8_t uid_len,
                                         uint8_t out_key[6]) {
    if (!io || !uid || uid_len < 4 || !out_key) return false;
    if (mfc_call_should_cancel() || mfc_call_should_skip_dict()) return false;
    if (target_key_b) {
        if (g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, sector)) return false;
    } else {
        if (g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, sector)) return false;
    }

    uint8_t known_block = 0;
    bool known_key_b = false;
    uint8_t known_key[6] = {0};
    if (!mfc_nested_find_anchor(t, &known_block, &known_key_b, known_key)) return false;

    uint8_t target_block = (uint8_t)(mfc_first_block_of_sector(t, sector) + mfc_blocks_in_sector(t, sector) - 1);
    mfc_nested_sample_t samples[MFC_NESTED_LOCAL_SAMPLES] = {0};
    int sample_goal = g_prng_weak ? MFC_NESTED_LOCAL_WEAK_SAMPLES : MFC_NESTED_LOCAL_SAMPLES;
    int sample_min = g_prng_weak ? 1 : MFC_NESTED_LOCAL_SAMPLES;
    ESP_LOGI(MFC_TAG, "Nested: collect sector=%d key=%c via anchor blk=%u key=%c prng=%s samples=%d",
             sector, target_key_b ? 'B' : 'A', known_block, known_key_b ? 'B' : 'A',
             g_prng_weak ? "weak" : "hard", sample_goal);
    int sample_count = mfc_nested_collect_samples(io, uid, uid_len, known_block, known_key_b, known_key,
                                                  target_block, target_key_b, sample_goal, samples);
    if (sample_count < sample_min) {
        ESP_LOGI(MFC_TAG, "Nested: insufficient samples sector=%d key=%c got=%d",
                 sector, target_key_b ? 'B' : 'A', sample_count);
        return false;
    }

    uint32_t cuid = mfc_cuid_from_uid(uid, uid_len);
    mfc_user_dict_ensure_loaded();
    if (g_prog_cb) g_prog_cb(0, g_user_key_count > 0 ? g_user_key_count : 1, g_prog_user);
    if (mfc_nested_search_key_list(io, target_block, target_key_b, uid, uid_len,
                                   samples, sample_count, cuid, g_prng_weak,
                                   g_user_keys, g_user_key_count, out_key)) {
        ESP_LOGI(MFC_TAG, "Nested: verified from user dict sector=%d key=%c", sector, target_key_b ? 'B' : 'A');
        goto accept;
    }

#if defined(CONFIG_HAS_NFC) && defined(CONFIG_MFC_DICT_EMBEDDED)
    mfc_dict_ensure_loaded();
    if (g_prog_cb) g_prog_cb(0, g_dict_key_count > 0 ? g_dict_key_count : 1, g_prog_user);
    if (mfc_nested_search_key_list(io, target_block, target_key_b, uid, uid_len,
                                   samples, sample_count, cuid, g_prng_weak,
                                   g_dict_keys, g_dict_key_count, out_key)) {
        ESP_LOGI(MFC_TAG, "Nested: verified from embedded dict sector=%d key=%c", sector, target_key_b ? 'B' : 'A');
        goto accept;
    }
#endif

    if (mfc_nested_search_key_list(io, target_block, target_key_b, uid, uid_len,
                                   samples, sample_count, cuid, g_prng_weak, &DEFAULT_KEYS[0][0],
                                   (int)(sizeof(DEFAULT_KEYS) / sizeof(DEFAULT_KEYS[0])), out_key)) {
        ESP_LOGI(MFC_TAG, "Nested: verified from defaults sector=%d key=%c", sector, target_key_b ? 'B' : 'A');
        goto accept;
    }
    ESP_LOGI(MFC_TAG, "Nested: no local dict candidate sector=%d key=%c", sector, target_key_b ? 'B' : 'A');
    return false;

accept:
    mfc_record_working_key(out_key, target_key_b);
    mfc_cache_record_sector_key(sector, target_key_b, out_key);
    ESP_LOGI(MFC_TAG, "Nested: accepted sector=%d key=%c %02X%02X%02X%02X%02X%02X",
             sector, target_key_b ? 'B' : 'A', out_key[0], out_key[1], out_key[2],
             out_key[3], out_key[4], out_key[5]);
    return true;
}
#else
static bool mfc_try_nested_dict_recovery(pn532_io_handle_t io, MFC_TYPE t, int sector,
                                         bool target_key_b, const uint8_t *uid, uint8_t uid_len,
                                         uint8_t out_key[6]) {
    (void)io; (void)t; (void)sector; (void)target_key_b; (void)uid; (void)uid_len; (void)out_key;
    return false;
}
#endif


bool mfc_is_classic_sak(uint8_t sak) {
    return (sak == 0x08) || (sak == 0x18) || (sak == 0x09);
}

// UI hooks provided by the view (declared here to avoid header coupling)
void mfc_ui_set_phase(int sector, int first_block, bool key_b, int total_keys);
void mfc_ui_set_paused(bool on);
void mfc_ui_set_cache_mode(bool on);

bool mfc_save_flipper_file(pn532_io_handle_t io,
                           const uint8_t* uid,
                           uint8_t uid_len,
                           uint16_t atqa,
                           uint8_t sak,
                           const char* out_dir,
                           char* out_path,
                           size_t out_path_len) {
    mfc_enable_debug_once();
    if (!uid || uid_len == 0 || !out_dir) return false;
    sd_card_create_directory(out_dir);

    // Build filename: Classic<Type>_<UID>.nfc
    char uid_part[40] = {0};
    int up = 0;
    for (uint8_t i = 0; i < uid_len && up < (int)sizeof(uid_part) - 3; ++i) {
        up += snprintf(uid_part + up, sizeof(uid_part) - up, "%02X", uid[i]);
        if (i + 1 < uid_len) up += snprintf(uid_part + up, sizeof(uid_part) - up, "-");
    }
    char path[192];
    const char *mtype = "Classic";
    switch (mfc_type_from_sak(sak)) {
        case MFC_MINI: mtype = "ClassicMini"; break;
        case MFC_1K:   mtype = "Classic1K"; break;
        case MFC_4K:   mtype = "Classic4K"; break;
        default:       mtype = "Classic"; break;
    }
    snprintf(path, sizeof(path), "%s/%s_%s.nfc", out_dir, mtype, uid_part);
    if (out_path && out_path_len) snprintf(out_path, out_path_len, "%s", path);

    MFC_TYPE t = mfc_type_from_sak(sak);
    int sectors = mfc_sector_count(t);
    if (sectors == 0) sectors = 16; // fallback for unknown

    if (io == NULL && (!mfc_cache_matches(uid, uid_len) || !mfc_cache_all_blocks_known())) {
        ESP_LOGW("MFC", "Offline cache incomplete; live save required");
        return false;
    }

    // Header
    char buf[256]; int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Filetype: Flipper NFC device\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Version: 4\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Device type: Mifare Classic\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "UID:");
    for (uint8_t i = 0; i < uid_len && pos < (int)sizeof(buf) - 4; ++i)
        pos += snprintf(buf + pos, sizeof(buf) - pos, " %02X", uid[i]);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "ATQA: %02X %02X\n", (atqa>>8)&0xFF, atqa&0xFF);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "SAK: %02X\n", sak);
    const char *tstr = (t==MFC_4K?"4K":(t==MFC_1K?"1K":(t==MFC_MINI?"Mini":"Unknown")));
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Mifare Classic type: %s\n", tstr);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Data format version: 2\n");
    if (sd_card_write_file(path, buf, (size_t)pos) != ESP_OK) {
        ESP_LOGE("MFC", "Header write failed: %s", path);
        toast_show("NFC dump save failed", TOAST_ERROR);
        return false;
    }

    // If no live driver, try writing from cache. Batch per-sector appends to
    // reduce repeated small file writes while preserving exact output format.
    if (io == NULL) {
        if (!mfc_cache_matches(uid, uid_len)) {
            ESP_LOGE("MFC", "No cache for this UID; cannot save without card");
            toast_show("NFC dump save failed", TOAST_ERROR);
            return false;
        }
        MFC_TYPE t = mfc_type_from_sak(sak);
        int sectors = mfc_sector_count(t); if (sectors == 0) sectors = 16;
        for (int s = 0; s < sectors; ++s) {
            int first = mfc_first_block_of_sector(t, s);
            int blocks = mfc_blocks_in_sector(t, s);
            int trailer = first + blocks - 1;
            // allocate an estimated sector buffer; grow if needed
            size_t cap = (size_t)blocks * 64 + 64;
            char *sector_buf = (char*)malloc(cap);
            if (!sector_buf) {
                // fallback to original per-block writes if allocation fails
                for (int b = 0; b < blocks; ++b) {
                    int absb = first + b;
                    pos = 0; pos += snprintf(buf + pos, sizeof(buf) - pos, "Block %d:", absb);
                    if (BITSET_TEST(g_mfc_known, absb)) {
                        uint8_t outb[16];
                        memcpy(outb, &g_mfc_cache[absb * 16], 16);
                        if (absb == trailer) {
                            if (g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s)) {
                                memcpy(outb + 0, &g_sector_key_a[s * 6], 6);
                            }
                            if (g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s)) {
                                memcpy(outb + 10, &g_sector_key_b[s * 6], 6);
                            }
                        }
                        for (int i = 0; i < 16 && pos < (int)sizeof(buf) - 4; ++i) pos += snprintf(buf + pos, sizeof(buf) - pos, " %02X", outb[i]);
                    } else {
                        for (int i = 0; i < 16 && pos < (int)sizeof(buf) - 4; ++i) pos += snprintf(buf + pos, sizeof(buf) - pos, " ??");
                    }
                    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
                    esp_err_t we = sd_card_append_file(path, buf, (size_t)pos);
                    if (we != ESP_OK) ESP_LOGE("MFC", "Append failed (%d): %s", (int)we, path);
                }
                continue;
            }
            size_t spos = 0;
            for (int b = 0; b < blocks; ++b) {
                int absb = first + b;
                char line[128]; int lpos = 0;
                lpos += snprintf(line + lpos, sizeof(line) - lpos, "Block %d:", absb);
                if (BITSET_TEST(g_mfc_known, absb)) {
                    uint8_t outb[16];
                    memcpy(outb, &g_mfc_cache[absb * 16], 16);
                    if (absb == trailer) {
                        if (g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s)) memcpy(outb + 0, &g_sector_key_a[s * 6], 6);
                        if (g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s)) memcpy(outb + 10, &g_sector_key_b[s * 6], 6);
                    }
                    for (int i = 0; i < 16 && lpos < (int)sizeof(line) - 4; ++i) lpos += snprintf(line + lpos, sizeof(line) - lpos, " %02X", outb[i]);
                } else {
                    for (int i = 0; i < 16 && lpos < (int)sizeof(line) - 4; ++i) lpos += snprintf(line + lpos, sizeof(line) - lpos, " ??");
                }
                lpos += snprintf(line + lpos, sizeof(line) - lpos, "\n");
                if (spos + (size_t)lpos + 1 > cap) {
                    size_t ncap = cap * 2 + (size_t)lpos + 64;
                    char *n = (char*)realloc(sector_buf, ncap);
                    if (!n) break; // OOM: stop building further lines
                    sector_buf = n; cap = ncap;
                }
                memcpy(sector_buf + spos, line, (size_t)lpos);
                spos += (size_t)lpos;
            }
            if (spos > 0) {
                esp_err_t we = sd_card_append_file(path, sector_buf, spos);
                if (we != ESP_OK) ESP_LOGE("MFC", "Append failed (%d): %s", (int)we, path);
            }
            free(sector_buf);
        }
        toast_show("NFC dump saved", TOAST_SUCCESS);
        return true;
    }

    // Ensure tag is selected before attempting sector-by-sector auth/read
    (void)pn532_in_list_passive_target(io);
    // Dump blocks sector-by-sector
    for (int s = 0; s < sectors; ++s) {
        int first = mfc_first_block_of_sector(t, s);
        int blocks = mfc_blocks_in_sector(t, s);
        bool authed = false;
        bool usedB_cur = false;
        const uint8_t *used_key_cur = NULL;
        // Attempt magic backdoor once; if enabled, treat as authed (no auth needed)
        mfc_try_backdoor_once(io);
        if (g_backdoor_enabled) authed = true;
        // Try known/session/user keys first (A then B)
        uint8_t auth_blk = mfc_auth_target_block(t, s);
        uint8_t trailer_blk = (uint8_t)(first + blocks - 1);
        // Prefer per-sector recorded keys from scan (avoid dict during save)
        if (!authed && g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s)) {
            const uint8_t *ka = &g_sector_key_a[s*6];
            if (mfc_auth_block(io, auth_blk, false, ka, uid, uid_len) == ESP_OK) {
                authed = true; usedB_cur = false; used_key_cur = ka;
            }
        }
        if (!authed && g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s)) {
            const uint8_t *kb = &g_sector_key_b[s*6];
            if (mfc_auth_block(io, auth_blk, true, kb, uid, uid_len) == ESP_OK) {
                authed = true; usedB_cur = true; used_key_cur = kb;
            }
        }
        if (!authed) {
            if (mfc_auth_with_known(io, auth_blk, false, uid, uid_len)) {
                authed = true; usedB_cur = false; used_key_cur = s_last_key_a;
            }
        }
        if (!authed) {
            if (mfc_auth_with_known(io, auth_blk, true, uid, uid_len)) {
                authed = true; usedB_cur = true; used_key_cur = s_last_key_b;
            }
        }
        // Then user dictionary (interleaved A/B)
        if (!authed) {
            // Honor UI request to skip dictionary attempts
            bool skip_dict = false;
            #ifdef CONFIG_HAS_NFC
            if (&nfc_is_dict_skip_requested && nfc_is_dict_skip_requested()) skip_dict = true;
            #endif
            if (!skip_dict) {
                bool usedB = false; const uint8_t *uk = NULL;
                if (mfc_auth_with_user_interleaved(io, auth_blk, uid, uid_len, &usedB, &uk)) {
                    authed = true; usedB_cur = usedB; used_key_cur = uk;
                }
            }
        }
        // Then defaults
        for (int k = 0; k < (int)(sizeof(DEFAULT_KEYS)/6) && !authed; ++k) {
            if (mfc_auth_block(io, auth_blk, false, DEFAULT_KEYS[k], uid, uid_len) == ESP_OK){
                authed = true; mfc_record_working_key(DEFAULT_KEYS[k], false);
                usedB_cur = false; used_key_cur = DEFAULT_KEYS[k];
            }
        }
        for (int k = 0; k < (int)(sizeof(DEFAULT_KEYS)/6) && !authed; ++k) {
            if (mfc_auth_block(io, auth_blk, true, DEFAULT_KEYS[k], uid, uid_len) == ESP_OK){
                authed = true; mfc_record_working_key(DEFAULT_KEYS[k], true);
                usedB_cur = true; used_key_cur = DEFAULT_KEYS[k];
            }
        }
#if defined(CONFIG_HAS_NFC) && defined(CONFIG_MFC_DICT_EMBEDDED)
        if (!authed) {
            bool usedB = false;
            ESP_LOGI("MFC", "Save: try dict interleaved sector=%d blk=%d", s, auth_blk);
            if (mfc_auth_with_dict_interleaved(io, auth_blk, uid, uid_len, &usedB)) {
                authed = true; usedB_cur = usedB;
                if (usedB && g_last_key_b_valid) used_key_cur = g_last_key_b; else if (!usedB && g_last_key_a_valid) used_key_cur = g_last_key_a;
            }
        }
#endif
        // Fallback: retry full auth sequence targeting the trailer block
        if (!authed) {
            if (g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s)) {
                const uint8_t *ka = &g_sector_key_a[s*6];
                if (mfc_auth_block(io, trailer_blk, false, ka, uid, uid_len) == ESP_OK) {
                    authed = true; usedB_cur = false; used_key_cur = ka;
                }
            }
            if (!authed && g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s)) {
                const uint8_t *kb = &g_sector_key_b[s*6];
                if (mfc_auth_block(io, trailer_blk, true, kb, uid, uid_len) == ESP_OK) {
                    authed = true; usedB_cur = true; used_key_cur = kb;
                }
            }
            if (!authed) {
                if (mfc_auth_with_known(io, trailer_blk, false, uid, uid_len)) {
                    authed = true; usedB_cur = false; used_key_cur = s_last_key_a;
                }
            }
            if (!authed) {
                if (mfc_auth_with_known(io, trailer_blk, true, uid, uid_len)) {
                    authed = true; usedB_cur = true; used_key_cur = s_last_key_b;
                }
            }
            if (!authed) {
                bool skip_dict = false;
                #ifdef CONFIG_HAS_NFC
                if (&nfc_is_dict_skip_requested && nfc_is_dict_skip_requested()) skip_dict = true;
                #endif
                if (!skip_dict) {
                    bool usedB = false; const uint8_t *uk = NULL;
                    if (mfc_auth_with_user_interleaved(io, trailer_blk, uid, uid_len, &usedB, &uk)) {
                        authed = true; usedB_cur = usedB; used_key_cur = uk;
                    }
                }
            }
            for (int k = 0; k < (int)(sizeof(DEFAULT_KEYS)/6) && !authed; ++k) {
                if (mfc_auth_block(io, trailer_blk, false, DEFAULT_KEYS[k], uid, uid_len) == ESP_OK){
                    authed = true; mfc_record_working_key(DEFAULT_KEYS[k], false);
                    usedB_cur = false; used_key_cur = DEFAULT_KEYS[k];
                }
            }
            for (int k = 0; k < (int)(sizeof(DEFAULT_KEYS)/6) && !authed; ++k) {
                if (mfc_auth_block(io, trailer_blk, true, DEFAULT_KEYS[k], uid, uid_len) == ESP_OK){
                    authed = true; mfc_record_working_key(DEFAULT_KEYS[k], true);
                    usedB_cur = true; used_key_cur = DEFAULT_KEYS[k];
                }
            }
#if defined(CONFIG_HAS_NFC) && defined(CONFIG_MFC_DICT_EMBEDDED)
            if (!authed) {
                bool usedB = false;
                ESP_LOGI("MFC", "Save: try dict interleaved (trailer) sector=%d blk=%d", s, trailer_blk);
                if (mfc_auth_with_dict_interleaved(io, trailer_blk, uid, uid_len, &usedB)) {
                    authed = true; usedB_cur = usedB;
                    if (usedB && g_last_key_b_valid) used_key_cur = g_last_key_b; else if (!usedB && g_last_key_a_valid) used_key_cur = g_last_key_a;
                }
            }
#endif
        }
        // Record sector key if we know which one succeeded
        if (authed && used_key_cur) mfc_cache_record_sector_key(s, usedB_cur, used_key_cur);
        // Optionally accelerate other sectors by sweeping this key
        if (authed && used_key_cur) mfc_key_reuse_sweep(io, t, uid, uid_len, usedB_cur, used_key_cur, s);
        // Build and append one sector-sized buffer instead of per-block writes
        for (int b = 0; b < blocks; ++b) {
            // If Skip requested mid-sector, exit cache fill immediately
            if (mfc_call_should_skip_dict()) {
                mfc_call_on_cache_mode(false);
                return false;
            }
        }

        // Pre-allocate and assemble sector output
        size_t sec_cap = (size_t)blocks * 96 + 64;
        char *sector_buf = (char*)malloc(sec_cap);
        if (!sector_buf) {
            // fallback to original per-block append behavior when OOM
            for (int b = 0; b < blocks; ++b) {
                // Reuse original logic per-block (keeps behavior)
                if (mfc_call_should_skip_dict()) { mfc_call_on_cache_mode(false); return false; }
                uint8_t data[16]; bool known = false;
                if (authed) {
                    uint8_t blk = (uint8_t)(first + b);
                    if (mfc_read_block(io, blk, data) == ESP_OK) {
                        known = true;
                    } else {
                        if (used_key_cur && mfc_auth_block(io, blk, usedB_cur, used_key_cur, uid, uid_len) == ESP_OK
                            && mfc_read_block(io, blk, data) == ESP_OK) {
                            known = true;
                        } else {
                            const uint8_t *comp = NULL; bool have_comp = false;
                            if (usedB_cur) {
                                if (g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s)) { comp = &g_sector_key_a[s*6]; have_comp = true; }
                            } else {
                                if (g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s)) { comp = &g_sector_key_b[s*6]; have_comp = true; }
                            }
                            if (!have_comp) {
                                mfc_try_find_complementary_user_key(io, t, s, auth_blk, uid, uid_len, usedB_cur);
                                if (usedB_cur) {
                                    if (g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s)) { comp = &g_sector_key_a[s*6]; have_comp = true; }
                                } else {
                                    if (g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s)) { comp = &g_sector_key_b[s*6]; have_comp = true; }
                                }
                            }
                            if (have_comp && mfc_auth_block(io, blk, !usedB_cur, comp, uid, uid_len) == ESP_OK
                                && mfc_read_block(io, blk, data) == ESP_OK) {
                                known = true;
                                usedB_cur = !usedB_cur; used_key_cur = comp;
                            }
                        }
                    }
                }
                pos = 0;
                pos += snprintf(buf + pos, sizeof(buf) - pos, "Block %d:", first + b);
                if (known) {
                    if (b == blocks - 1) {
                        mfc_harvest_trailer_keys(io, t, s, uid, uid_len, data);
                        if (usedB_cur) {
                            if (!(g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s))) {
                                mfc_try_find_complementary_user_key(io, t, s, auth_blk, uid, uid_len, usedB_cur);
                            }
                        } else {
                            if (!(g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s))) {
                                mfc_try_find_complementary_user_key(io, t, s, auth_blk, uid, uid_len, usedB_cur);
                            }
                        }
                        if (g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s)) { memcpy(data + 0, &g_sector_key_a[s * 6], 6); }
                        if (g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s)) { memcpy(data + 10, &g_sector_key_b[s * 6], 6); }
                    }
                    for (int i = 0; i < 16 && pos < (int)sizeof(buf) - 4; ++i) pos += snprintf(buf + pos, sizeof(buf) - pos, " %02X", data[i]);
                } else {
                    for (int i = 0; i < 16 && pos < (int)sizeof(buf) - 4; ++i) pos += snprintf(buf + pos, sizeof(buf) - pos, " ??");
                }
                pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
                esp_err_t we = sd_card_append_file(path, buf, (size_t)pos);
                if (we != ESP_OK) { ESP_LOGE("MFC", "Append failed (%d): %s", (int)we, path); }
            }
        } else {
            size_t spos = 0;
            for (int b = 0; b < blocks; ++b) {
                uint8_t data[16]; bool known = false;
                if (authed) {
                    uint8_t blk = (uint8_t)(first + b);
                    if (mfc_read_block(io, blk, data) == ESP_OK) {
                        known = true;
                    } else {
                        if (used_key_cur && mfc_auth_block(io, blk, usedB_cur, used_key_cur, uid, uid_len) == ESP_OK
                            && mfc_read_block(io, blk, data) == ESP_OK) {
                            known = true;
                        } else {
                            const uint8_t *comp = NULL; bool have_comp = false;
                            if (usedB_cur) {
                                if (g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s)) { comp = &g_sector_key_a[s*6]; have_comp = true; }
                            } else {
                                if (g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s)) { comp = &g_sector_key_b[s*6]; have_comp = true; }
                            }
                            if (!have_comp) {
                                mfc_try_find_complementary_user_key(io, t, s, auth_blk, uid, uid_len, usedB_cur);
                                if (usedB_cur) {
                                    if (g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s)) { comp = &g_sector_key_a[s*6]; have_comp = true; }
                                } else {
                                    if (g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s)) { comp = &g_sector_key_b[s*6]; have_comp = true; }
                                }
                            }
                            if (have_comp && mfc_auth_block(io, blk, !usedB_cur, comp, uid, uid_len) == ESP_OK
                                && mfc_read_block(io, blk, data) == ESP_OK) {
                                known = true;
                                usedB_cur = !usedB_cur; used_key_cur = comp;
                            }
                        }
                    }
                }
                char line[128]; int lpos = 0;
                lpos += snprintf(line + lpos, sizeof(line) - lpos, "Block %d:", first + b);
                if (known) {
                    if (b == blocks - 1) {
                        mfc_harvest_trailer_keys(io, t, s, uid, uid_len, data);
                        if (usedB_cur) {
                            if (!(g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s))) {
                                mfc_try_find_complementary_user_key(io, t, s, auth_blk, uid, uid_len, usedB_cur);
                            }
                        } else {
                            if (!(g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s))) {
                                mfc_try_find_complementary_user_key(io, t, s, auth_blk, uid, uid_len, usedB_cur);
                            }
                        }
                        if (g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s)) memcpy(data + 0, &g_sector_key_a[s * 6], 6);
                        if (g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s)) memcpy(data + 10, &g_sector_key_b[s * 6], 6);
                    }
                    for (int i = 0; i < 16 && lpos < (int)sizeof(line) - 4; ++i) lpos += snprintf(line + lpos, sizeof(line) - lpos, " %02X", data[i]);
                } else {
                    for (int i = 0; i < 16 && lpos < (int)sizeof(line) - 4; ++i) lpos += snprintf(line + lpos, sizeof(line) - lpos, " ??");
                }
                lpos += snprintf(line + lpos, sizeof(line) - lpos, "\n");
                if (spos + (size_t)lpos + 1 > sec_cap) {
                    size_t ncap = sec_cap * 2 + (size_t)lpos + 64;
                    char *n = (char*)realloc(sector_buf, ncap);
                    if (!n) break; // OOM: stop appending further lines
                    sector_buf = n; sec_cap = ncap;
                }
                memcpy(sector_buf + spos, line, (size_t)lpos);
                spos += (size_t)lpos;
            }
            if (spos > 0) {
                esp_err_t we = sd_card_append_file(path, sector_buf, spos);
                if (we != ESP_OK) ESP_LOGE("MFC", "Append failed (%d): %s", (int)we, path);
            }
            free(sector_buf);
        }
    }
    toast_show("NFC dump saved", TOAST_SUCCESS);
    return true;
}

/* Hardnested coverage and valid-sum collection mirrors Momentum-Firmware's
 * enhanced MIFARE Classic poller by noproto (GPL-3.0):
 * https://github.com/Next-Flip/Momentum-Firmware */
#define MFC_HARDNESTED_UNIQUE_MSBS 256
#define MFC_HARDNESTED_RETRY_MAX 3
#define MFC_HARDNESTED_DEFAULT_MAX_SAMPLES 4096
#define MFC_HARDNESTED_MAX_SAMPLE_LIMIT 8192

static const uint16_t MFC_HARDNESTED_VALID_SUMS[] = {
    0, 32, 56, 64, 80, 96, 104, 112, 120, 128,
    136, 144, 152, 160, 176, 192, 200, 224, 256,
};

static uint8_t mfc_hardnested_even_parity32(uint32_t x) {
    x ^= x >> 16;
    x ^= x >> 8;
    x ^= x >> 4;
    x ^= x >> 2;
    x ^= x >> 1;
    return (uint8_t)(x & 1u);
}

static uint16_t mfc_hardnested_sample_budget(uint16_t samples) {
    if (samples == 0) samples = MFC_HARDNESTED_DEFAULT_MAX_SAMPLES;
    if (samples < MFC_HARDNESTED_UNIQUE_MSBS) samples = MFC_HARDNESTED_UNIQUE_MSBS;
    if (samples > MFC_HARDNESTED_MAX_SAMPLE_LIMIT) samples = MFC_HARDNESTED_MAX_SAMPLE_LIMIT;
    return samples;
}

static bool mfc_hardnested_valid_sum(uint16_t sum) {
    for (size_t i = 0; i < sizeof(MFC_HARDNESTED_VALID_SUMS) / sizeof(MFC_HARDNESTED_VALID_SUMS[0]); ++i) {
        if (sum == MFC_HARDNESTED_VALID_SUMS[i]) return true;
    }
    return false;
}

static bool mfc_hardnested_prepare_log(const char *out_dir, char *path, size_t path_len,
                                       char *out_path, size_t out_path_len,
                                       bool rotate_existing) {
    if (!out_dir || !path || path_len == 0) return false;
    snprintf(path, path_len, "%s/.nested.log", out_dir);
    if (out_path && out_path_len) snprintf(out_path, out_path_len, "%s", path);

    /* Rotate any previous log out of the way. This is the only SD access here
     * and happens once at capture start (rotate_existing), under a brief JIT
     * mount. Directory and file creation are handled lazily by
     * mfc_nested_write_sd() on the first chunk flush, so the long RF capture
     * runs without holding a mount (which on bus-sharing boards would freeze
     * the display for the whole capture). */
    if (rotate_existing) {
        bool susp = false;
        if (sd_card_jit_begin(&susp, true)) {
            if (sd_card_exists(path)) {
                char old_path[192];
                snprintf(old_path, sizeof(old_path), "%s.old", path);
                if (sd_card_exists(old_path)) remove(old_path);
                rename(path, old_path);
            }
            sd_card_jit_end(susp);
        }
    }
    return true;
}

static void mfc_hardnested_reset_coverage(uint8_t seen[32], uint16_t *msb_count,
                                          uint16_t *msb_par_sum) {
    memset(seen, 0, 32);
    if (msb_count) *msb_count = 0;
    if (msb_par_sum) *msb_par_sum = 0;
}

// Buffered .nested.log accumulator. Sample lines are collected in RAM (PSRAM
// when available) and flushed to SD in large chunks, so a capture that emits
// thousands of samples does a handful of fopen/fwrite/fclose calls instead of
// one per sample. mfc_nested_log_finalize() must be called by the outermost
// capture entry point to flush the tail and free the buffer.
#define MFC_NESTED_LOG_BUF_CAP (32 * 1024)
static char *s_nested_log_buf = NULL;
static size_t s_nested_log_len = 0;

// Append one chunk to the log file, briefly JIT-mounting the SD. On bus-sharing
// boards (somethingsomething) this suspends the display only for the write, so
// the UI stays live during the RF capture between chunks. Refcount-safe if a
// mount is already held; ensure_dirs=true creates the nfc directory.
static bool mfc_nested_write_sd(const char *path, const char *data, size_t len) {
    if (!path || !data || len == 0) return true;
    bool susp = false;
    if (!sd_card_jit_begin(&susp, true)) {
        ESP_LOGW(MFC_TAG, "Nested log: failed to mount storage for chunk");
        return false;
    }
    bool ok = (sd_card_append_file(path, data, len) == ESP_OK);
    sd_card_jit_end(susp);
    return ok;
}

static bool mfc_nested_log_flush(const char *path) {
    if (!s_nested_log_buf || s_nested_log_len == 0) return true;
    bool ok = mfc_nested_write_sd(path, s_nested_log_buf, s_nested_log_len);
    s_nested_log_len = 0;
    return ok;
}

static void mfc_nested_log_finalize(const char *path) {
    if (path) mfc_nested_log_flush(path);
    if (s_nested_log_buf) { free(s_nested_log_buf); s_nested_log_buf = NULL; }
    s_nested_log_len = 0;
}

static bool mfc_hardnested_append_sample(const char *path, int target_sector, bool target_key_b,
                                         uint32_t cuid, uint32_t nt_enc, uint8_t par) {
    char buf[192];
    int pos = snprintf(buf, sizeof(buf),
                       "Sec %d key %c cuid %08lx nt0 00000000 ks0 %08lx par0 %u%u%u%u\n",
                       target_sector, target_key_b ? 'B' : 'A', (unsigned long)cuid,
                       (unsigned long)nt_enc, (par >> 3) & 1, (par >> 2) & 1,
                       (par >> 1) & 1, par & 1);
    if (pos <= 0) return false;

    /* Lazily allocate the accumulator on the first sample, preferring PSRAM.
     * If it can't be allocated, fall back to a direct per-sample append. */
    if (!s_nested_log_buf) {
        s_nested_log_buf = heap_caps_malloc(MFC_NESTED_LOG_BUF_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_nested_log_buf) s_nested_log_buf = malloc(MFC_NESTED_LOG_BUF_CAP);
        s_nested_log_len = 0;
    }
    if (s_nested_log_buf) {
        /* pos is always < MFC_NESTED_LOG_BUF_CAP (buf is 192B), so one flush
         * always makes room. */
        if (s_nested_log_len + (size_t)pos > MFC_NESTED_LOG_BUF_CAP) {
            if (!mfc_nested_log_flush(path)) return false;
        }
        memcpy(s_nested_log_buf + s_nested_log_len, buf, (size_t)pos);
        s_nested_log_len += (size_t)pos;
        return true;
    }
    return mfc_nested_write_sd(path, buf, (size_t)pos);
}

#if defined(CONFIG_NFC_ST25R3916)
#define MFC_NESTED_CALIBRATION_COUNT 12

// Measure the PRNG advancement window between consecutive nested auths, mirroring
// Momentum's nested calibration. Authenticates once to the known sector, chains
// nested auths to it, decrypts each nonce with the known key, and measures the
// PRNG "distance" (successor steps) between consecutive plaintext nonces. Fills
// d_min/d_max (with a +/-3 margin, values within 3 sigma of the median) and flags
// static-encrypted tags. Returns false if the hardware can't chain nested auths
// or there aren't enough samples, in which case the caller proceeds with the
// plain 256-MSB collection.
//
// NOTE: the continuous nested-auth chain differs from the collection loop (which
// full-auths every iteration) and needs on-device validation on the ST25R3916 /
// PN532 backends. d_min/d_max are not yet used to recover nonces on-device — that
// (the calibrated-collection rewrite) is the follow-up.
static bool mfc_hardnested_calibrate(pn532_io_handle_t io, uint8_t known_block,
                                     bool known_key_b, const uint8_t known_key[6],
                                     const uint8_t *uid, uint8_t uid_len, uint32_t cuid,
                                     uint16_t *out_d_min, uint16_t *out_d_max, bool *out_static) {
    *out_d_min = 0; *out_d_max = 0; *out_static = false;
    if (!io || !known_key || !uid) return false;

    (void)pn532_in_list_passive_target(io);
    if (mfc_auth_block(io, known_block, known_key_b, known_key, uid, uid_len) != ESP_OK) return false;

    uint32_t nt[MFC_NESTED_CALIBRATION_COUNT];
    uint32_t nt_enc_arr[MFC_NESTED_CALIBRATION_COUNT];
    int n = 0;
    uint8_t cmd[2] = { known_key_b ? MIFARE_CMD_AUTH_B : MIFARE_CMD_AUTH_A, known_block };
    for (int c = 0; c < MFC_NESTED_CALIBRATION_COUNT; c++) {
        uint8_t resp[16] = {0};
        uint8_t rl = sizeof(resp);
        if (pn532_in_data_exchange(io, cmd, sizeof(cmd), resp, &rl) != ESP_OK || rl < 4) break;
        uint32_t ne = ((uint32_t)resp[0] << 24) | ((uint32_t)resp[1] << 16) |
                      ((uint32_t)resp[2] << 8) | resp[3];
        nt_enc_arr[n] = ne;
        nt[n] = mfc_nested_decrypt_nt_enc(cuid, ne, known_key);
        n++;
    }
    if (n < 3) return false;

    // Static encrypted: the same encrypted nonce repeats (4+ in a row).
    int same = 0;
    for (int i = 1; i < n; i++) {
        if (nt_enc_arr[i] == nt_enc_arr[i - 1]) {
            if (++same > 3) { *out_static = true; return true; }
        } else {
            same = 0;
        }
    }

    // Distance between consecutive plaintext nonces = PRNG successor steps.
    uint16_t dist[MFC_NESTED_CALIBRATION_COUNT];
    int nd = 0;
    for (int i = 1; i < n; i++) {
        for (uint32_t d = 0; d < 65535; d++) {
            if (crypto1_prng_successor(nt[i - 1], d) == nt[i]) { dist[nd++] = (uint16_t)d; break; }
        }
    }
    if (nd < 2) return false;

    // Sort, median, stddev; keep values within 3 sigma of the median.
    for (int i = 0; i < nd - 1; i++)
        for (int j = 0; j < nd - 1 - i; j++)
            if (dist[j] > dist[j + 1]) { uint16_t tmp = dist[j]; dist[j] = dist[j + 1]; dist[j + 1] = tmp; }
    float median = (nd % 2 == 0) ? (dist[nd / 2 - 1] + dist[nd / 2]) / 2.0f : (float)dist[nd / 2];
    float sum = 0.0f, sq = 0.0f;
    for (int i = 0; i < nd; i++) { sum += dist[i]; sq += (float)dist[i] * dist[i]; }
    float mean = sum / nd;
    float var = sq / nd - mean * mean;
    float sd = (var > 0.0f) ? sqrtf(var) : 0.0f;

    uint16_t dmin = 0xFFFF, dmax = 0;
    for (int i = 0; i < nd; i++) {
        if (fabsf((float)dist[i] - median) <= 3.0f * sd) {
            if (dist[i] < dmin) dmin = dist[i];
            if (dist[i] > dmax) dmax = dist[i];
        }
    }
    if (dmin == 0xFFFF) return false;
    *out_d_min = (dmin > 3) ? (uint16_t)(dmin - 3) : 0;
    *out_d_max = (uint16_t)(dmax + 3);
    return true;
}
#endif // CONFIG_NFC_ST25R3916

static bool mfc_hardnested_capture_target_file(pn532_io_handle_t io,
                                               const uint8_t* uid,
                                               uint8_t uid_len,
                                               MFC_TYPE t,
                                               uint8_t known_block,
                                               bool known_key_b,
                                               const uint8_t known_key[6],
                                               uint8_t target_block,
                                               bool target_key_b,
                                               uint16_t samples,
                                               const char* out_dir,
                                               char* out_path,
                                               size_t out_path_len,
                                               bool rotate_log) {
    if (!io || !uid || uid_len < 4 || !known_key || !out_dir) return false;

    char path[192];
    if (!mfc_hardnested_prepare_log(out_dir, path, sizeof(path), out_path, out_path_len, rotate_log)) return false;

    uint16_t max_samples = mfc_hardnested_sample_budget(samples);
    uint32_t cuid = mfc_cuid_from_uid(uid, uid_len);
    int target_sector = mfc_sector_of_block(t, target_block);
    if (target_sector < 0) target_sector = 0;

    /* Calibrate the PRNG window and detect static-encrypted tags before the
     * (uncalibrated) 256-MSB collection. Non-fatal: on failure we fall back to
     * plain collection. d_min/d_max are reserved for a future calibrated
     * on-device recovery; the collection below is unchanged for now. */
#if defined(CONFIG_NFC_ST25R3916)
    uint16_t cal_d_min = 0, cal_d_max = 0;
    bool cal_static = false;
    if (mfc_hardnested_calibrate(io, known_block, known_key_b, known_key, uid, uid_len, cuid,
                                 &cal_d_min, &cal_d_max, &cal_static)) {
        if (cal_static) {
            ESP_LOGW(MFC_TAG, "Hardnested: static-encrypted tag (sector=%d); nonce set may not be solvable",
                     target_sector);
        } else {
            ESP_LOGI(MFC_TAG, "Hardnested: PRNG window d_min=%u d_max=%u", cal_d_min, cal_d_max);
        }
    } else {
        ESP_LOGD(MFC_TAG, "Hardnested: calibration unavailable; using uncalibrated collection");
    }
    (void)cal_d_min; (void)cal_d_max;
#endif

    uint8_t seen[32] = {0};
    uint16_t msb_count = 0;
    uint16_t msb_par_sum = 0;
    uint8_t retry_count = 0;
    uint16_t got = 0;

    ESP_LOGI(MFC_TAG, "Hardnested: collect sector=%d key=%c max=%u", target_sector,
             target_key_b ? 'B' : 'A', (unsigned)max_samples);
    mfc_call_on_phase(target_sector, target_block, target_key_b, MFC_HARDNESTED_UNIQUE_MSBS);
    if (g_prog_cb) g_prog_cb(0, MFC_HARDNESTED_UNIQUE_MSBS, g_prog_user);

    for (uint16_t i = 0; i < max_samples; ++i) {
        if (mfc_call_should_cancel() || mfc_call_should_skip_dict()) break;

        (void)pn532_in_list_passive_target(io);
        if (mfc_auth_block(io, known_block, known_key_b, known_key, uid, uid_len) != ESP_OK) {
            ESP_LOGD(MFC_TAG, "Hardnested: known auth failed idx=%u", (unsigned)i);
            continue;
        }

        uint8_t cmd[2] = { target_key_b ? MIFARE_CMD_AUTH_B : MIFARE_CMD_AUTH_A, target_block };
        uint8_t resp[16] = {0};
        uint8_t resp_len = sizeof(resp);
        esp_err_t err = pn532_in_data_exchange(io, cmd, sizeof(cmd), resp, &resp_len);
        if (err != ESP_OK || resp_len < 8) {
            ESP_LOGD(MFC_TAG, "Hardnested: sample failed idx=%u err=%d len=%u",
                     (unsigned)i, (int)err, (unsigned)resp_len);
            continue;
        }

        uint32_t nt_enc = ((uint32_t)resp[0] << 24) | ((uint32_t)resp[1] << 16) |
                          ((uint32_t)resp[2] << 8) | resp[3];
        uint8_t par = (uint8_t)(((resp[4] & 1) << 3) | ((resp[5] & 1) << 2) |
                                ((resp[6] & 1) << 1) | (resp[7] & 1));
        uint8_t sum_par = (uint8_t)(par ^ 0x0F);
        if (!mfc_hardnested_append_sample(path, target_sector, target_key_b, cuid, nt_enc, par)) {
            ESP_LOGE(MFC_TAG, "Hardnested: failed to append nested log");
            return false;
        }
        got++;

        uint8_t msb = (uint8_t)(nt_enc >> 24);
        if (!BITSET_TEST(seen, msb)) {
            BITSET_SET(seen, msb);
            msb_count++;
            msb_par_sum += mfc_hardnested_even_parity32((uint32_t)(sum_par & 0x08));
            if (g_prog_cb) g_prog_cb(msb_count, MFC_HARDNESTED_UNIQUE_MSBS, g_prog_user);
        }

        if (msb_count == MFC_HARDNESTED_UNIQUE_MSBS) {
            if (mfc_hardnested_valid_sum(msb_par_sum)) {
                ESP_LOGI(MFC_TAG, "Hardnested: complete sector=%d key=%c samples=%u sum=%u",
                         target_sector, target_key_b ? 'B' : 'A', (unsigned)got,
                         (unsigned)msb_par_sum);
                return true;
            }
            retry_count++;
            ESP_LOGW(MFC_TAG, "Hardnested: invalid parity sum=%u retry=%u", (unsigned)msb_par_sum,
                     (unsigned)retry_count);
            if (retry_count >= MFC_HARDNESTED_RETRY_MAX) break;
            mfc_hardnested_reset_coverage(seen, &msb_count, &msb_par_sum);
            if (g_prog_cb) g_prog_cb(0, MFC_HARDNESTED_UNIQUE_MSBS, g_prog_user);
        }
    }

    ESP_LOGW(MFC_TAG, "Hardnested: incomplete sector=%d key=%c samples=%u msb=%u sum=%u",
             target_sector, target_key_b ? 'B' : 'A', (unsigned)got, (unsigned)msb_count,
             (unsigned)msb_par_sum);
    return false;
}

bool mfc_hardnested_capture_file(pn532_io_handle_t io,
                                 const uint8_t* uid,
                                 uint8_t uid_len,
                                 uint16_t atqa,
                                 uint8_t sak,
                                 uint8_t known_block,
                                 bool known_key_b,
                                 const uint8_t known_key[6],
                                 uint8_t target_block,
                                 bool target_key_b,
                                 uint16_t samples,
                                 const char* out_dir,
                                 char* out_path,
                                 size_t out_path_len) {
    (void)atqa;
    bool ok = mfc_hardnested_capture_target_file(io, uid, uid_len, mfc_type_from_sak(sak), known_block,
                                                  known_key_b, known_key, target_block, target_key_b,
                                                  samples, out_dir, out_path, out_path_len, true);
    mfc_nested_log_finalize(out_path);
    return ok;
}

bool mfc_hardnested_capture_missing_file(pn532_io_handle_t io,
                                         const uint8_t* uid,
                                         uint8_t uid_len,
                                         uint16_t atqa,
                                         uint8_t sak,
                                         uint16_t samples_per_key,
                                         const char* out_dir,
                                         char* out_path,
                                         size_t out_path_len) {
    (void)atqa;
    if (!io || !uid || uid_len < 4 || !out_dir) return false;
    if (!g_mfc_cache || g_mfc_type == MFC_UNKNOWN || !mfc_cache_matches(uid, uid_len)) return false;

    uint8_t known_block = 0;
    uint8_t unused_target_block = 0;
    bool known_key_b = false;
    bool unused_target_key_b = false;
    uint8_t known_key[6] = {0};
    if (!mfc_get_hardnested_defaults(&known_block, &known_key_b, known_key,
                                     &unused_target_block, &unused_target_key_b)) {
        return false;
    }

    MFC_TYPE t = g_mfc_type != MFC_UNKNOWN ? g_mfc_type : mfc_type_from_sak(sak);
    int sectors = mfc_sector_count(t);
    if (sectors <= 0) return false;

    char path[192];
    if (!mfc_hardnested_prepare_log(out_dir, path, sizeof(path), out_path, out_path_len, true)) return false;

    int targets = 0;
    int completed = 0;
    for (int s = 0; s < sectors; ++s) {
        if (mfc_call_should_cancel() || mfc_call_should_skip_dict()) break;
        uint8_t target_block = (uint8_t)(mfc_first_block_of_sector(t, s) + mfc_blocks_in_sector(t, s) - 1);
        if (!(g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s))) {
            targets++;
            if (mfc_hardnested_capture_target_file(io, uid, uid_len, t, known_block, known_key_b,
                                                    known_key, target_block, false, samples_per_key,
                                                    out_dir, out_path, out_path_len, false)) {
                completed++;
            }
        }
        if (mfc_call_should_cancel() || mfc_call_should_skip_dict()) break;
        if (!(g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s))) {
            targets++;
            if (mfc_hardnested_capture_target_file(io, uid, uid_len, t, known_block, known_key_b,
                                                    known_key, target_block, true, samples_per_key,
                                                    out_dir, out_path, out_path_len, false)) {
                completed++;
            }
        }
    }

    mfc_nested_log_finalize(path);
    ESP_LOGI(MFC_TAG, "Hardnested: missing-key capture complete %d/%d", completed, targets);
    return targets > 0 && completed == targets;
}

bool mfc_get_hardnested_defaults(uint8_t *known_block,
                                 bool *known_key_b,
                                 uint8_t known_key[6],
                                 uint8_t *target_block,
                                 bool *target_key_b) {
    if (!known_block || !known_key_b || !known_key || !target_block || !target_key_b) return false;
    if (!g_mfc_cache || g_mfc_type == MFC_UNKNOWN || g_mfc_uid_len == 0) return false;
    int sectors = mfc_sector_count(g_mfc_type);
    if (sectors <= 0) return false;

    int known_sector = -1;
    for (int s = 0; s < sectors; ++s) {
        if (g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s)) {
            known_sector = s;
            *known_key_b = false;
            memcpy(known_key, &g_sector_key_a[s * 6], 6);
            break;
        }
        if (g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s)) {
            known_sector = s;
            *known_key_b = true;
            memcpy(known_key, &g_sector_key_b[s * 6], 6);
            break;
        }
    }
    if (known_sector < 0) return false;
    *known_block = mfc_auth_target_block(g_mfc_type, known_sector);

    for (int s = 0; s < sectors; ++s) {
        if (!(g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s))) {
            *target_key_b = false;
            *target_block = (uint8_t)(mfc_first_block_of_sector(g_mfc_type, s) + mfc_blocks_in_sector(g_mfc_type, s) - 1);
            return true;
        }
        if (!(g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s))) {
            *target_key_b = true;
            *target_block = (uint8_t)(mfc_first_block_of_sector(g_mfc_type, s) + mfc_blocks_in_sector(g_mfc_type, s) - 1);
            return true;
        }
    }
    return false;
}
#endif // CONFIG_NFC_PN532

MFC_TYPE mfc_type_from_sak(uint8_t sak) {
    if (sak == 0x18) return MFC_4K;
    if (sak == 0x08) return MFC_1K;
    if (sak == 0x09) return MFC_MINI;
    return MFC_UNKNOWN;
}

int mfc_sector_count(MFC_TYPE t) {
    switch (t) {
        case MFC_MINI: return 5;   // 320B
        case MFC_1K:  return 16;  // 1KB
        case MFC_4K:  return 40;  // 4KB
        default: return 0;
    }
}

int mfc_blocks_in_sector(MFC_TYPE t, int sector) {
    if (t == MFC_4K && sector >= 32) return 16;
    return 4;
}

int mfc_first_block_of_sector(MFC_TYPE t, int sector) {
    if (t == MFC_4K) {
        if (sector < 32) return sector * 4;
        return 32 * 4 + (sector - 32) * 16; // 128 + (s-32)*16
    }
    return sector * 4;
}

#if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_ST25R3916)
static esp_err_t mfc_auth_block(pn532_io_handle_t io, uint8_t block, bool use_key_b,
                                const uint8_t key[6], const uint8_t *uid, uint8_t uid_len) {
    if (!io || !uid || uid_len < 4) return ESP_ERR_INVALID_ARG;
    uint8_t cmd[12];
    cmd[0] = use_key_b ? MIFARE_CMD_AUTH_B : MIFARE_CMD_AUTH_A;
    cmd[1] = block;
    memcpy(&cmd[2], key, 6);
    const uint8_t *cuid = mfc_cuid_bytes_from_uid(uid, uid_len);
    if (!cuid) return ESP_ERR_INVALID_ARG;
    memcpy(&cmd[8], cuid, 4);
    uint8_t resp[2] = {0};
    uint8_t resp_len = sizeof(resp);
    esp_err_t err = pn532_in_data_exchange(io, cmd, sizeof(cmd), resp, &resp_len);
    if (err != ESP_OK) {
        if (mfc_call_should_cancel()) {
            return err;
        }
        // Wrong dictionary keys are expected. Retrying the same failed AUTH doubles
        // brute-force time. Prefer a backend-specific lightweight recovery when
        // available; fall back to full reselect for native PN532 and errors.
        if (!io->hl_mfc_auth_recover || io->hl_mfc_auth_recover(io) != ESP_OK) {
            (void)pn532_in_list_passive_target(io);
        }
    }
    return err;
}

static esp_err_t mfc_read_block(pn532_io_handle_t io, uint8_t block, uint8_t out16[16]) {
    if (!io || !out16) return ESP_ERR_INVALID_ARG;
    uint8_t cmd[2] = { MIFARE_CMD_READ, block };
    uint8_t resp_len = 16;
    esp_err_t err = pn532_in_data_exchange(io, cmd, sizeof(cmd), out16, &resp_len);
    if (err != ESP_OK) return err;
    return (resp_len == 16) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static const char* mfc_type_str(MFC_TYPE t) {
    switch (t) {
        case MFC_MINI: return "MIFARE Classic Mini";
        case MFC_1K: return "MIFARE Classic 1K";
        case MFC_4K: return "MIFARE Classic 4K";
        default: return "MIFARE Classic";
    }
}

// Choose a safe block to authenticate within a sector:
// - Avoid the trailer (last block of sector)
// - For sector 0, avoid block 0 (manufacturer block); use block 1 instead
static uint8_t mfc_auth_target_block(MFC_TYPE t, int sector) {
    int first = mfc_first_block_of_sector(t, sector);
    int blocks = mfc_blocks_in_sector(t, sector);
    // Default to first data block in the sector
    int target = first;
    if (sector == 0) {
        // Sector 0: block 0 is manufacturer; pick block 1
        target = first + 1;
    }
    // Ensure we never target the trailer as initial auth block
    int trailer = first + blocks - 1;
    if (target == trailer) {
        target = (blocks >= 2) ? (first + 0) : trailer; // fallback to first block if odd layout
        if (sector == 0 && target == first) target = first + 1; // keep avoiding block 0
    }
    return (uint8_t)target;
}


// Presence helpers
static bool mfc_tag_is_present(pn532_io_handle_t io, const uint8_t* uid, uint8_t uid_len) {
    if (!io || !uid || uid_len == 0) return false;
    uint8_t tmp_uid[8] = {0}; uint8_t tmp_len = 0; uint16_t atqa = 0; uint8_t sak = 0;
    // Short timeout probe
    if (pn532_read_passive_target_id_ex(io, 0x00, tmp_uid, &tmp_len, &atqa, &sak, 50) == ESP_OK) {
        if (tmp_len == uid_len && memcmp(tmp_uid, uid, uid_len) == 0) {
            (void)pn532_in_list_passive_target(io); // reselect for stability
            return true;
        }
    }
    return false;
}
static bool mfc_wait_for_tag_return(pn532_io_handle_t io, const uint8_t* uid, uint8_t uid_len) {
    if (!io || !uid || uid_len == 0) return false;
    mfc_call_on_paused(true);
    while (true) {
        if (mfc_call_should_cancel()) { mfc_call_on_paused(false); return false; }
        if (mfc_tag_is_present(io, uid, uid_len)) { mfc_call_on_paused(false); return true; }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

// Complete cache using the same robust read/auth strategy used in the live save path
static void mfc_cache_complete_with_save_logic(pn532_io_handle_t io,
                                               const uint8_t* uid,
                                               uint8_t uid_len,
                                               uint8_t sak) {
    if (!io || !uid || uid_len == 0) return;
    // If UI requested a fast finish (Skip), avoid heavy second-pass read entirely
    if (mfc_call_should_skip_dict()) {
        return;
    }
    mfc_call_on_cache_mode(true);
    MFC_TYPE t = mfc_type_from_sak(sak);
    int sectors = mfc_sector_count(t); if (sectors == 0) sectors = 16;
    (void)pn532_in_list_passive_target(io);
    mfc_try_backdoor_once(io);
    for (int s = 0; s < sectors; ++s) {
        if (mfc_call_should_skip_dict()) break;
        if (mfc_call_should_cancel()) break;
        // If every block in this sector is already cached from the first pass,
        // avoid re-auth and re-reading for this sector in the second pass.
        if (mfc_sector_all_blocks_known(t, s)) {
            if (g_prog_cb) g_prog_cb(s + 1, sectors, g_prog_user);
            continue;
        }
        int first = mfc_first_block_of_sector(t, s);
        int blocks = mfc_blocks_in_sector(t, s);
        uint8_t auth_blk = mfc_auth_target_block(t, s);
        uint8_t trailer_blk = (uint8_t)(first + blocks - 1);
        mfc_call_on_phase(s, auth_blk, false, sectors);
        if (g_prog_cb) g_prog_cb(s, sectors, g_prog_user);
        bool authed = false;
        bool usedB_cur = false;
        const uint8_t *used_key_cur = NULL;
        if (g_backdoor_enabled) authed = true;
        if (!authed && g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s)) {
            const uint8_t *ka = &g_sector_key_a[s*6];
            if (mfc_auth_block(io, auth_blk, false, ka, uid, uid_len) == ESP_OK) {
                authed = true; usedB_cur = false; used_key_cur = ka;
            }
        }
        if (!authed && g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s)) {
            const uint8_t *kb = &g_sector_key_b[s*6];
            if (mfc_auth_block(io, auth_blk, true, kb, uid, uid_len) == ESP_OK) {
                authed = true; usedB_cur = true; used_key_cur = kb;
            }
        }
        if (!authed && mfc_auth_with_known(io, auth_blk, false, uid, uid_len)) {
            authed = true; usedB_cur = false; used_key_cur = s_last_key_a;
        }
        if (!authed && mfc_auth_with_known(io, auth_blk, true, uid, uid_len)) {
            authed = true; usedB_cur = true; used_key_cur = s_last_key_b;
        }
        if (!authed) {
            if (g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s)) {
                const uint8_t *ka = &g_sector_key_a[s*6];
                if (mfc_auth_block(io, trailer_blk, false, ka, uid, uid_len) == ESP_OK) {
                    authed = true; usedB_cur = false; used_key_cur = ka;
                }
            }
            if (!authed && g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s)) {
                const uint8_t *kb = &g_sector_key_b[s*6];
                if (mfc_auth_block(io, trailer_blk, true, kb, uid, uid_len) == ESP_OK) {
                    authed = true; usedB_cur = true; used_key_cur = kb;
                }
            }
            if (!authed && mfc_auth_with_known(io, trailer_blk, false, uid, uid_len)) {
                authed = true; usedB_cur = false; used_key_cur = s_last_key_a;
            }
            if (!authed && mfc_auth_with_known(io, trailer_blk, true, uid, uid_len)) {
                authed = true; usedB_cur = true; used_key_cur = s_last_key_b;
            }
            if (!authed) {
                bool usedB = false;
                const uint8_t *uk = NULL;
                if (mfc_auth_with_user_interleaved(io, auth_blk, uid, uid_len, &usedB, &uk) ||
                    mfc_auth_with_user_interleaved(io, trailer_blk, uid, uid_len, &usedB, &uk)) {
                    authed = true;
                    usedB_cur = usedB;
                    used_key_cur = uk;
                }
            }
        }
        if (authed && used_key_cur) mfc_cache_record_sector_key(s, usedB_cur, used_key_cur);
        if (authed && used_key_cur) mfc_key_reuse_sweep(io, t, uid, uid_len, usedB_cur, used_key_cur, s);
        for (int b = 0; b < blocks; ++b) {
            uint8_t data[16]; bool known = false;
            if (authed) {
                uint8_t blk = (uint8_t)(first + b);
                if (mfc_read_block(io, blk, data) == ESP_OK) {
                    known = true;
                } else {
                    if (used_key_cur && mfc_auth_block(io, blk, usedB_cur, used_key_cur, uid, uid_len) == ESP_OK
                        && mfc_read_block(io, blk, data) == ESP_OK) {
                        known = true;
                    } else {
                        const uint8_t *comp = NULL; bool have_comp = false;
                        if (usedB_cur) {
                            if (g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s)) { comp = &g_sector_key_a[s*6]; have_comp = true; }
                        } else {
                            if (g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s)) { comp = &g_sector_key_b[s*6]; have_comp = true; }
                        }
                        if (!have_comp) {
                            mfc_try_find_complementary_user_key(io, t, s, auth_blk, uid, uid_len, usedB_cur);
                            if (usedB_cur) {
                                if (g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s)) { comp = &g_sector_key_a[s*6]; have_comp = true; }
                            } else {
                                if (g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s)) { comp = &g_sector_key_b[s*6]; have_comp = true; }
                            }
                        }
                        if (have_comp && mfc_auth_block(io, blk, !usedB_cur, comp, uid, uid_len) == ESP_OK
                            && mfc_read_block(io, blk, data) == ESP_OK) {
                            known = true;
                            usedB_cur = !usedB_cur; used_key_cur = comp;
                        } else {
                            // Persistent error -> likely tag removed; wait for return then continue
                            if (!mfc_wait_for_tag_return(io, uid, uid_len)) return; // cancelled
                            (void)pn532_in_list_passive_target(io);
                        }
                    }
                }
            }
            if (known) {
                if (b == blocks - 1) {
                    // Harvest keys from trailer
                    mfc_harvest_trailer_keys(io, t, s, uid, uid_len, data);
                    // Try to discover missing complementary key now (user dict only)
                    if (usedB_cur) {
                        if (!(g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s))) {
                            mfc_try_find_complementary_user_key(io, t, s, auth_blk, uid, uid_len, usedB_cur);
                        }
                    } else {
                        if (!(g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s))) {
                            mfc_try_find_complementary_user_key(io, t, s, auth_blk, uid, uid_len, usedB_cur);
                        }
                    }
                    // Overlay any now-known keys into the trailer bytes before caching
                    if (g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s)) {
                        memcpy(data + 0, &g_sector_key_a[s * 6], 6);
                    }
                    if (g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s)) {
                        memcpy(data + 10, &g_sector_key_b[s * 6], 6);
                    }
                }
                mfc_cache_store_block(first + b, data);
            }
        }
        // Sector completed; bump progress
        if (g_prog_cb) g_prog_cb(s + 1, sectors, g_prog_user);
    }
    mfc_call_on_cache_mode(false);
}

char* mfc_build_details_summary(pn532_io_handle_t io,
                                const uint8_t* uid,
                                uint8_t uid_len,
                                uint16_t atqa,
                                uint8_t sak) {
    mfc_enable_debug_once();
    // Set PN532 timings suitable for repeated AUTH attempts
    pn532_set_quiet(true);
    pn532_set_indata_wait_timeout(20);
    pn532_set_thru_wait_timeout(100);
    pn532_set_inlist_wait_timeout(40);
    // Avoid I2C contention: pause fuel gauge polling while we run tight NFC loops
    fuel_gauge_manager_set_paused(true);
    MFC_TYPE t = mfc_type_from_sak(sak);
    int sectors = mfc_sector_count(t);
    if (sectors == 0) sectors = 16; // fallback
    // prepare cache for possible offline save
    mfc_cache_begin(t, uid, uid_len, atqa, sak);

    // Try auth first-block of each sector with common keys
    int readable = 0, a_cnt = 0, b_cnt = 0;
    // Collect compact per-sector result into a small buffer
    char sec_buf[256]; int spos = 0; sec_buf[0] = '\0';

    // Attempt magic backdoor once; if enabled, we may read without auth
    mfc_try_backdoor_once(io);
    // Analyze PRNG once (non-intrusive). If weak, future nested flow can be enabled.
    mfc_analyze_prng_once(io);
    // Ensure tag is selected for following InDataExchange auth/read
    (void)pn532_in_list_passive_target(io);

    for (int s = 0; s < sectors; ++s) {
        if (mfc_call_should_skip_dict()) { ESP_LOGI("MFC", "Summary: fast-finish requested at sector %d", s); break; }
        if (mfc_call_should_cancel()) { ESP_LOGW("MFC", "Summary: cancelled at sector %d", s); break; }
        int first = mfc_first_block_of_sector(t, s);
        uint8_t auth_blk = mfc_auth_target_block(t, s);
        bool ok = false;
        bool usedB_cur = false;
        const uint8_t *used_key_cur = NULL;
        ESP_LOGI("MFC", "Summary: sector=%d auth_blk=%d skip=%d backdoor=%d", s, auth_blk,
                 mfc_call_should_skip_dict() ? 1 : 0,
                 g_backdoor_enabled ? 1 : 0);
        // If backdoor is enabled, try to read without auth
        if (g_backdoor_enabled) {
            uint8_t tmp[16];
            if (mfc_read_block(io, (uint8_t)first, tmp) == ESP_OK) {
                readable++; ok = true;
                if (spos < (int)sizeof(sec_buf) - 6) spos += snprintf(sec_buf + spos, sizeof(sec_buf) - spos, "%dD ", s);
                ESP_LOGI("MFC", "Summary: sector=%d readable via backdoor", s);
            }
        }
        // Try known/user keys first (show progress phase tied to user dict size when available)
        // Last/session keys first (A then B)
        if (!ok && mfc_auth_with_known(io, auth_blk, false, uid, uid_len)) {
            readable++; a_cnt++; ok = true;
            if (spos < (int)sizeof(sec_buf) - 6) spos += snprintf(sec_buf + spos, sizeof(sec_buf) - spos, "%dA ", s);
            mfc_cache_record_sector_key(s, false, s_last_key_a);
            usedB_cur = false; used_key_cur = s_last_key_a;
            ESP_LOGI("MFC", "Summary: sector=%d auth success via known A", s);
        }
        if (!ok && mfc_auth_with_known(io, auth_blk, true, uid, uid_len)) {
            readable++; b_cnt++; ok = true;
            if (spos < (int)sizeof(sec_buf) - 6) spos += snprintf(sec_buf + spos, sizeof(sec_buf) - spos, "%dB ", s);
            mfc_cache_record_sector_key(s, true, s_last_key_b);
            usedB_cur = true; used_key_cur = s_last_key_b;
            ESP_LOGI("MFC", "Summary: sector=%d auth success via known B", s);
        }
        // User dict interleaved A/B
        if (!ok) {
            int total = (g_user_key_count > 0) ? (g_user_key_count * 2) : (int)(sizeof(DEFAULT_KEYS)/6);
            mfc_call_on_phase(s, auth_blk, false, total);
            if (g_prog_cb) g_prog_cb(0, total, g_prog_user);
            bool usedB = false; const uint8_t *uk = NULL;
            if (mfc_auth_with_user_interleaved(io, auth_blk, uid, uid_len, &usedB, &uk)) {
                readable++; if (usedB) b_cnt++; else a_cnt++; ok = true;
                if (spos < (int)sizeof(sec_buf) - 6) spos += snprintf(sec_buf + spos, sizeof(sec_buf) - spos, usedB ? "%dB " : "%dA ", s);
                if (uk) mfc_cache_record_sector_key(s, usedB, uk);
                usedB_cur = usedB; used_key_cur = uk;
                ESP_LOGI("MFC", "Summary: sector=%d auth success via user dict %c", s, usedB ? 'B' : 'A');
            } else {
                ESP_LOGD("MFC", "Summary: sector=%d user dict failed", s);
            }
        }
        // Then defaults
        for (int k = 0; k < (int)(sizeof(DEFAULT_KEYS)/6) && !ok; ++k) {
            if (&nfc_is_dict_skip_requested && nfc_is_dict_skip_requested()) break; // skip defaults on fast-finish
            if (mfc_auth_block(io, auth_blk, false, DEFAULT_KEYS[k], uid, uid_len) == ESP_OK) {
                readable++; a_cnt++; ok = true; mfc_record_working_key(DEFAULT_KEYS[k], false);
                if (spos < (int)sizeof(sec_buf) - 6) spos += snprintf(sec_buf + spos, sizeof(sec_buf) - spos, "%dA ", s);
                mfc_cache_record_sector_key(s, false, DEFAULT_KEYS[k]);
                usedB_cur = false; used_key_cur = DEFAULT_KEYS[k];
                ESP_LOGI("MFC", "Summary: sector=%d auth success via defaults A (idx=%d)", s, k);
                break;
            } else {
                ESP_LOGD("MFC", "Summary: sector=%d defaults A (idx=%d) failed", s, k);
            }
            if (g_prog_cb) g_prog_cb(k+1, (int)(sizeof(DEFAULT_KEYS)/6), g_prog_user);
        }
        if (!ok) {
            for (int k = 0; k < (int)(sizeof(DEFAULT_KEYS)/6) && !ok; ++k) {
                if (&nfc_is_dict_skip_requested && nfc_is_dict_skip_requested()) break; // skip defaults on fast-finish
                if (mfc_auth_block(io, auth_blk, true, DEFAULT_KEYS[k], uid, uid_len) == ESP_OK) {
                    readable++; b_cnt++; ok = true; mfc_record_working_key(DEFAULT_KEYS[k], true);
                    if (spos < (int)sizeof(sec_buf) - 6) spos += snprintf(sec_buf + spos, sizeof(sec_buf) - spos, "%dB ", s);
                    mfc_cache_record_sector_key(s, true, DEFAULT_KEYS[k]);
                    usedB_cur = true; used_key_cur = DEFAULT_KEYS[k];
                    ESP_LOGI("MFC", "Summary: sector=%d auth success via defaults B (idx=%d)", s, k);
                    break;
                } else {
                    ESP_LOGD("MFC", "Summary: sector=%d defaults B (idx=%d) failed", s, k);
                }
                if (g_prog_cb) g_prog_cb(k+1, (int)(sizeof(DEFAULT_KEYS)/6), g_prog_user);
            }
        }
#if defined(CONFIG_HAS_NFC) && defined(CONFIG_MFC_DICT_EMBEDDED)
        if (!ok) {
            if (mfc_call_should_cancel()) { ESP_LOGW("MFC", "Summary: cancelled before dict sector=%d", s); break; }
            ESP_LOGI("MFC", "Summary: defaults failed, try dict interleaved sector=%d blk=%d", s, auth_blk);
            mfc_call_on_phase(s, auth_blk, false, mfc_dict_total_keys());
            bool usedB = false;
            if (mfc_auth_with_dict_interleaved(io, auth_blk, uid, uid_len, &usedB)) {
                readable++; if (usedB) b_cnt++; else a_cnt++; ok = true;
                if (spos < (int)sizeof(sec_buf) - 6) spos += snprintf(sec_buf + spos, sizeof(sec_buf) - spos, usedB? "%dB " : "%dA ", s);
                // record dict key used (from embedded dict fast-path caches)
                if (usedB && g_last_key_b_valid) mfc_cache_record_sector_key(s, true, g_last_key_b);
                if (!usedB && g_last_key_a_valid) mfc_cache_record_sector_key(s, false, g_last_key_a);
                usedB_cur = usedB;
#if defined(CONFIG_HAS_NFC) && defined(CONFIG_MFC_DICT_EMBEDDED)
                used_key_cur = usedB ? g_last_key_b : g_last_key_a;
#else
                used_key_cur = NULL;
#endif
                ESP_LOGI("MFC", "Summary: sector=%d auth success via embedded dict %c", s, usedB ? 'B' : 'A');
            } else {
                ESP_LOGD("MFC", "Summary: sector=%d embedded dict failed", s);
            }
        }
#endif
#if defined(CONFIG_NFC_ST25R3916)
        if (!ok && !mfc_call_should_skip_dict()) {
            uint8_t nested_key[6] = {0};
            ESP_LOGI("MFC", "Summary: try nested local recovery sector=%d", s);
            mfc_call_on_phase(s, auth_blk, false, MFC_NESTED_LOCAL_SAMPLES);
            if (mfc_try_nested_dict_recovery(io, t, s, false, uid, uid_len, nested_key)) {
                readable++; a_cnt++; ok = true;
                if (spos < (int)sizeof(sec_buf) - 6) spos += snprintf(sec_buf + spos, sizeof(sec_buf) - spos, "%dA ", s);
                usedB_cur = false;
                used_key_cur = &g_sector_key_a[s * 6];
                ESP_LOGI("MFC", "Summary: sector=%d auth success via nested local A", s);
            } else if (mfc_try_nested_dict_recovery(io, t, s, true, uid, uid_len, nested_key)) {
                readable++; b_cnt++; ok = true;
                if (spos < (int)sizeof(sec_buf) - 6) spos += snprintf(sec_buf + spos, sizeof(sec_buf) - spos, "%dB ", s);
                usedB_cur = true;
                used_key_cur = &g_sector_key_b[s * 6];
                ESP_LOGI("MFC", "Summary: sector=%d auth success via nested local B", s);
            }
        }
#endif
        // read and cache all blocks when authenticated
        if (ok) {
            int blocks = mfc_blocks_in_sector(t, s);
            uint8_t data[16];
            for (int b = 0; b < blocks; ++b) {
                // If Skip requested mid-sector, stop reading blocks of this sector
                if (&nfc_is_dict_skip_requested && nfc_is_dict_skip_requested()) break;
                if (mfc_read_block(io, (uint8_t)(first + b), data) == ESP_OK) {
                    mfc_cache_store_block(first + b, data);
                    if (b == blocks - 1) {
                        mfc_harvest_trailer_keys(io, t, s, uid, uid_len, data);
                        ESP_LOGI("MFC", "Summary: sector=%d trailer read; harvest attempted", s);
                    }
                }
            }
            // Optionally try to discover the complementary key using user dict (lightweight)
            mfc_try_find_complementary_user_key(io, t, s, auth_blk, uid, uid_len, usedB_cur);
            // Kick off a reuse sweep using the just-found key to accelerate other sectors
            if (used_key_cur) mfc_key_reuse_sweep(io, t, uid, uid_len, usedB_cur, used_key_cur, s);
        }
        (void)mfc_read_block; // silence unused if we expand later
    }

    // Second pass: only if Skip wasn't requested
    if (!(&nfc_is_dict_skip_requested && nfc_is_dict_skip_requested())) {
        mfc_cache_complete_with_save_logic(io, uid, uid_len, sak);
    }

    size_t cap = 1024; char *out = (char*)malloc(cap); if (!out) { fuel_gauge_manager_set_paused(false); return NULL; }
    char *w = out; size_t rem = cap; int n = 0;
    n = snprintf(w, rem, "Type: %s\nUID:", mfc_type_str(t)); w += n; rem -= n;
    for (uint8_t i = 0; i < uid_len && rem > 4; ++i) { n = snprintf(w, rem, " %02X", uid[i]); w += n; rem -= n; }
    n = snprintf(w, rem, "\nATQA: %02X %02X  SAK: %02X\n", (atqa>>8)&0xFF, atqa&0xFF, sak); w+=n; rem-=n;
    // Compute accurate key counts discovered so far
    int found_a = 0, found_b = 0;
    for (int i = 0; i < sectors; ++i) {
        if (g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, i)) found_a++;
        if (g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, i)) found_b++;
    }
    int keys_found = found_a + found_b;
    int keys_total = sectors * 2;
    n = snprintf(w, rem, "Keys %d/%d | Sectors %d/%d\n", keys_found, keys_total, readable, sectors); w+=n; rem-=n;
    // key map will be printed only if no NDEF summary is found
    // Avoid unused-variable warnings for a_cnt/b_cnt (kept for logs/metrics above)
    (void)a_cnt; (void)b_cnt;

    // Try Flipper parsers first (heap allocation to avoid stack overflow)
    MfClassicData* flipper_data = (MfClassicData*)calloc(1, sizeof(MfClassicData));
    if (!flipper_data) goto skip_flipper_parse; // Skip if allocation fails
    flipper_data->type = (t == MFC_4K) ? MfClassicType4k : (t == MFC_MINI) ? MfClassicTypeMini : MfClassicType1k;
    // Copy blocks
    if (g_mfc_cache && g_mfc_known) {
        int max_blocks = g_mfc_blocks;
        if (max_blocks > 256) max_blocks = 256;
        for (int b = 0; b < max_blocks; b++) {
            if (BITSET_TEST(g_mfc_known, b)) {
                memcpy(flipper_data->block[b].data, &g_mfc_cache[b * 16], 16);
                // Set bit in block_read_mask (32 bytes = 256 bits)
                flipper_data->block_read_mask[b / 8] |= (1 << (b % 8));
            }
        }
    }
    // Copy UID into Flipper-style view for plugins that rely on mf_classic_get_uid
    if (g_mfc_uid_len > 0 && g_mfc_uid_len <= sizeof(flipper_data->uid)) {
        flipper_data->uid_len = g_mfc_uid_len;
        memcpy(flipper_data->uid, g_mfc_uid, g_mfc_uid_len);
    }
    // Copy keys (important for SmartRider which checks sector 0 key)
    // We put them in the trailer blocks of MfClassicData because that's where flipper expects them
    // but our cache keeps them separate.
    // However, the SmartRider plugin uses mf_classic_get_sector_trailer_by_sector which we implemented
    // to return a pointer to the block. So we must ensure the trailer block IN flipper_data contains the keys.
    for (int s = 0; s < sectors; s++) {
        int trailer_idx = mfc_first_block_of_sector(t, s) + mfc_blocks_in_sector(t, s) - 1;
        if (trailer_idx < 256) {
            // If we have keys, write them to the trailer block in our temp struct
            bool haveA = g_sector_key_a_valid && BITSET_TEST(g_sector_key_a_valid, s);
            bool haveB = g_sector_key_b_valid && BITSET_TEST(g_sector_key_b_valid, s);
            if (haveA && g_sector_key_a) {
                memcpy(flipper_data->block[trailer_idx].data, &g_sector_key_a[s * 6], 6);
            } else {
                // Default FF?
                memset(flipper_data->block[trailer_idx].data, 0xFF, 6);
            }
            // Access bits default
            flipper_data->block[trailer_idx].data[6] = 0xFF;
            flipper_data->block[trailer_idx].data[7] = 0x07;
            flipper_data->block[trailer_idx].data[8] = 0x80;
            flipper_data->block[trailer_idx].data[9] = 0x69;
            
            if (haveB && g_sector_key_b) {
                memcpy(flipper_data->block[trailer_idx].data + 10, &g_sector_key_b[s * 6], 6);
            } else {
                memset(flipper_data->block[trailer_idx].data + 10, 0xFF, 6);
            }
            
            // Mark trailer as read so plugin sees it
            flipper_data->block_read_mask[trailer_idx / 8] |= (1 << (trailer_idx % 8));
        }
    }

    char* plugin_text = flipper_nfc_try_parse_mfclassic_from_cache(flipper_data);
    free(flipper_data); // Done with flipper_data
    flipper_data = NULL;

    if (plugin_text) {
        // If plugin parsed successfully, we use that text.
        // We can either replace entirely or append.
        // Let's prepend the generic header + plugin text.
        size_t p_len = strlen(plugin_text);
        size_t h_len = (size_t)(w - out); // generic header length so far
        char* final_out = (char*)malloc(p_len + h_len + 2);
        if (final_out) {
            memcpy(final_out, out, h_len);
            final_out[h_len] = '\n';
            memcpy(final_out + h_len + 1, plugin_text, p_len);
            final_out[h_len + 1 + p_len] = '\0';
            free(out);
            free(plugin_text);
            fuel_gauge_manager_set_paused(false);
            return final_out;
        }
        free(plugin_text);
        // fallback to generic if malloc fails
    }

skip_flipper_parse:
    // Attempt to locate and parse NDEF TLV within sector data blocks and append details
    if (g_mfc_cache && mfc_cache_matches(uid, uid_len)) {
        int sectors = mfc_sector_count(t); if (sectors == 0) sectors = 16;
        for (int s = 0; s < sectors; ++s) {
            if (s == 16 && sectors > 16) continue; // skip MAD2 sector on 4K
            int first = mfc_first_block_of_sector(t, s);
            int blocks = mfc_blocks_in_sector(t, s);
            int data_blocks = blocks - 1; if (data_blocks <= 0) continue;
            size_t sec_bytes = (size_t)data_blocks * 16;
            uint8_t *sec_buf = (uint8_t*)malloc(sec_bytes);
            if (!sec_buf) break;
            size_t woff = 0;
            for (int b = 0; b < data_blocks; ++b) {
                int absb = first + b;
                if (BITSET_TEST(g_mfc_known, absb)) memcpy(sec_buf + woff, &g_mfc_cache[absb * 16], 16);
                else memset(sec_buf + woff, 0, 16);
                woff += 16;
            }
            size_t off = 0, mlen = 0;
            if (ntag_t2_find_ndef(sec_buf, sec_bytes, &off, &mlen) && off < sec_bytes && mlen > 0) {
                // Build a contiguous buffer from this offset across following sectors to cover message length
                size_t need = off + mlen;
                size_t have = sec_bytes;
                size_t total_cap = need;
                // If message spills to next sectors, extend
                int ss = s + 1;
                while (have < need && ss < sectors) {
                    if (ss == 16 && sectors > 16) { ss++; continue; }
                    int bl2 = mfc_blocks_in_sector(t, ss);
                    have += (size_t)(bl2 - 1) * 16;
                    ss++;
                }
                total_cap = have;
                uint8_t *cat = (uint8_t*)malloc(total_cap);
                if (cat) {
                    // copy current sector
                    memcpy(cat, sec_buf, sec_bytes);
                    size_t cat_off = sec_bytes;
                    // append next sectors' data blocks
                    for (int s2 = s + 1; cat_off < total_cap && s2 < sectors; ++s2) {
                        if (s2 == 16 && sectors > 16) continue;
                        int f2 = mfc_first_block_of_sector(t, s2);
                        int bl2 = mfc_blocks_in_sector(t, s2);
                        for (int b2 = 0; b2 < bl2 - 1 && cat_off < total_cap; ++b2) {
                            int absb2 = f2 + b2;
                            if (BITSET_TEST(g_mfc_known, absb2)) memcpy(cat + cat_off, &g_mfc_cache[absb2 * 16], 16);
                            else memset(cat + cat_off, 0, 16);
                            cat_off += 16;
                        }
                    }
                    // Now we have a contiguous view; pass exact message window
                    char *ndef_text = ndef_build_details_from_message(cat + off, mlen, uid, uid_len, mfc_type_str(t));
                    if (ndef_text) {
                        // Extract first decoded record line (e.g., URL ..., Text ..., SmartPoster ...)
                        const char *p = strstr(ndef_text, "\nR");
                        if (!p) {
                            if (ndef_text[0] == 'R') p = ndef_text;
                        } else {
                            p++; // move to 'R'
                        }
                        if (p && p[0] == 'R') {
                            const char *colon = strchr(p, ':');
                            const char *start = NULL;
                            if (colon) {
                                start = colon + 1;
                                if (*start == ' ') start++;
                            }
                            if (start) {
                                const char *end = strchr(start, '\n');
                                size_t linelen = end ? (size_t)(end - start) : strlen(start);
                                // append single-line summary
                                if (linelen > 0) {
                                    if (linelen + 8 > rem) {
                                        size_t used = w - out;
                                        size_t newcap = (cap * 2) + linelen + 256;
                                        char *n = (char*)realloc(out, newcap);
                                        if (n) { w = n + used; out = n; rem = newcap - used; cap = newcap; }
                                    }
                                    if (rem > 0) {
                                        int nn = snprintf(w, rem, "NDEF: ");
                                        if (nn > 0) { w += nn; rem -= nn; }
                                        size_t to_copy = (linelen < rem) ? linelen : rem - 1;
                                        memcpy(w, start, to_copy);
                                        w += to_copy; rem -= to_copy;
                                        if (rem > 0) { *w = '\n'; w++; rem--; }
                                    }
                                }
                            }
                        }
                        free(ndef_text);
                        free(cat);
                        free(sec_buf);
                        fuel_gauge_manager_set_paused(false);
                        return out; // done after first found message
                    }
                    free(cat);
                }
            }
            free(sec_buf);
        }
    }

    fuel_gauge_manager_set_paused(false);
    return out;
}
#endif // CONFIG_NFC_PN532
