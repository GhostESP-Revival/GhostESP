#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include "managers/nfc/ndef.h"
#include <stdbool.h>

static size_t append_str(char **p, size_t *cap, const char *s) {
    size_t l = strlen(s);
    if (l + 1 > *cap) return 0;
    memcpy(*p, s, l);
    *p += l;
    *cap -= l;
    **p = '\0';
    return l;
}

// forward declarations used by raw/TLV parser
static size_t append_fmt(char **p, size_t *cap, const char *fmt, ...);
static void parse_ndef_record(uint8_t tnf,
                              const uint8_t *type,
                              uint8_t type_len,
                              const uint8_t *payload,
                              size_t payload_len,
                              char **out,
                              size_t *cap);

// ---=== ndef memory abstraction and TLV/message parsing (raw buffer) ===---

typedef struct {
    const uint8_t *raw_data;
    size_t raw_size;
} NdefRaw;

static bool ndef_get_raw(NdefRaw* ndef, size_t pos, size_t len, void* buf) {
    if (!ndef || !buf) return false;
    if (pos + len > ndef->raw_size) return false;
    memcpy(buf, ndef->raw_data + pos, len);
    return true;
}

static bool ndef_parse_message_raw(NdefRaw* ndef,
                                   size_t pos,
                                   size_t len,
                                   size_t message_num,
                                   bool smart_poster,
                                   char **out,
                                   size_t *cap) {
    size_t end = pos + len;
    size_t record_num = 0;
    bool last_record = false;

    while (pos < end) {
        uint8_t flags_tnf;
        if (!ndef_get_raw(ndef, pos++, 1, &flags_tnf)) return false;

        if (record_num++ && (flags_tnf & 0x80)) return false; // MB after first
        if (last_record) return false;
        if (flags_tnf & 0x40) last_record = true;
        if (flags_tnf & 0x20) return false; // chunking unsupported

        uint8_t type_len;
        if (!ndef_get_raw(ndef, pos++, 1, &type_len)) return false;

        uint32_t payload_len = 0;
        if (flags_tnf & 0x10) {
            uint8_t plen8;
            if (!ndef_get_raw(ndef, pos++, 1, &plen8)) return false;
            payload_len = plen8;
        } else {
            uint8_t plen_be[4];
            if (!ndef_get_raw(ndef, pos, 4, plen_be)) return false;
            payload_len = ((uint32_t)plen_be[0] << 24) | ((uint32_t)plen_be[1] << 16) | ((uint32_t)plen_be[2] << 8) | plen_be[3];
            pos += 4;
        }

        uint8_t id_len = 0;
        if (flags_tnf & 0x08) {
            if (!ndef_get_raw(ndef, pos++, 1, &id_len)) return false;
        }

        uint8_t type_buf_stack[32];
        uint8_t *type_buf = type_buf_stack;
        bool type_alloc = false;
        if (type_len) {
            if (type_len > sizeof(type_buf_stack)) {
                type_buf = malloc(type_len);
                if (!type_buf) return false;
                type_alloc = true;
            }
            if (!ndef_get_raw(ndef, pos, type_len, type_buf)) { if (type_alloc) free(type_buf); return false; }
            pos += type_len;
        }

        pos += id_len;

        uint8_t *payload_buf = NULL;
        if (payload_len) {
            payload_buf = malloc(payload_len);
            if (!payload_buf) { if (type_alloc) free(type_buf); return false; }
            if (!ndef_get_raw(ndef, pos, payload_len, payload_buf)) { free(payload_buf); if (type_alloc) free(type_buf); return false; }
        }

        if (smart_poster) append_fmt(out, cap, "\e*> SP-R%zu: ", record_num);
        else append_fmt(out, cap, "\e*> M%zu-R%zu: ", message_num, record_num);

        parse_ndef_record(flags_tnf & 0x07, type_buf, type_len, payload_buf ? payload_buf : (const uint8_t*)"", payload_len, out, cap);

        if (payload_buf) free(payload_buf);
        if (type_alloc) free(type_buf);

        if (flags_tnf & 0x40) break;
        pos += payload_len;
    }

    if (record_num == 0) {
        if (smart_poster) append_str(out, cap, "\e*> SP: Empty\n\n");
        else append_fmt(out, cap, "\e*> M%zu: Empty\n\n", message_num);
    }

    return true;
}

