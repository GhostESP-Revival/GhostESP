#include "managers/peer_ota_manager.h"
#include "managers/ota_manager.h"
#include "managers/self_ota_manager.h"
#include "managers/settings_manager.h"
#include "core/esp_comm_manager.h"
#include "core/glog.h"
#include "managers/status_display_manager.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define PEER_OTA_PEER_BOARD_KEY "somethingsomething2"
#define PEER_OTA_SEND_WAIT_MS 2000
#define PEER_OTA_RESPONSE_WAIT_MS 10000
#define PEER_OTA_BACKGROUND_MIN_INTERVAL_SEC (24 * 60 * 60)
#define PEER_OTA_DOWNLOAD_TASK_STACK_BYTES 12288

static SemaphoreHandle_t s_status_mutex;
static PeerOtaStatus s_status;

// --- Shared "send a command, wait for a text response" helper -------------
// Mirrors the temporary-registration pattern already used by
// ble_bridge_manager.c: the response callback slot is process-wide, so it's
// only held for the duration of a single request/response round trip.

static SemaphoreHandle_t s_response_sem;
static char s_response_buf[128];

static void peer_ota_response_cb(const uint8_t *data, size_t length, void *user_data) {
    (void)user_data;
    size_t copy_len = length < sizeof(s_response_buf) - 1 ? length : sizeof(s_response_buf) - 1;
    memcpy(s_response_buf, data, copy_len);
    s_response_buf[copy_len] = '\0';
    xSemaphoreGive(s_response_sem);
}

