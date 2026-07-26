#include "managers/peer_storage_manager.h"

#include "core/esp_comm_manager.h"
#include "core/glog.h"
#include "managers/sd_card_manager.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PEER_STORAGE_TAG "peer_storage"

/* ---- wire format ------------------------------------------------------ */
/*
 * Frames are ASCII text, NUL-terminated, sent on COMM_STREAM_CHANNEL_STORAGE.
 * The first byte of every packet is the frame type to keep the peer parser
 * branch-free. We deliberately avoid the process-wide response callback slot
 * (contended by peer_ota_manager) and instead receive ack frames back on the
 * SAME stream channel via a registered handler that signals a semaphore.
 *
 * Frame grammar (caller -> peer):
 *   "OPEN|<mode>|<path>"          mode in {"w","a","wb","ab"}
 *   "WRITE|<handle>|<nbytes>"     next STREAM packet on this channel is raw
 *   "CLOSE|<handle>"
 *   "MKDIR|<path>"
 *
 * Ack grammar (peer -> caller):
 *   "OK|<handle>"                 OPEN success
 *   "OK"                          WRITE/CLOSE/MKDIR success
 *   "ERR|<code>"                  any failure
 */

#define PEER_STORAGE_MAX_PATH 128
#define PEER_STORAGE_MAX_FRAME 128
#define PEER_STORAGE_MAX_WRITE_PAYLOAD 4096
#define PEER_STORAGE_TIMEOUT_MS 4000
#define PEER_STORAGE_SEND_WAIT_MS 50

/* Peer-side file handle table. Two open files at once is plenty for the
 * current consumer set (GPS CSV + one Ethernet scan, or a scan + manifest). */
#define PEER_STORAGE_MAX_OPEN 4

typedef struct {
    bool in_use;
    FILE *fp;
} peer_storage_slot_t;

/* ---- caller-side state ---- */
static SemaphoreHandle_t s_caller_mutex = NULL;
static SemaphoreHandle_t s_ack_sem = NULL;
static char s_ack_buf[64];
static size_t s_ack_len;
static bool s_waiting_for_ack;
static bool s_next_packet_is_raw;     /* set by WRITE so the rx cb knows */
static size_t s_raw_expected;        /* to skip parsing the next packet */

/* ---- peer-side state ---- */
static peer_storage_slot_t s_peer_slots[PEER_STORAGE_MAX_OPEN];

/* ---- shared helpers --------------------------------------------------- */

static bool peer_storage_is_local_sd_available(void) {
    return sd_card_manager.is_initialized;
}

bool peer_storage_manager_is_peer(void) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    /* The board that owns the SD on the GhostLink pair. Today only the
     * somethingsomething (Banshee C5) build has both a writable SD slot and
     * a peer (somethingsomething2) that lacks one. */
    return strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0;
#else
    return false;
#endif
}

bool peer_storage_manager_is_client(void) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    /* The headless peer that has no local SD. Add future SD-less boards
     * here when they join the GhostLink pair. */
    return strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething2") == 0;
#else
    return false;
#endif
}

/* ---- caller-side ack path -------------------------------------------- */

static void peer_storage_client_rx_cb(uint8_t channel, const uint8_t *data,
                                       size_t length, void *user_data) {
    (void)channel;
    (void)user_data;
    if (!s_waiting_for_ack || length == 0) return;

    /* The peer only ever sends us ack frames on this channel, never raw
     * bytes, so we don't need the s_next_packet_is_raw guard here. */
    size_t copy = length;
    if (copy > sizeof(s_ack_buf) - 1) copy = sizeof(s_ack_buf) - 1;
    memcpy(s_ack_buf, data, copy);
    s_ack_buf[copy] = '\0';
    s_ack_len = copy;
    s_waiting_for_ack = false;
    xSemaphoreGive(s_ack_sem);
}

static peer_storage_err_t peer_storage_send_frame(const char *frame) {
    if (!esp_comm_manager_is_connected()) return PEER_STORAGE_ERR_NOT_CONNECTED;
    size_t len = strlen(frame);
    if (len >= PEER_STORAGE_MAX_FRAME) return PEER_STORAGE_ERR_BAD_ARG;
    /* +1 to carry the NUL so the peer's strtok end-of-frame logic works. */
    return esp_comm_manager_send_stream_wait(COMM_STREAM_CHANNEL_STORAGE,
                                             (const uint8_t *)frame, len + 1,
                                             PEER_STORAGE_SEND_WAIT_MS)
           ? PEER_STORAGE_OK
           : PEER_STORAGE_ERR_PROTOCOL;
}