static size_t ndef_parse_tlv_raw(NdefRaw* ndef, size_t pos, size_t len, size_t already_parsed, char **out, size_t *cap) {
    size_t end = pos + len;
    size_t message_num = 0;

    while (pos < end) {
        uint8_t tlv;
        if (!ndef_get_raw(ndef, pos++, 1, &tlv)) return 0;

        switch (tlv) {
        case 0x00: // padding
            break;
        case 0xFE: // terminator
            return message_num;
        case 0x01: // Lock Control
        case 0x02: // Memory Control
        case 0xFD: // Proprietary
        case 0x03: { // NDEF Message
            uint16_t l = 0;
            uint8_t len_type;
            if (!ndef_get_raw(ndef, pos++, 1, &len_type)) return 0;
            if (len_type != 0xFF) {
                l = len_type;
            } else {
                uint8_t lbe[2];
                if (!ndef_get_raw(ndef, pos, 2, lbe)) return 0;
                l = ((uint16_t)lbe[0] << 8) | lbe[1];
                pos += 2;
            }

            if (tlv != 0x03) { pos += l; break; }

            if (!ndef_parse_message_raw(ndef, pos, l, ++message_num + already_parsed, false, out, cap)) return 0;
            pos += l;
            break;
        }
        default:
            return 0;
        }
    }

    return message_num;
}

static size_t append_fmt(char **p, size_t *cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*p, *cap, fmt, ap);
    va_end(ap);
    if (n <= 0) return 0;
    size_t used = (size_t)n;
    if (used >= *cap) used = *cap ? *cap - 1 : 0;
    *p += used;
    *cap -= used;
    return used;
}

static const char* ndef_uri_prefix[] = {
    [0x00] = NULL,
    [0x01] = "http://www.",
    [0x02] = "https://www.",
    [0x03] = "http://",
    [0x04] = "https://",
    [0x05] = "tel:",
    [0x06] = "mailto:",
    [0x07] = "ftp://anonymous:anonymous@",
    [0x08] = "ftp://ftp.",
    [0x09] = "ftps://",
    [0x0A] = "sftp://",
    [0x0B] = "smb://",
    [0x0C] = "nfs://",
    [0x0D] = "ftp://",
    [0x0E] = "dav://",
    [0x0F] = "news:",
    [0x10] = "telnet://",
    [0x11] = "imap:",
    [0x12] = "rtsp://",
    [0x13] = "urn:",
    [0x14] = "pop:",
    [0x15] = "sip:",
    [0x16] = "sips:",
    [0x17] = "tftp:",
    [0x18] = "btspp://",
    [0x19] = "btl2cap://",
    [0x1A] = "btgoep://",
    [0x1B] = "tcpobex://",
    [0x1C] = "irdaobex://",
    [0x1D] = "file://",
    [0x1E] = "urn:epc:id:",
    [0x1F] = "urn:epc:tag:",
    [0x20] = "urn:epc:pat:",
    [0x21] = "urn:epc:raw:",
    [0x22] = "urn:epc:",
    [0x23] = "urn:nfc:",
};

static char lowerc(char c) { return (c >= 'A' && c <= 'Z') ? (c + 32) : c; }
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}
static void append_percent_decoded(const char *s, size_t len, char **out, size_t *cap) {
    size_t i = 0;
    while (i < len && *cap > 1) {
        char c = s[i++];
        if (c == '%' && i + 1 < len) {
            int h1 = hexval(s[i]);
            int h2 = (i + 1 < len) ? hexval(s[i + 1]) : -1;
            if (h1 >= 0 && h2 >= 0) {
                char d = (char)((h1 << 4) | h2);
                **out = d; (*out)++; (*cap)--; i += 2; continue;
            }
        } else if (c == '+') {
            **out = ' '; (*out)++; (*cap)--; continue;
        }
        **out = c; (*out)++; (*cap)--;
    }
}

