// meshcore_companion.c
// MeshCore companion (phone) protocol frame layer. Command/response formats
// mirror upstream firmware v1.17.1 examples/companion_radio/MyMesh.cpp.

#include "managers/meshcore_config.h"
#include "sdkconfig.h"

#ifdef CONFIG_HAS_MESHCORE

#include "managers/meshcore_companion.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "managers/meshcore_alloc.h"
#include "managers/meshcore_ble.h"
#include "managers/meshcore_crypto.h"
#include "managers/meshcore_identity.h"
#include "managers/meshcore_manager.h"
#include "managers/meshcore_mesh.h"
#include "managers/meshcore_packet.h"

#define MC_SIGN_BUF_LEN (8 * 1024)
#define MC_EXPECTED_ACK 8

static const char *TAG = "MeshCoreApp";

static bool s_connected;
static uint8_t s_app_ver;

// Pending contacts iterator (CMD_GET_CONTACTS).
static bool s_iter_started;
static int s_iter_idx;
static uint32_t s_iter_filter_since;
static uint32_t s_most_recent_lastmod;

// Offline queue of received-message frames (drained by SYNC_NEXT_MESSAGE).
// Allocated on mc_companion_init() so a stopped stack holds no RAM.
typedef struct {
    uint16_t len;
    uint8_t buf[MC_MAX_FRAME_SIZE];
} mc_queue_entry_t;
static uint8_t s_queue_len;
static mc_queue_entry_t *s_queue;  // MC_OFFLINE_QUEUE_SIZE

// Expected ACK table for SEND_CONFIRMED pushes.
static struct {
    uint32_t ack;
    uint8_t pub[MC_PUB_KEY_SIZE];
    uint32_t sent_ms;
} s_ack_table[MC_EXPECTED_ACK];
static int s_ack_next;

// Pending application requests, matched against incoming responses.
// status matches the first 4 bytes of the peer pubkey (legacy scheme); the
// others match the request tag echoed back by the responder.
static uint32_t s_pending_status;
static uint32_t s_pending_telemetry;
static uint32_t s_pending_req;

static void clear_pending_reqs(void) {
    s_pending_status = 0;
    s_pending_telemetry = 0;
    s_pending_req = 0;
}

// Sign buffer (allocated only while a CMD_SIGN_* sequence is active).
static uint8_t *s_sign_buf;
static size_t s_sign_len;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void write_frame(const uint8_t *buf, size_t len) {
    if (!s_connected || len == 0) return;
    if (len > MC_MAX_FRAME_SIZE) len = MC_MAX_FRAME_SIZE;
    mc_ble_notify(buf, len);
}

static void write_ok(void) {
    uint8_t b = MC_RESP_OK;
    write_frame(&b, 1);
}

static void write_err(uint8_t code) {
    uint8_t b[2] = {MC_RESP_ERR, code};
    write_frame(b, 2);
}

static void write_sent(uint32_t tag, int result, uint32_t est_timeout) {
    uint8_t out[10];
    out[0] = MC_RESP_SENT;
    out[1] = (result == MC_MSG_SEND_SENT_FLOOD) ? 1 : 0;
    memcpy(&out[2], &tag, 4);
    memcpy(&out[6], &est_timeout, 4);
    write_frame(out, sizeof(out));
}

static void write_disabled(void) {
    uint8_t b = MC_RESP_DISABLED;
    write_frame(&b, 1);
}

static void strzcpy(char *dest, const char *src, size_t n) {
    size_t i = 0;
    if (src) {
        for (; i < n && src[i]; ++i) dest[i] = src[i];
    }
    for (; i < n; ++i) dest[i] = 0;
}

static bool is_channel_frame(uint8_t code) {
    return code == MC_RESP_CHANNEL_MSG_RECV || code == MC_RESP_CHANNEL_MSG_RECV_V3 ||
           code == MC_RESP_CHANNEL_DATA_RECV;
}

static void queue_push(const uint8_t *frame, size_t len) {
    if (!s_queue) return;
    if (len > MC_MAX_FRAME_SIZE) len = MC_MAX_FRAME_SIZE;
    if (s_queue_len < MC_OFFLINE_QUEUE_SIZE) {
        s_queue[s_queue_len].len = (uint16_t)len;
        memcpy(s_queue[s_queue_len].buf, frame, len);
        s_queue_len++;
        return;
    }
    // Full: drop the oldest channel message, else drop.
    for (uint8_t i = 0; i < s_queue_len; ++i) {
        if (is_channel_frame(s_queue[i].buf[0])) {
            for (uint8_t j = i; j + 1 < s_queue_len; ++j) s_queue[j] = s_queue[j + 1];
            s_queue[s_queue_len - 1].len = (uint16_t)len;
            memcpy(s_queue[s_queue_len - 1].buf, frame, len);
            return;
        }
    }
}

static bool queue_pop(uint8_t *out, uint16_t *out_len) {
    if (!s_queue || s_queue_len == 0) return false;
    *out_len = s_queue[0].len;
    memcpy(out, s_queue[0].buf, s_queue[0].len);
    s_queue_len--;
    for (uint8_t i = 0; i < s_queue_len; ++i) s_queue[i] = s_queue[i + 1];
    return true;
}

static void tickle(void) {
    uint8_t b = MC_PUSH_MSG_WAITING;
    write_frame(&b, 1);
}

static void contact_resp(uint8_t code, const mc_contact_t *c) {
    uint8_t out[MC_MAX_FRAME_SIZE];
    int i = 0;
    out[i++] = code;
    memcpy(&out[i], c->pub_key, MC_PUB_KEY_SIZE); i += MC_PUB_KEY_SIZE;
    out[i++] = c->type;
    out[i++] = c->flags;
    out[i++] = c->out_path_len;
    memcpy(&out[i], c->out_path, MC_MAX_PATH_SIZE); i += MC_MAX_PATH_SIZE;
    strzcpy((char *)&out[i], c->name, 32); i += 32;
    memcpy(&out[i], &c->last_advert_timestamp, 4); i += 4;
    memcpy(&out[i], &c->gps_lat, 4); i += 4;
    memcpy(&out[i], &c->gps_lon, 4); i += 4;
    memcpy(&out[i], &c->lastmod, 4); i += 4;
    write_frame(out, (size_t)i);
}

