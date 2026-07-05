#include "managers/ota_manager.h"
#include "managers/settings_manager.h"
#include "managers/sd_card_manager.h"
#include "core/glog.h"
#include "managers/status_display_manager.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "mbedtls/sha256.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>

static const char *TAG = "OtaManager";

// Cloudflare R2 custom domain bound to the OTA bucket (see CI setup notes at
// the top of .github/workflows/compile_all.yml -- R2_PUBLIC_BASE_URL there
// must match this host).
#ifndef GHOSTESP_OTA_MANIFEST_URL
#define GHOSTESP_OTA_MANIFEST_URL "https://gespota.fuckyourcdn.com/ota-manifest.json"
#endif

#define OTA_MANIFEST_HTTP_BUFFER_SIZE 1024
#define OTA_MANIFEST_RESPONSE_INITIAL_SIZE 2048
#define OTA_BACKGROUND_CHECK_MIN_INTERVAL_SEC (24 * 60 * 60)
#define OTA_DOWNLOAD_TASK_STACK_BYTES 12288
#define OTA_READBACK_CHUNK_SIZE 1024
#define OTA_SD_READ_CHUNK_SIZE 4096

#define OTA_SD_FIRMWARE_PATH "/mnt/ghostesp/firmware_update.bin"
#define OTA_SD_SHA256_PATH "/mnt/ghostesp/firmware_update.sha256"

static SemaphoreHandle_t s_status_mutex;
static OtaStatus s_status;

// Discovered by the manifest check, consumed by the download task.
static char s_download_url[256];
static char s_expected_sha256[65];

// Raw-write session state (used both internally and by peer_ota_manager.c).
static const esp_partition_t *s_raw_write_partition;
static esp_ota_handle_t s_raw_write_handle;
static bool s_raw_write_active;
static mbedtls_sha256_context s_raw_write_sha_ctx;
static size_t s_raw_write_total;

static void ota_set_state(OtaState state) {
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.state = state;
    xSemaphoreGive(s_status_mutex);
}

static void ota_set_error(const char *msg) {
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.state = OTA_STATE_FAILED;
    strncpy(s_status.error_msg, msg, sizeof(s_status.error_msg) - 1);
    s_status.error_msg[sizeof(s_status.error_msg) - 1] = '\0';
    xSemaphoreGive(s_status_mutex);
}

bool ota_manager_is_supported(void) {
#if GHOSTESP_OTA_SUPPORTED
    return esp_ota_get_next_update_partition(NULL) != NULL;
#else
    return false;
#endif
}

// somethingsomething2 (Banshee S3) has a real dual-OTA partition table and
// needs the boot-confirm/rollback machinery above, but it has no Wi-Fi of
// its own -- it only ever receives updates via a GhostLink relay from its
// somethingsomething (Banshee C5) peer (see peer_ota_manager.c). Network
// self-checks must never run on it.
static bool ota_manager_board_has_network(void) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    return strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething2") != 0;
#else
    return true;
#endif
}

esp_err_t ota_manager_init(void) {
    if (!s_status_mutex) {
        s_status_mutex = xSemaphoreCreateMutex();
        if (!s_status_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = OTA_STATE_IDLE;
    return ESP_OK;
}

OtaStatus ota_manager_get_status(void) {
    OtaStatus copy;
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    copy = s_status;
    xSemaphoreGive(s_status_mutex);
    return copy;
}

void ota_manager_confirm_boot_ok(void) {
#if GHOSTESP_OTA_SUPPORTED
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) != ESP_OK) {
        return;
    }
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA image marked valid; rollback cancelled");
        } else {
            ESP_LOGW(TAG, "Failed to mark OTA image valid: %s", esp_err_to_name(err));
        }
    }
#endif
}

// ---------------------------------------------------------------------------
// Manifest fetch + parse
// ---------------------------------------------------------------------------

typedef struct {
    char *buffer;
    size_t len;
    size_t capacity;
} ota_response_buf_t;