static bool ndef_parse_bt(const uint8_t *payload, size_t payload_len, char **out, size_t *cap) {
    // Display Bluetooth data as colon-separated bytes (shows full sequence)
    if (payload_len < 1) return false;

    // Reference implementation uses 8-byte payload where bytes [2..7] are the MAC.
    // Prefer that formatting when possible to match other implementations.
    append_str(out, cap, "BT: ");
    if (payload_len == 8) {
        // standard OOB payload: skip first two bytes
        for (size_t i = 2; i < 8 && *cap > 3; ++i) {
            if (i > 2) append_str(out, cap, ":");
            append_fmt(out, cap, "%02X", payload[i]);
        }
        append_str(out, cap, "\n");
        return true;
    }

    // If we have at least 6 bytes, print first 6 as MAC and any remaining as extras
    if (payload_len >= 6) {
        for (size_t i = 0; i < 6 && *cap > 3; ++i) {
            if (i) append_str(out, cap, ":");
            append_fmt(out, cap, "%02X", payload[i]);
        }
        if (payload_len > 6) {
            append_str(out, cap, " (");
            for (size_t i = 6; i < payload_len && *cap > 3; ++i) append_fmt(out, cap, "%02X", payload[i]);
            append_str(out, cap, ")");
        }
        append_str(out, cap, "\n");
        return true;
    }

    // Fallback: dump all bytes colon-separated
    for (size_t i = 0; i < payload_len && *cap > 3; ++i) {
        if (i) append_str(out, cap, ":");
        append_fmt(out, cap, "%02X", payload[i]);
    }
    append_str(out, cap, "\n");
    return true;
}

static bool ndef_parse_vcard(const uint8_t *payload, size_t payload_len, char **out, size_t *cap) {
    static const char* const begin_tag = "BEGIN:VCARD";
    static const size_t begin_len = 10;
    static const char* const version_tag = "VERSION:";
    static const size_t version_len = 8;
    static const char* const end_tag = "END:VCARD";
    static const size_t end_len = 8;

    size_t pos = 0;
    size_t len = payload_len;

    // Skip BEGIN tag + optional CRLF
    if (len >= begin_len && memcmp(payload, begin_tag, begin_len) == 0) {
        pos += begin_len;
        if (pos < payload_len && payload[pos] == '\r') pos++;
        if (pos < payload_len && payload[pos] == '\n') pos++;
        len = payload_len - pos;
    }

    // Skip VERSION: line if present
    if (len >= version_len && memcmp(&payload[pos], version_tag, version_len) == 0) {
        // advance until newline
        while (pos < payload_len) {
            if (payload[pos] == '\n') { pos++; break; }
            pos++;
        }
        len = payload_len - pos;
    }

    // Trim END tag from end if present
    if (len >= end_len) {
        for (size_t off = 0; off + end_len <= len; ++off) {
            size_t idx = pos + off;
            if (memcmp(&payload[idx], end_tag, end_len) == 0) {
                len = off; // exclude END from length
                break;
            }
        }
    }

    // Try to extract FN: (full name) from vCard and print compactly
    const char *fn = NULL; size_t fn_len = 0;
    for (size_t i = 0; i + 3 < len; ++i) {
        if (payload[pos + i] == 'F' && payload[pos + i + 1] == 'N' && payload[pos + i + 2] == ':') {
            fn = (const char*)&payload[pos + i + 3];
            const char *nl = memchr(fn, '\n', (size_t)(len - i - 3));
            fn_len = nl ? (size_t)(nl - fn) : (size_t)(len - i - 3);
            break;
        }
    }
    if (fn && fn_len > 0) {
        append_str(out, cap, "Contact: ");
        for (size_t i = 0; i < fn_len && *cap > 1; ++i) { char c = fn[i]; if (c >= 32 && c <= 126) { **out = c; (*out)++; (*cap)--; } }
        append_str(out, cap, "\n");
        return true;
    }
    // fallback: short preview
    append_str(out, cap, "Contact: ");
    size_t preview = (len < 64) ? len : 64;
    for (size_t i = 0; i < preview && *cap > 1; ++i) { unsigned char c = payload[pos + i]; if (c >= 32 && c <= 126) { **out = (char)c; (*out)++; (*cap)--; } }
    append_str(out, cap, "\n");
    return true;
}

