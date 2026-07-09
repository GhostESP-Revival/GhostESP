#include "managers/cloud_store_manager.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "managers/http_proxy.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "gui/asset_pack.h"
#include "managers/ap_manager.h"
#include "managers/plugin_installer.h"
#include "managers/plugin_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/settings_manager.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// Catalogs live on GitHub raw; per-entry download URLs point at the CDN.
//   Apps       -> GhostESP-Revival/GhostESP-Apps/main/catalog.json
//                  each app has downloads{ <target>: <url> } and targets[]
//   Asset packs-> GhostESP-Revival/GhostESP-AssetPacks/main/catalog.json
//                  each pack has a single url string under assets[]
#ifndef GHOSTESP_APPS_CATALOG_URL
#define GHOSTESP_APPS_CATALOG_URL "https://raw.githubusercontent.com/GhostESP-Revival/GhostESP-Apps/main/catalog.json"
#endif
#ifndef GHOSTESP_ASSET_PACKS_CATALOG_URL
#define GHOSTESP_ASSET_PACKS_CATALOG_URL "https://raw.githubusercontent.com/GhostESP-Revival/GhostESP-AssetPacks/main/catalog.json"
#endif

#define CLOUD_HTTP_BUFFER_SIZE 1024
#define CLOUD_RESPONSE_INITIAL_SIZE 1024
#define CLOUD_DOWNLOAD_RANGE_CHUNK_SIZE (32 * 1024)
#define CLOUD_DOWNLOAD_RANGE_ATTEMPTS 5
#ifdef CONFIG_SPIRAM
#define CLOUD_INSTALL_TASK_STACK_BYTES 12288
#else
// The Cloud Store keeps the AP stopped on no-PSRAM boards, but their heap can
// still be fragmented enough that a 12 KB task stack cannot be allocated.
#define CLOUD_INSTALL_TASK_STACK_BYTES 8192
#endif
#define CLOUD_INSTALL_TASK_FALLBACK_STACK_BYTES 6144
#define CLOUD_DOWNLOAD_DIR "/mnt/ghostesp/downloads"
#define CLOUD_THEMES_DIR "/mnt/ghostesp/themes"

static const char *TAG = "CloudStore";

typedef struct {
    char *buffer;
    size_t len;
    size_t capacity;
} response_buf_t;

typedef struct {
    FILE *file;
    size_t written;
    size_t total;         // Content-Length if the server sent one, else 0
    size_t last_reported; // bytes at last UI status push (throttling)
    const cloud_store_item_t *item;
    bool ok;
} download_ctx_t;

// Push a live download update to the UI at most every ~16KB so the progress
// bar animates without hammering the status mutex on every TCP segment.
#define CLOUD_DOWNLOAD_REPORT_STEP (16 * 1024)

typedef struct {
    cloud_store_item_type_t type;
    char id[CLOUD_STORE_ID_MAX];
} install_request_t;

typedef struct {
    SemaphoreHandle_t mutex;
    cloud_store_status_t status;
    cloud_store_item_t *items;
    int item_count;
    bool task_running;
    volatile bool cancel_requested; // set from UI to abort an in-flight install
} cloud_store_ctx_t;

static cloud_store_ctx_t *s_ctx; // 4 bytes BSS; everything else heap-allocated on demand
static bool s_ap_paused_for_cloud;

static void cloud_store_restore_ap_if_needed(void);
static void cloud_store_pause_ap_if_needed(void);

static cloud_store_ctx_t *ensure_ctx(void) {
    if (s_ctx) return s_ctx;
    s_ctx = calloc(1, sizeof(*s_ctx));
    if (!s_ctx) return NULL;
    s_ctx->mutex = xSemaphoreCreateMutex();
    if (!s_ctx->mutex) {
        free(s_ctx);
        s_ctx = NULL;
        return NULL;
    }
    s_ctx->status.state = CLOUD_STORE_STATE_IDLE;
    return s_ctx;
}

esp_err_t cloud_store_manager_init(void) {
    if (!ensure_ctx()) return ESP_ERR_NO_MEM;
    // Keep the AP/Web UI down for the whole time the store is open (not just
    // during network ops); cloud_store_manager_cleanup() restores it on exit.
    cloud_store_pause_ap_if_needed();
    return ESP_OK;
}

