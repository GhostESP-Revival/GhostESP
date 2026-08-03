#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "mbedtls/private/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define SELF_OTA_NVS_NS "self_ota"
#define SELF_OTA_KEY_URL "url"
#define SELF_OTA_KEY_SHA256 "sha256"
#define SELF_OTA_KEY_SIZE "size"
#define SELF_OTA_KEY_STA_SSID "sta_ssid"
#define SELF_OTA_KEY_STA_PASSWORD "sta_password"
#define SELF_OTA_KEY_LAST_ERROR "last_err"
#define SETTINGS_NVS_NS "storage"
#define SETTINGS_KEY_STA_SSID "sta_ssid"
#define SETTINGS_KEY_STA_PASSWORD "sta_password"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_CONNECT_TIMEOUT_MS 30000
#define UPDATE_RANGE_CHUNK_SIZE (32 * 1024)
#define UPDATE_RANGE_ATTEMPTS 5

static const char *TAG = "BansheeUpdater";
static EventGroupHandle_t s_wifi_events;

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t written;
} updater_http_ctx_t;

static void sha256_to_hex(const uint8_t *digest, char *out_hex) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2] = hex[(digest[i] >> 4) & 0x0f];
        out_hex[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out_hex[64] = '\0';
}

static esp_err_t nvs_read_str(nvs_handle_t h, const char *key, char *out, size_t out_len) {
    if (!out || out_len == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    size_t len = out_len;
    return nvs_get_str(h, key, out, &len);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_connect_from_nvs(void) {
    char ssid[65] = {0};
    char password[65] = {0};
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SELF_OTA_NVS_NS, NVS_READONLY, &nvs);

    if (err == ESP_OK) {
        err = nvs_read_str(nvs, SELF_OTA_KEY_STA_SSID, ssid, sizeof(ssid));
        if (err == ESP_OK) {
            esp_err_t pass_err = nvs_read_str(nvs, SELF_OTA_KEY_STA_PASSWORD, password, sizeof(password));
            if (pass_err != ESP_OK) password[0] = '\0';
        }
        nvs_close(nvs);
    }

    if (err != ESP_OK || ssid[0] == '\0') {
        err = nvs_open(SETTINGS_NVS_NS, NVS_READONLY, &nvs);
        if (err != ESP_OK) return err;

        err = nvs_read_str(nvs, SETTINGS_KEY_STA_SSID, ssid, sizeof(ssid));
        if (err == ESP_OK) {
            esp_err_t pass_err = nvs_read_str(nvs, SETTINGS_KEY_STA_PASSWORD, password, sizeof(password));
            if (pass_err != ESP_OK) password[0] = '\0';
        }
        nvs_close(nvs);
    }

    if (err != ESP_OK || ssid[0] == '\0') {
        ESP_LOGE(TAG, "No saved STA credentials");
        return ESP_ERR_NOT_FOUND;
    }

    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "WiFi connect timed out");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "WiFi connected");
    return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;

    updater_http_ctx_t *ctx = (updater_http_ctx_t *)evt->user_data;
    if (!ctx || !evt->data || evt->data_len <= 0) return ESP_OK;

    if (ctx->written + (size_t)evt->data_len > ctx->capacity) {
        ESP_LOGE(TAG, "Downloaded image exceeds expected size");
        return ESP_FAIL;
    }

    memcpy(ctx->buffer + ctx->written, evt->data, evt->data_len);
    ctx->written += evt->data_len;
    return ESP_OK;
}

