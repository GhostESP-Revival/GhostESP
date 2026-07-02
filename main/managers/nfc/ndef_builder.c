// NDEF record/message encoder used by the NFC "Generate Tag" UI flow.
//
// Wi-Fi credentials use 2-byte TLV lengths throughout (per the WSC spec and
// to match ndef_parse_wifi() in ndef.c), wrapped as TNF=0x02
// "application/vnd.wfa.wsc". vCard is wrapped as MIME type "text/vcard"
// (RFC 6350, matches ndef.c's decoder).
#include "managers/nfc/ndef_builder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *build_ndef_record(uint8_t tnf, const uint8_t *type, uint8_t type_len,
                                  const uint8_t *payload, size_t payload_len, size_t *out_len) {
    bool short_record = payload_len < 256;
    size_t header_len = short_record ? (size_t)(2 + 1 + type_len) : (size_t)(2 + 4 + type_len);
    size_t total = header_len + payload_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return NULL;

    size_t p = 0;
    buf[p++] = (uint8_t)((short_record ? 0xD0 : 0xC0) | (tnf & 0x07)); // MB=1 ME=1 CF=0 IL=0
    buf[p++] = type_len;
    if (short_record) {
        buf[p++] = (uint8_t)payload_len;
    } else {
        buf[p++] = (uint8_t)((payload_len >> 24) & 0xFF);
        buf[p++] = (uint8_t)((payload_len >> 16) & 0xFF);
        buf[p++] = (uint8_t)((payload_len >> 8) & 0xFF);
        buf[p++] = (uint8_t)(payload_len & 0xFF);
    }
    if (type_len) { memcpy(buf + p, type, type_len); p += type_len; }
    if (payload_len) { memcpy(buf + p, payload, payload_len); p += payload_len; }
    *out_len = p;
    return buf;
}

static bool wrap_tlv(const uint8_t *record, size_t record_len, uint8_t **out, size_t *out_len) {
    bool ext = record_len >= 0xFF;
    size_t header = ext ? 4 : 2;
    size_t total = header + record_len + 1; // +1 terminator TLV (0xFE)
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return false;

    size_t p = 0;
    buf[p++] = 0x03;
    if (ext) {
        buf[p++] = 0xFF;
        buf[p++] = (uint8_t)((record_len >> 8) & 0xFF);
        buf[p++] = (uint8_t)(record_len & 0xFF);
    } else {
        buf[p++] = (uint8_t)record_len;
    }
    memcpy(buf + p, record, record_len);
    p += record_len;
    buf[p++] = 0xFE;

    *out = buf;
    *out_len = p;
    return true;
}

static bool build_and_wrap(uint8_t tnf, const uint8_t *type, uint8_t type_len,
                           const uint8_t *payload, size_t payload_len,
                           uint8_t **out, size_t *out_len) {
    size_t rec_len = 0;
    uint8_t *rec = build_ndef_record(tnf, type, type_len, payload, payload_len, &rec_len);
    if (!rec) return false;
    bool ok = wrap_tlv(rec, rec_len, out, out_len);
    free(rec);
    return ok;
}

// Longest-prefix-first match against the subset of NFC Forum URI Record Type
// Definition abbreviation codes we can encode unambiguously. Anything else
// (geo:, sms:, custom schemes, bare hostnames) is written out in full under
// code 0x00 -- always valid, just without the size optimization.
static const struct { const char *prefix; uint8_t code; } k_uri_prefixes[] = {
    { "https://www.", 0x02 },
    { "http://www.",  0x01 },
    { "https://",     0x04 },
    { "http://",      0x03 },
    { "tel:",         0x05 },
    { "mailto:",      0x06 },
    { "ftp://",       0x0D },
};

bool ndef_builder_uri(const char *uri, uint8_t **out, size_t *out_len) {
    if (!uri || !*uri || !out || !out_len) return false;

    uint8_t code = 0x00;
    const char *rest = uri;
    for (size_t i = 0; i < sizeof(k_uri_prefixes) / sizeof(k_uri_prefixes[0]); ++i) {
        size_t plen = strlen(k_uri_prefixes[i].prefix);
        if (strncmp(uri, k_uri_prefixes[i].prefix, plen) == 0) {
            code = k_uri_prefixes[i].code;
            rest = uri + plen;
            break;
        }
    }

    size_t rest_len = strlen(rest);
    size_t payload_len = 1 + rest_len;
    uint8_t *payload = (uint8_t *)malloc(payload_len);
    if (!payload) return false;
    payload[0] = code;
    if (rest_len) memcpy(payload + 1, rest, rest_len);

    bool ok = build_and_wrap(0x01, (const uint8_t *)"U", 1, payload, payload_len, out, out_len);
    free(payload);
    return ok;
}