void cloud_store_manager_cleanup(void) {
    if (!s_ctx) return;
    xSemaphoreTake(s_ctx->mutex, portMAX_DELAY);
    if (s_ctx->items) {
        free(s_ctx->items);
        s_ctx->items = NULL;
    }
    s_ctx->item_count = 0;
    // Items are freed to reclaim heap while the view is closed, so the cached
    // manifest no longer exists. Drop any terminal READY/DONE/FAILED state back
    // to IDLE so reopening the store triggers a fresh fetch instead of showing
    // an empty list. Leave in-flight states (FETCHING/DOWNLOADING/INSTALLING)
    // alone so a running background task is not misrepresented.
    if (s_ctx->status.state == CLOUD_STORE_STATE_READY ||
        s_ctx->status.state == CLOUD_STORE_STATE_DONE ||
        s_ctx->status.state == CLOUD_STORE_STATE_FAILED) {
        s_ctx->status.state = CLOUD_STORE_STATE_IDLE;
        s_ctx->status.error[0] = '\0';
    }
    bool still_running = s_ctx->task_running;
    char running_item[CLOUD_STORE_NAME_MAX];
    strncpy(running_item, s_ctx->status.active_name, sizeof(running_item) - 1);
    running_item[sizeof(running_item) - 1] = '\0';
    xSemaphoreGive(s_ctx->mutex);
    if (still_running) {
        ESP_LOGW(TAG, "Cloud Store closed while install/download still running (item=%s); "
                      "it continues in the background", running_item);
    }
    cloud_store_restore_ap_if_needed();
}

static void set_status(cloud_store_state_t state, const char *name, size_t done, size_t total, const char *error) {
    if (!ensure_ctx()) return;
    xSemaphoreTake(s_ctx->mutex, portMAX_DELAY);
    s_ctx->status.state = state;
    if (name) {
        strncpy(s_ctx->status.active_name, name, sizeof(s_ctx->status.active_name) - 1);
        s_ctx->status.active_name[sizeof(s_ctx->status.active_name) - 1] = '\0';
    }
    s_ctx->status.bytes_done = done;
    s_ctx->status.bytes_total = total;
    if (error) {
        strncpy(s_ctx->status.error, error, sizeof(s_ctx->status.error) - 1);
        s_ctx->status.error[sizeof(s_ctx->status.error) - 1] = '\0';
    } else if (state != CLOUD_STORE_STATE_FAILED) {
        s_ctx->status.error[0] = '\0';
    }
    xSemaphoreGive(s_ctx->mutex);
}

cloud_store_status_t cloud_store_get_status(void) {
    cloud_store_status_t copy = { .state = CLOUD_STORE_STATE_IDLE };
    if (!ensure_ctx()) return copy;
    xSemaphoreTake(s_ctx->mutex, portMAX_DELAY);
    copy = s_ctx->status;
    xSemaphoreGive(s_ctx->mutex);
    return copy;
}

static bool safe_id(const char *id) {
    if (!id || id[0] == '\0') return false;
    for (const char *p = id; *p; ++p) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) return false;
    }
    return true;
}

static bool mkdir_if_missing(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return true;
    return mkdir(path, 0775) == 0 || errno == EEXIST;
}

static void cloud_store_pause_ap_if_needed(void) {
#ifndef CONFIG_SPIRAM
    if (s_ap_paused_for_cloud || !settings_get_ap_enabled(&G_Settings)) return;
    s_ap_paused_for_cloud = true;
    ESP_LOGI(TAG, "Temporarily stopping AP/Web UI services for Cloud Store network request");
    ap_manager_stop_services_keep_wifi();
#endif
}

static void cloud_store_restore_ap_if_needed(void) {
#ifndef CONFIG_SPIRAM
    if (!s_ap_paused_for_cloud) return;
    s_ap_paused_for_cloud = false;
    if (settings_get_ap_enabled(&G_Settings)) {
        ESP_LOGI(TAG, "Restoring AP/Web UI services after Cloud Store network request");
        ap_manager_start_services();
    }
#endif
}

static bool join_path(char *dst, size_t dst_len, const char *base, const char *name) {
    int n = snprintf(dst, dst_len, "%s/%s", base, name);
    return n > 0 && (size_t)n < dst_len;
}

static void resolve_url(const char *base, const char *raw, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!raw || raw[0] == '\0') return;
    if (strncmp(raw, "https://", 8) == 0 || strncmp(raw, "http://", 7) == 0) {
        strncpy(out, raw, out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }

    const char *manifest = base ? base : GHOSTESP_APPS_CATALOG_URL;
    const char *scheme = strstr(manifest, "://");
    if (!scheme) return;
    const char *host_start = scheme + 3;
    const char *path_start = strchr(host_start, '/');
    if (!path_start) return;

    if (raw[0] == '/') {
        size_t origin_len = (size_t)(path_start - manifest);
        snprintf(out, out_len, "%.*s%s", (int)origin_len, manifest, raw);
        return;
    }

    const char *last_slash = strrchr(manifest, '/');
    size_t base_len = last_slash ? (size_t)(last_slash + 1 - manifest) : (size_t)(path_start + 1 - manifest);
    snprintf(out, out_len, "%.*s%s", (int)base_len, manifest, raw);
}