static peer_storage_err_t peer_storage_send_raw(const uint8_t *data, size_t len) {
    if (!esp_comm_manager_is_connected()) return PEER_STORAGE_ERR_NOT_CONNECTED;
    return esp_comm_manager_send_stream_wait(COMM_STREAM_CHANNEL_STORAGE,
                                             data, len, PEER_STORAGE_SEND_WAIT_MS)
           ? PEER_STORAGE_OK
           : PEER_STORAGE_ERR_PROTOCOL;
}

static peer_storage_err_t peer_storage_wait_ack(char *out, size_t out_len) {
    if (!s_ack_sem) return PEER_STORAGE_ERR_NO_MEM;
    /* drain stale signal */
    xSemaphoreTake(s_ack_sem, 0);
    s_waiting_for_ack = true;
    s_ack_buf[0] = '\0';
    s_ack_len = 0;

    bool got = xSemaphoreTake(s_ack_sem, pdMS_TO_TICKS(PEER_STORAGE_TIMEOUT_MS)) == pdTRUE;
    s_waiting_for_ack = false;
    if (!got) return PEER_STORAGE_ERR_TIMEOUT;

    if (out && out_len) {
        size_t n = s_ack_len;
        if (n >= out_len) n = out_len - 1;
        memcpy(out, s_ack_buf, n);
        out[n] = '\0';
    }
    if (strncmp(s_ack_buf, "OK", 2) == 0) return PEER_STORAGE_OK;
    if (strncmp(s_ack_buf, "ERR", 3) == 0) return PEER_STORAGE_ERR_PEER;
    return PEER_STORAGE_ERR_PROTOCOL;
}

/* ---- caller-side session --------------------------------------------- */

static peer_storage_err_t peer_storage_ensure_caller_init(void) {
    if (!s_caller_mutex) {
        s_caller_mutex = xSemaphoreCreateMutex();
        if (!s_caller_mutex) return PEER_STORAGE_ERR_NO_MEM;
    }
    if (!s_ack_sem) {
        s_ack_sem = xSemaphoreCreateBinary();
        if (!s_ack_sem) return PEER_STORAGE_ERR_NO_MEM;
    }
    return PEER_STORAGE_OK;
}

peer_storage_err_t peer_storage_begin(void) {
    peer_storage_err_t e = peer_storage_ensure_caller_init();
    if (e != PEER_STORAGE_OK) return e;
    /* Serialize with other callers (peer_ota uses a different mechanism, but
     * two storage sessions on the same task would race the ack state). */
    xSemaphoreTake(s_caller_mutex, portMAX_DELAY);

    /* On the SD-less client we register our rx cb so we can receive acks.
     * Register once; esp_comm_manager_register_stream_handler is idempotent
     * in practice (it just overwrites the slot). */
    if (peer_storage_manager_is_client() && !peer_storage_is_local_sd_available()) {
        esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_STORAGE,
                                                 peer_storage_client_rx_cb, NULL);
    }
    return PEER_STORAGE_OK;
}

void peer_storage_end(void) {
    if (s_caller_mutex) xSemaphoreGive(s_caller_mutex);
}

/* Local-SD passthrough helpers ---------------------------------------- */

static peer_storage_err_t peer_storage_local_open(const char *path,
                                                   const char *mode,
                                                   int *out_handle) {
    FILE *fp = fopen(path, mode);
    if (!fp) {
        ESP_LOGW(PEER_STORAGE_TAG, "local fopen(%s, %s) failed", path, mode);
        return PEER_STORAGE_ERR_LOCAL;
    }
    /* Find a free slot in the caller-side handle table. We reuse the peer
     * slot table since as a client we never also act as the peer. */
    for (int i = 0; i < PEER_STORAGE_MAX_OPEN; i++) {
        if (!s_peer_slots[i].in_use) {
            s_peer_slots[i].in_use = true;
            s_peer_slots[i].fp = fp;
            *out_handle = i;
            return PEER_STORAGE_OK;
        }
    }
    fclose(fp);
    return PEER_STORAGE_ERR_NO_MEM;
}