static bool ndef_parse_wifi(const uint8_t *payload, size_t payload_len, char **out, size_t *cap) {
    // Adapted parsing of WFA WSC blobs used by Android reference implementation
    // Simple TLV: field id (2 bytes BE), length (2 bytes BE), value
    // Simplified WiFi output: single-line SSID/auth/pwd when present
    char ssid[65] = {0}; size_t ssid_len = 0;
    const char *auths = "Unknown";
    char pwd[65] = {0}; size_t pwd_len = 0;
    size_t pos = 0;
    while (pos + 4 <= payload_len) {
        uint16_t field_id = ((uint16_t)payload[pos] << 8) | payload[pos + 1];
        uint16_t field_len = ((uint16_t)payload[pos + 2] << 8) | payload[pos + 3];
        pos += 4;
        if (pos + field_len > payload_len) return false;

        if (field_id == 0x100E) { // CREDENTIAL
            size_t field_end = pos + field_len;
            while (pos + 4 <= field_end) {
                uint16_t cfg_id = ((uint16_t)payload[pos] << 8) | payload[pos + 1];
                uint16_t cfg_len = ((uint16_t)payload[pos + 2] << 8) | payload[pos + 3];
                pos += 4;
                if (pos + cfg_len > field_end) return false;
                switch (cfg_id) {
                case 0x1045: // SSID
                    ssid_len = (cfg_len < sizeof(ssid)-1) ? cfg_len : (sizeof(ssid)-1);
                    for (size_t i = 0; i < ssid_len; ++i) ssid[i] = (char)payload[pos + i];
                    break;
                case 0x1027: // NETWORK KEY
                    pwd_len = (cfg_len < sizeof(pwd)-1) ? cfg_len : (sizeof(pwd)-1);
                    for (size_t i = 0; i < pwd_len; ++i) pwd[i] = (char)payload[pos + i];
                    break;
                case 0x1003: // AUTH TYPE (2 bytes)
                    if (cfg_len == 2) {
                        uint16_t auth = ((uint16_t)payload[pos] << 8) | payload[pos + 1];
                        switch (auth) {
                        case 0x0001: auths = "Open"; break;
                        case 0x0004: auths = "WEP"; break;
                        case 0x0002: auths = "WPA Personal"; break;
                        case 0x0008: auths = "WPA Enterprise"; break;
                        case 0x0010: auths = "WPA2 Enterprise"; break;
                        case 0x0020: auths = "WPA2 Personal"; break;
                        case 0x0022: auths = "WPA/WPA2 Personal"; break;
                        default: auths = "Unknown"; break;
                        }
                    }
                    break;
                default:
                    break;
                }
                pos += cfg_len;
            }
            // output single-line summary
            append_str(out, cap, "WiFi: ");
            if (ssid_len) {
                append_str(out, cap, "SSID \"");
                for (size_t i = 0; i < ssid_len && *cap > 1; ++i) { **out = ssid[i]; (*out)++; (*cap)--; }
                append_str(out, cap, "\"");
            }
            append_str(out, cap, " ");
            append_str(out, cap, auths);
            if (pwd_len) { append_str(out, cap, " PWD="); for (size_t i = 0; i < pwd_len && *cap > 1; ++i) { **out = pwd[i]; (*out)++; (*cap)--; } }
            append_str(out, cap, "\n");
            return true;
        }

        pos += field_len;
    }

    append_str(out, cap, "No data parsed\n");
    return true;
}