// Chip family this build runs on (e.g. "esp32s3", "esp32c5"). Matches the
// keys used in each app entry's downloads{} object.
static const char *current_target(void) {
#ifdef CONFIG_IDF_TARGET
    return CONFIG_IDF_TARGET;
#else
    return NULL;
#endif
}

static esp_err_t response_event_handler(esp_http_client_event_t *evt) {
    response_buf_t *buf = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (buf->len + evt->data_len + 1 > buf->capacity) {
        size_t new_cap = buf->capacity ? buf->capacity * 2 : CLOUD_RESPONSE_INITIAL_SIZE;
        while (new_cap < buf->len + evt->data_len + 1) new_cap *= 2;
        char *grown = heap_caps_realloc(buf->buffer, new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!grown) grown = realloc(buf->buffer, new_cap);
        if (!grown) return ESP_ERR_NO_MEM;
        buf->buffer = grown;
        buf->capacity = new_cap;
    }
    memcpy(buf->buffer + buf->len, evt->data, evt->data_len);
    buf->len += evt->data_len;
    buf->buffer[buf->len] = '\0';
    return ESP_OK;
}

static esp_err_t fetch_url_text(const char *url, char **out_text) {
    *out_text = NULL;
    response_buf_t resp = {0};

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 15000,
        .event_handler = response_event_handler,
        .user_data = &resp,
        .buffer_size = CLOUD_HTTP_BUFFER_SIZE,
    };
    char proxy_url_buf[HTTP_PROXY_URL_MAX];
    esp_err_t proxy_err = proxy_apply(&config, proxy_url_buf, sizeof(proxy_url_buf));
    if (proxy_err != ESP_OK) {
        ESP_LOGW(TAG, "catalog proxy URL failed url=%s err=%s", url, esp_err_to_name(proxy_err));
        free(resp.buffer);
        return proxy_err;
    }
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp.buffer);
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "catalog fetch failed url=%s err=%s http=%d", url, esp_err_to_name(err), status);
        free(resp.buffer);
        return ESP_FAIL;
    }
    if (!resp.buffer) {
        resp.buffer = strdup("");
        if (!resp.buffer) return ESP_ERR_NO_MEM;
    }
    *out_text = resp.buffer;
    return ESP_OK;
}

static void copy_json_string(cJSON *root, const char *key, char *dst, size_t dst_len) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(dst, item->valuestring, dst_len - 1);
        dst[dst_len - 1] = '\0';
    }
}

// authors[] array -> comma-joined string; falls back to a scalar "author" key.
static void join_authors(cJSON *root, char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    cJSON *authors = cJSON_GetObjectItemCaseSensitive(root, "authors");
    if (!cJSON_IsArray(authors)) {
        copy_json_string(root, "author", dst, dst_len);
        return;
    }
    size_t used = 0;
    cJSON *a = NULL;
    cJSON_ArrayForEach(a, authors) {
        if (!cJSON_IsString(a) || !a->valuestring) continue;
        size_t add = strlen(a->valuestring);
        size_t need = add + (used ? 2u : 0u) + 1u;
        if (need > dst_len) break;
        if (used) { dst[used++] = ','; dst[used++] = ' '; }
        memcpy(dst + used, a->valuestring, add);
        used += add;
    }
    dst[used] = '\0';
}

static bool target_supported(cJSON *entry, const char *target) {
    if (!target) return true;
    cJSON *targets = cJSON_GetObjectItemCaseSensitive(entry, "targets");
    if (!cJSON_IsArray(targets)) return true;
    cJSON *t = NULL;
    cJSON_ArrayForEach(t, targets) {
        if (cJSON_IsString(t) && t->valuestring && strcmp(t->valuestring, target) == 0) {
            return true;
        }
    }
    return false;
}