static void update_contact_from_frame(mc_contact_t *c, uint32_t *last_mod,
                                      const uint8_t *frame, int len) {
    int i = 0;
    i++; // command code
    memcpy(c->pub_key, &frame[i], MC_PUB_KEY_SIZE); i += MC_PUB_KEY_SIZE;
    c->type = frame[i++];
    c->flags = frame[i++];
    c->out_path_len = frame[i++];
    memcpy(c->out_path, &frame[i], MC_MAX_PATH_SIZE); i += MC_MAX_PATH_SIZE;
    memcpy(c->name, &frame[i], 32); i += 32;
    c->name[31] = 0;
    memcpy(&c->last_advert_timestamp, &frame[i], 4); i += 4;
    if (len >= i + 8) {
        memcpy(&c->gps_lat, &frame[i], 4); i += 4;
        memcpy(&c->gps_lon, &frame[i], 4); i += 4;
        if (len >= i + 4) memcpy(last_mod, &frame[i], 4);
    }
}

static void ack_table_add(uint32_t ack, const mc_contact_t *c) {
    s_ack_table[s_ack_next].ack = ack;
    if (c) memcpy(s_ack_table[s_ack_next].pub, c->pub_key, MC_PUB_KEY_SIZE);
    else memset(s_ack_table[s_ack_next].pub, 0, MC_PUB_KEY_SIZE);
    s_ack_table[s_ack_next].sent_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_ack_next = (s_ack_next + 1) % MC_EXPECTED_ACK;
}

// ---------------------------------------------------------------------------
// Event hooks
// ---------------------------------------------------------------------------

void mc_companion_on_channel_message(uint8_t channel_idx, uint32_t timestamp,
                                     const char *text, float snr,
                                     uint8_t path_len, bool is_flood) {
    uint8_t out[MC_MAX_FRAME_SIZE];
    int i = 0;
    if (s_app_ver >= 3) {
        out[i++] = MC_RESP_CHANNEL_MSG_RECV_V3;
        out[i++] = (uint8_t)(int8_t)(snr * 4.0f);
        out[i++] = 0;
        out[i++] = 0;
    } else {
        out[i++] = MC_RESP_CHANNEL_MSG_RECV;
    }
    out[i++] = channel_idx;
    out[i++] = is_flood ? path_len : MC_OUT_PATH_UNKNOWN;
    out[i++] = MC_TXT_TYPE_PLAIN;
    memcpy(&out[i], &timestamp, 4); i += 4;
    size_t tlen = strlen(text);
    if ((size_t)i + tlen > MC_MAX_FRAME_SIZE) tlen = MC_MAX_FRAME_SIZE - (size_t)i;
    memcpy(&out[i], text, tlen); i += (int)tlen;
    queue_push(out, (size_t)i);
    tickle();
}

void mc_companion_on_contact_message(const mc_contact_t *from, uint32_t timestamp,
                                     const char *text, uint8_t txt_type, float snr,
                                     uint8_t path_len, bool is_flood) {
    if (txt_type == 0xFF) return; // raw application response; not a chat frame
    uint8_t out[MC_MAX_FRAME_SIZE];
    int i = 0;
    if (s_app_ver >= 3) {
        out[i++] = MC_RESP_CONTACT_MSG_RECV_V3;
        out[i++] = (uint8_t)(int8_t)(snr * 4.0f);
        out[i++] = 0;
        out[i++] = 0;
    } else {
        out[i++] = MC_RESP_CONTACT_MSG_RECV;
    }
    if (from) {
        memcpy(&out[i], from->pub_key, 6);
    } else {
        memset(&out[i], 0, 6);
    }
    i += 6;
    out[i++] = is_flood ? path_len : MC_OUT_PATH_UNKNOWN;
    out[i++] = txt_type;
    memcpy(&out[i], &timestamp, 4); i += 4;
    size_t tlen = strlen(text);
    if ((size_t)i + tlen > MC_MAX_FRAME_SIZE) tlen = MC_MAX_FRAME_SIZE - (size_t)i;
    memcpy(&out[i], text, tlen); i += (int)tlen;
    queue_push(out, (size_t)i);
    tickle();
}

void mc_companion_on_signed_message(const mc_contact_t *from, uint32_t timestamp,
                                    const uint8_t *sender_prefix, const char *text,
                                    float snr, uint8_t path_len, bool is_flood) {
    uint8_t out[MC_MAX_FRAME_SIZE];
    int i = 0;
    if (s_app_ver >= 3) {
        out[i++] = MC_RESP_CONTACT_MSG_RECV_V3;
        out[i++] = (uint8_t)(int8_t)(snr * 4.0f);
        out[i++] = 0;
        out[i++] = 0;
    } else {
        out[i++] = MC_RESP_CONTACT_MSG_RECV;
    }
    if (from) memcpy(&out[i], from->pub_key, 6);
    else memset(&out[i], 0, 6);
    i += 6;
    out[i++] = is_flood ? path_len : MC_OUT_PATH_UNKNOWN;
    out[i++] = MC_TXT_TYPE_SIGNED_PLAIN;
    memcpy(&out[i], &timestamp, 4); i += 4;
    // 4-byte sender prefix (upstream `extra` of len 4).
    if (sender_prefix) memcpy(&out[i], sender_prefix, 4);
    else memset(&out[i], 0, 4);
    i += 4;
    size_t tlen = strlen(text);
    if ((size_t)i + tlen > MC_MAX_FRAME_SIZE) tlen = MC_MAX_FRAME_SIZE - (size_t)i;
    memcpy(&out[i], text, tlen); i += (int)tlen;
    queue_push(out, (size_t)i);
    tickle();
}

void mc_companion_on_contact_response(const mc_contact_t *from, const uint8_t *data, uint8_t len,
                                      float snr, uint8_t path_len, bool is_flood) {
    (void)snr; (void)path_len; (void)is_flood;
    if (!from || !data || len < 4) return;
    uint32_t tag;
    memcpy(&tag, data, 4);
    uint8_t out[MC_MAX_FRAME_SIZE];
    int i = 0;
    if (s_pending_status && memcmp(&s_pending_status, from->pub_key, 4) == 0 && len > 4) {
        s_pending_status = 0;
        out[i++] = MC_PUSH_STATUS_RESPONSE;
        out[i++] = 0; // reserved
        memcpy(&out[i], from->pub_key, 6); i += 6;
        memcpy(&out[i], &data[4], len - 4); i += len - 4;
        write_frame(out, (size_t)i);
    } else if (s_pending_telemetry && tag == s_pending_telemetry && len > 4) {
        s_pending_telemetry = 0;
        out[i++] = MC_PUSH_TELEMETRY_RESPONSE;
        out[i++] = 0; // reserved
        memcpy(&out[i], from->pub_key, 6); i += 6;
        memcpy(&out[i], &data[4], len - 4); i += len - 4;
        write_frame(out, (size_t)i);
    } else if (s_pending_req && tag == s_pending_req && len > 4) {
        s_pending_req = 0;
        out[i++] = MC_PUSH_BINARY_RESPONSE;
        out[i++] = 0; // reserved
        memcpy(&out[i], &tag, 4); i += 4; // app matches this to RESP_CODE_SENT.tag
        memcpy(&out[i], &data[4], len - 4); i += len - 4;
        write_frame(out, (size_t)i);
    }
}