bool ndef_builder_text(const char *text, uint8_t **out, size_t *out_len) {
    if (!text || !out || !out_len) return false;
    static const char lang[] = "en";
    size_t lang_len = sizeof(lang) - 1;
    size_t text_len = strlen(text);
    size_t payload_len = 1 + lang_len + text_len;

    uint8_t *payload = (uint8_t *)malloc(payload_len);
    if (!payload) return false;
    payload[0] = (uint8_t)(lang_len & 0x3F); // UTF-8, lang code length
    memcpy(payload + 1, lang, lang_len);
    if (text_len) memcpy(payload + 1 + lang_len, text, text_len);

    bool ok = build_and_wrap(0x01, (const uint8_t *)"T", 1, payload, payload_len, out, out_len);
    free(payload);
    return ok;
}

bool ndef_builder_vcard(const char *name, const char *phone, const char *email,
                        uint8_t **out, size_t *out_len) {
    if (!out || !out_len) return false;
    if ((!name || !*name) && (!phone || !*phone) && (!email || !*email)) return false;

    char buf[384];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "BEGIN:VCARD\r\nVERSION:3.0\r\n");
    if (name && *name) pos += snprintf(buf + pos, sizeof(buf) - pos, "FN:%s\r\n", name);
    if (phone && *phone) pos += snprintf(buf + pos, sizeof(buf) - pos, "TEL:%s\r\n", phone);
    if (email && *email) pos += snprintf(buf + pos, sizeof(buf) - pos, "EMAIL:%s\r\n", email);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "END:VCARD\r\n");
    if (pos <= 0 || pos >= (int)sizeof(buf)) return false;

    static const char mime[] = "text/vcard";
    return build_and_wrap(0x02, (const uint8_t *)mime, (uint8_t)(sizeof(mime) - 1),
                          (const uint8_t *)buf, (size_t)pos, out, out_len);
}

bool ndef_builder_wifi(const char *ssid, const char *password, ndef_wifi_auth_t auth,
                       uint8_t **out, size_t *out_len) {
    if (!ssid || !*ssid || !out || !out_len) return false;
    if (!password) password = "";

    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(password);
    if (ssid_len > 32 || pass_len > 64) return false;

    uint16_t auth_code;
    switch (auth) {
        case NDEF_WIFI_AUTH_OPEN: auth_code = 0x0001; break;
        case NDEF_WIFI_AUTH_WEP:  auth_code = 0x0004; break;
        case NDEF_WIFI_AUTH_WPA:  auth_code = 0x0002; break;
        case NDEF_WIFI_AUTH_WPA2:
        default:                 auth_code = 0x0020; break;
    }

    // Credential sub-elements (WFA Wi-Fi Simple Config): 2-byte ID + 2-byte
    // length + value, matching ndef_parse_wifi()'s decoder in ndef.c.
    size_t cred_val_len = (4 + ssid_len) + (4 + 2) + (4 + pass_len);
    size_t payload_len = 4 + cred_val_len; // outer Credential TLV header + value

    uint8_t *payload = (uint8_t *)malloc(payload_len);
    if (!payload) return false;

    size_t p = 0;
    payload[p++] = 0x10; payload[p++] = 0x0E; // Credential
    payload[p++] = (uint8_t)((cred_val_len >> 8) & 0xFF);
    payload[p++] = (uint8_t)(cred_val_len & 0xFF);

    payload[p++] = 0x10; payload[p++] = 0x45; // SSID
    payload[p++] = (uint8_t)((ssid_len >> 8) & 0xFF);
    payload[p++] = (uint8_t)(ssid_len & 0xFF);
    memcpy(payload + p, ssid, ssid_len); p += ssid_len;

    payload[p++] = 0x10; payload[p++] = 0x03; // Auth Type
    payload[p++] = 0x00; payload[p++] = 0x02;
    payload[p++] = (uint8_t)((auth_code >> 8) & 0xFF);
    payload[p++] = (uint8_t)(auth_code & 0xFF);

    payload[p++] = 0x10; payload[p++] = 0x27; // Network Key
    payload[p++] = (uint8_t)((pass_len >> 8) & 0xFF);
    payload[p++] = (uint8_t)(pass_len & 0xFF);
    if (pass_len) memcpy(payload + p, password, pass_len);
    p += pass_len;

    static const char mime[] = "application/vnd.wfa.wsc";
    bool ok = build_and_wrap(0x02, (const uint8_t *)mime, (uint8_t)(sizeof(mime) - 1),
                             payload, payload_len, out, out_len);
    free(payload);
    return ok;
}

bool ndef_builder_aar(const char *package_name, uint8_t **out, size_t *out_len) {
    if (!package_name || !*package_name || !out || !out_len) return false;
    static const char type[] = "android.com:pkg";
    size_t pkg_len = strlen(package_name);
    return build_and_wrap(0x04, (const uint8_t *)type, (uint8_t)(sizeof(type) - 1),
                          (const uint8_t *)package_name, pkg_len, out, out_len);
}