// Apps catalog: each entry has downloads{ <target>: <url> }; pick the URL for
// the running chip and skip apps that don't list this target.
static void parse_apps_array(cJSON *array, cloud_store_item_t *items, int *count) {
    if (!cJSON_IsArray(array)) return;
    const char *target = current_target();
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, array) {
        if (*count >= CLOUD_STORE_MAX_ITEMS || !cJSON_IsObject(entry)) continue;
        cloud_store_item_t item = { .type = CLOUD_STORE_TYPE_APP };
        copy_json_string(entry, "id", item.id, sizeof(item.id));
        if (!safe_id(item.id)) continue;
        if (!target_supported(entry, target)) continue;
        copy_json_string(entry, "name", item.name, sizeof(item.name));
        copy_json_string(entry, "version", item.version, sizeof(item.version));
        copy_json_string(entry, "description", item.description, sizeof(item.description));
        copy_json_string(entry, "category", item.category, sizeof(item.category));
        join_authors(entry, item.author, sizeof(item.author));
        if (item.name[0] == '\0') strncpy(item.name, item.id, sizeof(item.name) - 1);

        cJSON *downloads = cJSON_GetObjectItemCaseSensitive(entry, "downloads");
        if (cJSON_IsObject(downloads) && target) {
            cJSON *url_item = cJSON_GetObjectItemCaseSensitive(downloads, target);
            if (cJSON_IsString(url_item) && url_item->valuestring) {
                resolve_url(GHOSTESP_APPS_CATALOG_URL, url_item->valuestring,
                            item.download_url, sizeof(item.download_url));
            }
        }
        if (item.download_url[0] == '\0') continue;

        items[*count] = item;
        (*count)++;
    }
}

// Asset packs catalog: each entry has a single url string.
static void parse_assets_array(cJSON *array, cloud_store_item_t *items, int *count) {
    if (!cJSON_IsArray(array)) return;
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, array) {
        if (*count >= CLOUD_STORE_MAX_ITEMS || !cJSON_IsObject(entry)) continue;
        cloud_store_item_t item = { .type = CLOUD_STORE_TYPE_ASSET_PACK };
        copy_json_string(entry, "id", item.id, sizeof(item.id));
        if (!safe_id(item.id)) continue;
        copy_json_string(entry, "name", item.name, sizeof(item.name));
        copy_json_string(entry, "version", item.version, sizeof(item.version));
        copy_json_string(entry, "description", item.description, sizeof(item.description));
        copy_json_string(entry, "category", item.category, sizeof(item.category));
        join_authors(entry, item.author, sizeof(item.author));
        if (item.name[0] == '\0') strncpy(item.name, item.id, sizeof(item.name) - 1);

        char raw_url[CLOUD_STORE_URL_MAX] = {0};
        copy_json_string(entry, "url", raw_url, sizeof(raw_url));
        resolve_url(GHOSTESP_ASSET_PACKS_CATALOG_URL, raw_url, item.download_url, sizeof(item.download_url));
        if (item.download_url[0] == '\0') continue;

        items[*count] = item;
        (*count)++;
    }
}

static bool json_copy_string_span(const char *start, const char *end, const char *key, char *dst, size_t dst_len) {
    if (!start || !end || !key || !dst || dst_len == 0) return false;
    dst[0] = '\0';

    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = start;
    while (p && p < end) {
        p = strstr(p, needle);
        if (!p || p >= end) return false;
        p += strlen(needle);
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end || *p != ':') continue;
        p++;
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end || *p != '"') continue;
        p++;

        size_t used = 0;
        bool esc = false;
        while (p < end && (esc || *p != '"')) {
            char ch = *p++;
            if (esc) {
                esc = false;
            } else if (ch == '\\') {
                esc = true;
                continue;
            }
            if (used < dst_len - 1) dst[used++] = ch;
        }
        dst[used] = '\0';
        return used > 0;
    }
    return false;
}

static bool json_copy_first_array_string_span(const char *start, const char *end, const char *key, char *dst, size_t dst_len) {
    if (!start || !end || !key || !dst || dst_len == 0) return false;
    dst[0] = '\0';

    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(start, needle);
    if (!p || p >= end) return false;
    p += strlen(needle);
    while (p < end && *p != '[') p++;
    if (p >= end) return false;
    while (p < end && *p != '"') p++;
    if (p >= end) return false;
    p++;

    size_t used = 0;
    bool esc = false;
    while (p < end && (esc || *p != '"')) {
        char ch = *p++;
        if (esc) {
            esc = false;
        } else if (ch == '\\') {
            esc = true;
            continue;
        }
        if (used < dst_len - 1) dst[used++] = ch;
    }
    dst[used] = '\0';
    return used > 0;
}

static const char *json_object_end(const char *start) {
    bool in_str = false;
    bool esc = false;
    int depth = 0;
    for (const char *p = start; p && *p; ++p) {
        char ch = *p;
        if (esc) { esc = false; continue; }
        if (in_str && ch == '\\') { esc = true; continue; }
        if (ch == '"') { in_str = !in_str; continue; }
        if (in_str) continue;
        if (ch == '{') depth++;
        else if (ch == '}') {
            depth--;
            if (depth == 0) return p + 1;
        }
    }
    return NULL;
}

