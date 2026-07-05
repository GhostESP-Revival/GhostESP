#include "managers/self_ota_manager.h"
#include "managers/ota_manager.h"
#include "managers/settings_manager.h"
#include "core/glog.h"
#include "managers/status_display_manager.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_private/system_internal.h" // esp_restart_noos() -- IRAM-resident, never returns
#include "mbedtls/private/sha256.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

// 4KB erase-then-write chunks, interleaved: each chunk's sector is erased
// and immediately rewritten before moving to the next, rather than erasing
// the whole partition first. This keeps every single interrupts/cache-
// disabled window (inside esp_flash_erase_region()/esp_flash_write(), which
// already handle that internally) short enough to not trip the interrupt
// watchdog, AND means a power loss mid-operation only ever leaves a single
// 4KB window with no valid data, instead of the entire partition erased
// before any of it is rewritten.
#define SELF_OTA_CHUNK_SIZE 4096

static SemaphoreHandle_t s_status_mutex;
static SelfOtaStatus s_status;

typedef struct { uint8_t *buf; size_t capacity; size_t written; } self_ota_dl_ctx_t;

static esp_err_t self_ota_http_event_handler(esp_http_client_event_t *evt) {
    self_ota_dl_ctx_t *ctx = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    if (ctx->written + evt->data_len > ctx->capacity) {
        return ESP_FAIL;
    }
    memcpy(ctx->buf + ctx->written, evt->data, evt->data_len);
    ctx->written += evt->data_len;

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.bytes_written = ctx->written;
    xSemaphoreGive(s_status_mutex);
    return ESP_OK;
}

bool self_ota_manager_is_supported(void) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    return strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0;
#else
    return false;
#endif
}