void mc_companion_on_channel_data(uint8_t channel_idx, uint16_t data_type,
                                  const uint8_t *data, uint8_t len, float snr,
                                  uint8_t path_len, bool is_flood) {
    if (len > MC_MAX_CHANNEL_DATA_LENGTH) return;
    uint8_t out[MC_MAX_FRAME_SIZE];
    int i = 0;
    out[i++] = MC_RESP_CHANNEL_DATA_RECV;
    out[i++] = (uint8_t)(int8_t)(snr * 4.0f);
    out[i++] = 0;
    out[i++] = 0;
    out[i++] = channel_idx;
    out[i++] = is_flood ? path_len : MC_OUT_PATH_UNKNOWN;
    out[i++] = (uint8_t)(data_type & 0xFF);
    out[i++] = (uint8_t)(data_type >> 8);
    out[i++] = len;
    if (len) { memcpy(&out[i], data, len); i += len; }
    queue_push(out, (size_t)i);
    tickle();
}

void mc_companion_on_advert(const mc_contact_t *contact, bool is_new) {
    if (!contact) return;
    if (is_new) {
        contact_resp(MC_PUSH_NEW_ADVERT, contact);
    } else {
        uint8_t out[1 + MC_PUB_KEY_SIZE];
        out[0] = MC_PUSH_ADVERT;
        memcpy(&out[1], contact->pub_key, MC_PUB_KEY_SIZE);
        write_frame(out, sizeof(out));
    }
}

void mc_companion_on_ack(const mc_contact_t *from, uint32_t ack) {
    (void)from;
    for (int i = 0; i < MC_EXPECTED_ACK; ++i) {
        if (s_ack_table[i].ack == ack && s_ack_table[i].ack != 0) {
            uint8_t out[9];
            out[0] = MC_PUSH_SEND_CONFIRMED;
            memcpy(&out[1], &ack, 4);
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            uint32_t trip = now - s_ack_table[i].sent_ms;
            memcpy(&out[5], &trip, 4);
            write_frame(out, sizeof(out));
            s_ack_table[i].ack = 0;
            break;
        }
    }
}

void mc_companion_on_path_updated(const mc_contact_t *contact) {
    if (!contact) return;
    uint8_t out[1 + MC_PUB_KEY_SIZE];
    out[0] = MC_PUSH_PATH_UPDATED;
    memcpy(&out[1], contact->pub_key, MC_PUB_KEY_SIZE);
    write_frame(out, sizeof(out));
}

void mc_companion_on_contact_deleted(const uint8_t *pub_key) {
    if (!pub_key) return;
    uint8_t out[1 + MC_PUB_KEY_SIZE];
    out[0] = MC_PUSH_CONTACT_DELETED;
    memcpy(&out[1], pub_key, MC_PUB_KEY_SIZE);
    write_frame(out, sizeof(out));
}

// ---------------------------------------------------------------------------
// Frame dispatch
// ---------------------------------------------------------------------------

static void handle_device_query(const uint8_t *f, size_t len) {
    mc_prefs_t *p = mc_mesh_prefs();
    if (len >= 2) s_app_ver = f[1];
    uint8_t out[MC_MAX_FRAME_SIZE];
    int i = 0;
    out[i++] = MC_RESP_DEVICE_INFO;
    out[i++] = MC_FIRMWARE_VER_CODE;
    out[i++] = MC_MAX_CONTACTS / 2;
    out[i++] = MC_MAX_GROUP_CHANNELS;
    uint32_t pin = p->ble_pin;
    memcpy(&out[i], &pin, 4); i += 4;
    strzcpy((char *)&out[i], MC_FIRMWARE_BUILD_DATE, 12); i += 12;
    strzcpy((char *)&out[i], MC_MANUFACTURER_MODEL, 40); i += 40;
    strzcpy((char *)&out[i], MC_FIRMWARE_VERSION, 20); i += 20;
    out[i++] = 0; // client repeat not supported
    out[i++] = p->path_hash_mode;
    write_frame(out, (size_t)i);
}

static void handle_app_start(const uint8_t *f, size_t len) {
    (void)f;
    mc_prefs_t *p = mc_mesh_prefs();
    const mc_identity_t *id = mc_identity_get();
    if (!id || !id->valid) { write_err(MC_ERR_BAD_STATE); return; }
    (void)len;
    s_iter_started = false;

    uint8_t out[MC_MAX_FRAME_SIZE];
    int i = 0;
    out[i++] = MC_RESP_SELF_INFO;
    out[i++] = MC_ADV_TYPE_CHAT;
    out[i++] = (uint8_t)p->tx_dbm;
    out[i++] = 22; // max TX power
    memcpy(&out[i], id->pub_key, MC_PUB_KEY_SIZE); i += MC_PUB_KEY_SIZE;
    memcpy(&out[i], &p->node_lat, 4); i += 4;
    memcpy(&out[i], &p->node_lon, 4); i += 4;
    out[i++] = p->multi_acks;
    out[i++] = p->advert_loc_policy;
    out[i++] = 0; // telemetry modes
    out[i++] = p->manual_add_contacts;
    uint32_t freq = (uint32_t)(p->freq_mhz * 1000.0f);
    memcpy(&out[i], &freq, 4); i += 4;
    uint32_t bw = (uint32_t)(p->bw_khz * 1000.0f);
    memcpy(&out[i], &bw, 4); i += 4;
    out[i++] = p->sf;
    out[i++] = p->cr;
    size_t nlen = strlen(p->node_name);
    if ((size_t)i + nlen > MC_MAX_FRAME_SIZE) nlen = MC_MAX_FRAME_SIZE - (size_t)i;
    memcpy(&out[i], p->node_name, nlen); i += (int)nlen;
    write_frame(out, (size_t)i);
}

static void handle_send_channel_txt(const uint8_t *f, size_t len) {
    if (len < 7) { write_err(MC_ERR_ILLEGAL_ARG); return; }
    uint8_t txt_type = f[1];
    uint8_t channel_idx = f[2];
    uint32_t timestamp;
    memcpy(&timestamp, &f[3], 4);
    // Text is a raw binary tail: no NUL terminator, so pass the frame's real
    // remaining length or strlen() would read past it into stale bytes.
    const char *text = (const char *)&f[7];
    if (txt_type != MC_TXT_TYPE_PLAIN) { write_err(MC_ERR_UNSUPPORTED_CMD); return; }
    if (!mc_mesh_channel(channel_idx)) { write_err(MC_ERR_NOT_FOUND); return; }
    if (mc_mesh_send_group_text_len(channel_idx, text, (int)len - 7, timestamp)) write_ok();
    else write_err(MC_ERR_NOT_FOUND);
}