static void parse_assets_text_fallback(const char *text, cloud_store_item_t *items, int *count) {
    if (!text || !items || !count) return;
    const char *assets = strstr(text, "\"assets\"");
    if (!assets) return;
    const char *p = strchr(assets, '[');
    if (!p) return;

    while (*p && *count < CLOUD_STORE_MAX_ITEMS) {
        const char *obj = strchr(p, '{');
        if (!obj) break;
        const char *end = json_object_end(obj);
        if (!end) break;

        cloud_store_item_t item = { .type = CLOUD_STORE_TYPE_ASSET_PACK };
        json_copy_string_span(obj, end, "id", item.id, sizeof(item.id));
        if (!safe_id(item.id)) { p = end; continue; }
        json_copy_string_span(obj, end, "name", item.name, sizeof(item.name));
        json_copy_string_span(obj, end, "version", item.version, sizeof(item.version));
        json_copy_string_span(obj, end, "description", item.description, sizeof(item.description));
        json_copy_string_span(obj, end, "category", item.category, sizeof(item.category));
        if (!json_copy_first_array_string_span(obj, end, "authors", item.author, sizeof(item.author))) {
            json_copy_string_span(obj, end, "author", item.author, sizeof(item.author));
        }
        char raw_url[CLOUD_STORE_URL_MAX] = {0};
        json_copy_string_span(obj, end, "url", raw_url, sizeof(raw_url));
        resolve_url(GHOSTESP_ASSET_PACKS_CATALOG_URL, raw_url, item.download_url, sizeof(item.download_url));
        if (item.name[0] == '\0') strncpy(item.name, item.id, sizeof(item.name) - 1);
        if (item.download_url[0] != '\0') {
            items[*count] = item;
            (*count)++;
        }
        p = end;
    }
}

static esp_err_t refresh_manifest(void) {
    cloud_store_item_t *parsed = heap_caps_calloc(CLOUD_STORE_MAX_ITEMS, sizeof(cloud_store_item_t),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!parsed) parsed = calloc(CLOUD_STORE_MAX_ITEMS, sizeof(cloud_store_item_t));
    if (!parsed) return ESP_ERR_NO_MEM;
    int count = 0;

    char *text = NULL;
    if (cloud_store_apps_available() && fetch_url_text(GHOSTESP_APPS_CATALOG_URL, &text) == ESP_OK && text) {
        cJSON *root = cJSON_Parse(text);
        if (root) {
            parse_apps_array(cJSON_GetObjectItemCaseSensitive(root, "apps"), parsed, &count);
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "apps catalog JSON parse failed");
        }
        ESP_LOGI(TAG, "apps catalog parsed, total items=%d", count);
        free(text);
    }
    text = NULL;
    if (fetch_url_text(GHOSTESP_ASSET_PACKS_CATALOG_URL, &text) == ESP_OK && text) {
        int before_assets = count;
        cJSON *root = cJSON_Parse(text);
        if (root) {
            parse_assets_array(cJSON_GetObjectItemCaseSensitive(root, "assets"), parsed, &count);
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "asset catalog JSON parse failed; trying fallback parser");
        }
        if (count == before_assets) {
            parse_assets_text_fallback(text, parsed, &count);
        }
        ESP_LOGI(TAG, "asset catalog parsed, added=%d total=%d", count - before_assets, count);
        free(text);
    }

    xSemaphoreTake(s_ctx->mutex, portMAX_DELAY);
    cloud_store_item_t *old = s_ctx->items;
    s_ctx->items = parsed;
    s_ctx->item_count = count;
    xSemaphoreGive(s_ctx->mutex);
    free(old);
    return count > 0 ? ESP_OK : ESP_FAIL;
}

int cloud_store_get_count(cloud_store_item_type_t type) {
    if (!ensure_ctx()) return 0;
    int count = 0;
    xSemaphoreTake(s_ctx->mutex, portMAX_DELAY);
    if (s_ctx->items) {
        for (int i = 0; i < s_ctx->item_count; ++i) {
            if (s_ctx->items[i].type == type) count++;
        }
    }
    xSemaphoreGive(s_ctx->mutex);
    return count;
}