esp_err_t self_ota_manager_init(void) {
    if (!s_status_mutex) {
        s_status_mutex = xSemaphoreCreateMutex();
        if (!s_status_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = SELF_OTA_STATE_IDLE;
    return ESP_OK;
}

SelfOtaStatus self_ota_manager_get_status(void) {
    SelfOtaStatus copy;
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    copy = s_status;
    xSemaphoreGive(s_status_mutex);
    return copy;
}

static void self_ota_set_error(const char *msg) {
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.state = SELF_OTA_STATE_FAILED;
    strncpy(s_status.error_msg, msg, sizeof(s_status.error_msg) - 1);
    s_status.error_msg[sizeof(s_status.error_msg) - 1] = '\0';
    xSemaphoreGive(s_status_mutex);
}

// ---------------------------------------------------------------------------
// The critical section. Everything from here down to esp_restart_noos() must
// stay IRAM/DRAM-resident for the entire duration -- no glog/ESP_LOG/heap
// calls, no calls to anything not explicitly known to be IRAM-safe. This
// function overwrites the exact flash region it may itself be executing
// from, so it can never safely "return" once it starts erasing -- it always
// ends in a reset, never a normal return.
// ---------------------------------------------------------------------------
static void IRAM_ATTR self_ota_flash_and_reset(esp_flash_t *chip, uint32_t base_addr,
                                                const uint8_t *image, size_t image_size) {
    size_t offset = 0;
    while (offset < image_size) {
        size_t chunk_len = image_size - offset;
        if (chunk_len > SELF_OTA_CHUNK_SIZE) {
            chunk_len = SELF_OTA_CHUNK_SIZE;
        }
        // Erase covers the full sector even if this is the final, partial
        // chunk -- flash erase granularity requires the whole sector.
        if (esp_flash_erase_region(chip, base_addr + offset, SELF_OTA_CHUNK_SIZE) != ESP_OK) {
            break; // can't log or recover meaningfully here -- just stop and reset below
        }
        if (esp_flash_write(chip, image + offset, base_addr + offset, chunk_len) != ESP_OK) {
            break;
        }
        offset += chunk_len;
    }
    // Whether we made it all the way through or broke out early, there is no
    // safe way to "return" to the caller from here -- the flash region this
    // function's own code was mapped from may already have changed. Reset
    // unconditionally; on a partial failure the device will boot into a
    // corrupted image and need a USB/serial reflash to recover, but the
    // bootloader and partition table were never touched.
    esp_restart_noos();
}

// ---------------------------------------------------------------------------

static void self_ota_update_task(void *pv) {
    (void)pv;

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.state = SELF_OTA_STATE_CHECKING;
    xSemaphoreGive(s_status_mutex);

#ifndef CONFIG_BUILD_CONFIG_TEMPLATE
    self_ota_set_error("Board has no CONFIG_BUILD_CONFIG_TEMPLATE");
    vTaskDelete(NULL);
    return;
#else
    OtaManifestEntry entry;
    uint8_t channel = settings_get_ota_channel(&G_Settings);
    if (ota_manager_fetch_manifest_entry(CONFIG_BUILD_CONFIG_TEMPLATE, channel, &entry) != ESP_OK || !entry.found) {
        self_ota_set_error("No manifest entry for this board");
        vTaskDelete(NULL);
        return;
    }
    if (entry.size == 0 || entry.download_url[0] == '\0' || entry.sha256[0] == '\0') {
        self_ota_set_error("Manifest entry missing size/url/sha256");
        vTaskDelete(NULL);
        return;
    }
    if (entry.build_number <= 0 || entry.build_number <= (long)(GHOSTESP_BUILD_NUMBER)) {
        self_ota_set_error("Already up to date");
        vTaskDelete(NULL);
        return;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        self_ota_set_error("Could not determine running partition");
        vTaskDelete(NULL);
        return;
    }
    if (entry.size > running->size) {
        glog("Self-OTA image (%u bytes) exceeds running partition size (%u bytes)\n",
             (unsigned)entry.size, (unsigned)running->size);
        self_ota_set_error("Image too large for this board's partition");
        vTaskDelete(NULL);
        return;
    }

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.state = SELF_OTA_STATE_DOWNLOADING;
    strncpy(s_status.latest_version, entry.version, sizeof(s_status.latest_version) - 1);
    s_status.image_size = entry.size;
    s_status.bytes_written = 0;
    xSemaphoreGive(s_status_mutex);
    status_display_show_status("Downloading update...");

    uint8_t *staged = heap_caps_malloc(entry.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!staged) {
        self_ota_set_error("Out of memory staging image");
        vTaskDelete(NULL);
        return;
    }

    self_ota_dl_ctx_t dl = { .buf = staged, .capacity = entry.size, .written = 0 };

    esp_http_client_config_t http_config = {
        .url = entry.download_url,
        .timeout_ms = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .event_handler = self_ota_http_event_handler,
        .user_data = &dl,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        heap_caps_free(staged);
        self_ota_set_error("Failed to init HTTP client");
        vTaskDelete(NULL);
        return;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || dl.written != entry.size) {
        glog("Self-OTA download failed (err=%s, http=%d, got=%u/%u)\n",
             esp_err_to_name(err), status, (unsigned)dl.written, (unsigned)entry.size);
        heap_caps_free(staged);
        self_ota_set_error("Download failed");
        vTaskDelete(NULL);
        return;
    }

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.state = SELF_OTA_STATE_VERIFYING;
    xSemaphoreGive(s_status_mutex);
    status_display_show_status("Verifying update...");

    uint8_t digest[32];
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);
    mbedtls_sha256_update(&sha_ctx, staged, dl.written);
    mbedtls_sha256_finish(&sha_ctx, digest);
    mbedtls_sha256_free(&sha_ctx);

    char actual_hex[65];
    static const char hex_chars[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        actual_hex[i * 2] = hex_chars[(digest[i] >> 4) & 0xF];
        actual_hex[i * 2 + 1] = hex_chars[digest[i] & 0xF];
    }
    actual_hex[64] = '\0';

    if (strcasecmp(actual_hex, entry.sha256) != 0) {
        glog("Self-OTA sha256 mismatch: expected %s, got %s\n", entry.sha256, actual_hex);
        heap_caps_free(staged);
        self_ota_set_error("Downloaded image failed verification");
        vTaskDelete(NULL);
        return;
    }

    // Point of no return. Everything above this line can fail safely --
    // the running firmware is untouched until this point. From here on,
    // no more logging/heap/status calls are safe once we enter the actual
    // flash routine below.
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.state = SELF_OTA_STATE_FLASHING;
    xSemaphoreGive(s_status_mutex);
    status_display_show_status("Overwriting firmware -- DO NOT POWER OFF");
    glog("Self-OTA: image verified, overwriting running partition now\n");
    vTaskDelay(pdMS_TO_TICKS(300)); // let the status message actually reach the display

    // Best-effort: stop this task being tracked by the task watchdog before
    // the long uninterruptible section below (harmless if it wasn't
    // subscribed). This is a normal, non-IRAM call, safe to make here since
    // we haven't touched flash yet.
    esp_task_wdt_delete(NULL);

    esp_flash_t *chip = esp_flash_default_chip;
    self_ota_flash_and_reset(chip, running->address, staged, dl.written);
    // Unreachable: self_ota_flash_and_reset() always ends in esp_restart_noos(),
    // which never returns.
    vTaskDelete(NULL);
#endif
}

esp_err_t self_ota_manager_start_update(void) {
    if (!self_ota_manager_is_supported()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    BaseType_t rc = xTaskCreate(self_ota_update_task, "self_ota", 12288, NULL, 5, NULL);
    return (rc == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
