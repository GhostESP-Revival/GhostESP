#include "managers/peer_ota_manager.h"
#include "managers/ota_manager.h"

#if GHOSTESP_OTA_SUPPORTED

#include "managers/self_ota_manager.h"
#include "managers/settings_manager.h"
#include "core/esp_comm_manager.h"
#include "core/glog.h"
#include "core/ghostesp_version.h"
#include "managers/status_display_manager.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define PEER_OTA_PEER_BOARD_KEY "somethingsomething2"
#define PEER_OTA_SEND_WAIT_MS 2000
#define PEER_OTA_RESPONSE_WAIT_MS 10000
#define PEER_OTA_DOWNLOAD_TASK_STACK_BYTES 12288
#define PEER_OTA_DOWNLOAD_RANGE_CHUNK_SIZE (32 * 1024)
#define PEER_OTA_DOWNLOAD_RANGE_ATTEMPTS 5

static SemaphoreHandle_t s_status_mutex;
static PeerOtaStatus s_status;

// --- Shared "send a command, wait for a text response" helper -------------
// Mirrors the temporary-registration pattern already used by
// ble_bridge_manager.c: the response callback slot is process-wide, so it's
// only held for the duration of a single request/response round trip.

static SemaphoreHandle_t s_response_sem;
static char s_response_buf[256];
static size_t s_response_len;
static const char *s_response_expected[4];
static size_t s_response_expected_count;

static bool peer_ota_capture_expected_response(void) {
    for (size_t i = 0; i < s_response_expected_count; i++) {
        const char *match = strstr(s_response_buf, s_response_expected[i]);
        if (!match) continue;

        size_t len = strcspn(match, "\r\n");
        if (len >= sizeof(s_response_buf)) len = sizeof(s_response_buf) - 1;
        memmove(s_response_buf, match, len);
        s_response_buf[len] = '\0';
        s_response_len = len;
        return true;
    }
    return false;
}

static void peer_ota_response_cb(const uint8_t *data, size_t length, void *user_data) {
    (void)user_data;
    if (!data || length == 0) return;

    size_t copy_len = length;
    if (copy_len > sizeof(s_response_buf) - 1 - s_response_len) {
        copy_len = sizeof(s_response_buf) - 1 - s_response_len;
    }
    if (copy_len > 0) {
        memcpy(s_response_buf + s_response_len, data, copy_len);
        s_response_len += copy_len;
        s_response_buf[s_response_len] = '\0';
    }

    if (peer_ota_capture_expected_response()) {
        xSemaphoreGive(s_response_sem);
        return;
    }

    if (s_response_len >= sizeof(s_response_buf) - 1) {
        size_t keep = sizeof(s_response_buf) / 2;
        memmove(s_response_buf, s_response_buf + s_response_len - keep, keep);
        s_response_len = keep;
        s_response_buf[s_response_len] = '\0';
    }
}

// Sends `command`/`data` over GhostLink and waits up to timeout_ms for a
// response. On success, copies the response text into out_response and
// returns true. Not reentrant -- callers must serialize (this whole feature
// only ever runs one relay at a time).
static bool peer_ota_send_and_wait_for(const char *command, const char *data,
                                       char *out_response, size_t out_len, uint32_t timeout_ms,
                                       const char *expected_a, const char *expected_b,
                                       const char *expected_c) {
    if (!s_response_sem) {
        s_response_sem = xSemaphoreCreateBinary();
        if (!s_response_sem) return false;
    }
    xSemaphoreTake(s_response_sem, 0); // drain any stale signal
    s_response_buf[0] = '\0';
    s_response_len = 0;
    s_response_expected_count = 0;
    if (expected_a) s_response_expected[s_response_expected_count++] = expected_a;
    if (expected_b) s_response_expected[s_response_expected_count++] = expected_b;
    if (expected_c) s_response_expected[s_response_expected_count++] = expected_c;

    esp_comm_manager_set_response_callback(peer_ota_response_cb, NULL);
    char command_line[128];
    if (data && data[0]) {
        snprintf(command_line, sizeof(command_line), "%s %s", command, data);
    } else {
        snprintf(command_line, sizeof(command_line), "%s", command);
    }
    bool sent = esp_comm_manager_send_command_line(command_line);
    if (!sent) {
        esp_comm_manager_set_response_callback(NULL, NULL);
        return false;
    }

    bool got = xSemaphoreTake(s_response_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    esp_comm_manager_set_response_callback(NULL, NULL);

    if (got && out_response) {
        strncpy(out_response, s_response_buf, out_len - 1);
        out_response[out_len - 1] = '\0';
    }
    return got;
}

static bool peer_ota_send_and_wait(const char *command, const char *data,
                                    char *out_response, size_t out_len, uint32_t timeout_ms) {
    return peer_ota_send_and_wait_for(command, data, out_response, out_len, timeout_ms,
                                      "OK", "ERROR", NULL);
}

// ---------------------------------------------------------------------------

bool peer_ota_manager_is_supported(void) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    return strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0;
#else
    return false;
#endif
}