static void parse_ndef_record(uint8_t tnf,
                              const uint8_t *type,
                              uint8_t type_len,
                              const uint8_t *payload,
                              size_t payload_len,
                              char **out,
                              size_t *cap) {
    // Media types (TNF=0x02)
    if (tnf == 0x02) {
        // application/vnd.bluetooth.ep.oob
        const char bt_str[] = "application/vnd.bluetooth.ep.oob";
        const char vcard_str[] = "text/vcard";
        const char wifi_str[] = "application/vnd.wfa.wsc";
        if (type_len == (uint8_t)strlen(bt_str) && memcmp(type, bt_str, type_len) == 0) {
            ndef_parse_bt(payload, payload_len, out, cap);
            return;
        } else if (type_len == (uint8_t)strlen(vcard_str) && memcmp(type, vcard_str, type_len) == 0) {
            ndef_parse_vcard(payload, payload_len, out, cap);
            return;
        } else if (type_len == (uint8_t)strlen(wifi_str) && memcmp(type, wifi_str, type_len) == 0) {
            ndef_parse_wifi(payload, payload_len, out, cap);
            return;
        }
        // Unknown media type: dump
        append_str(out, cap, "Unknown\n");
        append_fmt(out, cap, "Media Type ");
        for (uint8_t i = 0; i < type_len && *cap > 1; ++i) { **out = (char)type[i]; (*out)++; (*cap)--; }
        append_str(out, cap, "\n");
        for (size_t i = 0; i < payload_len && *cap > 3; ++i) append_fmt(out, cap, "%02X", payload[i]);
        append_str(out, cap, "\n");
        return;
    }
    if (tnf == 0x01 && type_len == 1 && type[0] == 'U' && payload_len >= 1) {
        uint8_t code = payload[0];
        const char *pre = (code < (sizeof(ndef_uri_prefix)/sizeof(ndef_uri_prefix[0]))) ? ndef_uri_prefix[code] : NULL;
        const char *scheme = pre;
        const char *rest = (const char *)(payload + 1);
        size_t rest_len = (payload_len > 1) ? (payload_len - 1) : 0;
        if (!scheme && rest_len >= 4 && lowerc(rest[0])=='s' && lowerc(rest[1])=='m' && lowerc(rest[2])=='s' && rest[3]==':') {
            append_str(out, cap, "SMS ");
            const char *num = rest + 4;
            const char *q = memchr(num, '?', rest_len - 4);
            size_t num_len = q ? (size_t)(q - num) : (size_t)rest_len - 4;
            append_percent_decoded(num, num_len, out, cap);
            if (q) {
                const char *params = q + 1;
                size_t params_len = (size_t)((rest + rest_len) - params);
                const char *bodyk = NULL;
                // bounded search for "body=" inside params
                for (size_t i = 0; i + 5 <= params_len; ++i) {
                    if (params[i] == 'b' && i + 5 <= params_len &&
                        params[i+1] == 'o' && params[i+2] == 'd' && params[i+3] == 'y' && params[i+4] == '=') {
                        bodyk = params + i + 5;
                        break;
                    }
                }
                if (bodyk) {
                    const char *params_end = rest + rest_len;
                    const char *amp = memchr(bodyk, '&', (size_t)(params_end - bodyk));
                    size_t blen = amp ? (size_t)(amp - bodyk) : (size_t)(params_end - bodyk);
                    append_str(out, cap, " - ");
                    append_percent_decoded(bodyk, blen, out, cap);
                }
            }
            append_str(out, cap, "\n");
            return;
        }
        if (!scheme && rest_len >= 6 && lowerc(rest[0])=='s' && lowerc(rest[1])=='m' && lowerc(rest[2])=='s' && lowerc(rest[3])=='t' && lowerc(rest[4])=='o' && rest[5]==':') {
            append_str(out, cap, "SMS ");
            const char *p = rest + 6;
            size_t p_len = rest_len - 6;
            const char *col = memchr(p, ':', p_len);
            if (col) {
                append_percent_decoded(p, (size_t)(col - p), out, cap);
                append_str(out, cap, " - ");
                const char *msg = col + 1;
                size_t msg_len = (size_t)((rest + rest_len) - msg);
                append_percent_decoded(msg, msg_len, out, cap);
            } else {
                append_percent_decoded(p, p_len, out, cap);
            }
            append_str(out, cap, "\n");
            return;
        }
        if (pre && strcmp(pre, "tel:") == 0) append_str(out, cap, "TEL ");
        else if (pre && strcmp(pre, "mailto:") == 0) append_str(out, cap, "MAIL ");
        else append_str(out, cap, "URL ");
        if (pre) append_str(out, cap, pre);
        append_percent_decoded(rest, rest_len, out, cap);
        append_str(out, cap, "\n");
        return;
    }
    if (tnf == 0x01 && type_len == 1 && type[0] == 'T' && payload_len >= 1) {
        uint8_t status = payload[0];
        uint8_t lang_len = status & 0x3F;
        size_t text_off = 1 + lang_len;
        if (text_off > payload_len) text_off = payload_len;
        append_str(out, cap, "Text \"");
        for (size_t i = text_off; i < payload_len && *cap > 1; ++i) {
            unsigned char c = payload[i];
            if (c >= 32 && c <= 126) { **out = (char)c; (*out)++; (*cap)--; }
        }
        append_str(out, cap, "\"\n");
        return;
    }
    if (tnf == 0x01 && type_len == 2 && type[0] == 'S' && type[1] == 'p' && payload_len > 0) {
        const char *url_pre = NULL; const uint8_t *url_bytes = NULL; size_t url_len = 0;
        const uint8_t *title_bytes = NULL; size_t title_len = 0; size_t pos = 0;
        while (pos < payload_len) {
            if (pos + 1 > payload_len) break;
            uint8_t flags = payload[pos++];
            uint8_t tlen = (pos < payload_len) ? payload[pos++] : 0;
            uint32_t plen = 0;
            if (flags & 0x10) { plen = (pos < payload_len) ? payload[pos++] : 0; }
            else { if (pos + 4 > payload_len) break; plen = ((uint32_t)payload[pos] << 24) | ((uint32_t)payload[pos+1] << 16) | ((uint32_t)payload[pos+2] << 8) | payload[pos+3]; pos += 4; }
            if (flags & 0x08) { if (pos < payload_len) pos++; }
            const uint8_t *tt = (pos + tlen <= payload_len) ? &payload[pos] : NULL; pos += tlen;
            const uint8_t *pl = (pos + plen <= payload_len) ? &payload[pos] : NULL; pos += plen;
            if (!tt || !pl) break;
            if (tlen == 1 && tt[0] == 'U' && plen >= 1) { url_pre = (pl[0] < (sizeof(ndef_uri_prefix)/sizeof(ndef_uri_prefix[0])) ? ndef_uri_prefix[pl[0]] : NULL); url_bytes = &pl[1]; url_len = plen - 1; }
            else if (tlen == 1 && tt[0] == 'T' && plen >= 1) { uint8_t ll = pl[0] & 0x3F; size_t toff = 1 + ll; if (toff <= plen) { title_bytes = &pl[toff]; title_len = plen - toff; } }
            if (flags & 0x40) break;
        }
        append_str(out, cap, "SmartPoster ");
        if (url_bytes) {
            append_str(out, cap, "URL "); if (url_pre) append_str(out, cap, url_pre);
            append_percent_decoded((const char*)url_bytes, url_len, out, cap);
        }
        if (title_bytes && title_len) {
            append_str(out, cap, url_bytes ? " | Title \"" : "Title \"");
            for (size_t i = 0; i < title_len && *cap > 1; ++i) { unsigned char c = title_bytes[i]; if (c >= 32 && c <= 126) { **out = (char)c; (*out)++; (*cap)--; } }
            append_str(out, cap, "\"");
        }
        append_str(out, cap, "\n");
        return;
    }
    append_fmt(out, cap, "Record tnf=0x%02X type=", tnf);
    for (uint8_t i = 0; i < type_len; ++i) { if (*cap > 1) { **out = (char)type[i]; (*out)++; (*cap)--; } }
    append_fmt(out, cap, " len=%u\n", (unsigned)payload_len);
}