// Sends `command`/`data` over GhostLink and waits up to timeout_ms for a
// response. On success, copies the response text into out_response and
// returns true. Not reentrant -- callers must serialize (this whole feature
// only ever runs one relay at a time).
static bool peer_ota_send_and_wait(const char *command, const char *data,
                                    char *out_response, size_t out_len, uint32_t timeout_ms) {
    if (!s_response_sem) {
        s_response_sem = xSemaphoreCreateBinary();
        if (!s_response_sem) return false;
    }
    xSemaphoreTake(s_response_sem, 0); // drain any stale signal

    esp_comm_manager_set_response_callback(peer_ota_response_cb, NULL);
    bool sent = esp_comm_manager_send_command(command, data);
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

// Shared by both the throttled background check and the manual "check now":
// only proceeds while GhostLink reports a connected peer session; only ever
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

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.peer_connected = connected;
    if (entry.found) {
        strncpy(s_status.peer_version, entry.version, sizeof(s_status.peer_version) - 1);
        s_status.peer_build_number = entry.build_number;
        s_status.state = PEER_OTA_STATE_UPDATE_AVAILABLE;
    } else {
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
    uint32_t last_check = settings_get_ota_last_check_time(&G_Settings);
    uint32_t now = (uint32_t)time(NULL);
    if (last_check != 0 && now > last_check && (now - last_check) < PEER_OTA_BACKGROUND_MIN_INTERVAL_SEC) {
        return;
    }
    settings_set_ota_last_check_time(&G_Settings, now);
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

// Runs the full peer-relay flow synchronously (handshake -> stream straight
// through -> wait for peer's result), updating s_status as it goes. Returns
// true only if the peer confirmed a successful, verified write (it will be
// rebooting into the new image). Does not touch this board's own firmware --
// see peer_ota_manager_start_full_update() for the combined "peer then self"
// sequence.
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
    if (!peer_ota_send_and_wait("otarecv", cmd_data, response, sizeof(response), PEER_OTA_RESPONSE_WAIT_MS) ||
        strncmp(response, "READY", 5) != 0) {
        glog("Peer did not ack otarecv (response='%s')\n", response);
        peer_ota_set_error("Peer did not accept update");
        return false;
    }

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.state = PEER_OTA_STATE_SENDING;
    xSemaphoreGive(s_status_mutex);
    status_display_show_status("Flashing peer...");

    peer_ota_stream_ctx_t ctx = { .total_sent = 0, .ok = true };
    esp_http_client_config_t http_config = {
        .url = entry.download_url,
        .timeout_ms = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = peer_ota_stream_http_event_handler,
        .user_data = &ctx,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        peer_ota_set_error("Failed to init HTTP client");
        peer_ota_send_abort();
        return false;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || !ctx.ok || ctx.total_sent != entry.size) {
        glog("Peer firmware relay failed (err=%s, http=%d, got=%u/%u)\n",
             esp_err_to_name(err), status, (unsigned)ctx.total_sent, (unsigned)entry.size);
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
        if (peer_ota_send_and_wait("otastatus", NULL, response, sizeof(response), PEER_OTA_RESPONSE_WAIT_MS)) {
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

// Combined flow for the primary (somethingsomething), which now has its own
// real dual-partition OTA table too: update the network-less peer first,
// then this board's own firmware. If a peer update is available but fails,
// self-update is skipped -- reboot only once the peer relay has actually
// succeeded (or wasn't needed), so a bad peer attempt can still be retried
// without this board having already moved to new firmware itself.
static void peer_ota_full_update_task(void *pv) {
    (void)pv;

    PeerOtaStatus peer_status = peer_ota_manager_get_status();
    if (peer_status.state == PEER_OTA_STATE_UPDATE_AVAILABLE) {
        if (!peer_ota_run_update_once()) {
            vTaskDelete(NULL);
            return;
        }
    }

    // Peer is done (or had nothing to do) -- now update this board itself,
    // if a self-update is available. Both start_update() calls spawn their
    // own task and return immediately; once one succeeds it reboots this
    // board, which is the natural end of this combined flow.
    if (ota_manager_is_supported() && ota_manager_get_status().state == OTA_STATE_UPDATE_AVAILABLE) {
        ota_manager_start_update();
    } else if (self_ota_manager_is_supported()) {
        // somethingsomething has no dual-partition table of its own (8MB
        // flash can't fit one alongside the required napps reservation), so
        // it falls back to the single-partition self-overwrite path instead.
        // Unlike the branch above there's no separate "update available"
        // flag to check first -- self_ota_manager_start_update() does its
        // own manifest/version check and simply reports "Already up to
        // date" via status if there's nothing newer.
        self_ota_manager_start_update();
    }
    vTaskDelete(NULL);
}

// Combined "update peer then self" entry point used by somethingsomething's
// Firmware Update screen. Falls back to a self-only update on any other
// direct-OTA board (where there's no peer to relay to first).
esp_err_t peer_ota_manager_start_full_update(void) {
    if (!peer_ota_manager_is_supported()) {
        return ota_manager_is_supported() ? ota_manager_start_update() : ESP_ERR_NOT_SUPPORTED;
    }
    BaseType_t rc = xTaskCreate(peer_ota_full_update_task, "peer_ota_full",
                                PEER_OTA_DOWNLOAD_TASK_STACK_BYTES, NULL, 5, NULL);
    return (rc == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

// ---------------------------------------------------------------------------
// Peer-side: react to "otarecv <size> <sha256>" from the primary.
// ---------------------------------------------------------------------------

#define PEER_OTA_RECEIVE_TIMEOUT_MS 45000

static bool s_peer_receiving;
static size_t s_peer_expected_size;
static size_t s_peer_received;
static char s_peer_expected_sha256[65];
static bool s_peer_last_result_ok;
static bool s_peer_have_result;
static esp_timer_handle_t s_peer_receive_timeout_timer;

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
    if (s_peer_receive_timeout_timer) {
        esp_timer_stop(s_peer_receive_timeout_timer);
    }
    s_peer_receiving = false;
    esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_OTA, NULL, NULL);
    ota_manager_raw_write_abort();
    s_peer_have_result = true;
    s_peer_last_result_ok = false;
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
    if (ota_manager_raw_write_chunk(data, length) != ESP_OK) {
        peer_ota_abort_receive();
        return;
    }
    s_peer_received += length;
    if (s_peer_received >= s_peer_expected_size) {
        if (s_peer_receive_timeout_timer) {
            esp_timer_stop(s_peer_receive_timeout_timer);
        }
        s_peer_receiving = false;
        esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_OTA, NULL, NULL);
        esp_err_t err = ota_manager_raw_write_finish(s_peer_expected_sha256);
        s_peer_have_result = true;
        s_peer_last_result_ok = (err == ESP_OK);
        BaseType_t rc = xTaskCreate(peer_ota_finish_and_maybe_reboot_task, "peer_ota_fin", 2048,
                                     (void *)(intptr_t)s_peer_last_result_ok, tskIDLE_PRIORITY + 1, NULL);
        (void)rc;
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

    if (ota_manager_raw_write_begin(size) != ESP_OK) {
        esp_comm_manager_send_response((const uint8_t *)"ERROR:begin", strlen("ERROR:begin"));
        return;
    }

    strncpy(s_peer_expected_sha256, argv[2], sizeof(s_peer_expected_sha256) - 1);
    s_peer_expected_sha256[sizeof(s_peer_expected_sha256) - 1] = '\0';
    s_peer_expected_size = size;
    s_peer_received = 0;
    s_peer_have_result = false;
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
}
