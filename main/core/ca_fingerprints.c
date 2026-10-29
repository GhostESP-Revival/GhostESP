// ca_fingerprints.c
// Public fingerprints for the compromised eMudhra intermediates.
//
// See include/core/ca_fingerprints.h for what this is and why it exists.
// Only public data is stored here: a label, the subject of the intermediate
// and its Subject Key Identifier.

#include "core/ca_fingerprints.h"

#include "mbedtls/x509_crt.h"

#include <string.h>

#define SKI(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t) \
    {a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t}

// The eight keys behind the sixteen compromised certificates.  Every entry is
// listed once because the eMudhra issued certificate and the Sectigo
// cross-signature share a key and therefore a Subject Key Identifier.
//
// The TLS intermediates chain to "emSign Root TLS CA - G3" and to "AAA
// Certificate Services".  The code signing intermediates chain to "emSign Root
// CS CA - G3" and to "AAA Certificate Services".
static const ca_compromised_entry_t s_table[] = {
    {"EM DV TLS CA - G3B", "CN=EM DV TLS CA - G3B,O=eMudhra BV,C=NL", true,
     SKI(0xE2, 0x1E, 0x2F, 0xE7, 0x1C, 0x19, 0x39, 0x0E, 0x55, 0x4B,
         0x62, 0xC1, 0x20, 0x24, 0x57, 0x35, 0x24, 0x8A, 0x25, 0x14)},
    {"EM OV TLS CA - G3B", "CN=EM OV TLS CA - G3B,O=eMudhra BV,C=NL", true,
     SKI(0x9B, 0xD8, 0x65, 0x51, 0xAF, 0xE7, 0xE7, 0x2C, 0x37, 0xAD,
         0x08, 0xEE, 0x69, 0x19, 0xD9, 0xBF, 0x37, 0x84, 0xAE, 0x0A)},
    {"EM EV TLS CA - G3B", "CN=EM EV TLS CA - G3B,O=eMudhra BV,C=NL", true,
     SKI(0x3E, 0xDC, 0x20, 0xD9, 0x89, 0x46, 0x98, 0x87, 0x7B, 0x87,
         0x3F, 0xD8, 0x81, 0x47, 0xB2, 0x5C, 0x15, 0xD8, 0x47, 0x06)},
    {"EM DV TLS CA - G3C", "CN=EM DV TLS CA - G3C,O=eMudhra Inc,C=US", true,
     SKI(0xAE, 0x7E, 0x10, 0x5C, 0x3C, 0x29, 0x96, 0xC1, 0xA8, 0x47,
         0x27, 0x05, 0x59, 0xF1, 0xE0, 0x3D, 0xD4, 0xE5, 0x0A, 0xF3)},
    {"EM OV TLS CA - G3C", "CN=EM OV TLS CA - G3C,O=eMudhra Inc,C=US", true,
     SKI(0x34, 0xEA, 0xEA, 0x0C, 0xEB, 0xC9, 0x0A, 0xDA, 0xF9, 0xD0,
         0xC5, 0xE2, 0xED, 0x9B, 0x78, 0xC9, 0x87, 0x9E, 0x1B, 0x6C)},
    {"EM EV TLS CA - G3C", "CN=EM EV TLS CA - G3C,O=eMudhra Inc,C=US", true,
     SKI(0xA6, 0x25, 0x5E, 0x80, 0xA9, 0x4D, 0xB7, 0xF3, 0x96, 0x7C,
         0x1B, 0xC7, 0x91, 0xDF, 0x92, 0xBC, 0xEE, 0x4B, 0xEB, 0x5F)},
    {"EM OV CS CA - G3B", "CN=EM OV CS CA - G3B,O=eMudhra BV,C=NL", false,
     SKI(0x79, 0xFA, 0xE8, 0xEC, 0xCF, 0xDB, 0x0D, 0x75, 0x8D, 0xA4,
         0x58, 0xD6, 0x9F, 0xB6, 0xED, 0xDC, 0x7C, 0x49, 0xDE, 0xE6)},
    {"EM OV CS CA - G3C", "CN=EM OV CS CA - G3C,O=eMudhra Inc,C=US", false,
     SKI(0xEE, 0x6F, 0x73, 0x29, 0x8D, 0x6F, 0xBF, 0xA7, 0xB3, 0x1B,
         0xC3, 0x95, 0x34, 0xF6, 0x15, 0x56, 0x44, 0x78, 0x66, 0xB7)},
};

const ca_compromised_entry_t *ca_compromised_table(size_t *count) {
    if (count) *count = sizeof(s_table) / sizeof(s_table[0]);
    return s_table;
}

const ca_compromised_entry_t *ca_compromised_match(const void *crt) {
    if (!crt) return NULL;

    const mbedtls_x509_crt *cert = (const mbedtls_x509_crt *)crt;
    if (!cert->subject_key_id.p || cert->subject_key_id.len != CA_FINGERPRINT_SKI_LEN) {
        return NULL;
    }

    for (size_t i = 0; i < sizeof(s_table) / sizeof(s_table[0]); i++) {
        if (memcmp(cert->subject_key_id.p, s_table[i].ski, CA_FINGERPRINT_SKI_LEN) == 0) {
            return &s_table[i];
        }
    }
    return NULL;
}

const char *ca_compromised_summary(void) {
    return "eMudhra Oct 2026: 8 intermediate CA keys compromised "
           "(6 TLS, 2 code signing), 16 certificates revoked";
}