static esp_err_t ota_http_event_handler(esp_http_client_event_t *evt) {
    ota_response_buf_t *buf = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    if (buf->len + evt->data_len + 1 > buf->capacity) {
        size_t new_cap = buf->capacity ? buf->capacity * 2 : OTA_MANIFEST_RESPONSE_INITIAL_SIZE;
        while (new_cap < buf->len + evt->data_len + 1) {
            new_cap *= 2;
        }
        char *grown = realloc(buf->buffer, new_cap);
        if (!grown) {
            ESP_LOGE(TAG, "Failed to grow manifest buffer");
            return ESP_FAIL;
        }
        buf->buffer = grown;
        buf->capacity = new_cap;
    }
    memcpy(buf->buffer + buf->len, evt->data, evt->data_len);
    buf->len += evt->data_len;
    buf->buffer[buf->len] = '\0';
    return ESP_OK;
}

esp_err_t ota_manager_fetch_manifest_entry(const char *board_key, uint8_t channel, OtaManifestEntry *out_entry) {
    if (!board_key || !out_entry) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_entry, 0, sizeof(*out_entry));

    ota_response_buf_t resp = {0};
    resp.buffer = malloc(OTA_MANIFEST_RESPONSE_INITIAL_SIZE);
    if (!resp.buffer) {
        return ESP_ERR_NO_MEM;
    }
    resp.capacity = OTA_MANIFEST_RESPONSE_INITIAL_SIZE;

    esp_http_client_config_t config = {
        .url = GHOSTESP_OTA_MANIFEST_URL,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = ota_http_event_handler,
        .user_data = &resp,
        .buffer_size = OTA_MANIFEST_HTTP_BUFFER_SIZE,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp.buffer);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        glog("OTA manifest fetch failed (err=%s, http=%d)\n", esp_err_to_name(err), status);
        free(resp.buffer);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp.buffer);
    free(resp.buffer);
    if (!root) {
        return ESP_FAIL;
    }

    cJSON *boards = cJSON_GetObjectItem(root, "boards");
    if (!boards) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    char prerelease_key[64];
    cJSON *entry = NULL;
    if (channel == 1) {
        snprintf(prerelease_key, sizeof(prerelease_key), "%s-prerelease", board_key);
        entry = cJSON_GetObjectItem(boards, prerelease_key);
    }
    if (!entry) {
        // No prerelease published for this board -- fall back to stable.
        entry = cJSON_GetObjectItem(boards, board_key);
    }
    if (!entry) {
        cJSON_Delete(root);
        glog("No OTA manifest entry for board '%s'\n", board_key);
        return ESP_OK; // not an error -- out_entry->found stays false
    }

    cJSON *j_version = cJSON_GetObjectItem(entry, "version");
    cJSON *j_commit = cJSON_GetObjectItem(entry, "commit");
    cJSON *j_build_number = cJSON_GetObjectItem(entry, "build_number");
    cJSON *j_sha256 = cJSON_GetObjectItem(entry, "sha256");
    cJSON *j_size = cJSON_GetObjectItem(entry, "size");
    cJSON *j_url = cJSON_GetObjectItem(entry, "download_url");

    if (j_version && cJSON_IsString(j_version)) {
        strncpy(out_entry->version, j_version->valuestring, sizeof(out_entry->version) - 1);
    }
    if (j_commit && cJSON_IsString(j_commit)) {
        strncpy(out_entry->commit, j_commit->valuestring, sizeof(out_entry->commit) - 1);
    }
    out_entry->build_number = (j_build_number && cJSON_IsNumber(j_build_number)) ? (long)j_build_number->valuedouble : -1;
    if (j_sha256 && cJSON_IsString(j_sha256)) {
        strncpy(out_entry->sha256, j_sha256->valuestring, sizeof(out_entry->sha256) - 1);
    }
    out_entry->size = (j_size && cJSON_IsNumber(j_size)) ? (size_t)j_size->valuedouble : 0;
    if (j_url && cJSON_IsString(j_url)) {
        strncpy(out_entry->download_url, j_url->valuestring, sizeof(out_entry->download_url) - 1);
    }
    out_entry->found = true;

    cJSON_Delete(root);
    return ESP_OK;
}