char* ndef_build_details_from_message(const uint8_t* ndef_msg,
                                      size_t ndef_len,
                                      const uint8_t* uid,
                                      uint8_t uid_len,
                                      const char* card_label) {
    size_t cap = 2048;
    char *out = (char*)malloc(cap);
    if (!out) return NULL;
    char *w = out; *w = '\0';

    if (!card_label) card_label = "NTAG2xx";
    append_fmt(&w, &cap, "Type: %s\nUID:", card_label);
    for (uint8_t i = 0; i < uid_len && cap > 3; ++i)
        append_fmt(&w, &cap, " %02X", uid[i]);
    append_str(&w, &cap, "\n");

    if (!ndef_msg || ndef_len == 0) {
        append_str(&w, &cap, "NDEF: none\n");
        return out;
    }

    size_t rc_count = 0; {
        size_t p = 0, e = ndef_len;
        while (p < e) {
            if (p + 1 > e) break;
            uint8_t flags = ndef_msg[p++];
            uint8_t tlen = (p < e) ? ndef_msg[p++] : 0;
            uint32_t plen = 0;
            if (flags & 0x10) { plen = (p < e) ? ndef_msg[p++] : 0; }
            else { if (p + 4 > e) break; plen = ((uint32_t)ndef_msg[p] << 24) | ((uint32_t)ndef_msg[p+1] << 16) | ((uint32_t)ndef_msg[p+2] << 8) | ndef_msg[p+3]; p += 4; }
            if (flags & 0x08) { if (p < e) p++; }
            p += tlen; p += plen; rc_count++;
            if (flags & 0x40) break;
        }
    }
    append_fmt(&w, &cap, "NDEF: %u B / %u rec\n", (unsigned)ndef_len, (unsigned)rc_count);

    size_t mpos = 0; size_t mend = ndef_len; int rec_idx = 0;
    while (mpos < mend) {
        if (mpos + 1 > mend) break;
        uint8_t flags = ndef_msg[mpos++];
        uint8_t tlen = (mpos < mend) ? ndef_msg[mpos++] : 0;
        uint32_t plen = 0;
        if (flags & 0x10) {
            plen = (mpos < mend) ? ndef_msg[mpos++] : 0;
        } else {
            if (mpos + 4 > mend) break;
            plen = ((uint32_t)ndef_msg[mpos] << 24) | ((uint32_t)ndef_msg[mpos+1] << 16) | ((uint32_t)ndef_msg[mpos+2] << 8) | ndef_msg[mpos+3];
            mpos += 4;
        }
        uint8_t idlen = (flags & 0x08) ? ((mpos < mend) ? ndef_msg[mpos++] : 0) : 0;
        const uint8_t *type = (mpos + tlen <= mend) ? &ndef_msg[mpos] : NULL; mpos += tlen;
        (void)idlen;
        const uint8_t *pl = (mpos + plen <= mend) ? &ndef_msg[mpos] : NULL; mpos += plen;
        if (!type || !pl) break;
        append_fmt(&w, &cap, "R%d: ", ++rec_idx);
        parse_ndef_record(flags & 0x07, type, tlen, pl, plen, &w, &cap);
        if (flags & 0x40) break;
    }

    return out;
}

// TLV entrypoint: build details by parsing TLV area (entire tag memory)
char* ndef_build_details_from_tlv(const uint8_t* tlv_area,
                                  size_t tlv_len,
                                  const uint8_t* uid,
                                  uint8_t uid_len,
                                  const char* card_label) {
    size_t cap = 2048;
    char *out = (char*)malloc(cap);
    if (!out) return NULL;
    char *w = out; *w = '\0';

    if (!card_label) card_label = "TAG";
    append_fmt(&w, &cap, "Type: %s\nUID:", card_label);
    for (uint8_t i = 0; i < uid_len && cap > 3; ++i)
        append_fmt(&w, &cap, " %02X", uid[i]);
    append_str(&w, &cap, "\n");

    NdefRaw raw = { .raw_data = tlv_area, .raw_size = tlv_len };
    size_t parsed = ndef_parse_tlv_raw(&raw, 0, tlv_len, 0, &w, &cap);
    if (!parsed) {
        free(out);
        return NULL;
    }

    // trim trailing newlines
    while (w > out && (*(w - 1) == '\n' || *(w - 1) == '\r')) { *(--w) = '\0'; cap++; }
    append_str(&w, &cap, "\n");

    return out;
}