bool cloud_store_get_item(cloud_store_item_type_t type, int index, cloud_store_item_t *out) {
    if (!out || !ensure_ctx()) return false;
    bool found = false;
    int seen = 0;
    xSemaphoreTake(s_ctx->mutex, portMAX_DELAY);
    if (s_ctx->items) {
        for (int i = 0; i < s_ctx->item_count; ++i) {
            if (s_ctx->items[i].type != type) continue;
            if (seen == index) {
                *out = s_ctx->items[i];
                found = true;
                break;
            }
            seen++;
        }
    }
    xSemaphoreGive(s_ctx->mutex);
    return found;
}

bool cloud_store_find_item(cloud_store_item_type_t type, const char *id, cloud_store_item_t *out) {
    if (!id || !out || !ensure_ctx()) return false;
    bool found = false;
    xSemaphoreTake(s_ctx->mutex, portMAX_DELAY);
    if (s_ctx->items) {
        for (int i = 0; i < s_ctx->item_count; ++i) {
            if (s_ctx->items[i].type == type && strcmp(s_ctx->items[i].id, id) == 0) {
                *out = s_ctx->items[i];
                found = true;
                break;
            }
        }
    }
    xSemaphoreGive(s_ctx->mutex);
    return found;
}

static esp_err_t download_event_handler(esp_http_client_event_t *evt) {
    download_ctx_t *ctx = evt->user_data;
    if (!ctx) return ESP_OK;
    // Abort the transfer promptly if the user cancelled from the UI.
    if (s_ctx && s_ctx->cancel_requested) return ESP_FAIL;

    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (evt->header_key && evt->header_value &&
            strcasecmp(evt->header_key, "Content-Length") == 0) {
            long len = atol(evt->header_value);
            if (len > 0) ctx->total = (size_t)len;
        }
        return ESP_OK;
    }
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (!ctx->file || fwrite(evt->data, 1, evt->data_len, ctx->file) != (size_t)evt->data_len) {
        ctx->ok = false;
        return ESP_FAIL;
    }
    ctx->written += evt->data_len;
    // Live progress, throttled. Always report the final chunk so the bar lands
    // on 100% before the state flips to INSTALLING.
    if (ctx->written - ctx->last_reported >= CLOUD_DOWNLOAD_REPORT_STEP ||
        (ctx->total && ctx->written >= ctx->total)) {
        ctx->last_reported = ctx->written;
        set_status(CLOUD_STORE_STATE_DOWNLOADING, ctx->item ? ctx->item->name : NULL,
                   ctx->written, ctx->total, NULL);
    }
    return ESP_OK;
}

static esp_err_t download_with_retries(const cloud_store_item_t *item, const char *path) {
    download_ctx_t ctx = { .item = item, .ok = true };

    for (int attempt = 1; attempt <= CLOUD_DOWNLOAD_RANGE_ATTEMPTS; ++attempt) {
        if (s_ctx && s_ctx->cancel_requested) return ESP_FAIL;
        ctx.written = 0;
        ctx.last_reported = 0;
        ctx.total = 0;
        ctx.ok = true;
        ctx.file = fopen(path, "wb");
        if (!ctx.file) return ESP_FAIL;

        esp_http_client_config_t config = {
            .url = item->download_url,
            .timeout_ms = 60000,
            .event_handler = download_event_handler,
            .user_data = &ctx,
            .buffer_size = CLOUD_HTTP_BUFFER_SIZE,
        };
        char proxy_url_buf[HTTP_PROXY_URL_MAX];
        esp_err_t proxy_err = proxy_apply(&config, proxy_url_buf, sizeof(proxy_url_buf));
        if (proxy_err != ESP_OK) {
            ESP_LOGW(TAG, "download proxy URL failed url=%s err=%s", item->download_url, esp_err_to_name(proxy_err));
            fclose(ctx.file);
            return proxy_err;
        }
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            fclose(ctx.file);
            return ESP_FAIL;
        }

        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        fclose(ctx.file);
        ctx.file = NULL;

        if (ctx.written > 0 || ctx.total > 0) {
            set_status(CLOUD_STORE_STATE_DOWNLOADING, item->name, ctx.written, ctx.total, NULL);
        }

        if (s_ctx && s_ctx->cancel_requested) return ESP_FAIL;
        if (err != ESP_OK || !ctx.ok || (status != 200 && status != 206)) {
            ESP_LOGW(TAG, "Download failed err=%s http=%d written=%u total=%u",
                     esp_err_to_name(err), status, (unsigned)ctx.written, (unsigned)ctx.total);
            vTaskDelay(pdMS_TO_TICKS(400 * attempt));
            continue;
        }
        if (ctx.total > 0 && ctx.written == ctx.total) return ESP_OK;
        if (ctx.total == 0 && ctx.written > 0) return ESP_OK;
        ESP_LOGW("CloudStore", "Incomplete download: %d/%d bytes, retrying", (int)ctx.written, (int)ctx.total);
        vTaskDelay(pdMS_TO_TICKS(400 * attempt));
    }
    return ESP_FAIL;
}