esp_err_t peer_ota_manager_init(void) {
    if (!s_status_mutex) {
        s_status_mutex = xSemaphoreCreateMutex();
        if (!s_status_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = PEER_OTA_STATE_IDLE;
    return ESP_OK;
}

PeerOtaStatus peer_ota_manager_get_status(void) {
    PeerOtaStatus copy;
    if (!s_status_mutex) {
        memset(&copy, 0, sizeof(copy));
        copy.state = PEER_OTA_STATE_IDLE;
        return copy;
    }
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    copy = s_status;
    xSemaphoreGive(s_status_mutex);
    return copy;
}

static void peer_ota_set_error(const char *msg) {
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.state = PEER_OTA_STATE_FAILED;
    strncpy(s_status.error_msg, msg, sizeof(s_status.error_msg) - 1);
    s_status.error_msg[sizeof(s_status.error_msg) - 1] = '\0';
    xSemaphoreGive(s_status_mutex);
}

// Shared by both the background check and the manual "check now": only
// proceeds while GhostLink reports a connected peer session; only ever
// fetches the peer's manifest entry and updates status.
static void peer_ota_do_check(void) {
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.state = PEER_OTA_STATE_CHECKING;
    xSemaphoreGive(s_status_mutex);

    bool connected = esp_comm_manager_is_connected();
    OtaManifestEntry entry;
    memset(&entry, 0, sizeof(entry));

    if (connected) {
        uint8_t channel = settings_get_ota_channel(&G_Settings);
        ota_manager_fetch_manifest_entry(PEER_OTA_PEER_BOARD_KEY, channel, &entry);
    }

    long peer_current_build = -1;
    if (connected) {
        char info_response[128] = {0};
        if (peer_ota_send_and_wait_for("otainfo", NULL, info_response, sizeof(info_response),
                                       3000, "BUILD", NULL, NULL)) {
            (void)sscanf(info_response, "BUILD %ld", &peer_current_build);
        }
    }

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.peer_connected = connected;
    s_status.peer_current_build_number = peer_current_build;
    if (entry.found) {
        strncpy(s_status.peer_version, entry.version, sizeof(s_status.peer_version) - 1);
        s_status.peer_build_number = entry.build_number;
        s_status.state = PEER_OTA_STATE_UPDATE_AVAILABLE;
    } else {
        s_status.peer_version[0] = '\0';
        s_status.peer_build_number = entry.found ? entry.build_number : -1;
        s_status.state = PEER_OTA_STATE_IDLE;
    }
    xSemaphoreGive(s_status_mutex);

    if (entry.found) {
        settings_set_ota_update_available(&G_Settings, true);
        settings_persist_setting(SETTING_OTA_UPDATE_AVAILABLE);
    }
}

void peer_ota_manager_background_check(void) {
    if (!peer_ota_manager_is_supported()) {
        return;
    }
    // Runs every boot, but only actually does anything while a peer session
    // is up -- no point spinning up the manifest fetch for a peer that isn't
    // there to receive it.
    if (!esp_comm_manager_is_connected()) {
        return;
    }
    settings_set_ota_last_check_time(&G_Settings, (uint32_t)time(NULL));
    settings_persist_setting(SETTING_OTA_LAST_CHECK_TIME);
    peer_ota_do_check();
}

static void peer_ota_check_task(void *pv) {
    (void)pv;
    peer_ota_do_check();
    settings_set_ota_last_check_time(&G_Settings, (uint32_t)time(NULL));
    settings_persist_setting(SETTING_OTA_LAST_CHECK_TIME);
    vTaskDelete(NULL);
}

esp_err_t peer_ota_manager_check_now(void) {
    if (!peer_ota_manager_is_supported()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    BaseType_t rc = xTaskCreate(peer_ota_check_task, "peer_ota_chk", 6144, NULL, tskIDLE_PRIORITY + 1, NULL);
    return (rc == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

// ---------------------------------------------------------------------------
// Primary-side relay: stream peer's firmware.bin from R2 straight through to
// GhostLink as it downloads, instead of staging the whole image in a PSRAM
// buffer first. This is safe without a local pre-verify because the peer
// independently checks the SHA-256 (handed to it up front in the otarecv
// handshake, straight from the manifest) before it ever commits the write --
// buffering and re-hashing a full local copy first would just be redundant.
// ---------------------------------------------------------------------------

typedef struct {
    size_t total_sent;
    bool ok;
} peer_ota_stream_ctx_t;

static esp_err_t peer_ota_stream_http_event_handler(esp_http_client_event_t *evt) {
    peer_ota_stream_ctx_t *ctx = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    if (!ctx->ok) {
        return ESP_FAIL; // already failed -- stop the transfer early
    }
    if (!esp_comm_manager_send_stream_wait(COMM_STREAM_CHANNEL_OTA, (const uint8_t *)evt->data,
                                            evt->data_len, PEER_OTA_SEND_WAIT_MS)) {
        ctx->ok = false;
        return ESP_FAIL;
    }
    ctx->total_sent += evt->data_len;

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.bytes_sent = ctx->total_sent;
    xSemaphoreGive(s_status_mutex);
    return ESP_OK;
}

// Tells the peer to give up on an in-progress receive (e.g. our own download
// failed partway through) so it doesn't sit waiting forever for bytes that
// are never coming. Best-effort -- the peer's own receive timeout (see the
// watchdog armed in peer_ota_manager_handle_otarecv_cmd) covers the case
// where this command itself never arrives (e.g. GhostLink dropped entirely).
static void peer_ota_send_abort(void) {
    char response[128] = {0};
    peer_ota_send_and_wait("otaabort", NULL, response, sizeof(response), 3000);
}

// Same rationale as self_ota_download_with_retries(): a single long-lived
// HTTPS response to the manifest CDN reliably drops mid-stream on some
// networks. Since bytes are forwarded to the peer as they arrive (rather
// than staged in a local buffer), ctx->total_sent already IS the resumable
// progress counter -- each retry just re-requests the next Range starting
// from wherever the peer stream got to.
static bool peer_ota_stream_download_with_retries(const char *url, size_t image_size,
                                                    peer_ota_stream_ctx_t *ctx) {
    while (ctx->total_sent < image_size) {
        size_t chunk_start = ctx->total_sent;
        size_t chunk_end = chunk_start + PEER_OTA_DOWNLOAD_RANGE_CHUNK_SIZE - 1;
        if (chunk_end >= image_size) chunk_end = image_size - 1;
        size_t target = chunk_end + 1;
        bool made_progress = false;

        for (int attempt = 1; attempt <= PEER_OTA_DOWNLOAD_RANGE_ATTEMPTS; attempt++) {
            size_t before = ctx->total_sent;

            esp_http_client_config_t http_config = {
                .url = url,
                .timeout_ms = 60000,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .event_handler = peer_ota_stream_http_event_handler,
                .user_data = ctx,
                .buffer_size = 2048,
            };
            esp_http_client_handle_t client = esp_http_client_init(&http_config);
            if (!client) {
                return false;
            }

            char range_header[64];
            snprintf(range_header, sizeof(range_header), "bytes=%u-%u", (unsigned)before, (unsigned)chunk_end);
            esp_http_client_set_header(client, "Range", range_header);

            esp_err_t err = esp_http_client_perform(client);
            int status = esp_http_client_get_status_code(client);
            esp_http_client_cleanup(client);

            if (!ctx->ok) {
                // Peer-side stream send failed -- not an HTTP problem, retrying
                // the download won't help.
                return false;
            }

            bool status_ok = (status == 206) || (before == 0 && status == 200);
            if (!status_ok) {
                glog("Peer OTA relay unexpected HTTP status %d for range %u-%u\n",
                     status, (unsigned)before, (unsigned)chunk_end);
                return false;
            }

            if (ctx->total_sent >= target) {
                made_progress = true;
                break;
            }

            if (ctx->total_sent > before) {
                glog("Peer OTA relay range interrupted (err=%s, http=%d, got=%u/%u), continuing\n",
                     esp_err_to_name(err), status, (unsigned)ctx->total_sent, (unsigned)image_size);
                made_progress = true;
                break;
            }

            glog("Peer OTA relay range made no progress (err=%s, http=%d, range=%u-%u, attempt=%d)\n",
                 esp_err_to_name(err), status, (unsigned)chunk_start, (unsigned)chunk_end, attempt);
            vTaskDelay(pdMS_TO_TICKS(400 * attempt));
        }

        if (!made_progress) {
            return false;
        }
    }

    return true;
}

// Runs the full peer-relay flow synchronously (handshake -> stream straight
// through -> wait for peer's result), updating s_status as it goes. Returns
// true only if the peer confirmed a successful, verified write (it will be
// rebooting into the new image). Does not touch this board's own firmware --
// see peer_ota_manager_start_update() (explicit peer-only) below.
static bool peer_ota_run_update_once(void) {
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.state = PEER_OTA_STATE_CHECKING;
    s_status.bytes_sent = 0;
    xSemaphoreGive(s_status_mutex);

    if (!esp_comm_manager_is_connected()) {
        peer_ota_set_error("GhostLink not connected");
        return false;
    }

    OtaManifestEntry entry;
    uint8_t channel = settings_get_ota_channel(&G_Settings);
    if (ota_manager_fetch_manifest_entry(PEER_OTA_PEER_BOARD_KEY, channel, &entry) != ESP_OK || !entry.found) {
        peer_ota_set_error("No manifest entry for peer");
        return false;
    }
    if (entry.size == 0 || entry.download_url[0] == '\0' || entry.sha256[0] == '\0') {
        peer_ota_set_error("Manifest entry missing size/url/sha256");
        return false;
    }

    long peer_current_build = -1;
    char info_response[128] = {0};
    if (peer_ota_send_and_wait_for("otainfo", NULL, info_response, sizeof(info_response),
                                   3000, "BUILD", NULL, NULL)) {
        (void)sscanf(info_response, "BUILD %ld", &peer_current_build);
    }
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.peer_current_build_number = peer_current_build;
    s_status.peer_build_number = entry.build_number;
    xSemaphoreGive(s_status_mutex);

    // Handshake first -- the peer needs to know the expected size/hash
    // before any bytes arrive, since we're streaming straight through rather
    // than staging a pre-verified copy locally.
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.state = PEER_OTA_STATE_WAITING_PEER;
    s_status.total_bytes = entry.size;
    xSemaphoreGive(s_status_mutex);
    status_display_show_status("Waiting for peer...");

    char cmd_data[96];
    snprintf(cmd_data, sizeof(cmd_data), "%u %s", (unsigned)entry.size, entry.sha256);
    char response[128] = {0};
    if (!peer_ota_send_and_wait_for("otarecv", cmd_data, response, sizeof(response), PEER_OTA_RESPONSE_WAIT_MS,
                                    "READY", "ERROR", NULL) ||
        strncmp(response, "READY", 5) != 0) {
        glog("Peer did not ack otarecv (response='%s')\n", response);
        peer_ota_set_error("Peer did not accept update");
        return false;
    }

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.state = PEER_OTA_STATE_SENDING;
    xSemaphoreGive(s_status_mutex);
    status_display_show_status("Flashing peer...");

    size_t relay_internal_free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t relay_psram_free_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    glog("[peer OTA relay] before: internal_free=%u bytes, psram_free=%u bytes\n",
         (unsigned)relay_internal_free_before, (unsigned)relay_psram_free_before);

    peer_ota_stream_ctx_t ctx = { .total_sent = 0, .ok = true };
    bool relay_ok = peer_ota_stream_download_with_retries(entry.download_url, entry.size, &ctx) &&
                     ctx.total_sent == entry.size;

    glog("[peer OTA relay] after: internal_free=%u bytes (used=%d), psram_free=%u bytes (used=%d)\n",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         (int)((long)relay_internal_free_before - (long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
         (int)((long)relay_psram_free_before - (long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    if (!relay_ok) {
        glog("Peer firmware relay failed (got=%u/%u)\n",
             (unsigned)ctx.total_sent, (unsigned)entry.size);
        peer_ota_set_error("Peer firmware relay failed");
        peer_ota_send_abort();
        return false;
    }

    // Wait for the peer's final verify/commit result. This is normally
    // synchronous on the peer's side (finish happens right in the stream rx
    // callback for the last chunk), but poll a few times rather than trust a
    // single query, in case the peer's last chunk is still being processed.
    bool peer_done = false;
    for (int attempt = 0; attempt < 5 && !peer_done; attempt++) {
        if (peer_ota_send_and_wait_for("otastatus", NULL, response, sizeof(response), PEER_OTA_RESPONSE_WAIT_MS,
                                       "DONE", "ERROR", "PENDING")) {
            if (strncmp(response, "DONE", 4) == 0) {
                peer_done = true;
                break;
            }
            if (strncmp(response, "ERROR", 5) == 0) {
                break; // peer explicitly failed -- no point retrying
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!peer_done) {
        glog("Peer reported failure or timed out (response='%s')\n", response);
        peer_ota_set_error("Peer failed to apply update");
        return false;
    }

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.state = PEER_OTA_STATE_DONE;
    xSemaphoreGive(s_status_mutex);
    status_display_show_status("Peer update complete");
    glog("Peer firmware update complete; peer is rebooting\n");

    settings_set_ota_update_available(&G_Settings, false);
    settings_persist_setting(SETTING_OTA_UPDATE_AVAILABLE);
    return true;
}

static void peer_ota_update_task(void *pv) {
    (void)pv;
    peer_ota_run_update_once();
    vTaskDelete(NULL);
}

esp_err_t peer_ota_manager_start_update(void) {
    if (!peer_ota_manager_is_supported()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!esp_comm_manager_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    // xTaskCreate's stack-depth argument is in bytes on ESP-IDF's FreeRTOS
    // port, unlike xTaskCreateStatic (which takes StackType_t words) --
    // do not divide by sizeof(StackType_t) here.
    BaseType_t rc = xTaskCreate(peer_ota_update_task, "peer_ota", PEER_OTA_DOWNLOAD_TASK_STACK_BYTES,
                                NULL, 5, NULL);
    return (rc == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

// ---------------------------------------------------------------------------
// Peer-side: react to "otarecv <size> <sha256>" from the primary.
// ---------------------------------------------------------------------------

#define PEER_OTA_RECEIVE_TIMEOUT_MS 45000
#define PEER_OTA_RX_TASK_STACK_BYTES 6144
#define PEER_OTA_RX_QUEUE_LEN 16
#define PEER_OTA_RX_CHUNK_MAX 64

typedef enum {
    PEER_OTA_RX_MSG_BEGIN,
    PEER_OTA_RX_MSG_CHUNK,
    PEER_OTA_RX_MSG_ABORT,
} peer_ota_rx_msg_type_t;

typedef struct {
    peer_ota_rx_msg_type_t type;
    size_t image_size;
    size_t length;
    uint8_t data[PEER_OTA_RX_CHUNK_MAX];
} peer_ota_rx_msg_t;

static bool s_peer_receiving;
static size_t s_peer_expected_size;
static size_t s_peer_received;
static size_t s_peer_queued;
static char s_peer_expected_sha256[65];
static bool s_peer_last_result_ok;
static bool s_peer_have_result;
static esp_timer_handle_t s_peer_receive_timeout_timer;
static QueueHandle_t s_peer_rx_queue;
static TaskHandle_t s_peer_rx_task_handle;
static StackType_t *s_peer_rx_task_stack;
static StaticTask_t *s_peer_rx_task_tcb;
static SemaphoreHandle_t s_peer_begin_sem;
static esp_err_t s_peer_begin_result;
static volatile bool s_peer_abort_requested;
static bool s_peer_reboot_pending;
static bool s_peer_reboot_task_started;

static void peer_ota_finish_and_maybe_reboot_task(void *pv);

static void peer_ota_request_worker_abort(TickType_t wait_ticks) {
    s_peer_abort_requested = true;
    if (!s_peer_rx_queue) {
        return;
    }
    peer_ota_rx_msg_t msg = { .type = PEER_OTA_RX_MSG_ABORT };
    (void)xQueueSendToFront(s_peer_rx_queue, &msg, wait_ticks);
}

static void peer_ota_mark_receive_failed(void) {
    if (s_peer_receive_timeout_timer) {
        esp_timer_stop(s_peer_receive_timeout_timer);
    }
    s_peer_receiving = false;
    esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_OTA, NULL, NULL);
    s_peer_have_result = true;
    s_peer_last_result_ok = false;
    s_peer_reboot_pending = false;
}

static void peer_ota_rx_worker_task(void *pv) {
    (void)pv;
    bool raw_active = false;

    for (;;) {
        peer_ota_rx_msg_t msg;
        if (xQueueReceive(s_peer_rx_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (msg.type == PEER_OTA_RX_MSG_ABORT || s_peer_abort_requested) {
            if (raw_active) {
                ota_manager_raw_write_abort();
                raw_active = false;
            }
            s_peer_abort_requested = false;
            if (msg.type == PEER_OTA_RX_MSG_ABORT) {
                continue;
            }
        }

        if (msg.type == PEER_OTA_RX_MSG_BEGIN) {
            glog("[peer OTA receive] before: internal_free=%u bytes, psram_free=%u bytes\n",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            s_peer_begin_result = ota_manager_raw_write_begin(msg.image_size);
            raw_active = (s_peer_begin_result == ESP_OK);
            if (s_peer_begin_sem) {
                xSemaphoreGive(s_peer_begin_sem);
            }
            continue;
        }

        if (msg.type != PEER_OTA_RX_MSG_CHUNK || !s_peer_receiving || !raw_active) {
            continue;
        }

        if (ota_manager_raw_write_chunk(msg.data, msg.length) != ESP_OK) {
            ota_manager_raw_write_abort();
            raw_active = false;
            peer_ota_mark_receive_failed();
            continue;
        }

        s_peer_received += msg.length;
        if (s_peer_received >= s_peer_expected_size) {
            if (s_peer_receive_timeout_timer) {
                esp_timer_stop(s_peer_receive_timeout_timer);
            }
            s_peer_receiving = false;
            esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_OTA, NULL, NULL);
            esp_err_t err = ota_manager_raw_write_finish(s_peer_expected_sha256);
            raw_active = false;
            s_peer_have_result = true;
            s_peer_last_result_ok = (err == ESP_OK);
            s_peer_reboot_pending = (err == ESP_OK);
            glog("[peer OTA receive] after: internal_free=%u bytes, psram_free=%u bytes\n",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
    }
}

static bool peer_ota_ensure_rx_worker(void) {
    if (!s_peer_begin_sem) {
        s_peer_begin_sem = xSemaphoreCreateBinary();
        if (!s_peer_begin_sem) {
            return false;
        }
    }
    if (!s_peer_rx_queue) {
        s_peer_rx_queue = xQueueCreate(PEER_OTA_RX_QUEUE_LEN, sizeof(peer_ota_rx_msg_t));
        if (!s_peer_rx_queue) {
            return false;
        }
    }
    if (s_peer_rx_task_handle) {
        return true;
    }

    const uint32_t stack_words = (PEER_OTA_RX_TASK_STACK_BYTES + sizeof(StackType_t) - 1) / sizeof(StackType_t);
    s_peer_rx_task_stack = (StackType_t *)heap_caps_malloc(stack_words * sizeof(StackType_t),
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_peer_rx_task_stack) {
        s_peer_rx_task_stack = (StackType_t *)heap_caps_malloc(stack_words * sizeof(StackType_t),
                                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    s_peer_rx_task_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t),
                                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_peer_rx_task_stack || !s_peer_rx_task_tcb) {
        if (s_peer_rx_task_stack) {
            heap_caps_free(s_peer_rx_task_stack);
            s_peer_rx_task_stack = NULL;
        }
        if (s_peer_rx_task_tcb) {
            heap_caps_free(s_peer_rx_task_tcb);
            s_peer_rx_task_tcb = NULL;
        }
        return false;
    }

    s_peer_rx_task_handle = xTaskCreateStatic(peer_ota_rx_worker_task, "peer_ota_rx", stack_words,
                                              NULL, tskIDLE_PRIORITY + 2,
                                              s_peer_rx_task_stack, s_peer_rx_task_tcb);
    if (!s_peer_rx_task_handle) {
        heap_caps_free(s_peer_rx_task_stack);
        heap_caps_free(s_peer_rx_task_tcb);
        s_peer_rx_task_stack = NULL;
        s_peer_rx_task_tcb = NULL;
    }
    return s_peer_rx_task_handle != NULL;
}

static void peer_ota_finish_and_maybe_reboot_task(void *pv) {
    bool ok = (bool)(intptr_t)pv;
    // Give the "DONE"/"ERROR" response a moment to actually go out over the
    // UART before we reset (esp_restart tears down peripherals immediately).
    vTaskDelay(pdMS_TO_TICKS(500));
    if (ok) {
        esp_restart();
    }
    vTaskDelete(NULL);
}

// Cleans up an in-progress receive without committing anything -- shared by
// a mid-stream write failure, the receive-timeout watchdog (primary went
// silent, e.g. GhostLink dropped entirely), and an explicit "otaabort" from
// the primary (its own download failed after the handshake).
static void peer_ota_abort_receive(void) {
    peer_ota_mark_receive_failed();
    peer_ota_request_worker_abort(pdMS_TO_TICKS(100));
}

static void peer_ota_receive_timeout_cb(void *arg) {
    (void)arg;
    if (!s_peer_receiving) {
        return; // already finished/aborted before the timer fired
    }
    glog("GhostLink OTA receive timed out waiting for more data; aborting\n");
    peer_ota_abort_receive();
}

static void peer_ota_ensure_timeout_timer(void) {
    if (s_peer_receive_timeout_timer) {
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = peer_ota_receive_timeout_cb,
        .name = "peer_ota_rx_to",
    };
    esp_timer_create(&args, &s_peer_receive_timeout_timer);
}

static void peer_ota_stream_rx_cb(uint8_t channel, const uint8_t *data, size_t length, void *user_data) {
    (void)channel;
    (void)user_data;
    if (!s_peer_receiving) {
        return;
    }
    if (s_peer_receive_timeout_timer) {
        esp_timer_stop(s_peer_receive_timeout_timer);
        esp_timer_start_once(s_peer_receive_timeout_timer, (uint64_t)PEER_OTA_RECEIVE_TIMEOUT_MS * 1000);
    }
    if (length > PEER_OTA_RX_CHUNK_MAX) {
        peer_ota_abort_receive();
        return;
    }
    peer_ota_rx_msg_t msg = {
        .type = PEER_OTA_RX_MSG_CHUNK,
        .length = length,
    };
    memcpy(msg.data, data, length);
    if (xQueueSend(s_peer_rx_queue, &msg, pdMS_TO_TICKS(PEER_OTA_SEND_WAIT_MS)) != pdTRUE) {
        peer_ota_abort_receive();
        return;
    }
    s_peer_queued += length;
    if (s_peer_queued >= s_peer_expected_size) {
        if (s_peer_receive_timeout_timer) {
            esp_timer_stop(s_peer_receive_timeout_timer);
        }
    }
}

void peer_ota_manager_handle_otarecv_cmd(int argc, char **argv) {
    if (argc < 3) {
        esp_comm_manager_send_response((const uint8_t *)"ERROR:usage", strlen("ERROR:usage"));
        return;
    }
    if (s_peer_receiving) {
        esp_comm_manager_send_response((const uint8_t *)"ERROR:busy", strlen("ERROR:busy"));
        return;
    }

    size_t size = (size_t)strtoul(argv[1], NULL, 10);
    if (size == 0) {
        esp_comm_manager_send_response((const uint8_t *)"ERROR:size", strlen("ERROR:size"));
        return;
    }

    if (!peer_ota_ensure_rx_worker()) {
        esp_comm_manager_send_response((const uint8_t *)"ERROR:no_mem", strlen("ERROR:no_mem"));
        return;
    }

    strncpy(s_peer_expected_sha256, argv[2], sizeof(s_peer_expected_sha256) - 1);
    s_peer_expected_sha256[sizeof(s_peer_expected_sha256) - 1] = '\0';
    s_peer_expected_size = size;
    s_peer_received = 0;
    s_peer_queued = 0;
    s_peer_have_result = false;
    s_peer_last_result_ok = false;
    s_peer_abort_requested = false;
    s_peer_reboot_pending = false;
    s_peer_reboot_task_started = false;
    xSemaphoreTake(s_peer_begin_sem, 0);

    peer_ota_rx_msg_t msg = {
        .type = PEER_OTA_RX_MSG_BEGIN,
        .image_size = size,
    };
    if (xQueueSend(s_peer_rx_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE ||
        xSemaphoreTake(s_peer_begin_sem, pdMS_TO_TICKS(PEER_OTA_RESPONSE_WAIT_MS)) != pdTRUE ||
        s_peer_begin_result != ESP_OK) {
        peer_ota_request_worker_abort(pdMS_TO_TICKS(100));
        s_peer_have_result = true;
        s_peer_last_result_ok = false;
        esp_comm_manager_send_response((const uint8_t *)"ERROR:begin", strlen("ERROR:begin"));
        return;
    }

    s_peer_receiving = true;

    esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_OTA, peer_ota_stream_rx_cb, NULL);
    peer_ota_ensure_timeout_timer();
    if (s_peer_receive_timeout_timer) {
        esp_timer_start_once(s_peer_receive_timeout_timer, (uint64_t)PEER_OTA_RECEIVE_TIMEOUT_MS * 1000);
    }
    status_display_show_status("Receiving update...");
    esp_comm_manager_send_response((const uint8_t *)"READY", strlen("READY"));
}

// Explicit abort from the primary (its own download failed after we already
// acked "READY"). Distinct from the watchdog timeout above, which covers the
// case where this command itself never arrives.
void peer_ota_manager_handle_otaabort_cmd(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (!s_peer_receiving) {
        esp_comm_manager_send_response((const uint8_t *)"ERROR:not_receiving", strlen("ERROR:not_receiving"));
        return;
    }
    peer_ota_abort_receive();
    esp_comm_manager_send_response((const uint8_t *)"OK", strlen("OK"));
}

// The primary polls for the final result via a lightweight "otastatus"
// command (also routed through the standard CLI dispatcher, see cmd_ota.c).
void peer_ota_manager_handle_otastatus_cmd(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (!s_peer_have_result) {
        esp_comm_manager_send_response((const uint8_t *)"PENDING", strlen("PENDING"));
        return;
    }
    const char *msg = s_peer_last_result_ok ? "DONE" : "ERROR:verify";
    esp_comm_manager_send_response((const uint8_t *)msg, strlen(msg));
    if (s_peer_reboot_pending && !s_peer_reboot_task_started) {
        s_peer_reboot_task_started = true;
        BaseType_t rc = xTaskCreate(peer_ota_finish_and_maybe_reboot_task, "peer_ota_fin", 2048,
                                     (void *)(intptr_t)true, tskIDLE_PRIORITY + 1, NULL);
        (void)rc;
    }
}

void peer_ota_manager_handle_otainfo_cmd(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char response[64];
    snprintf(response, sizeof(response), "BUILD %ld VERSION %s", (long)GHOSTESP_BUILD_NUMBER, GHOSTESP_VERSION);
    esp_comm_manager_send_response((const uint8_t *)response, strlen(response));
}

#else

bool peer_ota_manager_is_supported(void) { return false; }
esp_err_t peer_ota_manager_init(void) { return ESP_OK; }
void peer_ota_manager_background_check(void) {}
esp_err_t peer_ota_manager_check_now(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t peer_ota_manager_start_update(void) { return ESP_ERR_NOT_SUPPORTED; }
PeerOtaStatus peer_ota_manager_get_status(void) { return (PeerOtaStatus){ .state = PEER_OTA_STATE_IDLE }; }
void peer_ota_manager_handle_otarecv_cmd(int argc, char **argv) { (void)argc; (void)argv; }
void peer_ota_manager_handle_otastatus_cmd(int argc, char **argv) { (void)argc; (void)argv; }
void peer_ota_manager_handle_otaabort_cmd(int argc, char **argv) { (void)argc; (void)argv; }
void peer_ota_manager_handle_otainfo_cmd(int argc, char **argv) { (void)argc; (void)argv; }

#endif
