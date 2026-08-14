#include "managers/ota_manager.h"

#if GHOSTESP_OTA_SUPPORTED

#include "managers/ap_manager.h"
#include "managers/settings_manager.h"
#include "managers/sd_card_manager.h"
#include "core/glog.h"
#include "core/memory_debug.h"
#include "managers/status_display_manager.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "managers/http_proxy.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "mbedtls/private/sha256.h"

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

#define GHOSTESP_OTA_HOST "gespota.fuckyourcdn.com"

#define OTA_MANIFEST_HTTP_BUFFER_SIZE 1024
#define OTA_DOWNLOAD_TASK_STACK_BYTES 12288
#define OTA_READBACK_CHUNK_SIZE 1024
#define OTA_SD_READ_CHUNK_SIZE 4096
#define OTA_TIME_VALID_AFTER 1767225600LL /* 2026-01-01, avoids TLS cert-not-yet-valid at boot. */

#define OTA_NVS_NS "ota_mgr"
#define OTA_NVS_AP_RESTORE_KEY "ap_restore"

#define OTA_SD_FIRMWARE_PATH "/mnt/ghostesp/firmware_update.bin"
#define OTA_SD_SHA256_PATH "/mnt/ghostesp/firmware_update.sha256"
#define OTA_SD_ROOT_PATH "/mnt/ghostesp"

typedef struct {
    const char *template_key;
    const char *manifest_key;
} ota_board_key_alias_t;

static const ota_board_key_alias_t OTA_BOARD_KEY_ALIASES[] = {
    {"marauderv4", "MarauderV4_FlipperHub"},
    {"cardputer", "ESP32-S3-Cardputer"},
    {"waveshare7inch", "Waveshare_LCD"},
    {"sunton7inch", "Sunton_LCD"},
    {"LilyGo TEmbedC1101", "LilyGo-TEmbedC1101"},
    {"LilyGo T-Dongle-S3", "LilyGo-TDongleS3"},
    {"LilyGo T-Dongle-C5", "LilyGo-TDongleC5"},
    {"Cardputer ADV", "CardputerADV"},
    {"somethingsomething", "Banshee_C5"},
    {"somethingsomething2", "Banshee_S3"},
    {"xiao_esp32s3_sense", "XIAO_S3_Sense"},
    {"xiao_esp32s3", "XIAO_S3"},
};

static bool ota_key_seen(const char *const *keys, size_t count, const char *key) {
    if (!key || key[0] == '\0') return true;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(keys[i], key) == 0) return true;
    }
    return false;
}

static size_t ota_manifest_keys_for_board(const char *board_key, const char **keys, size_t max_keys) {
    size_t count = 0;
    if (!keys || max_keys == 0) return 0;

    if (!ota_key_seen(keys, count, board_key)) {
        keys[count++] = board_key;
    }

    for (size_t i = 0; i < sizeof(OTA_BOARD_KEY_ALIASES) / sizeof(OTA_BOARD_KEY_ALIASES[0]); i++) {
        const ota_board_key_alias_t *alias = &OTA_BOARD_KEY_ALIASES[i];
        const char *candidate = NULL;

        if (board_key && strcmp(board_key, alias->template_key) == 0) {
            candidate = alias->manifest_key;
        } else if (board_key && strcmp(board_key, alias->manifest_key) == 0) {
            candidate = alias->template_key;
        }

        if (candidate && !ota_key_seen(keys, count, candidate)) {
            keys[count++] = candidate;
            if (count >= max_keys) break;
        }
    }

    return count;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
static void ota_resolve_download_url(const char *raw_url, char *out, size_t out_len) {
    if (!raw_url || !out || out_len == 0) return;
    out[0] = '\0';

    if (strncmp(raw_url, "https://", 8) == 0 || strncmp(raw_url, "http://", 7) == 0) {
        strncpy(out, raw_url, out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }

    const char *manifest = GHOSTESP_OTA_MANIFEST_URL;
    const char *scheme = strstr(manifest, "://");
    if (!scheme) return;

    const char *host_start = scheme + 3;
    const char *path_start = strchr(host_start, '/');
    if (!path_start) return;

    if (raw_url[0] == '/') {
        size_t origin_len = (size_t)(path_start - manifest);
        snprintf(out, out_len, "%.*s%s", (int)origin_len, manifest, raw_url);
        return;
    }

    const char *last_slash = strrchr(manifest, '/');
    size_t base_len = last_slash ? (size_t)(last_slash + 1 - manifest) : (size_t)(path_start + 1 - manifest);
    snprintf(out, out_len, "%.*s%s", (int)base_len, manifest, raw_url);
}
#pragma GCC diagnostic pop

static bool ota_system_time_valid(void) {
    return time(NULL) >= OTA_TIME_VALID_AFTER;
}

static bool ota_wait_for_valid_time(uint32_t timeout_ms) {
    if (ota_system_time_valid()) return true;

    if (!esp_sntp_enabled()) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
    }

    uint32_t waited = 0;
    while (!ota_system_time_valid() && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(500));
        waited += 500;
    }
    if (!ota_system_time_valid()) {
        ESP_LOGW(TAG, "OTA check skipped: system time is not valid for TLS verification");
        return false;
    }
    return true;
}