// Fetches this board's own manifest entry (honouring the stable/prerelease
// channel setting) and updates s_status/s_download_url/s_expected_sha256 /
// OTA_STATE_UPDATE_AVAILABLE accordingly. Does not download or flash anything.
static esp_err_t ota_fetch_and_parse_manifest(bool *out_update_available) {
    *out_update_available = false;

#ifndef CONFIG_BUILD_CONFIG_TEMPLATE
    ota_set_error("Board has no CONFIG_BUILD_CONFIG_TEMPLATE");
    return ESP_ERR_NOT_SUPPORTED;
#else
    OtaManifestEntry entry;
    esp_err_t err = ota_manager_fetch_manifest_entry(CONFIG_BUILD_CONFIG_TEMPLATE,
                                                      settings_get_ota_channel(&G_Settings), &entry);
    if (err != ESP_OK) {
        ota_set_error("Manifest fetch failed");
        return err;
    }
    if (!entry.found) {
        ota_set_state(OTA_STATE_IDLE);
        return ESP_OK;
    }

    long local_build = (long)(GHOSTESP_BUILD_NUMBER);
    bool update_available = (entry.build_number > 0) && (entry.build_number > local_build);

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    strncpy(s_status.latest_version, entry.version, sizeof(s_status.latest_version) - 1);
    strncpy(s_status.latest_commit, entry.commit, sizeof(s_status.latest_commit) - 1);
    s_status.image_size = entry.size;
    s_status.state = update_available ? OTA_STATE_UPDATE_AVAILABLE : OTA_STATE_IDLE;
    xSemaphoreGive(s_status_mutex);

    strncpy(s_download_url, entry.download_url, sizeof(s_download_url) - 1);
    s_download_url[sizeof(s_download_url) - 1] = '\0';
    strncpy(s_expected_sha256, entry.sha256, sizeof(s_expected_sha256) - 1);
    s_expected_sha256[sizeof(s_expected_sha256) - 1] = '\0';

    *out_update_available = update_available;
    return ESP_OK;
#endif
}

static void ota_check_task(void *pv) {
    (void)pv;
    ota_set_state(OTA_STATE_CHECKING);

    bool update_available = false;
    ota_fetch_and_parse_manifest(&update_available);

    settings_set_ota_update_available(&G_Settings, update_available);
    settings_persist_setting(SETTING_OTA_UPDATE_AVAILABLE);
    settings_set_ota_last_check_time(&G_Settings, (uint32_t)time(NULL));
    settings_persist_setting(SETTING_OTA_LAST_CHECK_TIME);

    vTaskDelete(NULL);
}