static esp_err_t download_with_retries(const char *url, uint8_t *staged, size_t image_size) {
    size_t downloaded = 0;

    while (downloaded < image_size) {
        size_t start = downloaded;
        size_t end = start + UPDATE_RANGE_CHUNK_SIZE - 1;
        if (end >= image_size) end = image_size - 1;
        size_t target = end + 1;
        bool made_progress = false;

        for (int attempt = 1; attempt <= UPDATE_RANGE_ATTEMPTS; attempt++) {
            updater_http_ctx_t ctx = {
                .buffer = staged,
                .capacity = image_size,
                .written = downloaded,
            };

            esp_http_client_config_t http_config = {
                .url = url,
                .timeout_ms = 60000,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .buffer_size = 4096,
                .event_handler = http_event_handler,
                .user_data = &ctx,
                .keep_alive_enable = true,
            };

            esp_http_client_handle_t client = esp_http_client_init(&http_config);
            if (!client) return ESP_ERR_NO_MEM;

            char range_header[64];
            snprintf(range_header, sizeof(range_header), "bytes=%lu-%lu",
                     (unsigned long)downloaded, (unsigned long)end);
            esp_http_client_set_header(client, "Range", range_header);

            esp_err_t err = esp_http_client_perform(client);
            int status = esp_http_client_get_status_code(client);
            esp_http_client_cleanup(client);

            bool status_ok = (status == 206) || (downloaded == 0 && status == 200);
            if (!status_ok) {
                ESP_LOGE(TAG, "Unexpected HTTP status %d for range %lu-%lu",
                         status, (unsigned long)downloaded, (unsigned long)end);
                return ESP_FAIL;
            }

            if (ctx.written > image_size) return ESP_FAIL;

            if (ctx.written >= target || ctx.written == image_size) {
                downloaded = ctx.written;
                made_progress = true;
                ESP_LOGI(TAG, "Downloaded %lu/%lu bytes", (unsigned long)downloaded, (unsigned long)image_size);
                break;
            }

            if (ctx.written > downloaded) {
                ESP_LOGW(TAG, "Range interrupted (err=%s, http=%d, got=%lu/%lu), continuing",
                         esp_err_to_name(err), status, (unsigned long)ctx.written, (unsigned long)image_size);
                downloaded = ctx.written;
                made_progress = true;
                break;
            }

            ESP_LOGW(TAG, "Range made no progress (err=%s, http=%d, range=%lu-%lu, attempt=%d)",
                     esp_err_to_name(err), status, (unsigned long)start, (unsigned long)end, attempt);
            vTaskDelay(pdMS_TO_TICKS(400 * attempt));
        }

        if (!made_progress) return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static esp_err_t read_pending_update(char *url, size_t url_len, char *sha256, size_t sha_len, uint32_t *size_out) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SELF_OTA_NVS_NS, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_read_str(nvs, SELF_OTA_KEY_URL, url, url_len);
    if (err == ESP_OK) err = nvs_read_str(nvs, SELF_OTA_KEY_SHA256, sha256, sha_len);
    if (err == ESP_OK) err = nvs_get_u32(nvs, SELF_OTA_KEY_SIZE, size_out);
    nvs_close(nvs);
    return err;
}

static void clear_pending_update(void) {
    nvs_handle_t nvs;
    if (nvs_open(SELF_OTA_NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, SELF_OTA_KEY_URL);
        nvs_erase_key(nvs, SELF_OTA_KEY_SHA256);
        nvs_erase_key(nvs, SELF_OTA_KEY_SIZE);
        nvs_erase_key(nvs, SELF_OTA_KEY_STA_SSID);
        nvs_erase_key(nvs, SELF_OTA_KEY_STA_PASSWORD);
        nvs_erase_key(nvs, SELF_OTA_KEY_LAST_ERROR);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static void record_failure(const char *stage, esp_err_t err) {
    nvs_handle_t nvs;
    if (nvs_open(SELF_OTA_NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s: %s", stage ? stage : "Updater failed", esp_err_to_name(err));
        nvs_set_str(nvs, SELF_OTA_KEY_LAST_ERROR, msg);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static void reboot_to_app0(void) {
    const esp_partition_t *target = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                            ESP_PARTITION_SUBTYPE_APP_FACTORY,
                                                            "app0");
    if (target) {
        esp_err_t err = esp_ota_set_boot_partition(target);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to select app0 fallback: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "Could not find app0 fallback partition");
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t apply_update(bool *app0_safe_out) {
    if (app0_safe_out) *app0_safe_out = true;

    char url[256];
    char expected_sha[65];
    uint32_t expected_size = 0;
    esp_err_t err = read_pending_update(url, sizeof(url), expected_sha, sizeof(expected_sha), &expected_size);
    if (err != ESP_OK || url[0] == '\0' || expected_sha[0] == '\0' || expected_size == 0) {
        ESP_LOGE(TAG, "No pending update in NVS");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_partition_t *target = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                            ESP_PARTITION_SUBTYPE_APP_FACTORY,
                                                            "app0");
    if (!target) {
        ESP_LOGE(TAG, "Could not find app0 partition");
        return ESP_ERR_NOT_FOUND;
    }
    if (expected_size > target->size) {
        ESP_LOGE(TAG, "Image too large: %lu > %lu", (unsigned long)expected_size, (unsigned long)target->size);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *staged = heap_caps_malloc(expected_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!staged) {
        ESP_LOGE(TAG, "Out of PSRAM staging %lu byte firmware", (unsigned long)expected_size);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Downloading firmware to PSRAM (%lu bytes)", (unsigned long)expected_size);
    err = download_with_retries(url, staged, expected_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Download failed: %s", esp_err_to_name(err));
        heap_caps_free(staged);
        return err;
    }

    uint8_t digest[32];
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);
    mbedtls_sha256_update(&sha_ctx, staged, expected_size);
    mbedtls_sha256_finish(&sha_ctx, digest);
    mbedtls_sha256_free(&sha_ctx);

    char actual_sha[65];
    sha256_to_hex(digest, actual_sha);
    if (strcasecmp(actual_sha, expected_sha) != 0) {
        ESP_LOGE(TAG, "SHA mismatch: expected %s got %s", expected_sha, actual_sha);
        heap_caps_free(staged);
        return ESP_ERR_INVALID_CRC;
    }

    ESP_LOGI(TAG, "Firmware verified; erasing app0 (%lu bytes)", (unsigned long)target->size);
    if (app0_safe_out) *app0_safe_out = false;
    err = esp_partition_erase_range(target, 0, target->size);
    if (err != ESP_OK) {
        heap_caps_free(staged);
        return err;
    }

    ESP_LOGI(TAG, "Writing firmware to app0");
    err = esp_partition_write(target, 0, staged, expected_size);
    heap_caps_free(staged);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_write failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to select app0: %s", esp_err_to_name(err));
        return err;
    }

    clear_pending_update();
    ESP_LOGI(TAG, "Update complete; rebooting to app0");
    return ESP_OK;
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    for (;;) {
        err = wifi_connect_from_nvs();
        if (err == ESP_OK) {
            bool app0_safe = true;
            err = apply_update(&app0_safe);
            if (err == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
            record_failure("Updater failed", err);
            ESP_LOGE(TAG, "Updater failed: %s", esp_err_to_name(err));
            if (app0_safe) {
                ESP_LOGI(TAG, "app0 untouched; returning to main firmware");
                reboot_to_app0();
            }
        } else {
            record_failure("WiFi connect failed", err);
            ESP_LOGE(TAG, "WiFi connect failed: %s; returning to main firmware", esp_err_to_name(err));
            reboot_to_app0();
        }

        ESP_LOGE(TAG, "Updater failed after app0 write started: %s; retrying after reboot", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }
}