static void handle_send_channel_data(const uint8_t *f, size_t len) {
    if (len < 4) { write_err(MC_ERR_ILLEGAL_ARG); return; }
    int i = 1;
    uint8_t channel_idx = f[i++];
    uint8_t path_len = f[i++];
    uint8_t path[MC_MAX_PATH_SIZE];
    if (path_len != MC_OUT_PATH_UNKNOWN) {
        if (!mc_packet_is_valid_path_len(path_len)) { write_err(MC_ERR_ILLEGAL_ARG); return; }
        size_t bl = mc_packet_write_path(path, &f[i], path_len);
        if (bl == 0 && (path_len & 63) != 0) { write_err(MC_ERR_ILLEGAL_ARG); return; }
        i += (int)bl;
    }
    if (i + 2 > (int)len) { write_err(MC_ERR_ILLEGAL_ARG); return; }
    uint16_t data_type = (uint16_t)(f[i] | (f[i + 1] << 8)); i += 2;
    const uint8_t *payload = &f[i];
    int payload_len = (int)len - i;
    if (!mc_mesh_channel(channel_idx)) { write_err(MC_ERR_NOT_FOUND); return; }
    if (data_type == MC_DATA_TYPE_RESERVED) { write_err(MC_ERR_ILLEGAL_ARG); return; }
    if (payload_len > MC_MAX_CHANNEL_DATA_LENGTH) { write_err(MC_ERR_ILLEGAL_ARG); return; }
    if (mc_mesh_send_group_data(channel_idx, path_len, path, data_type, payload, payload_len)) write_ok();
    else write_err(MC_ERR_TABLE_FULL);
}

static void handle_send_txt(const uint8_t *f, size_t len) {
    if (len < 14) { write_err(MC_ERR_ILLEGAL_ARG); return; }
    int i = 1;
    uint8_t txt_type = f[i++];
    uint8_t attempt = f[i++];
    uint32_t msg_timestamp;
    memcpy(&msg_timestamp, &f[i], 4); i += 4;
    const uint8_t *prefix = &f[i]; i += 6;
    // Raw binary tail after the 6-byte prefix: length is what remains in the
    // frame, not a NUL-terminated string.
    const char *text = (const char *)&f[i];
    int text_len = (int)len - i;
    if (text_len < 0) text_len = 0;
    mc_contact_t *recipient = mc_mesh_find_contact_pubkey(prefix, 6);
    if (!recipient || (txt_type != MC_TXT_TYPE_PLAIN && txt_type != MC_TXT_TYPE_CLI_DATA)) {
        write_err(recipient ? MC_ERR_UNSUPPORTED_CMD : MC_ERR_NOT_FOUND);
        return;
    }
    if (txt_type == MC_TXT_TYPE_CLI_DATA) {
        // Upstream uses the node RTC (unique) to avoid replay protection and
        // expects no ACK.
        msg_timestamp = mc_mesh_now_unique();
    }
    uint32_t expected_ack = 0, est_timeout = 0;
    int rc = mc_mesh_send_direct_text_len(recipient, msg_timestamp, attempt, txt_type, text,
                                          text_len, &expected_ack, &est_timeout);
    if (rc == MC_MSG_SEND_FAILED) { write_err(MC_ERR_TABLE_FULL); return; }
    if (expected_ack) ack_table_add(expected_ack, recipient);
    uint8_t out[10];
    out[0] = MC_RESP_SENT;
    out[1] = (rc == MC_MSG_SEND_SENT_FLOOD) ? 1 : 0;
    memcpy(&out[2], &expected_ack, 4);
    memcpy(&out[6], &est_timeout, 4);
    write_frame(out, sizeof(out));
}

static void handle_get_contacts(const uint8_t *f, size_t len) {
    if (s_iter_started) { write_err(MC_ERR_BAD_STATE); return; }
    s_iter_filter_since = 0;
    if (len >= 5) memcpy(&s_iter_filter_since, &f[1], 4);
    uint8_t reply[5];
    reply[0] = MC_RESP_CONTACTS_START;
    uint32_t count = (uint32_t)mc_mesh_contact_count();
    memcpy(&reply[1], &count, 4);
    write_frame(reply, sizeof(reply));
    s_iter_idx = 0;
    s_most_recent_lastmod = 0;
    s_iter_started = true;
}

static void handle_get_channel(const uint8_t *f, size_t len) {
    if (len < 2) { write_err(MC_ERR_ILLEGAL_ARG); return; }
    uint8_t idx = f[1];
    const mc_channel_t *ch = mc_mesh_channel(idx);
    if (!ch) { write_err(MC_ERR_NOT_FOUND); return; }
    uint8_t out[1 + 1 + 32 + 16];
    int i = 0;
    out[i++] = MC_RESP_CHANNEL_INFO;
    out[i++] = idx;
    strzcpy((char *)&out[i], ch->name, 32); i += 32;
    memcpy(&out[i], ch->secret, 16); i += 16; // 128-bit only
    write_frame(out, (size_t)i);
}

static void handle_set_channel(const uint8_t *f, size_t len) {
    if (len >= 2 + 32 + 32) { write_err(MC_ERR_UNSUPPORTED_CMD); return; }
    if (len < 2 + 32 + 16) { write_err(MC_ERR_ILLEGAL_ARG); return; }
    uint8_t idx = f[1];
    mc_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    memcpy(ch.name, &f[2], 32);
    ch.name[31] = 0; // emulate upstream StrHelper::strncpy truncation
    memcpy(ch.secret, &f[2 + 32], 16);
    if (mc_mesh_set_channel(idx, &ch)) write_ok();
    else write_err(MC_ERR_NOT_FOUND);
}

static void handle_get_batt(const uint8_t *f, size_t len) {
    (void)f; (void)len;
    uint8_t reply[11];
    int i = 0;
    reply[i++] = MC_RESP_BATT_AND_STORAGE;
    uint16_t mv = 3700; // no fuel gauge exposed to this layer yet
    uint32_t used = 0, total = 0;
    memcpy(&reply[i], &mv, 2); i += 2;
    memcpy(&reply[i], &used, 4); i += 4;
    memcpy(&reply[i], &total, 4); i += 4;
    write_frame(reply, (size_t)i);
}