static peer_storage_err_t peer_storage_local_write(int handle,
                                                    const void *data, size_t len) {
    if (handle < 0 || handle >= PEER_STORAGE_MAX_OPEN || !s_peer_slots[handle].in_use) {
        return PEER_STORAGE_ERR_BAD_ARG;
    }
    FILE *fp = s_peer_slots[handle].fp;
    if (len == 0) return PEER_STORAGE_OK;
    if (fwrite(data, 1, len, fp) != len) {
        return PEER_STORAGE_ERR_LOCAL;
    }
    return PEER_STORAGE_OK;
}

static peer_storage_err_t peer_storage_local_close(int handle) {
    if (handle < 0 || handle >= PEER_STORAGE_MAX_OPEN) return PEER_STORAGE_ERR_BAD_ARG;
    if (!s_peer_slots[handle].in_use) return PEER_STORAGE_OK;
    fclose(s_peer_slots[handle].fp);
    s_peer_slots[handle].in_use = false;
    s_peer_slots[handle].fp = NULL;
    return PEER_STORAGE_OK;
}

/* ---- caller-side public API ----------------------------------------- */

peer_storage_err_t peer_storage_open(const char *path, const char *mode,
                                      int *out_handle) {
    if (!path || !mode || !out_handle) return PEER_STORAGE_ERR_BAD_ARG;
    *out_handle = -1;

    /* Local SD fast path: use local fopen. */
    if (peer_storage_is_local_sd_available()) {
        return peer_storage_local_open(path, mode, out_handle);
    }

    /* Remote path only valid on the SD-less client. */
    if (!peer_storage_manager_is_client() ||
        !esp_comm_manager_is_connected()) {
        return PEER_STORAGE_ERR_NOT_CONNECTED;
    }

    char frame[PEER_STORAGE_MAX_FRAME];
    int n = snprintf(frame, sizeof(frame), "OPEN|%s|%s", mode, path);
    if (n < 0 || n >= (int)sizeof(frame)) return PEER_STORAGE_ERR_BAD_ARG;

    peer_storage_err_t e = peer_storage_send_frame(frame);
    if (e != PEER_STORAGE_OK) return e;

    char ack[32];
    e = peer_storage_wait_ack(ack, sizeof(ack));
    if (e != PEER_STORAGE_OK) return e;

    /* ack is "OK|<handle>" */
    const char *hstr = strchr(ack, '|');
    if (!hstr) return PEER_STORAGE_ERR_PROTOCOL;
    int h = atoi(hstr + 1);
    if (h < 0 || h >= PEER_STORAGE_MAX_OPEN) return PEER_STORAGE_ERR_PROTOCOL;
    *out_handle = h;
    return PEER_STORAGE_OK;
}

peer_storage_err_t peer_storage_write(int handle, const void *data, size_t len) {
    if (handle < 0 || handle >= PEER_STORAGE_MAX_OPEN || (!data && len)) {
        return PEER_STORAGE_ERR_BAD_ARG;
    }

    if (peer_storage_is_local_sd_available()) {
        return peer_storage_local_write(handle, data, len);
    }

    if (!peer_storage_manager_is_client() ||
        !esp_comm_manager_is_connected()) {
        return PEER_STORAGE_ERR_NOT_CONNECTED;
    }

    if (len > PEER_STORAGE_MAX_WRITE_PAYLOAD) return PEER_STORAGE_ERR_BAD_ARG;

    char frame[PEER_STORAGE_MAX_FRAME];
    int n = snprintf(frame, sizeof(frame), "WRITE|%d|%zu", handle, len);
    if (n < 0 || n >= (int)sizeof(frame)) return PEER_STORAGE_ERR_BAD_ARG;

    peer_storage_err_t e = peer_storage_send_frame(frame);
    if (e != PEER_STORAGE_OK) return e;

    /* The packet immediately following carries the raw bytes. The peer's
     * handler sees the WRITE frame first, arms itself to treat the next
     * packet as payload, then receives the bytes. */
    e = peer_storage_send_raw((const uint8_t *)data, len);
    if (e != PEER_STORAGE_OK) return e;

    return peer_storage_wait_ack(NULL, 0);
}

