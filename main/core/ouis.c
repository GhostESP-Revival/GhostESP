#include "core/ouis.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

extern const uint8_t ouis_bin_start[] asm("_binary_ouis_bin_start");
extern const uint8_t ouis_bin_end[]   asm("_binary_ouis_bin_end");

#define OUIB_MAGIC      0x4249554FU  /* "OUIB" little-endian */
#define ENTRY_SIZE      5            /* 3 bytes OUI + 2 bytes vendor index */
#define HEADER_SIZE     16

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t count;
    uint32_t vt_offset;
    uint32_t reserved;
} oui_header_t;
#pragma pack(pop)

static void normalize_prefix(const char *mac, uint8_t *out3) {
    int oi = 0;
    uint8_t byte_val = 0;
    int nybble = 0;
    for (const char *p = mac; *p && oi < 3; ++p) {
        char c = *p;
        int val = -1;
        if (c >= '0' && c <= '9') val = c - '0';
        else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
        if (val < 0) continue;
        if (nybble == 0) {
            byte_val = (uint8_t)(val << 4);
            nybble = 1;
        } else {
            byte_val |= (uint8_t)val;
            out3[oi++] = byte_val;
            nybble = 0;
        }
    }
    while (oi < 3) out3[oi++] = 0;
}

static bool is_locally_administered(const uint8_t *oui3) {
    return (oui3[0] & 0x02) != 0;
}

static void to_proper_caps(char *s) {
    bool new_word = true;
    for (size_t i = 0; s[i]; ++i) {
        if (s[i] == ' ' || s[i] == '-' || s[i] == '_') { new_word = true; continue; }
        if (new_word) { s[i] = (char)toupper((unsigned char)s[i]); new_word = false; }
        else { s[i] = (char)tolower((unsigned char)s[i]); }
    }
}

static bool lookup_vendor(const uint8_t *oui3, char *out_vendor, size_t out_sz) {
    const uint8_t *buf = ouis_bin_start;
    const uint8_t *end = ouis_bin_end;

    if ((size_t)(end - buf) < HEADER_SIZE) return false;

    oui_header_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != OUIB_MAGIC || hdr.count == 0) return false;

    const uint8_t *entries = buf + HEADER_SIZE;
    const uint8_t *vtable  = buf + hdr.vt_offset;

    /* binary search */
    uint32_t lo = 0, hi = hdr.count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t *entry = entries + mid * ENTRY_SIZE;
        int cmp = memcmp(oui3, entry, 3);
        if (cmp == 0) {
            uint16_t vidx;
            memcpy(&vidx, entry + 3, 2);
            /* bounds-check vendor index */
            if (hdr.vt_offset + vidx >= (uint32_t)(end - buf)) return false;
            const char *vname = (const char *)(vtable + vidx);
            size_t vlen = strnlen(vname, (size_t)(end - vtable - vidx));
            if (vlen >= out_sz) vlen = out_sz - 1;
            memcpy(out_vendor, vname, vlen);
            out_vendor[vlen] = '\0';
            to_proper_caps(out_vendor);
            return true;
        }
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    return false;
}

bool ouis_lookup_vendor(const char *mac, char *out_vendor, size_t out_sz) {
    uint8_t oui3[3];
    normalize_prefix(mac, oui3);
    if (is_locally_administered(oui3)) return false;
    return lookup_vendor(oui3, out_vendor, out_sz);
}
