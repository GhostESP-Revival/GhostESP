#include "core/ouis.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

extern const uint8_t ouis_bin_start[] asm("_binary_ouis_bin_start");
extern const uint8_t ouis_bin_end[]   asm("_binary_ouis_bin_end");

typedef struct __attribute__((packed)) {
    uint8_t  oui[3];
    uint16_t idx;
} oui_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t count;
    uint16_t reserved;
} oui_header_t;

static const oui_header_t *oui_header(void) {
    return (const oui_header_t *)ouis_bin_start;
}

static const oui_entry_t *oui_entries(void) {
    return (const oui_entry_t *)(ouis_bin_start + sizeof(oui_header_t));
}

static const uint8_t *oui_string_table(void) {
    return (const uint8_t *)ouis_bin_start + sizeof(oui_header_t)
           + sizeof(oui_entry_t) * oui_header()->count;
}

static void parse_prefix(const char *mac, uint8_t out3[3], int *out_nibbles) {
    int nibbles = 0;
    int oi = 0;
    for (const char *p = mac; *p && oi < 3; ++p) {
        char c = *p;
        if (!isxdigit((int)(unsigned char)c)) continue;
        char hi = (char)toupper((unsigned char)c);
        if (++nibbles & 1) {
            out3[oi] = (uint8_t)((hi <= '9' ? hi - '0' : hi - 'A' + 10) << 4);
        } else {
            out3[oi] |= (uint8_t)(hi <= '9' ? hi - '0' : hi - 'A' + 10);
            oi++;
        }
    }
    while (oi < 3) out3[oi++] = 0;
    *out_nibbles = nibbles;
}

static bool is_locally_administered(const uint8_t oui[3]) {
    // LAA bit: bit 1 of the first transmitted octet.
    return (oui[0] & 0x02) != 0;
}

static void to_proper_caps(char *s) {
    bool new_word = true;
    for (size_t i = 0; s[i]; ++i) {
        if (s[i] == ' ' || s[i] == '-' || s[i] == '_') { new_word = true; continue; }
        if (new_word) { s[i] = (char)toupper((unsigned char)s[i]); new_word = false; }
        else          { s[i] = (char)tolower((unsigned char)s[i]); }
    }
}

static bool contains_ci(const char *haystack, const char *needle) {
    if (needle == NULL || needle[0] == '\0') return true;
    if (haystack == NULL) return false;

    size_t needle_len = strlen(needle);
    if (needle_len == 0) return true;

    for (const char *h = haystack; *h; h++) {
        size_t i = 0;
        while (i < needle_len && h[i] &&
               tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_len) return true;
    }

    return false;
}

static bool vendor_in_list(char vendors[][64], int count, const char *vendor) {
    for (int i = 0; i < count; i++) {
        if (strcmp(vendors[i], vendor) == 0) {
            return true;
        }
    }
    return false;
}

static bool lookup_vendor(const uint8_t oui3[3], char *out_vendor, size_t out_sz) {
    const oui_header_t *h = oui_header();
    const oui_entry_t *es = oui_entries();
    uint16_t lo = 0, hi = h->count;
    while (lo < hi) {
        uint16_t mid = (uint16_t)(lo + ((hi - lo) >> 1));
        const oui_entry_t *e = &es[mid];
        int cmp = memcmp(e->oui, oui3, 3);
        if (cmp == 0) {
            const uint8_t *strs = oui_string_table();
            const char *v = (const char *)&strs[e->idx];
            size_t vlen = 0;
            while (vlen < 256 && v[vlen] != '\0') vlen++;
            if (vlen >= out_sz) vlen = out_sz - 1;
            memcpy(out_vendor, v, vlen);
            out_vendor[vlen] = '\0';
            to_proper_caps(out_vendor);
            return true;
        }
        if (cmp < 0) lo = (uint16_t)(mid + 1);
        else         hi = mid;
    }
    return false;
}

bool ouis_lookup_vendor(const char *mac, char *out_vendor, size_t out_sz) {
    uint8_t p3[3] = {0};
    int nibbles = 0;
    parse_prefix(mac, p3, &nibbles);
    if (nibbles < 6) return false;
    if (is_locally_administered(p3)) return false;
    return lookup_vendor(p3, out_vendor, out_sz);
}

bool ouis_parse_prefix(const char *prefix, uint8_t out_oui[3]) {
    if (prefix == NULL || out_oui == NULL) return false;

    uint8_t parsed[3] = {0};
    int nibbles = 0;
    parse_prefix(prefix, parsed, &nibbles);
    if (nibbles < 6) return false;

    memcpy(out_oui, parsed, sizeof(parsed));
    return true;
}

bool ouis_lookup_vendor_bytes(const uint8_t oui[3], char *out_vendor, size_t out_sz) {
    if (oui == NULL || out_vendor == NULL || out_sz == 0) return false;
    if (is_locally_administered(oui)) return false;
    return lookup_vendor(oui, out_vendor, out_sz);
}

int ouis_foreach_unique_vendor(const char *filter, ouis_vendor_iter_cb_t callback,
                               void *user_data, int max_results) {
    if (callback == NULL || max_results <= 0) return 0;

    const oui_header_t *h = oui_header();
    const oui_entry_t *es = oui_entries();
    const uint8_t *strs = oui_string_table();
    char (*seen)[64] = NULL;
    int seen_count = 0;
    int seen_cap = max_results;

    seen = calloc((size_t)seen_cap, sizeof(*seen));
    if (seen == NULL) return 0;

    for (uint16_t i = 0; i < h->count; i++) {
        const char *raw = (const char *)&strs[es[i].idx];
        char vendor[64];
        size_t vlen = 0;
        while (vlen < sizeof(vendor) - 1 && raw[vlen] != '\0') vlen++;
        memcpy(vendor, raw, vlen);
        vendor[vlen] = '\0';
        to_proper_caps(vendor);

        if (!contains_ci(vendor, filter) || vendor_in_list(seen, seen_count, vendor)) {
            continue;
        }

        strncpy(seen[seen_count], vendor, sizeof(seen[seen_count]) - 1);
        seen[seen_count][sizeof(seen[seen_count]) - 1] = '\0';
        seen_count++;

        if (!callback(vendor, user_data)) {
            break;
        }

        if (max_results > 0 && seen_count >= max_results) {
            break;
        }
    }

    free(seen);
    return seen_count;
}