peer_storage_err_t peer_storage_close(int handle) {
    if (peer_storage_is_local_sd_available()) {
        return peer_storage_local_close(handle);
    }

    if (!peer_storage_manager_is_client() ||
        !esp_comm_manager_is_connected()) {
        return PEER_STORAGE_ERR_NOT_CONNECTED;
    }

    char frame[PEER_STORAGE_MAX_FRAME];
    int n = snprintf(frame, sizeof(frame), "CLOSE|%d", handle);
    if (n < 0 || n >= (int)sizeof(frame)) return PEER_STORAGE_ERR_BAD_ARG;

    peer_storage_err_t e = peer_storage_send_frame(frame);
    if (e != PEER_STORAGE_OK) return e;
    return peer_storage_wait_ack(NULL, 0);
}

peer_storage_err_t peer_storage_mkdir(const char *path) {
    if (!path) return PEER_STORAGE_ERR_BAD_ARG;

    if (peer_storage_is_local_sd_available()) {
        return sd_card_create_directory(path) == ESP_OK
               ? PEER_STORAGE_OK
               : PEER_STORAGE_ERR_LOCAL;
    }

    if (!peer_storage_manager_is_client() ||
        !esp_comm_manager_is_connected()) {
        return PEER_STORAGE_ERR_NOT_CONNECTED;
    }

    char frame[PEER_STORAGE_MAX_FRAME];
    int n = snprintf(frame, sizeof(frame), "MKDIR|%s", path);
    if (n < 0 || n >= (int)sizeof(frame)) return PEER_STORAGE_ERR_BAD_ARG;

    peer_storage_err_t e = peer_storage_send_frame(frame);
    if (e != PEER_STORAGE_OK) return e;
    return peer_storage_wait_ack(NULL, 0);
}

peer_storage_err_t peer_storage_save_file(const char *path, const void *data,
                                           size_t len) {
    if (!path) return PEER_STORAGE_ERR_BAD_ARG;
    peer_storage_err_t e = peer_storage_begin();
    if (e != PEER_STORAGE_OK) return e;
    int h = -1;
    e = peer_storage_open(path, "wb", &h);
    if (e != PEER_STORAGE_OK) {
        peer_storage_end();
        return e;
    }
    if (data && len) {
        /* Chunk to respect PEER_STORAGE_MAX_WRITE_PAYLOAD even on local SD. */
        const uint8_t *p = (const uint8_t *)data;
        size_t remaining = len;
        while (remaining) {
            size_t chunk = remaining < PEER_STORAGE_MAX_WRITE_PAYLOAD
                           ? remaining : PEER_STORAGE_MAX_WRITE_PAYLOAD;
            e = peer_storage_write(h, p, chunk);
            if (e != PEER_STORAGE_OK) {
                peer_storage_close(h);
                peer_storage_end();
                return e;
            }
            p += chunk;
            remaining -= chunk;
        }
    }
    peer_storage_err_t close_e = peer_storage_close(h);
    peer_storage_end();
    return close_e;
}

peer_storage_err_t peer_storage_append_chunk(const char *path, const void *data,
                                              size_t len, int *handle) {
    if (!path || !handle) return PEER_STORAGE_ERR_BAD_ARG;
    peer_storage_err_t e = peer_storage_begin();
    if (e != PEER_STORAGE_OK) return e;

    if (*handle < 0) {
        e = peer_storage_open(path, "ab", handle);
        if (e != PEER_STORAGE_OK) {
            peer_storage_end();
            return e;
        }
    }
    if (data && len) {
        const uint8_t *p = (const uint8_t *)data;
        size_t remaining = len;
        while (remaining) {
            size_t chunk = remaining < PEER_STORAGE_MAX_WRITE_PAYLOAD
                           ? remaining : PEER_STORAGE_MAX_WRITE_PAYLOAD;
            e = peer_storage_write(*handle, p, chunk);
            if (e != PEER_STORAGE_OK) {
                *handle = -1;
                peer_storage_end();
                return e;
            }
            p += chunk;
            remaining -= chunk;
        }
    }
    peer_storage_end();
    return PEER_STORAGE_OK;
}

/* ---- peer-side handler ----------------------------------------------- */

static bool s_peer_next_is_raw;
static int s_peer_raw_handle;
static size_t s_peer_raw_expected;
static size_t s_peer_raw_received;

/* esp_comm_manager_send_stream_wait silently fragments any payload larger
 * than ~58 bytes into multiple STREAM packets with no reassembly of its
 * own -- each fragment reaches this module as a separate rx callback
 * invocation. Control frames (paths routinely exceed 58 bytes) and WRITE
 * payloads (up to PEER_STORAGE_MAX_WRITE_PAYLOAD) must therefore be
 * reassembled here rather than assumed complete in one call. */