static SemaphoreHandle_t s_status_mutex;
static SemaphoreHandle_t s_manifest_mutex;
static OtaStatus s_status;

// Discovered by the manifest check, consumed by the download task.
static char s_download_url[256];
static char s_expected_sha256[65];
static bool s_ap_disabled_for_download;
static volatile bool s_download_cancel_requested;
static volatile esp_http_client_handle_t s_download_client;

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

void ota_manager_cancel_update(void) {
    s_download_cancel_requested = true;
    esp_http_client_handle_t client = (esp_http_client_handle_t)s_download_client;
    if (client) {
        esp_http_client_close(client);
    }
}

static void ota_clear_available_state(void) {
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = OTA_STATE_IDLE;
    xSemaphoreGive(s_status_mutex);
    s_download_url[0] = '\0';
    s_expected_sha256[0] = '\0';
}

static void ota_restore_ap_marker_clear(void) {
    nvs_handle_t nvs;
    if (nvs_open(OTA_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_erase_key(nvs, OTA_NVS_AP_RESTORE_KEY);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void ota_restore_ap_enabled_setting(void) {
    settings_set_ap_enabled(&G_Settings, true);
    settings_persist_setting(SETTING_AP_ENABLED);
}

static void ota_restore_ap_after_download_failure(void) {
#ifndef CONFIG_SPIRAM
    if (!s_ap_disabled_for_download) return;
    s_ap_disabled_for_download = false;
    ota_restore_ap_marker_clear();
    ota_restore_ap_enabled_setting();
    ESP_LOGI(TAG, "Restored AP/Web UI setting after failed OTA download");
    ap_manager_start_services();
#endif
}

static void ota_restore_ap_after_successful_boot(void) {
#ifndef CONFIG_SPIRAM
    nvs_handle_t nvs;
    if (nvs_open(OTA_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return;

    uint8_t restore = 0;
    esp_err_t err = nvs_get_u8(nvs, OTA_NVS_AP_RESTORE_KEY, &restore);
    if (err == ESP_OK && restore) {
        nvs_erase_key(nvs, OTA_NVS_AP_RESTORE_KEY);
        nvs_commit(nvs);
        nvs_close(nvs);
        ota_restore_ap_enabled_setting();
        ESP_LOGI(TAG, "Restored AP/Web UI setting after OTA boot confirmation");
        return;
    }

    nvs_close(nvs);
#endif
}

static void ota_disable_ap_for_download_if_needed(void) {
#ifndef CONFIG_SPIRAM
    if (!settings_get_ap_enabled(&G_Settings)) return;

    nvs_handle_t nvs;
    if (nvs_open(OTA_NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        if (nvs_set_u8(nvs, OTA_NVS_AP_RESTORE_KEY, 1) == ESP_OK) {
            nvs_commit(nvs);
        }
        nvs_close(nvs);
    }

    settings_set_ap_enabled(&G_Settings, false);
    settings_persist_setting(SETTING_AP_ENABLED);
    s_ap_disabled_for_download = true;

    ESP_LOGI(TAG, "Temporarily disabling AP/Web UI for OTA download on no-PSRAM build");
    ap_manager_stop_services_keep_wifi();
#endif
}

bool ota_manager_is_supported(void) {
#if GHOSTESP_OTA_SUPPORTED
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        return false;
    }
#endif
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
    if (!s_manifest_mutex) {
        s_manifest_mutex = xSemaphoreCreateMutex();
        if (!s_manifest_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = OTA_STATE_IDLE;
    return ESP_OK;
}

bool ota_manager_has_update_ready(void) {
    return s_download_url[0] != '\0';
}

OtaStatus ota_manager_get_status(void) {
    OtaStatus copy;
    if (!s_status_mutex) {
        memset(&copy, 0, sizeof(copy));
        copy.state = OTA_STATE_IDLE;
        return copy;
    }
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    copy = s_status;
    xSemaphoreGive(s_status_mutex);
    return copy;
}

void ota_manager_confirm_boot_ok(void) {
#if GHOSTESP_OTA_SUPPORTED
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (running && esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA image marked valid; rollback cancelled");
        } else {
            ESP_LOGW(TAG, "Failed to mark OTA image valid: %s", esp_err_to_name(err));
        }
    }
    ota_restore_ap_after_successful_boot();
#endif
}

// ---------------------------------------------------------------------------
// Streaming JSON manifest parser (no full-buffer, no cJSON, no BSS)
// ---------------------------------------------------------------------------

#define MAN_KEY_MAX  32
#define MAN_VAL_MAX  256

typedef struct {
    int  pos;
    int  depth;
    bool in_str;
    bool esc;
    bool in_value;
    bool matched;
    int  key_depth;
    char key[MAN_KEY_MAX];
    int  key_pos;
    char val[MAN_VAL_MAX];
    int  val_pos;
    const char *board_key;
    const char *board_alias;
    uint8_t channel;
    bool found;
    OtaManifestEntry *out;
} manifest_ctx_t;

static void man_ctx_init(manifest_ctx_t *c, const char *bk, const char *ba,
                          uint8_t ch, OtaManifestEntry *o) {
    memset(c, 0, sizeof(*c));
    c->pos = 0; c->depth = 0; c->in_str = false; c->esc = false;
    c->in_value = false; c->matched = false; c->key_depth = 0;
    c->key_pos = 0; c->val_pos = 0;
    c->board_key = bk; c->board_alias = ba;
    c->channel = ch; c->found = false; c->out = o;
}

static bool key_eq(const char *buf, int len, const char *t) {
    return (int)strlen(t) == len && memcmp(buf, t, len) == 0;
}

static bool is_target_key(manifest_ctx_t *c) {
    if (c->key_pos == 0) return false;
    const char *k = c->key;
    int len = c->key_pos;
    char tmp[MAN_KEY_MAX];
    if (c->channel == 1) {
        if (c->board_key) {
            int n = snprintf(tmp, sizeof(tmp), "%s-prerelease", c->board_key);
            if (n > 0 && key_eq(k, len, tmp)) return true;
        }
        if (c->board_alias) {
            int n = snprintf(tmp, sizeof(tmp), "%s-prerelease", c->board_alias);
            if (n > 0 && key_eq(k, len, tmp)) return true;
        }
    }
    if (c->board_key && key_eq(k, len, c->board_key)) return true;
    if (c->board_alias && key_eq(k, len, c->board_alias)) return true;
    return false;
}

static void handle_key(manifest_ctx_t *c) {
    if (c->key_pos > 0 && is_target_key(c)) {
        c->matched = true;
        ESP_LOGI("ManParser", "MATCHED key='%.*s' depth=%d", c->key_pos, c->key, c->depth);
    } else if (c->depth == 2) {
        c->matched = false;
    }
}

static void handle_value(manifest_ctx_t *c) {
    if (!c->matched || !c->out) return;
    const char *v = c->val;
    ESP_LOGI("ManParser", "val key='%.*s' val='%s'", c->key_pos, c->key, v);
    if (key_eq(c->key, c->key_pos, "download_url")) {
        ota_resolve_download_url(v, c->out->download_url, sizeof(c->out->download_url));
    } else if (key_eq(c->key, c->key_pos, "sha256")) {
        strncpy(c->out->sha256, v, sizeof(c->out->sha256) - 1);
    } else if (key_eq(c->key, c->key_pos, "version")) {
        strncpy(c->out->version, v, sizeof(c->out->version) - 1);
    } else if (key_eq(c->key, c->key_pos, "commit")) {
        strncpy(c->out->commit, v, sizeof(c->out->commit) - 1);
    } else if (key_eq(c->key, c->key_pos, "build_number")) {
        c->out->build_number = atol(v);
    } else if (key_eq(c->key, c->key_pos, "size")) {
        c->out->size = (size_t)atol(v);
    }
    if (c->out->download_url[0] && c->out->sha256[0]) {
        c->out->found = true;
        c->found = true;
    }
}

static esp_err_t manifest_stream_handler(esp_http_client_event_t *evt) {
    manifest_ctx_t *c = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !c || !evt->data || evt->data_len == 0)
        return ESP_OK;

    const char *p = (const char *)evt->data;
    for (int i = 0; i < evt->data_len; i++) {
        char ch = p[i];
        c->pos++;

        if (c->esc) { c->esc = false; goto collect; }
        if (c->in_str && ch == '\\') { c->esc = true; continue; }

        if (!c->in_str) {
            if (ch == '"') { c->in_str = true; }
            else if (ch == '{' || ch == '[') { c->depth++; c->in_value = false; c->key_pos = 0; }
            else if (ch == '}' || ch == ']') {
                if (c->in_value && c->depth == c->key_depth) handle_value(c);
                c->depth--;
                if (c->depth == 1) { c->matched = false; c->in_value = false; }
            }
            else if (ch == ':') { c->in_value = true; c->val_pos = 0; c->val[0] = '\0'; }
            else if (ch == ',') {
                if (c->in_value && c->depth == c->key_depth) handle_value(c);
                c->in_value = false;
                c->key_pos = 0;
            }
            else if (c->in_value && c->matched && c->depth == c->key_depth &&
                     !isspace((unsigned char)ch)) {
                if (c->val_pos < MAN_VAL_MAX - 1) {
                    c->val[c->val_pos++] = ch;
                    c->val[c->val_pos] = '\0';
                }
            }
            continue;
        }

collect:
        if (c->in_str) {
            if (ch == '"') {
                c->in_str = false;
                if (c->in_value) {
                    if (c->depth == c->key_depth) handle_value(c);
                } else {
                    handle_key(c);
                }
            } else {
                if (!c->in_value) {
                    if (c->key_pos < MAN_KEY_MAX - 1) {
                        c->key[c->key_pos++] = ch;
                        c->key[c->key_pos] = '\0';
                    }
                    c->key_depth = c->depth;
                } else if (c->matched && c->depth == c->key_depth) {
                    if (c->val_pos < MAN_VAL_MAX - 1) {
                        c->val[c->val_pos++] = ch;
                        c->val[c->val_pos] = '\0';
                    }
                }
            }
        }
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Manifest fetch + parse
// ---------------------------------------------------------------------------

esp_err_t ota_manager_fetch_manifest_entry(const char *board_key, uint8_t channel, OtaManifestEntry *out_entry) {
    if (!board_key || !out_entry) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_entry, 0, sizeof(*out_entry));

    const char *lookup_keys[4] = {0};
    size_t lookup_key_count = ota_manifest_keys_for_board(board_key, lookup_keys,
                                                          sizeof(lookup_keys) / sizeof(lookup_keys[0]));
    const char *primary = (lookup_key_count > 0) ? lookup_keys[0] : board_key;
    const char *alias   = (lookup_key_count > 1) ? lookup_keys[1] : NULL;

    manifest_ctx_t ctx;
    man_ctx_init(&ctx, primary, alias, channel, out_entry);

    esp_http_client_config_t config = {
        .url = GHOSTESP_OTA_MANIFEST_URL,
        .timeout_ms = 15000,
        .event_handler = manifest_stream_handler,
        .user_data = &ctx,
        .buffer_size = OTA_MANIFEST_HTTP_BUFFER_SIZE,
    };
    char proxy_url_buf[HTTP_PROXY_URL_MAX];
    esp_err_t proxy_err = proxy_apply(&config, proxy_url_buf, sizeof(proxy_url_buf));
    if (proxy_err != ESP_OK) {
        glog("OTA manifest proxy URL failed: %s\n", esp_err_to_name(proxy_err));
        return proxy_err;
    }
    if (!s_manifest_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_manifest_mutex, portMAX_DELAY);
    memory_debug_log_snapshot("ota manifest before");
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        memory_debug_log_snapshot("ota manifest client alloc failed");
        xSemaphoreGive(s_manifest_mutex);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    memory_debug_log_snapshot("ota manifest after");
    xSemaphoreGive(s_manifest_mutex);

    if (err != ESP_OK || status != 200) {
        glog("OTA manifest fetch failed (err=%s, http=%d)\n", esp_err_to_name(err), status);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    if (ctx.found) {
        ESP_LOGI(TAG, "Manifest entry found for '%s'", board_key);
    } else {
        ESP_LOGW(TAG, "No manifest entry for '%s' (primary='%s', alias='%s')",
                 board_key, primary, alias ? alias : "(none)");
    }
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
        ota_clear_available_state();
        return ESP_OK;
    }

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    strncpy(s_status.latest_version, entry.version, sizeof(s_status.latest_version) - 1);
    strncpy(s_status.latest_commit, entry.commit, sizeof(s_status.latest_commit) - 1);
    s_status.latest_build_number = entry.build_number;
    s_status.image_size = entry.size;
    s_status.state = OTA_STATE_UPDATE_AVAILABLE;
    xSemaphoreGive(s_status_mutex);

    strncpy(s_download_url, entry.download_url, sizeof(s_download_url) - 1);
    s_download_url[sizeof(s_download_url) - 1] = '\0';
    {
        char encoded[256];
        size_t j = 0;
        for (size_t i = 0; s_download_url[i] && j < sizeof(encoded) - 4; i++) {
            if (s_download_url[i] == ' ') {
                encoded[j++] = '%';
                encoded[j++] = '2';
                encoded[j++] = '0';
            } else {
                encoded[j++] = s_download_url[i];
            }
        }
        encoded[j] = '\0';
        memcpy(s_download_url, encoded, j + 1);
    }
    strncpy(s_expected_sha256, entry.sha256, sizeof(s_expected_sha256) - 1);
    s_expected_sha256[sizeof(s_expected_sha256) - 1] = '\0';

    *out_update_available = true;
    return ESP_OK;
#endif
}

esp_err_t ota_manager_check_now_blocking(void) {
    if (!ota_manager_is_supported() || !ota_manager_board_has_network()) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    vTaskDelay(pdMS_TO_TICKS(5000));
    ota_set_state(OTA_STATE_CHECKING);

    if (!ota_wait_for_valid_time(20000)) {
        ota_set_state(OTA_STATE_IDLE);
        return ESP_ERR_TIMEOUT;
    }

    bool update_available = false;
    esp_err_t err = ota_fetch_and_parse_manifest(&update_available);

    settings_set_ota_update_available(&G_Settings, update_available);
    settings_persist_setting(SETTING_OTA_UPDATE_AVAILABLE);
    settings_set_ota_last_check_time(&G_Settings, (uint32_t)time(NULL));
    settings_persist_setting(SETTING_OTA_LAST_CHECK_TIME);

    return err;
}

static void ota_check_task(void *pv) {
    (void)pv;
    (void)ota_manager_check_now_blocking();
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
#ifndef CONFIG_SPIRAM
    ESP_LOGI(TAG, "Skipping OTA background check on no-PSRAM build");
    return ESP_OK;
#endif
    // Runs every boot -- ota_check_task fails fast (DNS/connect error) when
    // there's no real network, so there's no need to throttle by wall clock.
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
    s_download_cancel_requested = false;
    size_t ota_internal_free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t ota_psram_free_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "[OTA download] before: internal_free=%u bytes, psram_free=%u bytes",
             (unsigned)ota_internal_free_before, (unsigned)ota_psram_free_before);

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

    ota_disable_ap_for_download_if_needed();

    esp_http_client_config_t http_config = {
        .url = s_download_url,
        .timeout_ms = 60000,
        .buffer_size = OTA_SD_READ_CHUNK_SIZE,
    };
    char proxy_url_buf[HTTP_PROXY_URL_MAX];
    esp_err_t proxy_err = proxy_apply(&http_config, proxy_url_buf, sizeof(proxy_url_buf));
    if (proxy_err != ESP_OK) {
        ota_restore_ap_after_download_failure();
        ota_set_error("OTA proxy URL too long");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Starting OTA download from %s", s_download_url);
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        ota_restore_ap_after_download_failure();
        ota_set_error("Failed to create HTTP client");
        vTaskDelete(NULL);
        return;
    }
    s_download_client = client;

    uint8_t *download_buf = malloc(OTA_SD_READ_CHUNK_SIZE);
    if (!download_buf) {
        s_download_client = NULL;
        esp_http_client_cleanup(client);
        ota_restore_ap_after_download_failure();
        ota_set_error("Out of memory downloading update");
        vTaskDelete(NULL);
        return;
    }

    bool raw_write_started = false;
    size_t total_received = 0;
    esp_err_t err = s_download_cancel_requested ? ESP_ERR_INVALID_STATE : esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        int64_t content_length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            glog("OTA download HTTP status %d\n", status);
            err = ESP_FAIL;
        } else if (content_length >= 0 && s_status.image_size != 0 &&
                   (size_t)content_length != s_status.image_size) {
            glog("OTA size mismatch: manifest=%lu, http=%lld\n",
                 (unsigned long)s_status.image_size, (long long)content_length);
            err = ESP_ERR_INVALID_SIZE;
        }
    }

    if (err == ESP_OK) {
        err = ota_manager_raw_write_begin(s_status.image_size);
        raw_write_started = (err == ESP_OK);
    }

    while (err == ESP_OK && !s_download_cancel_requested) {
        int read_len = esp_http_client_read(client, (char *)download_buf, OTA_SD_READ_CHUNK_SIZE);
        if (read_len < 0) {
            err = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
            break;
        }
        err = ota_manager_raw_write_chunk(download_buf, (size_t)read_len);
        total_received += (size_t)read_len;
    }

    if (s_download_cancel_requested) {
        err = ESP_ERR_INVALID_STATE;
    }

    bool complete = err == ESP_OK && esp_http_client_is_complete_data_received(client);
    s_download_client = NULL;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(download_buf);

    if (s_download_cancel_requested) {
        if (raw_write_started) {
            ota_manager_raw_write_abort();
        }
        ota_restore_ap_after_download_failure();
        ota_set_error("Update cancelled");
        s_download_cancel_requested = false;
        vTaskDelete(NULL);
        return;
    }

    if (err != ESP_OK || !complete ||
        (s_status.image_size != 0 && total_received != s_status.image_size)) {
        glog("OTA download incomplete: err=%s, complete=%d, got=%lu/%lu\n",
             esp_err_to_name(err), complete, (unsigned long)total_received,
             (unsigned long)s_status.image_size);
        if (raw_write_started) {
            ota_manager_raw_write_abort();
        }
        ota_restore_ap_after_download_failure();
        ota_set_error("Download incomplete");
        vTaskDelete(NULL);
        return;
    }

    ota_set_state(OTA_STATE_VERIFYING);
    status_display_show_status("Verifying update...");

    err = ota_manager_raw_write_finish(s_expected_sha256);
    if (err != ESP_OK) {
        glog("OTA image validation failed: %s\n", esp_err_to_name(err));
        ota_restore_ap_after_download_failure();
        ota_set_error("Image validation failed");
        vTaskDelete(NULL);
        return;
    }

    ota_set_state(OTA_STATE_READY_TO_REBOOT);
    status_display_show_status("Update installed, rebooting...");
    glog("OTA update verified, rebooting into new firmware\n");
    ESP_LOGI(TAG, "[OTA download] after: internal_free=%u bytes (used=%d), psram_free=%u bytes (used=%d)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (int)((long)ota_internal_free_before - (long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (int)((long)ota_psram_free_before - (long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
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

    StackType_t *stack = heap_caps_malloc(OTA_DOWNLOAD_TASK_STACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!stack) {
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
                                             OTA_DOWNLOAD_TASK_STACK_BYTES, NULL,
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
// safety) that the GhostLink peer-relay path uses. The app-only firmware.bin
// from the GitHub zip is valid here; merged.bin is a full flash image and must
// not be written into an OTA slot. If a .sha256 sidecar is present its hash must
// match, otherwise the file is flashed unverified (the user already has
// physical possession of the card).
// ---------------------------------------------------------------------------

// Reads the first 64 hex characters found in the sidecar file (tolerates the
// standard "sha256sum" format of "<hash>  <filename>\n" as well as a bare
// hash on its own line). Returns false if no sidecar file exists.
static bool ota_sd_file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    fclose(f);
    return true;
}

static bool ota_sd_try_firmware_path(const char *path, char *out_path, size_t out_len) {
    if (!path || !out_path || out_len == 0 || !ota_sd_file_exists(path)) {
        return false;
    }
    strncpy(out_path, path, out_len - 1);
    out_path[out_len - 1] = '\0';
    return true;
}

static const char *ota_sd_artifact_base_for_board(void) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    const char *board = CONFIG_BUILD_CONFIG_TEMPLATE;
    if (strcmp(board, "marauderv4") == 0) return "MarauderV4_FlipperHub";
    if (strcmp(board, "cardputer") == 0) return "ESP32-S3-Cardputer";
    if (strcmp(board, "waveshare7inch") == 0) return "Waveshare_LCD";
    if (strcmp(board, "sunton7inch") == 0) return "Sunton_LCD";
    if (strcmp(board, "JC3248W535EN") == 0) return "JC3248W535EN_LCD";
    if (strcmp(board, "LilyGo TEmbedC1101") == 0) return "LilyGo-TEmbedC1101";
    if (strcmp(board, "LilyGo T-Dongle-S3") == 0) return "LilyGo-TDongleS3";
    if (strcmp(board, "LilyGo T-Dongle-C5") == 0) return "LilyGo-TDongleC5";
    if (strcmp(board, "S3TWatch") == 0) return "LilyGo-S3TWatch-2020";
    if (strcmp(board, "Cardputer ADV") == 0) return "CardputerADV";
    if (strcmp(board, "somethingsomething") == 0) return "Banshee_C5";
    if (strcmp(board, "somethingsomething2") == 0) return "Banshee_S3";
    if (strcmp(board, "NM-CYD-C5") == 0) return "NM-CYD-C5";
    if (strcmp(board, "xiao_esp32s3_sense") == 0) return "XIAO_S3_Sense";
    if (strcmp(board, "xiao_esp32s3") == 0) return "XIAO_S3";
#endif
    return NULL;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
static bool ota_sd_find_firmware(char *out_path, size_t out_len) {
    if (ota_sd_try_firmware_path(OTA_SD_FIRMWARE_PATH, out_path, out_len)) return true;
    if (ota_sd_try_firmware_path(OTA_SD_ROOT_PATH "/firmware.bin", out_path, out_len)) return true;

    const char *artifact_base = ota_sd_artifact_base_for_board();
    if (artifact_base && artifact_base[0]) {
        char candidate[256];
        snprintf(candidate, sizeof(candidate), OTA_SD_ROOT_PATH "/%s/firmware.bin", artifact_base);
        if (ota_sd_try_firmware_path(candidate, out_path, out_len)) return true;
        snprintf(candidate, sizeof(candidate), OTA_SD_ROOT_PATH "/%s.bin", artifact_base);
        if (ota_sd_try_firmware_path(candidate, out_path, out_len)) return true;
    }

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    char candidate[256];
    snprintf(candidate, sizeof(candidate), OTA_SD_ROOT_PATH "/%s/firmware.bin", CONFIG_BUILD_CONFIG_TEMPLATE);
    if (ota_sd_try_firmware_path(candidate, out_path, out_len)) return true;
    snprintf(candidate, sizeof(candidate), OTA_SD_ROOT_PATH "/%s.bin", CONFIG_BUILD_CONFIG_TEMPLATE);
    if (ota_sd_try_firmware_path(candidate, out_path, out_len)) return true;
#endif

    return false;
}
#pragma GCC diagnostic pop

static bool ota_sd_read_expected_sha256(const char *firmware_path, char *out_hex /* >= 65 bytes */) {
    char sidecar_path[320];
    const char *paths[3] = {NULL, NULL, NULL};

    if (firmware_path && firmware_path[0]) {
        snprintf(sidecar_path, sizeof(sidecar_path), "%s.sha256", firmware_path);
        paths[0] = sidecar_path;
        if (strcmp(firmware_path, OTA_SD_FIRMWARE_PATH) == 0) {
            paths[1] = OTA_SD_SHA256_PATH;
        }
    }

    FILE *f = NULL;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (!paths[i]) continue;
        f = fopen(paths[i], "r");
        if (f) break;
    }
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
    size_t sd_ota_internal_free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t sd_ota_psram_free_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "[SD OTA] before: internal_free=%u bytes, psram_free=%u bytes",
             (unsigned)sd_ota_internal_free_before, (unsigned)sd_ota_psram_free_before);

    char firmware_path[256];
    if (!ota_sd_find_firmware(firmware_path, sizeof(firmware_path))) {
        if (ota_sd_file_exists(OTA_SD_ROOT_PATH "/merged.bin")) {
            ota_set_error("Use firmware.bin, not merged.bin");
        } else {
            ota_set_error("No firmware.bin on SD card");
        }
        vTaskDelete(NULL);
        return;
    }

    FILE *f = fopen(firmware_path, "rb");
    if (!f) {
        ota_set_error("SD firmware file unreadable");
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
    bool have_hash = ota_sd_read_expected_sha256(firmware_path, expected_sha256);
    if (!have_hash) {
        glog("No SD firmware sha256 sidecar found -- flashing SD image unverified\n");
    }
    glog("Installing SD firmware from %s\n", firmware_path);

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
    ESP_LOGI(TAG, "[SD OTA] after: internal_free=%u bytes (used=%d), psram_free=%u bytes (used=%d)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (int)((long)sd_ota_internal_free_before - (long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (int)((long)sd_ota_psram_free_before - (long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
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

#else

bool ota_manager_is_supported(void) { return false; }
esp_err_t ota_manager_init(void) { return ESP_OK; }
esp_err_t ota_manager_check_now(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ota_manager_check_now_blocking(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ota_manager_background_check(void) { return ESP_ERR_NOT_SUPPORTED; }
bool ota_manager_has_update_ready(void) { return false; }
esp_err_t ota_manager_start_update(void) { return ESP_ERR_NOT_SUPPORTED; }
void ota_manager_cancel_update(void) {}
esp_err_t ota_manager_start_update_from_sd(void) { return ESP_ERR_NOT_SUPPORTED; }
OtaStatus ota_manager_get_status(void) { return (OtaStatus){ .state = OTA_STATE_IDLE }; }
esp_err_t ota_manager_fetch_manifest_entry(const char *board_key, uint8_t channel, OtaManifestEntry *out_entry) {
    (void)board_key;
    (void)channel;
    if (out_entry) *out_entry = (OtaManifestEntry){0};
    return ESP_ERR_NOT_SUPPORTED;
}
void ota_manager_confirm_boot_ok(void) {}
esp_err_t ota_manager_raw_write_begin(size_t image_size) { (void)image_size; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ota_manager_raw_write_chunk(const uint8_t *data, size_t len) { (void)data; (void)len; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ota_manager_raw_write_finish(const char *expected_sha256_hex) { (void)expected_sha256_hex; return ESP_ERR_NOT_SUPPORTED; }
void ota_manager_raw_write_abort(void) {}

#endif