static void handle_device_time(const uint8_t *f, size_t len, bool set) {
    if (set) {
        if (len < 5) { write_err(MC_ERR_ILLEGAL_ARG); return; }
        uint32_t secs;
        memcpy(&secs, &f[1], 4);
        if (secs < mc_mesh_now()) { write_err(MC_ERR_ILLEGAL_ARG); return; }
        mc_mesh_set_time(secs);
        write_ok();
    } else {
        uint8_t reply[5];
        reply[0] = MC_RESP_CURR_TIME;
        uint32_t now = mc_mesh_now();
        memcpy(&reply[1], &now, 4);
        write_frame(reply, sizeof(reply));
    }
}

static void handle_set_radio_params(const uint8_t *f, size_t len) {
    if (len < 10) { write_err(MC_ERR_ILLEGAL_ARG); return; }
    uint32_t freq, bw;
    memcpy(&freq, &f[1], 4);
    memcpy(&bw, &f[5], 4);
    uint8_t sf = f[9];
    uint8_t cr = (len > 10) ? f[10] : 0;
    if (cr == 0) cr = mc_mesh_prefs()->cr;

    bool was_running = mc_manager_is_running();
    mc_prefs_t prev = *mc_mesh_prefs();

    if (!mc_mesh_apply_radio((float)freq / 1000.0f, (float)bw / 1000.0f, sf, cr)) {
        write_err(MC_ERR_ILLEGAL_ARG);
        return;
    }
    // Re-tune the live radio in place. A full stop/start here would drop the
    // BLE connection (and re-run the Wi-Fi AP restore), so only the SX126x is
    // restarted; the app link stays up.
    if (was_running && !mc_manager_reconfigure_radio()) {
        // Roll back to the previous tuning and report the command as rejected.
        *mc_mesh_prefs() = prev;
        mc_mesh_save_prefs();
        (void)mc_manager_reconfigure_radio();
        write_err(MC_ERR_ILLEGAL_ARG);
        return;
    }
    write_ok();
}

static void handle_set_tx_power(const uint8_t *f, size_t len) {
    if (len < 2) { write_err(MC_ERR_ILLEGAL_ARG); return; }
    int8_t p = (int8_t)f[1];
    if (p < -9 || p > 22) { write_err(MC_ERR_ILLEGAL_ARG); return; }
    mc_mesh_set_tx_power(p);
    write_ok();
}

static void handle_add_update_contact(const uint8_t *f, size_t len) {
    if (len < 1 + MC_PUB_KEY_SIZE + 2 + 1) { write_err(MC_ERR_ILLEGAL_ARG); return; }
    uint8_t *pub = (uint8_t *)&f[1];
    mc_contact_t *c = mc_mesh_find_contact_pubkey(pub, MC_PUB_KEY_SIZE);
    uint32_t last_mod = mc_mesh_now();
    if (c) {
        update_contact_from_frame(c, &last_mod, f, (int)len);
        c->lastmod = last_mod;
        c->shared_secret_valid = false;
        mc_mesh_save_contacts();
        write_ok();
    } else {
        mc_contact_t nc;
        memset(&nc, 0, sizeof(nc));
        update_contact_from_frame(&nc, &last_mod, f, (int)len);
        nc.lastmod = last_mod;
        if (mc_mesh_add_contact(&nc)) {
            mc_mesh_save_contacts();
            write_ok();
        } else {
            write_err(MC_ERR_TABLE_FULL);
        }
    }
}

static void handle_remove_contact(const uint8_t *f, size_t len) {
    if (len < 1 + MC_PUB_KEY_SIZE) { write_err(MC_ERR_ILLEGAL_ARG); return; }
    mc_contact_t *c = mc_mesh_find_contact_pubkey(&f[1], MC_PUB_KEY_SIZE);
    if (c && mc_mesh_remove_contact(c)) {
        mc_mesh_save_contacts();
        write_ok();
    } else {
        write_err(MC_ERR_NOT_FOUND);
    }
}

static void handle_contact_by_key(const uint8_t *f, size_t len) {
    if (len < 1 + MC_PUB_KEY_SIZE) { write_err(MC_ERR_ILLEGAL_ARG); return; }
    mc_contact_t *c = mc_mesh_find_contact_pubkey(&f[1], MC_PUB_KEY_SIZE);
    if (c) contact_resp(MC_RESP_CONTACT, c);
    else write_err(MC_ERR_NOT_FOUND);
}

static void handle_set_advert_name(const uint8_t *f, size_t len) {
    if (len < 2) { write_err(MC_ERR_ILLEGAL_ARG); return; }
    size_t n = len - 1;
    char name[32];
    if (n > sizeof(name) - 1) n = sizeof(name) - 1;
    memcpy(name, &f[1], n);
    name[n] = 0;
    if (mc_mesh_set_node_name(name)) write_ok();
    else write_err(MC_ERR_FILE_IO_ERROR);
}

static void handle_set_advert_latlon(const uint8_t *f, size_t len) {
    if (len < 9) { write_err(MC_ERR_ILLEGAL_ARG); return; }
    int32_t lat, lon;
    memcpy(&lat, &f[1], 4);
    memcpy(&lon, &f[5], 4);
    if (lat <= 90 * 1000000 && lat >= -90 * 1000000 &&
        lon <= 180 * 1000000 && lon >= -180 * 1000000) {
        mc_prefs_t *p = mc_mesh_prefs();
        p->node_lat = lat;
        p->node_lon = lon;
        mc_mesh_save_prefs();
        write_ok();
    } else {
        write_err(MC_ERR_ILLEGAL_ARG);
    }
}

static void handle_set_other_params(const uint8_t *f, size_t len) {
    mc_prefs_t *p = mc_mesh_prefs();
    if (len < 2) { write_err(MC_ERR_ILLEGAL_ARG); return; }
    p->manual_add_contacts = f[1];
    if (len >= 3) {
        // telemetry mode byte ignored (no telemetry)
        if (len >= 4) {
            p->advert_loc_policy = f[3];
            if (len >= 5) p->multi_acks = f[4];
        }
    }
    mc_mesh_save_prefs();
    write_ok();
}