static char s_peer_frame_buf[PEER_STORAGE_MAX_FRAME];
static size_t s_peer_frame_len;

/* Peer-side JIT SD mount state. The C5's SD shares SPI2_HOST with the LVGL
 * TFT, so every file operation on the peer must funnel through
 * sd_card_jit_begin/end exactly like the scan saver / PCAP / WiGLE flushes
 * do. We hold the SD mounted for the lifetime of an open file handle, which
 * keeps latency low for streamed writes (the writer just suspends LVGL once
 * per session rather than once per chunk). */
static bool s_peer_sd_mounted;
static bool s_peer_display_suspended;

static bool peer_storage_peer_mount_sd(void) {
    if (s_peer_sd_mounted) return true;
    bool suspended = false;
    if (!sd_card_jit_begin(&suspended, false)) {
        return false;
    }
    s_peer_sd_mounted = true;
    s_peer_display_suspended = suspended;
    return true;
}

static void peer_storage_peer_unmount_sd(void) {
    if (!s_peer_sd_mounted) return;
    sd_card_jit_end(s_peer_display_suspended);
    s_peer_sd_mounted = false;
    s_peer_display_suspended = false;
}

static int peer_storage_alloc_slot(void) {
    for (int i = 0; i < PEER_STORAGE_MAX_OPEN; i++) {
        if (!s_peer_slots[i].in_use) return i;
    }
    return -1;
}

static FILE *peer_storage_file_for_handle(int handle) {
    if (handle < 0 || handle >= PEER_STORAGE_MAX_OPEN) return NULL;
    return s_peer_slots[handle].in_use ? s_peer_slots[handle].fp : NULL;
}

static void peer_storage_reply(const char *msg) {
    size_t len = strlen(msg);
    esp_comm_manager_send_stream_wait(COMM_STREAM_CHANNEL_STORAGE,
                                      (const uint8_t *)msg, len + 1,
                                      PEER_STORAGE_SEND_WAIT_MS);
}