esp_err_t ota_manager_check_now(void) {
    if (!ota_manager_is_supported() || !ota_manager_board_has_network()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    BaseType_t rc = xTaskCreate(ota_check_task, "ota_check", 6144, NULL, tskIDLE_PRIORITY + 1, NULL);
    return (rc == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ota_manager_background_check(void) {
    if (!ota_manager_is_supported() || !ota_manager_board_has_network()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint32_t last_check = settings_get_ota_last_check_time(&G_Settings);
    uint32_t now = (uint32_t)time(NULL);
    if (last_check != 0 && now > last_check && (now - last_check) < OTA_BACKGROUND_CHECK_MIN_INTERVAL_SEC) {
        return ESP_OK; // checked recently, nothing to do
    }
    return ota_manager_check_now();
}

// ---------------------------------------------------------------------------
// Raw write API (esp_ota_ops directly) -- shared by the HTTPS download task
// below and by peer_ota_manager.c's GhostLink relay path.
// ---------------------------------------------------------------------------

esp_err_t ota_manager_raw_write_begin(size_t image_size) {
    if (s_raw_write_active) {
        return ESP_ERR_INVALID_STATE;
    }
    s_raw_write_partition = esp_ota_get_next_update_partition(NULL);
    if (!s_raw_write_partition) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    // esp_ota_begin treats an unknown size as OTA_SIZE_UNKNOWN (0xFFFFFFFF),
    // which erases the whole partition up front -- a literal 0 would instead
    // tell it almost nothing needs erasing.
    size_t begin_size = image_size ? image_size : OTA_SIZE_UNKNOWN;
    esp_err_t err = esp_ota_begin(s_raw_write_partition, begin_size, &s_raw_write_handle);
    if (err != ESP_OK) {
        return err;
    }
    mbedtls_sha256_init(&s_raw_write_sha_ctx);
    mbedtls_sha256_starts(&s_raw_write_sha_ctx, 0);
    s_raw_write_total = 0;
    s_raw_write_active = true;
    return ESP_OK;
}

esp_err_t ota_manager_raw_write_chunk(const uint8_t *data, size_t len) {
    if (!s_raw_write_active) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_ota_write(s_raw_write_handle, data, len);
    if (err != ESP_OK) {
        return err;
    }
    mbedtls_sha256_update(&s_raw_write_sha_ctx, data, len);
    s_raw_write_total += len;

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.bytes_downloaded = s_raw_write_total;
    xSemaphoreGive(s_status_mutex);
    return ESP_OK;
}

void ota_manager_raw_write_abort(void) {
    if (!s_raw_write_active) {
        return;
    }
    esp_ota_abort(s_raw_write_handle);
    mbedtls_sha256_free(&s_raw_write_sha_ctx);
    s_raw_write_active = false;
}

static void ota_sha256_to_hex(const uint8_t *digest, char *out_hex /* >= 65 bytes */) {
    static const char hex_chars[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2] = hex_chars[(digest[i] >> 4) & 0xF];
        out_hex[i * 2 + 1] = hex_chars[digest[i] & 0xF];
    }
    out_hex[64] = '\0';
}

esp_err_t ota_manager_raw_write_finish(const char *expected_sha256_hex) {
    if (!s_raw_write_active) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&s_raw_write_sha_ctx, digest);
    mbedtls_sha256_free(&s_raw_write_sha_ctx);

    char actual_hex[65];
    ota_sha256_to_hex(digest, actual_hex);

    esp_err_t err = esp_ota_end(s_raw_write_handle);
    s_raw_write_active = false;
    if (err != ESP_OK) {
        ota_set_error("esp_ota_end failed (image validation)");
        return err;
    }

    if (expected_sha256_hex && expected_sha256_hex[0] != '\0' &&
        strcasecmp(actual_hex, expected_sha256_hex) != 0) {
        glog("OTA sha256 mismatch: expected %s, got %s\n", expected_sha256_hex, actual_hex);
        ota_set_error("SHA-256 mismatch after write");
        return ESP_ERR_INVALID_CRC;
    }

    err = esp_ota_set_boot_partition(s_raw_write_partition);
    if (err != ESP_OK) {
        ota_set_error("Failed to set boot partition");
        return err;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// HTTPS download + flash (this device updating itself from R2)
// ---------------------------------------------------------------------------

static void ota_download_task(void *pv) {
    (void)pv;
    ota_set_state(OTA_STATE_DOWNLOADING);
    status_display_show_status("Downloading update...");

    // Safety cross-check: physical flash must match what this build assumes.
    uint32_t actual_flash_size = 0;
    if (esp_flash_get_size(NULL, &actual_flash_size) == ESP_OK) {
        uint32_t expected_min =
#if CONFIG_ESPTOOLPY_FLASHSIZE_16MB
            16u * 1024u * 1024u;
#elif CONFIG_ESPTOOLPY_FLASHSIZE_8MB
            8u * 1024u * 1024u;
#else
            0;
#endif
        if (expected_min != 0 && actual_flash_size < expected_min) {
            glog("OTA aborted: physical flash (%u) smaller than expected (%u)\n",
                 (unsigned)actual_flash_size, (unsigned)expected_min);
            ota_set_error("Flash size mismatch");
            vTaskDelete(NULL);
            return;
        }
    }

    esp_http_client_config_t http_config = {
        .url = s_download_url,
        .timeout_ms = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
    if (err != ESP_OK) {
        glog("esp_https_ota_begin failed: %s\n", esp_err_to_name(err));
        ota_set_error("Failed to start download");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        int read = esp_https_ota_get_image_len_read(handle);
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_status.bytes_downloaded = (size_t)read;
        xSemaphoreGive(s_status_mutex);
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        glog("OTA download incomplete: %s\n", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        ota_set_error("Download incomplete");
        vTaskDelete(NULL);
        return;
    }

    // Capture the total received length before esp_https_ota_finish(), which
    // invalidates the handle on success.
    size_t total_received = (size_t)esp_https_ota_get_image_len_read(handle);

    ota_set_state(OTA_STATE_VERIFYING);
    status_display_show_status("Verifying update...");

    esp_err_t finish_err = esp_https_ota_finish(handle);
    if (finish_err != ESP_OK) {
        glog("esp_https_ota_finish failed: %s\n", esp_err_to_name(finish_err));
        ota_set_error("Image validation failed");
        vTaskDelete(NULL);
        return;
    }

    // Defense-in-depth: read back what actually landed in flash and compare
    // its SHA-256 against the manifest's expected value. esp_https_ota_finish
    // already set the new partition as the next-boot target on success --
    // if our own check disagrees, revert the boot target back to the
    // currently-running (still valid) partition instead of rebooting.
    const esp_partition_t *written = esp_ota_get_boot_partition();

    mbedtls_sha256_context verify_ctx;
    mbedtls_sha256_init(&verify_ctx);
    mbedtls_sha256_starts(&verify_ctx, 0);

    size_t remaining = s_status.image_size ? s_status.image_size : total_received;
    uint8_t *readback_buf = malloc(OTA_READBACK_CHUNK_SIZE);
    bool read_ok = (readback_buf != NULL) && (written != NULL);
    size_t offset = 0;
    while (read_ok && offset < remaining) {
        size_t chunk = remaining - offset;
        if (chunk > OTA_READBACK_CHUNK_SIZE) chunk = OTA_READBACK_CHUNK_SIZE;
        if (esp_partition_read(written, offset, readback_buf, chunk) != ESP_OK) {
            read_ok = false;
            break;
        }
        mbedtls_sha256_update(&verify_ctx, readback_buf, chunk);
        offset += chunk;
    }
    free(readback_buf);

    uint8_t digest[32];
    mbedtls_sha256_finish(&verify_ctx, digest);
    mbedtls_sha256_free(&verify_ctx);
    char actual_hex[65];
    ota_sha256_to_hex(digest, actual_hex);

    bool verified = read_ok && s_expected_sha256[0] != '\0' &&
                     strcasecmp(actual_hex, s_expected_sha256) == 0;

    if (!verified) {
        glog("OTA readback verification failed; reverting boot partition\n");
        const esp_partition_t *running = esp_ota_get_running_partition();
        if (running) {
            esp_ota_set_boot_partition(running);
        }
        ota_set_error("Post-write verification failed");
        vTaskDelete(NULL);
        return;
    }

    ota_set_state(OTA_STATE_READY_TO_REBOOT);
    status_display_show_status("Update installed, rebooting...");
    glog("OTA update verified, rebooting into new firmware\n");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

esp_err_t ota_manager_start_update(void) {
    if (!ota_manager_is_supported() || !ota_manager_board_has_network()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_download_url[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    StackType_t *stack = heap_caps_malloc(OTA_DOWNLOAD_TASK_STACK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stack) {
        // Fall back to internal RAM if PSRAM isn't available on this target.
        // xTaskCreate's stack-depth argument is in bytes on ESP-IDF's FreeRTOS
        // port (unlike xTaskCreateStatic below, which takes StackType_t words).
        BaseType_t rc = xTaskCreate(ota_download_task, "ota_dl",
                                     OTA_DOWNLOAD_TASK_STACK_BYTES, NULL,
                                     5, NULL);
        return (rc == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
    }
    StaticTask_t *tcb = malloc(sizeof(StaticTask_t));
    if (!tcb) {
        heap_caps_free(stack);
        return ESP_ERR_NO_MEM;
    }
    TaskHandle_t handle = xTaskCreateStatic(ota_download_task, "ota_dl",
                                             OTA_DOWNLOAD_TASK_STACK_BYTES / sizeof(StackType_t), NULL,
                                             5, stack, tcb);
    if (!handle) {
        heap_caps_free(stack);
        free(tcb);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// SD card update: an offline alternative to the R2/HTTPS path above, reusing
// the same raw-write API (and therefore the same dual-partition rollback
// safety) that the GhostLink peer-relay path uses. User drops a renamed
// firmware.bin (plus an optional .sha256 sidecar) onto the SD card; if the
// sidecar is present its hash must match, otherwise the file is flashed
// unverified (the user already has physical possession of the card).
// ---------------------------------------------------------------------------

// Reads the first 64 hex characters found in the sidecar file (tolerates the
// standard "sha256sum" format of "<hash>  <filename>\n" as well as a bare
// hash on its own line). Returns false if no sidecar file exists.
static bool ota_sd_read_expected_sha256(char *out_hex /* >= 65 bytes */) {
    FILE *f = fopen(OTA_SD_SHA256_PATH, "r");
    if (!f) {
        return false;
    }
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    int hex_len = 0;
    for (size_t i = 0; i < n && hex_len < 64; i++) {
        char c = buf[i];
        if (isxdigit((unsigned char)c)) {
            out_hex[hex_len++] = (char)tolower((unsigned char)c);
        } else if (hex_len > 0) {
            break; // stop at the first run of hex chars (the hash itself)
        }
    }
    out_hex[hex_len] = '\0';
    return hex_len == 64;
}

static void ota_sd_update_task(void *pv) {
    (void)pv;
    ota_set_state(OTA_STATE_DOWNLOADING);
    status_display_show_status("Reading SD firmware...");

    FILE *f = fopen(OTA_SD_FIRMWARE_PATH, "rb");
    if (!f) {
        ota_set_error("No firmware_update.bin on SD card");
        vTaskDelete(NULL);
        return;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size <= 0) {
        fclose(f);
        ota_set_error("SD firmware file is empty or unreadable");
        vTaskDelete(NULL);
        return;
    }

    char expected_sha256[65] = {0};
    bool have_hash = ota_sd_read_expected_sha256(expected_sha256);
    if (!have_hash) {
        glog("No firmware_update.sha256 sidecar found -- flashing SD image unverified\n");
    }

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.image_size = (size_t)file_size;
    s_status.bytes_downloaded = 0;
    xSemaphoreGive(s_status_mutex);

    if (ota_manager_raw_write_begin((size_t)file_size) != ESP_OK) {
        fclose(f);
        ota_set_error("Failed to begin OTA write");
        vTaskDelete(NULL);
        return;
    }

    status_display_show_status("Flashing from SD...");
    uint8_t *chunk_buf = malloc(OTA_SD_READ_CHUNK_SIZE);
    bool ok = (chunk_buf != NULL);
    if (!chunk_buf) {
        ota_set_error("Out of memory reading SD firmware");
    }
    while (ok) {
        size_t n = fread(chunk_buf, 1, OTA_SD_READ_CHUNK_SIZE, f);
        if (n == 0) break;
        if (ota_manager_raw_write_chunk(chunk_buf, n) != ESP_OK) {
            ota_set_error("Flash write failed");
            ok = false;
            break;
        }
    }
    if (ok && ferror(f)) {
        ota_set_error("SD read error");
        ok = false;
    }
    free(chunk_buf);
    fclose(f);

    if (!ok) {
        ota_manager_raw_write_abort();
        vTaskDelete(NULL);
        return;
    }

    ota_set_state(OTA_STATE_VERIFYING);
    esp_err_t err = ota_manager_raw_write_finish(have_hash ? expected_sha256 : NULL);
    if (err != ESP_OK) {
        // ota_manager_raw_write_finish() already set an error message.
        vTaskDelete(NULL);
        return;
    }

    ota_set_state(OTA_STATE_READY_TO_REBOOT);
    status_display_show_status("Update installed, rebooting...");
    glog("SD OTA update applied, rebooting into new firmware\n");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

esp_err_t ota_manager_start_update_from_sd(void) {
    if (!ota_manager_is_supported()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!sd_card_manager.is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t rc = xTaskCreate(ota_sd_update_task, "ota_sd",
                                OTA_DOWNLOAD_TASK_STACK_BYTES, NULL, 5, NULL);
    return (rc == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