static void handle_get_stats(const uint8_t *f, size_t len) {
    if (len < 2) { write_err(MC_ERR_ILLEGAL_ARG); return; }
    uint8_t type = f[1];
    mc_status_t st;
    mc_manager_get_status(&st);
    uint8_t out[40];
    int i = 0;
    out[i++] = MC_RESP_STATS;
    out[i++] = type;
    if (type == MC_STATS_TYPE_CORE) {
        uint16_t mv = 3700;
        uint32_t uptime = (uint32_t)(esp_timer_get_time() / 1000000);
        uint16_t err = 0;
        memcpy(&out[i], &mv, 2); i += 2;
        memcpy(&out[i], &uptime, 4); i += 4;
        memcpy(&out[i], &err, 2); i += 2;
        out[i++] = 0; // outbound queue length
    } else if (type == MC_STATS_TYPE_RADIO) {
        int16_t noise = -120;
        int8_t rssi = (int8_t)st.last_rssi;
        int8_t snr4 = (int8_t)(st.last_snr * 4.0f);
        uint32_t tx_air = 0, rx_air = 0;
        memcpy(&out[i], &noise, 2); i += 2;
        out[i++] = rssi;
        out[i++] = snr4;
        memcpy(&out[i], &tx_air, 4); i += 4;
        memcpy(&out[i], &rx_air, 4); i += 4;
    } else if (type == MC_STATS_TYPE_PACKETS) {
        uint32_t recv = st.rx_ok;
        uint32_t sent = st.tx_ok;
        uint32_t zero = 0;
        uint32_t errors = st.rx_bad;
        memcpy(&out[i], &recv, 4); i += 4;
        memcpy(&out[i], &sent, 4); i += 4;
        memcpy(&out[i], &zero, 4); i += 4; // sent flood
        memcpy(&out[i], &sent, 4); i += 4; // sent direct
        memcpy(&out[i], &zero, 4); i += 4; // recv flood
        memcpy(&out[i], &recv, 4); i += 4; // recv direct
        memcpy(&out[i], &errors, 4); i += 4;
    } else {
        write_err(MC_ERR_ILLEGAL_ARG);
        return;
    }
    write_frame(out, (size_t)i);
}