static void peer_storage_handle_frame(const char *frame) {
    /* frame is NUL-terminated ASCII: "OP|arg|arg..." */
    char buf[PEER_STORAGE_MAX_FRAME];
    strncpy(buf, frame, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *op = strtok(buf, "|");
    if (!op) {
        peer_storage_reply("ERR|protocol");
        return;
    }

    if (strcmp(op, "MKDIR") == 0) {
        char *path = strtok(NULL, "|");
        if (!path) {
            peer_storage_reply("ERR|path");
            return;
        }
        if (!peer_storage_peer_mount_sd()) {
            peer_storage_reply("ERR|mount");
            return;
        }
        /* mkdir -p semantics: success if it exists or was created. */
        if (sd_card_exists(path) ||
            sd_card_create_directory(path) == ESP_OK) {
            peer_storage_reply("OK");
        } else {
            peer_storage_reply("ERR|mkdir");
        }
        /* MKDIR is stateless: release the mount immediately so it doesn't
         * hold the bus between calls. */
        if (s_peer_sd_mounted) {
            bool any_in_use = false;
            for (int i = 0; i < PEER_STORAGE_MAX_OPEN; i++) {
                if (s_peer_slots[i].in_use) { any_in_use = true; break; }
            }
            if (!any_in_use) peer_storage_peer_unmount_sd();
        }
        return;
    }

    if (strcmp(op, "OPEN") == 0) {
        char *mode = strtok(NULL, "|");
        char *path = strtok(NULL, "|");
        if (!mode || !path) {
            peer_storage_reply("ERR|path");
            return;
        }
        /* JIT mount once per session; the corresponding CLOSE drops it. */
        if (!peer_storage_peer_mount_sd()) {
            peer_storage_reply("ERR|mount");
            return;
        }
        int slot = peer_storage_alloc_slot();
        if (slot < 0) {
            bool any_in_use = false;
            for (int i = 0; i < PEER_STORAGE_MAX_OPEN; i++) {
                if (s_peer_slots[i].in_use) { any_in_use = true; break; }
            }
            if (!any_in_use) peer_storage_peer_unmount_sd();
            peer_storage_reply("ERR|nomem");
            return;
        }
        FILE *fp = fopen(path, mode);
        if (!fp) {
            bool any_in_use = false;
            for (int i = 0; i < PEER_STORAGE_MAX_OPEN; i++) {
                if (s_peer_slots[i].in_use) { any_in_use = true; break; }
            }
            if (!any_in_use) peer_storage_peer_unmount_sd();
            peer_storage_reply("ERR|open");
            return;
        }
        s_peer_slots[slot].in_use = true;
        s_peer_slots[slot].fp = fp;
        char ack[32];
        snprintf(ack, sizeof(ack), "OK|%d", slot);
        peer_storage_reply(ack);
        return;
    }

    if (strcmp(op, "WRITE") == 0) {
        char *hstr = strtok(NULL, "|");
        char *nstr = strtok(NULL, "|");
        if (!hstr || !nstr) {
            peer_storage_reply("ERR|protocol");
            return;
        }
        int handle = atoi(hstr);
        size_t nbytes = (size_t)strtoul(nstr, NULL, 10);
        if (!peer_storage_file_for_handle(handle) || nbytes == 0) {
            peer_storage_reply("ERR|badhandle");
            return;
        }
        /* Arm the next-packet-is-raw expectation. The caller must send the
         * raw bytes as the very next STREAM packet on this channel. */
        s_peer_next_is_raw = true;
        s_peer_raw_handle = handle;
        s_peer_raw_expected = nbytes;
        s_peer_raw_received = 0;
        /* Don't ack yet; ack after all raw bytes arrive (possibly spread
         * across several fragmented packets). */
        return;
    }

    if (strcmp(op, "CLOSE") == 0) {
        char *hstr = strtok(NULL, "|");
        if (!hstr) {
            peer_storage_reply("ERR|protocol");
            return;
        }
        int handle = atoi(hstr);
        if (handle < 0 || handle >= PEER_STORAGE_MAX_OPEN ||
            !s_peer_slots[handle].in_use) {
            peer_storage_reply("ERR|badhandle");
            return;
        }
        if (s_peer_slots[handle].fp) {
            fflush(s_peer_slots[handle].fp);
            fclose(s_peer_slots[handle].fp);
        }
        s_peer_slots[handle].in_use = false;
        s_peer_slots[handle].fp = NULL;
        /* If that was the last open handle, release the JIT mount so the
         * LVGL task can resume using the shared SPI bus. */
        bool any_in_use = false;
        for (int i = 0; i < PEER_STORAGE_MAX_OPEN; i++) {
            if (s_peer_slots[i].in_use) { any_in_use = true; break; }
        }
        if (!any_in_use) peer_storage_peer_unmount_sd();
        peer_storage_reply("OK");
        return;
    }

    peer_storage_reply("ERR|op");
}

static void peer_storage_peer_rx_cb(uint8_t channel, const uint8_t *data,
                                     size_t length, void *user_data) {
    (void)channel;
    (void)user_data;
    if (!data || length == 0) return;

    if (s_peer_next_is_raw) {
        FILE *fp = peer_storage_file_for_handle(s_peer_raw_handle);
        if (!fp) {
            s_peer_next_is_raw = false;
            peer_storage_reply("ERR|badhandle");
            return;
        }
        size_t remaining = s_peer_raw_expected - s_peer_raw_received;
        size_t take = length < remaining ? length : remaining;
        if (take > 0 && fwrite(data, 1, take, fp) != take) {
            s_peer_next_is_raw = false;
            peer_storage_reply("ERR|write");
            return;
        }
        s_peer_raw_received += take;
        if (s_peer_raw_received >= s_peer_raw_expected) {
            s_peer_next_is_raw = false;
            peer_storage_reply("OK");
        }
        return;
    }

    /* Control frame: text, NUL-terminated, but possibly split across
     * multiple fragments -- accumulate until the terminator shows up. */
    for (size_t i = 0; i < length; i++) {
        if (s_peer_frame_len < sizeof(s_peer_frame_buf) - 1) {
            s_peer_frame_buf[s_peer_frame_len++] = (char)data[i];
        }
        if (data[i] == '\0') {
            peer_storage_handle_frame(s_peer_frame_buf);
            s_peer_frame_len = 0;
        }
    }
}

esp_err_t peer_storage_manager_init_peer(void) {
    if (!peer_storage_manager_is_peer()) return ESP_ERR_INVALID_STATE;
    esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_STORAGE,
                                              peer_storage_peer_rx_cb, NULL);
    ESP_LOGI(PEER_STORAGE_TAG, "peer storage handler registered");
    return ESP_OK;
}
