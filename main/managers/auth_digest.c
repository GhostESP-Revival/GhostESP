#include "managers/auth_digest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include "mbedtls/md.h"
#include "mbedtls/md5.h"
#include "mbedtls/base64.h"
#include "esp_log.h"

static const char *TAG = "auth_digest";

static void to_lower_hex(const unsigned char *in, size_t in_len, char *out, size_t out_len) {
    size_t i;
    const char hex[] = "0123456789abcdef";
    if (out_len < (in_len * 2 + 1)) return;
    for (i = 0; i < in_len; i++) {
        out[i*2] = hex[(in[i] >> 4) & 0xF];
        out[i*2 + 1] = hex[in[i] & 0xF];
    }
    out[in_len * 2] = '\0';
}

int generate_stateless_nonce(const char *key, size_t key_len, char *out, size_t out_len) {
    if (!key || !out) return -1;
    uint32_t now = (uint32_t)time(NULL);
    char ts_str[32];
    snprintf(ts_str, sizeof(ts_str), "%lu", (unsigned long)now);

    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md_info) return -2;
    unsigned char hmac[32];
    if (mbedtls_md_hmac(md_info, (const unsigned char *)key, key_len,
                        (const unsigned char *)ts_str, strlen(ts_str), hmac) != 0) {
        return -3;
    }
    char hmac_hex[65];
    to_lower_hex(hmac, sizeof(hmac), hmac_hex, sizeof(hmac_hex));

    char payload[128];
    int plen = snprintf(payload, sizeof(payload), "%s:%s", ts_str, hmac_hex);
    if (plen <= 0 || (size_t)plen >= sizeof(payload)) return -4;

    size_t olen = out_len;
    if (mbedtls_base64_encode((unsigned char *)out, out_len, &olen, (const unsigned char *)payload, plen) != 0) {
        return -5;
    }
    // ensure NUL
    if (olen >= out_len) return -6;
    out[olen] = '\0';
    return 0;
}

int validate_stateless_nonce(const char *key, size_t key_len, const char *nonce, unsigned int skew_s) {
    if (!key || !nonce) return -1;
    unsigned char dec[128];
    size_t dlen = 0;
    if (mbedtls_base64_decode(dec, sizeof(dec), &dlen, (const unsigned char *)nonce, strlen(nonce)) != 0) {
        ESP_LOGW(TAG, "base64 decode failed");
        return -2;
    }
    if (dlen == 0 || dlen >= sizeof(dec)) return -3;
    dec[dlen] = '\0';

    // parse "ts:hmachex"
    char *colon = (char *)memchr(dec, ':', dlen);
    if (!colon) return -4;
    *colon = '\0';
    const char *ts_str = (const char *)dec;
    const char *hmac_hex = colon + 1;

    // compute expected hmac
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md_info) return -5;
    unsigned char hmac[32];
    if (mbedtls_md_hmac(md_info, (const unsigned char *)key, key_len,
                        (const unsigned char *)ts_str, strlen(ts_str), hmac) != 0) {
        return -6;
    }
    char expected_hex[65];
    to_lower_hex(hmac, sizeof(hmac), expected_hex, sizeof(expected_hex));

    // constant time compare
    if (strlen(hmac_hex) != strlen(expected_hex)) return -7;
    volatile unsigned char diff = 0;
    for (size_t i = 0; i < strlen(expected_hex); i++) {
        diff |= (unsigned char)(hmac_hex[i] ^ expected_hex[i]);
    }
    if (diff != 0) return -8;

    // check timestamp skew
    uint32_t ts = (uint32_t)strtoul(ts_str, NULL, 10);
    uint32_t now = (uint32_t)time(NULL);
    if (ts > now) {
        if (ts - now > skew_s) return -9;
    } else {
        if (now - ts > skew_s) return -10;
    }
    return 0;
}

// helper to compute MD5 hex of input
static int md5_hex(const char *input, size_t ilen, char *out_hex, size_t out_len) {
    unsigned char md[16];
    if (mbedtls_md5((const unsigned char *)input, ilen, md) != 0) return -1;
    to_lower_hex(md, sizeof(md), out_hex, out_len);
    return 0;
}

int compute_digest_response(const char *username, const char *realm, const char *password,
                            const char *method, const char *uri,
                            const char *nonce, const char *cnonce, const char *nc,
                            const char *qop, char *out, size_t out_len) {
    if (!username || !realm || !password || !method || !uri || !nonce || !out) return -1;
    char ha1[33];
    char ha2[33];
    char a1[256];
    char a2[256];

    snprintf(a1, sizeof(a1), "%s:%s:%s", username, realm, password);
    if (md5_hex(a1, strlen(a1), ha1, sizeof(ha1)) != 0) return -2;

    snprintf(a2, sizeof(a2), "%s:%s", method, uri);
    if (md5_hex(a2, strlen(a2), ha2, sizeof(ha2)) != 0) return -3;

    char resp_src[512];
    if (qop && strlen(qop) > 0 && nc && cnonce) {
        // response = MD5(HA1:nonce:nonceCount:cnonce:qop:HA2)
        snprintf(resp_src, sizeof(resp_src), "%s:%s:%s:%s:%s:%s", ha1, nonce, nc, cnonce, qop, ha2);
    } else {
        // response = MD5(HA1:nonce:HA2)
        snprintf(resp_src, sizeof(resp_src), "%s:%s:%s", ha1, nonce, ha2);
    }
    if (md5_hex(resp_src, strlen(resp_src), out, out_len) != 0) return -4;
    return 0;
}