void mc_companion_handle_frame(const uint8_t *frame, size_t len) {
    if (!frame || len == 0) return;
    uint8_t cmd = frame[0];
    switch (cmd) {
        case MC_CMD_APP_START:
            handle_app_start(frame, len);
            break;
        case MC_CMD_DEVICE_QUERY:
            handle_device_query(frame, len);
            break;
        case MC_CMD_SEND_CHANNEL_TXT_MSG:
            handle_send_channel_txt(frame, len);
            break;
        case MC_CMD_SEND_CHANNEL_DATA:
            handle_send_channel_data(frame, len);
            break;
        case MC_CMD_SEND_TXT_MSG:
            handle_send_txt(frame, len);
            break;
        case MC_CMD_GET_CONTACTS:
            handle_get_contacts(frame, len);
            break;
        case MC_CMD_GET_CHANNEL:
            handle_get_channel(frame, len);
            break;
        case MC_CMD_SET_CHANNEL:
            handle_set_channel(frame, len);
            break;
        case MC_CMD_GET_BATT_AND_STORAGE:
            handle_get_batt(frame, len);
            break;
        case MC_CMD_GET_DEVICE_TIME:
            handle_device_time(frame, len, false);
            break;
        case MC_CMD_SET_DEVICE_TIME:
            handle_device_time(frame, len, true);
            break;
        case MC_CMD_SET_RADIO_PARAMS:
            handle_set_radio_params(frame, len);
            break;
        case MC_CMD_SET_RADIO_TX_POWER:
            handle_set_tx_power(frame, len);
            break;
        case MC_CMD_ADD_UPDATE_CONTACT:
            handle_add_update_contact(frame, len);
            break;
        case MC_CMD_REMOVE_CONTACT:
            handle_remove_contact(frame, len);
            break;
        case MC_CMD_GET_CONTACT_BY_KEY:
            handle_contact_by_key(frame, len);
            break;
        case MC_CMD_SET_ADVERT_NAME:
            handle_set_advert_name(frame, len);
            break;
        case MC_CMD_SET_ADVERT_LATLON:
            handle_set_advert_latlon(frame, len);
            break;
        case MC_CMD_SET_OTHER_PARAMS:
            handle_set_other_params(frame, len);
            break;
        case MC_CMD_GET_STATS:
            handle_get_stats(frame, len);
            break;
        case MC_CMD_SEND_SELF_ADVERT: {
            bool flood = (len >= 2 && frame[1] == 1);
            if (mc_mesh_send_advert(flood)) write_ok();
            else write_err(MC_ERR_TABLE_FULL);
            break;
        }
        case MC_CMD_RESET_PATH: {
            if (len < 1 + MC_PUB_KEY_SIZE) { write_err(MC_ERR_ILLEGAL_ARG); break; }
            mc_contact_t *c = mc_mesh_find_contact_pubkey(&frame[1], MC_PUB_KEY_SIZE);
            if (c) {
                c->out_path_len = MC_OUT_PATH_UNKNOWN;
                mc_mesh_save_contacts();
                write_ok();
            } else {
                write_err(MC_ERR_NOT_FOUND);
            }
            break;
        }
        case MC_CMD_SHARE_CONTACT: {
            if (len < 1 + MC_PUB_KEY_SIZE) { write_err(MC_ERR_ILLEGAL_ARG); break; }
            mc_contact_t *c = mc_mesh_find_contact_pubkey(&frame[1], MC_PUB_KEY_SIZE);
            if (!c) { write_err(MC_ERR_NOT_FOUND); break; }
            if (mc_mesh_share_contact(c)) write_ok();
            else write_err(MC_ERR_TABLE_FULL); // no cached advert blob
            break;
        }
        case MC_CMD_EXPORT_CONTACT: {
            uint8_t out[MC_MAX_FRAME_SIZE];
            uint8_t n = 0;
            if (len < 1 + MC_PUB_KEY_SIZE) {
                n = mc_mesh_export_self(&out[1]);
                if (n == 0) { write_err(MC_ERR_TABLE_FULL); break; }
            } else {
                mc_contact_t *c = mc_mesh_find_contact_pubkey(&frame[1], MC_PUB_KEY_SIZE);
                if (!c) { write_err(MC_ERR_NOT_FOUND); break; }
                n = mc_mesh_export_contact(c, &out[1]);
                if (n == 0) { write_err(MC_ERR_NOT_FOUND); break; }
            }
            out[0] = MC_RESP_EXPORT_CONTACT;
            write_frame(out, (size_t)n + 1);
            break;
        }
        case MC_CMD_IMPORT_CONTACT:
            if (len > 2 + MC_PUB_KEY_SIZE + 64 && mc_mesh_import_contact(&frame[1], (uint8_t)(len - 1))) {
                mc_mesh_save_contacts();
                write_ok();
            } else {
                write_err(MC_ERR_ILLEGAL_ARG);
            }
            break;
        case MC_CMD_SYNC_NEXT_MESSAGE: {
            uint8_t out[MC_MAX_FRAME_SIZE];
            uint16_t n = 0;
            if (queue_pop(out, &n)) {
                write_frame(out, n);
            } else {
                out[0] = MC_RESP_NO_MORE_MESSAGES;
                write_frame(out, 1);
            }
            break;
        }
        case MC_CMD_SIGN_START: {
            mc_free(s_sign_buf);
            s_sign_buf = (uint8_t *)mc_alloc(MC_SIGN_BUF_LEN);
            s_sign_len = 0;
            uint8_t out[6];
            out[0] = MC_RESP_SIGN_START;
            out[1] = 0;
            uint32_t cap = MC_SIGN_BUF_LEN;
            memcpy(&out[2], &cap, 4);
            write_frame(out, sizeof(out));
            break;
        }
        case MC_CMD_SIGN_DATA: {
            if (!s_sign_buf || len <= 1) {
                write_err(s_sign_buf ? MC_ERR_ILLEGAL_ARG : MC_ERR_BAD_STATE);
                break;
            }
            if (s_sign_len + (len - 1) > MC_SIGN_BUF_LEN) { write_err(MC_ERR_TABLE_FULL); break; }
            memcpy(&s_sign_buf[s_sign_len], &frame[1], len - 1);
            s_sign_len += len - 1;
            write_ok();
            break;
        }
        case MC_CMD_SIGN_FINISH: {
            if (!s_sign_buf) { write_err(MC_ERR_BAD_STATE); break; }
            uint8_t out[1 + MC_SIGNATURE_SIZE];
            out[0] = MC_RESP_SIGNATURE;
            mc_mesh_sign(s_sign_buf, s_sign_len, &out[1]);
            mc_free(s_sign_buf);
            s_sign_buf = NULL;
            s_sign_len = 0;
            write_frame(out, sizeof(out));
            break;
        }
        case MC_CMD_EXPORT_PRIVATE_KEY: {
            uint8_t out[1 + MC_PRV_KEY_SIZE];
            if (mc_identity_export(&out[1])) {
                out[0] = MC_RESP_PRIVATE_KEY;
                write_frame(out, sizeof(out));
            } else {
                write_err(MC_ERR_BAD_STATE);
            }
            break;
        }
        case MC_CMD_IMPORT_PRIVATE_KEY:
            if (len >= 1 + MC_PRV_KEY_SIZE && mc_identity_validate_private(&frame[1])) {
                mc_identity_import(&frame[1]);
                write_ok();
            } else {
                write_err(MC_ERR_ILLEGAL_ARG);
            }
            break;
        case MC_CMD_SET_DEVICE_PIN: {
            if (len < 5) { write_err(MC_ERR_ILLEGAL_ARG); break; }
            uint32_t pin;
            memcpy(&pin, &frame[1], 4);
            if (pin == 0 || (pin >= 100000 && pin <= 999999)) {
                mc_mesh_prefs()->ble_pin = pin;
                mc_mesh_save_prefs();
                write_ok();
            } else {
                write_err(MC_ERR_ILLEGAL_ARG);
            }
            break;
        }
        case MC_CMD_SET_PATH_HASH_MODE:
            if (len >= 3 && frame[1] == 0) {
                if (frame[2] >= 3) { write_err(MC_ERR_ILLEGAL_ARG); break; }
                mc_mesh_prefs()->path_hash_mode = frame[2];
                mc_mesh_save_prefs();
                write_ok();
            } else {
                write_err(MC_ERR_ILLEGAL_ARG);
            }
            break;
        case MC_CMD_GET_CUSTOM_VARS: {
            uint8_t out[1];
            out[0] = MC_RESP_CUSTOM_VARS;
            write_frame(out, 1);
            break;
        }
        case MC_CMD_GET_TUNING_PARAMS: {
            uint8_t out[9];
            int i = 0;
            uint32_t rx = 0, af = 0;
            out[i++] = MC_RESP_TUNING_PARAMS;
            memcpy(&out[i], &rx, 4); i += 4;
            memcpy(&out[i], &af, 4); i += 4;
            write_frame(out, (size_t)i);
            break;
        }
        case MC_CMD_SET_TUNING_PARAMS:
            write_ok(); // tuning factors unused
            break;
        case MC_CMD_SET_AUTOADD_CONFIG: {
            mc_prefs_t *p = mc_mesh_prefs();
            if (len >= 2) p->autoadd_config = frame[1];
            if (len >= 3) p->autoadd_max_hops = frame[2] > 64 ? 64 : frame[2];
            mc_mesh_save_prefs();
            write_ok();
            break;
        }
        case MC_CMD_GET_AUTOADD_CONFIG: {
            mc_prefs_t *p = mc_mesh_prefs();
            uint8_t out[3] = {MC_RESP_AUTOADD_CONFIG, p->autoadd_config, p->autoadd_max_hops};
            write_frame(out, sizeof(out));
            break;
        }
        case MC_CMD_GET_ALLOWED_REPEAT_FREQ: {
            uint8_t out[1];
            out[0] = MC_RESP_ALLOWED_REPEAT_FREQ;
            write_frame(out, 1);
            break;
        }
        case MC_CMD_SET_FLOOD_SCOPE_KEY: {
            // frame[1]: 0 = set/clear the send-scope override, 1 = force unscoped.
            if (len >= 2 && frame[1] == 1) {
                mc_mesh_set_send_unscoped(true);
                write_ok();
            } else if (len >= 2 && frame[1] == 0) {
                mc_mesh_set_send_scope((len >= 2 + 16) ? &frame[2] : NULL);
                write_ok();
            } else {
                write_err(MC_ERR_ILLEGAL_ARG);
            }
            break;
        }
        case MC_CMD_SET_DEFAULT_FLOOD_SCOPE: {
            // name[31] + key[16] when present, otherwise clear the default scope.
            if (len >= 1 + 31 + 16) {
                char name[32];
                memcpy(name, &frame[1], 31);
                name[31] = 0;
                if (name[0]) {
                    mc_mesh_set_default_scope(name, &frame[1 + 31]);
                    write_ok();
                } else {
                    write_err(MC_ERR_ILLEGAL_ARG);
                }
            } else {
                mc_mesh_set_default_scope(NULL, NULL);
                write_ok();
            }
            break;
        }
        case MC_CMD_GET_DEFAULT_FLOOD_SCOPE: {
            char name[31];
            uint8_t key[16];
            bool have = mc_mesh_get_default_scope(name, key);
            uint8_t out[1 + 31 + 16];
            out[0] = MC_RESP_DEFAULT_FLOOD_SCOPE;
            if (have) {
                memset(&out[1], 0, 31);
                memcpy(&out[1], name, 30);
                memcpy(&out[1 + 31], key, 16);
                write_frame(out, sizeof(out));
            } else {
                write_frame(out, 1); // no name/key means null
            }
            break;
        }
        case MC_CMD_HAS_CONNECTION:
            write_err(MC_ERR_NOT_FOUND);
            break;
        case MC_CMD_LOGOUT:
            write_ok();
            break;
        case MC_CMD_SEND_RAW_PACKET:
            if (len >= 4 && mc_mesh_send_raw_frame(&frame[2], (uint8_t)(len - 2), frame[1])) {
                write_ok();
            } else {
                write_err(MC_ERR_ILLEGAL_ARG);
            }
            break;
        case MC_CMD_REBOOT:
            if (len >= 7 && memcmp(&frame[1], "reboot", 6) == 0) {
                esp_restart();
            } else {
                write_err(MC_ERR_ILLEGAL_ARG);
            }
            break;
        case MC_CMD_SEND_STATUS_REQ: {
            if (len < 1 + MC_PUB_KEY_SIZE) { write_err(MC_ERR_ILLEGAL_ARG); break; }
            mc_contact_t *c = mc_mesh_find_contact_pubkey(&frame[1], MC_PUB_KEY_SIZE);
            if (!c) { write_err(MC_ERR_NOT_FOUND); break; }
            uint32_t tag = 0, to = 0;
            int r = mc_mesh_send_request_type(c, MC_REQ_TYPE_GET_STATUS, &tag, &to);
            if (r == MC_MSG_SEND_FAILED) { write_err(MC_ERR_TABLE_FULL); break; }
            clear_pending_reqs();
            memcpy(&s_pending_status, c->pub_key, 4); // legacy matching scheme
            write_sent(tag, r, to);
            break;
        }
        case MC_CMD_SEND_TELEMETRY_REQ: {
            // Only the peer form (tag + pubkey) is supported; the 'self'
            // telemetry query has no local sensor payload.
            if (len < 4 + MC_PUB_KEY_SIZE) { write_err(MC_ERR_UNSUPPORTED_CMD); break; }
            mc_contact_t *c = mc_mesh_find_contact_pubkey(&frame[4], MC_PUB_KEY_SIZE);
            if (!c) { write_err(MC_ERR_NOT_FOUND); break; }
            uint32_t tag = 0, to = 0;
            int r = mc_mesh_send_request_type(c, MC_REQ_TYPE_GET_TELEMETRY, &tag, &to);
            if (r == MC_MSG_SEND_FAILED) { write_err(MC_ERR_TABLE_FULL); break; }
            clear_pending_reqs();
            s_pending_telemetry = tag;
            write_sent(tag, r, to);
            break;
        }
        case MC_CMD_SEND_BINARY_REQ: {
            if (len < 2 + MC_PUB_KEY_SIZE) { write_err(MC_ERR_ILLEGAL_ARG); break; }
            mc_contact_t *c = mc_mesh_find_contact_pubkey(&frame[1], MC_PUB_KEY_SIZE);
            if (!c) { write_err(MC_ERR_NOT_FOUND); break; }
            uint32_t tag = 0, to = 0;
            int r = mc_mesh_send_request(c, &frame[1 + MC_PUB_KEY_SIZE],
                                         (uint8_t)(len - (1 + MC_PUB_KEY_SIZE)), &tag, &to);
            if (r == MC_MSG_SEND_FAILED) { write_err(MC_ERR_TABLE_FULL); break; }
            clear_pending_reqs();
            s_pending_req = tag;
            write_sent(tag, r, to);
            break;
        }
        // Explicitly unsupported (companion-only feature set).
        case MC_CMD_SEND_RAW_DATA:
        case MC_CMD_SEND_LOGIN:
        case MC_CMD_SEND_TRACE_PATH:
        case MC_CMD_FACTORY_RESET:
        case MC_CMD_SEND_PATH_DISCOVERY_REQ:
        case MC_CMD_SEND_CONTROL_DATA:
        case MC_CMD_SEND_ANON_REQ:
        case MC_CMD_GET_ADVERT_PATH:
        case MC_CMD_SET_CUSTOM_VAR:
            write_err(MC_ERR_UNSUPPORTED_CMD);
            break;
        default:
            write_err(MC_ERR_UNSUPPORTED_CMD);
            break;
    }
}