static esp_err_t install_item(const cloud_store_item_t *item) {
    if (!safe_id(item->id)) return ESP_ERR_INVALID_ARG;
    if (!sd_card_manager.is_initialized) return ESP_ERR_INVALID_STATE;
    mkdir_if_missing(CLOUD_DOWNLOAD_DIR);
    mkdir_if_missing(CLOUD_THEMES_DIR);

    char filename[CLOUD_STORE_ID_MAX + 16];
    snprintf(filename, sizeof(filename), "%s.%s", item->id, item->type == CLOUD_STORE_TYPE_APP ? "gapp" : "gtheme.tmp");
    char download_path[CLOUD_STORE_ID_MAX + 80];
    const char *base_dir = item->type == CLOUD_STORE_TYPE_APP ? CLOUD_DOWNLOAD_DIR : CLOUD_THEMES_DIR;
    if (!join_path(download_path, sizeof(download_path), base_dir, filename)) return ESP_ERR_INVALID_SIZE;

    set_status(CLOUD_STORE_STATE_DOWNLOADING, item->name, 0, 0, NULL);
    esp_err_t err = download_with_retries(item, download_path);
    if (err != ESP_OK) return err;

    set_status(CLOUD_STORE_STATE_INSTALLING, item->name, 0, 0, NULL);
    if (item->type == CLOUD_STORE_TYPE_APP) {
        err = plugin_installer_install_gapp(download_path);
        if (err == ESP_OK) plugin_manager_reload();
        return err;
    }

    char final_name[CLOUD_STORE_ID_MAX + 16];
    snprintf(final_name, sizeof(final_name), "%s.gtheme", item->id);
    char final_path[CLOUD_STORE_ID_MAX + 80];
    if (!join_path(final_path, sizeof(final_path), CLOUD_THEMES_DIR, final_name)) return ESP_ERR_INVALID_SIZE;
    unlink(final_path);
    if (rename(download_path, final_path) != 0) return ESP_FAIL;
    asset_pack_rescan_installed();
    return ESP_OK;
}

bool cloud_store_is_available(void) {
    return sd_card_manager.is_initialized;
}

bool cloud_store_apps_available(void) {
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
}

static void refresh_task(void *arg) {
    (void)arg;
    set_status(CLOUD_STORE_STATE_FETCHING, "Cloud Store", 0, 0, NULL);
    cloud_store_pause_ap_if_needed();
    esp_err_t err = refresh_manifest();
    if (err == ESP_OK) {
        set_status(CLOUD_STORE_STATE_READY, "Cloud Store", 0, 0, NULL);
    } else {
        set_status(CLOUD_STORE_STATE_FAILED, "Cloud Store", 0, 0, "Could not load cloud manifest");
    }
    xSemaphoreTake(s_ctx->mutex, portMAX_DELAY);
    s_ctx->task_running = false;
    xSemaphoreGive(s_ctx->mutex);
    vTaskDelete(NULL);
}