// ---------------------------------------------------------------------------
// Iterator pump + lifecycle
// ---------------------------------------------------------------------------

void mc_companion_loop(void) {
    if (!s_iter_started) return;
    int n = mc_mesh_contact_count();
    if (s_iter_idx < n) {
        const mc_contact_t *c = mc_mesh_contact_at(s_iter_idx);
        s_iter_idx++;
        if (c && c->lastmod > s_iter_filter_since) {
            contact_resp(MC_RESP_CONTACT, c);
            if (c->lastmod > s_most_recent_lastmod) s_most_recent_lastmod = c->lastmod;
        }
    } else {
        uint8_t out[5];
        out[0] = MC_RESP_END_OF_CONTACTS;
        memcpy(&out[1], &s_most_recent_lastmod, 4);
        write_frame(out, sizeof(out));
        s_iter_started = false;
    }
}

void mc_companion_set_connected(bool connected) {
    s_connected = connected;
    if (!connected) {
        s_iter_started = false;
        clear_pending_reqs();
        if (s_sign_buf) { mc_free(s_sign_buf); s_sign_buf = NULL; s_sign_len = 0; }
    } else {
        s_app_ver = 0;
        s_iter_started = false;
    }
}

bool mc_companion_is_connected(void) { return s_connected; }

bool mc_companion_has_data(void) { return s_queue && s_queue_len > 0; }

bool mc_companion_init(void) {
    if (!s_queue) {
        s_queue = mc_alloc(MC_OFFLINE_QUEUE_SIZE * sizeof(mc_queue_entry_t));
        if (!s_queue) return false;
    } else {
        memset(s_queue, 0, MC_OFFLINE_QUEUE_SIZE * sizeof(mc_queue_entry_t));
    }
    s_connected = false;
    s_app_ver = 0;
    s_iter_started = false;
    s_iter_idx = 0;
    s_queue_len = 0;
    s_ack_next = 0;
    s_sign_buf = NULL;
    s_sign_len = 0;
    clear_pending_reqs();
    memset(s_ack_table, 0, sizeof(s_ack_table));
    return true;
}

void mc_companion_deinit(void) {
    s_connected = false;
    s_iter_started = false;
    s_queue_len = 0;
    clear_pending_reqs();
    mc_free(s_queue);
    s_queue = NULL;
    mc_free(s_sign_buf);
    s_sign_buf = NULL;
    s_sign_len = 0;
}

#else
typedef int meshcore_companion_stub_guard;
#endif // CONFIG_HAS_MESHCORE