esp_err_t cloud_store_refresh_async(void) {
    if (!sd_card_manager.is_initialized) return ESP_ERR_INVALID_STATE;
    if (!ensure_ctx()) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_ctx->mutex, portMAX_DELAY);
    if (s_ctx->task_running) {
        xSemaphoreGive(s_ctx->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx->task_running = true;
    xSemaphoreGive(s_ctx->mutex);
    set_status(CLOUD_STORE_STATE_FETCHING, "Cloud Store", 0, 0, NULL);
    cloud_store_pause_ap_if_needed();
    BaseType_t rc = xTaskCreate(refresh_task, "cloud_refresh", 8192, NULL, 5, NULL);
    if (rc != pdPASS) {
        xSemaphoreTake(s_ctx->mutex, portMAX_DELAY);
        s_ctx->task_running = false;
        xSemaphoreGive(s_ctx->mutex);
        cloud_store_restore_ap_if_needed();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void install_task(void *arg) {
    install_request_t req = *(install_request_t *)arg;
    free(arg);
    ESP_LOGI(TAG, "install_task started for id=%s type=%d", req.id, req.type);
    cloud_store_item_t item;
    if (!cloud_store_find_item(req.type, req.id, &item)) {
        ESP_LOGW(TAG, "install_task: id=%s not found in cached catalog", req.id);
        set_status(CLOUD_STORE_STATE_FAILED, "Cloud Store", 0, 0, "Selected item is no longer available");
    } else {
        xSemaphoreTake(s_ctx->mutex, portMAX_DELAY);
        s_ctx->status.active_type = item.type;
        xSemaphoreGive(s_ctx->mutex);
        cloud_store_pause_ap_if_needed();
        esp_err_t err = install_item(&item);
        bool cancelled = s_ctx->cancel_requested;
        if (cancelled) {
            ESP_LOGI(TAG, "install cancelled for %s", item.id);
            // Return to the loaded catalog rather than an error state; the UI
            // shows its own "cancelled" toast when it requests the cancel.
            set_status(CLOUD_STORE_STATE_READY, "Cloud Store", 0, 0, NULL);
        } else if (err == ESP_OK) {
            set_status(CLOUD_STORE_STATE_DONE, item.name, 0, 0, NULL);
        } else {
            ESP_LOGW(TAG, "install failed for %s: %s", item.id, esp_err_to_name(err));
            set_status(CLOUD_STORE_STATE_FAILED, item.name, 0, 0, esp_err_to_name(err));
        }
        // Drop the partial download on cancel/failure so we don't leave broken
        // files on the SD card (a half-written .gapp can also trip the plugin
        // scanner). Success paths keep/rename the file, so they're excluded.
        if (cancelled || err != ESP_OK) {
            char tmp_name[CLOUD_STORE_ID_MAX + 16];
            char tmp_path[CLOUD_STORE_ID_MAX + 80];
            if (item.type == CLOUD_STORE_TYPE_ASSET_PACK) {
                snprintf(tmp_name, sizeof(tmp_name), "%s.gtheme.tmp", item.id);
                if (join_path(tmp_path, sizeof(tmp_path), CLOUD_THEMES_DIR, tmp_name)) unlink(tmp_path);
            } else {
                snprintf(tmp_name, sizeof(tmp_name), "%s.gapp", item.id);
                if (join_path(tmp_path, sizeof(tmp_path), CLOUD_DOWNLOAD_DIR, tmp_name)) unlink(tmp_path);
            }
        }
    }
    xSemaphoreTake(s_ctx->mutex, portMAX_DELAY);
    s_ctx->task_running = false;
    xSemaphoreGive(s_ctx->mutex);
    vTaskDelete(NULL);
}

esp_err_t cloud_store_install_async(cloud_store_item_type_t type, const char *id) {
    if (!sd_card_manager.is_initialized) return ESP_ERR_INVALID_STATE;
    if (!safe_id(id)) return ESP_ERR_INVALID_ARG;
    if (!ensure_ctx()) return ESP_ERR_NO_MEM;
    install_request_t *req = calloc(1, sizeof(*req));
    if (!req) return ESP_ERR_NO_MEM;
    req->type = type;
    strncpy(req->id, id, sizeof(req->id) - 1);

    xSemaphoreTake(s_ctx->mutex, portMAX_DELAY);
    if (s_ctx->task_running) {
        xSemaphoreGive(s_ctx->mutex);
        free(req);
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx->task_running = true;
    s_ctx->cancel_requested = false;
    xSemaphoreGive(s_ctx->mutex);

    cloud_store_pause_ap_if_needed();
    BaseType_t rc = xTaskCreate(install_task, "cloud_install", CLOUD_INSTALL_TASK_STACK_BYTES, req, 5, NULL);
    if (rc != pdPASS && CLOUD_INSTALL_TASK_STACK_BYTES != CLOUD_INSTALL_TASK_FALLBACK_STACK_BYTES) {
        ESP_LOGW(TAG, "install task stack allocation failed (%u bytes; free=%u largest=%u); retrying with %u bytes",
                 (unsigned)CLOUD_INSTALL_TASK_STACK_BYTES,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)CLOUD_INSTALL_TASK_FALLBACK_STACK_BYTES);
        rc = xTaskCreate(install_task, "cloud_install", CLOUD_INSTALL_TASK_FALLBACK_STACK_BYTES, req, 5, NULL);
    }
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "install task failed to start (free=%u largest=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        xSemaphoreTake(s_ctx->mutex, portMAX_DELAY);
        s_ctx->task_running = false;
        xSemaphoreGive(s_ctx->mutex);
        cloud_store_restore_ap_if_needed();
        free(req);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void cloud_store_cancel_install(void) {
    if (!s_ctx) return;
    xSemaphoreTake(s_ctx->mutex, portMAX_DELAY);
    bool running = s_ctx->task_running;
    xSemaphoreGive(s_ctx->mutex);
    // Only an active download/install can be cancelled; the flag is consumed by
    // the download loop and cleared when the next install starts.
    if (running) s_ctx->cancel_requested = true;
}
