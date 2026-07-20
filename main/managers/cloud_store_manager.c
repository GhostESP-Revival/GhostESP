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
#include "managers/ghostscript_manager.h"
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
#ifndef GHOSTESP_SCRIPTS_CATALOG_URL
#define GHOSTESP_SCRIPTS_CATALOG_URL "https://raw.githubusercontent.com/GhostESP-Revival/GhostESP-Scripts/main/catalog.json"
#endif

#define CLOUD_HTTP_BUFFER_SIZE 1024
#define CLOUD_RESPONSE_INITIAL_SIZE 1024
#define CLOUD_CATALOG_MAX_SIZE (256 * 1024)
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
#define CLOUD_SCRIPTS_DIR GHOSTSCRIPT_ROOT_DIR

static const char *TAG = "CloudStore";

typedef struct {
    char *buffer;
    size_t len;
    size_t capacity;
} response_buf_t;

typedef struct {
    FILE *file;              // direct mode: open output file; buffered mode: NULL
    size_t written;
    size_t total;            // Content-Length if the server sent one, else 0
    size_t last_reported;    // bytes at last UI status push (byte throttle)
    TickType_t last_report_tick; // tick at last UI status push (time throttle)
    const cloud_store_item_t *item;
    bool ok;
    // Buffered mode (JIT-mount / shared display+SD SPI boards): received bytes
    // accumulate here and flush to SD one slice at a time, so the shared SPI
    // bus is only taken briefly per flush. That leaves the display live during
    // the network receive (progress bar actually animates) instead of holding
    // the bus -- and the display frozen -- for the whole download.
    const char *path;        // SD destination the buffered slices append to
    uint8_t *buf;            // NULL => direct mode (write straight to ->file)
    size_t buf_len;
    size_t buf_cap;
    bool jit;                // flushes take the shared bus via sd_card_jit_begin/end
    bool file_started;       // first flush of an attempt truncates ("wb"), rest append ("ab")
} download_ctx_t;

// Push a live download update to the UI at most every ~4KB so the progress
// bar animates without hammering the status mutex on every TCP segment. On
// slow links even 4KB can take a while to arrive, so also push on a time
// floor comfortably under the UI's 100ms poll (cloud_store_screen.c) so the
// bar keeps animating instead of sitting still between byte-threshold
// updates -- small app installs can finish in well under a second, so both
// floors need to be tight enough to land multiple frames in that window.
#define CLOUD_DOWNLOAD_REPORT_STEP (4 * 1024)
#define CLOUD_DOWNLOAD_REPORT_INTERVAL_MS 60

// Buffered-download staging size for JIT-mount boards. Received data collects
// in PSRAM and flushes to SD a slice at a time; the display bus is only taken
// during each flush, not for the whole download. 256KB -> a ~500KB asset pack
// flushes ~twice, so the bar animates smoothly with only a couple brief
// flush hitches instead of freezing at 0% the entire time.
#define CLOUD_DOWNLOAD_BUFFER_CAP (256 * 1024)

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
    if (evt->data_len <= 0) return ESP_OK;

    size_t data_len = (size_t)evt->data_len;
    if (buf->len >= CLOUD_CATALOG_MAX_SIZE - 1 ||
        data_len > CLOUD_CATALOG_MAX_SIZE - 1 - buf->len) {
        ESP_LOGW(TAG, "catalog response exceeds %u byte limit", (unsigned)CLOUD_CATALOG_MAX_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    if (buf->len + data_len + 1 > buf->capacity) {
        size_t new_cap = buf->capacity ? buf->capacity * 2 : CLOUD_RESPONSE_INITIAL_SIZE;
        while (new_cap < buf->len + data_len + 1) new_cap *= 2;
        if (new_cap > CLOUD_CATALOG_MAX_SIZE) new_cap = CLOUD_CATALOG_MAX_SIZE;
        char *grown = heap_caps_realloc(buf->buffer, new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!grown) grown = realloc(buf->buffer, new_cap);
        if (!grown) return ESP_ERR_NO_MEM;
        buf->buffer = grown;
        buf->capacity = new_cap;
    }
    memcpy(buf->buffer + buf->len, evt->data, data_len);
    buf->len += data_len;
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

// Manifest-declared byte size, if the catalog publishes one. Used as the
// progress-bar total when the download itself doesn't get a usable
// Content-Length (e.g. through a proxy that strips it).
static void copy_json_size(cJSON *root, const char *key, size_t *out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item) && item->valuedouble > 0) {
        *out = (size_t)item->valuedouble;
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
        copy_json_size(entry, "size", &item.size);
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
        copy_json_size(entry, "size", &item.size);
        if (item.name[0] == '\0') strncpy(item.name, item.id, sizeof(item.name) - 1);

        char raw_url[CLOUD_STORE_URL_MAX] = {0};
        copy_json_string(entry, "url", raw_url, sizeof(raw_url));
        resolve_url(GHOSTESP_ASSET_PACKS_CATALOG_URL, raw_url, item.download_url, sizeof(item.download_url));
        if (item.download_url[0] == '\0') continue;

        items[*count] = item;
        (*count)++;
    }
}

// Scripts catalog: each entry has a single download URL string.
static void parse_scripts_array(cJSON *array, cloud_store_item_t *items, int *count) {
    if (!cJSON_IsArray(array)) return;
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, array) {
        if (*count >= CLOUD_STORE_MAX_ITEMS || !cJSON_IsObject(entry)) continue;
        cloud_store_item_t item = { .type = CLOUD_STORE_TYPE_SCRIPT };
        copy_json_string(entry, "id", item.id, sizeof(item.id));
        if (!safe_id(item.id)) continue;
        copy_json_string(entry, "name", item.name, sizeof(item.name));
        copy_json_string(entry, "version", item.version, sizeof(item.version));
        copy_json_string(entry, "description", item.description, sizeof(item.description));
        copy_json_string(entry, "category", item.category, sizeof(item.category));
        join_authors(entry, item.author, sizeof(item.author));
        copy_json_size(entry, "size", &item.size);
        if (item.name[0] == '\0') strncpy(item.name, item.id, sizeof(item.name) - 1);

        char raw_url[CLOUD_STORE_URL_MAX] = {0};
        copy_json_string(entry, "download", raw_url, sizeof(raw_url));
        resolve_url(GHOSTESP_SCRIPTS_CATALOG_URL, raw_url, item.download_url, sizeof(item.download_url));
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
    text = NULL;
    if (fetch_url_text(GHOSTESP_SCRIPTS_CATALOG_URL, &text) == ESP_OK && text) {
        int before_scripts = count;
        cJSON *root = cJSON_Parse(text);
        if (root) {
            parse_scripts_array(cJSON_GetObjectItemCaseSensitive(root, "scripts"), parsed, &count);
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "scripts catalog JSON parse failed");
        }
        ESP_LOGI(TAG, "scripts catalog parsed, added=%d total=%d", count - before_scripts, count);
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

// Append the staged slice to the SD file, briefly taking the shared SPI bus on
// JIT boards (which suspends the display for the duration of just this flush).
// The first flush of a download attempt truncates; later flushes append.
static esp_err_t download_flush_buffer(download_ctx_t *ctx) {
    if (!ctx->buf || ctx->buf_len == 0) return ESP_OK;
    bool suspended = false;
    if (ctx->jit && !sd_card_jit_begin(&suspended, false)) return ESP_FAIL;
    esp_err_t err = ESP_OK;
    FILE *f = fopen(ctx->path, ctx->file_started ? "ab" : "wb");
    if (!f) {
        err = ESP_FAIL;
    } else {
        size_t wrote = fwrite(ctx->buf, 1, ctx->buf_len, f);
        fclose(f);
        if (wrote != ctx->buf_len) err = ESP_FAIL;
    }
    if (ctx->jit) sd_card_jit_end(suspended);
    if (err == ESP_OK) {
        ctx->file_started = true;
        ctx->buf_len = 0;
    }
    return err;
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

    if (ctx->buf) {
        // Buffered mode: copy into the RAM staging buffer, flushing a slice to
        // SD whenever it fills. The display keeps the bus between flushes.
        const uint8_t *p = (const uint8_t *)evt->data;
        size_t remaining = (size_t)evt->data_len;
        while (remaining > 0) {
            size_t space = ctx->buf_cap - ctx->buf_len;
            size_t n = remaining < space ? remaining : space;
            memcpy(ctx->buf + ctx->buf_len, p, n);
            ctx->buf_len += n;
            p += n;
            remaining -= n;
            if (ctx->buf_len == ctx->buf_cap && download_flush_buffer(ctx) != ESP_OK) {
                ctx->ok = false;
                return ESP_FAIL;
            }
        }
    } else {
        // Direct mode: SD is mounted for the whole download; write straight out.
        if (!ctx->file || fwrite(evt->data, 1, evt->data_len, ctx->file) != (size_t)evt->data_len) {
            ctx->ok = false;
            return ESP_FAIL;
        }
    }

    bool first_chunk = (ctx->written == 0);
    ctx->written += evt->data_len;
    TickType_t now = xTaskGetTickCount();
    // Live progress, throttled by whichever floor is looser: a byte step (so
    // fast links don't hammer the status mutex every TCP segment) or a time
    // floor (so slow links, where 16KB can take seconds, still animate).
    // Always report the first chunk (small/fast downloads would otherwise
    // never cross either floor) and the final chunk (lands the bar on 100%
    // before the state flips to INSTALLING).
    if (first_chunk ||
        ctx->written - ctx->last_reported >= CLOUD_DOWNLOAD_REPORT_STEP ||
        (now - ctx->last_report_tick) >= pdMS_TO_TICKS(CLOUD_DOWNLOAD_REPORT_INTERVAL_MS) ||
        (ctx->total && ctx->written >= ctx->total)) {
        ctx->last_reported = ctx->written;
        ctx->last_report_tick = now;
        set_status(CLOUD_STORE_STATE_DOWNLOADING, ctx->item ? ctx->item->name : NULL,
                   ctx->written, ctx->total, NULL);
    }
    return ESP_OK;
}

static esp_err_t download_with_retries(const cloud_store_item_t *item, const char *path) {
    bool jit = sd_card_needs_jit_mount();
    download_ctx_t ctx = { .item = item, .ok = true, .jit = jit, .path = path };

    // JIT boards stage the download in RAM so the display bus is only taken per
    // flush; otherwise the SD (== display) bus is held for the whole download
    // and the progress bar freezes. Non-JIT boards keep the simple direct path.
    // If the PSRAM staging buffer can't be had, fall back to direct write.
    if (jit) {
        ctx.buf = heap_caps_malloc(CLOUD_DOWNLOAD_BUFFER_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ctx.buf) {
            ctx.buf_cap = CLOUD_DOWNLOAD_BUFFER_CAP;
        } else {
            ESP_LOGW(TAG, "download staging buffer alloc failed; direct write (display will pause)");
        }
    }

    esp_err_t result = ESP_FAIL;
    for (int attempt = 1; attempt <= CLOUD_DOWNLOAD_RANGE_ATTEMPTS; ++attempt) {
        if (s_ctx && s_ctx->cancel_requested) { result = ESP_FAIL; break; }
        ctx.written = 0;
        ctx.last_reported = 0;
        ctx.last_report_tick = xTaskGetTickCount();
        ctx.total = 0;
        ctx.ok = true;
        ctx.buf_len = 0;
        ctx.file_started = false;
        ctx.file = NULL;

        // Direct mode holds the output file (and, on a JIT board that couldn't
        // get a buffer, the SD mount) open for the whole transfer.
        bool direct_suspended = false;
        bool direct_mounted = false;
        if (!ctx.buf) {
            if (jit) {
                if (!sd_card_jit_begin(&direct_suspended, false)) { result = ESP_FAIL; break; }
                direct_mounted = true;
            }
            ctx.file = fopen(path, "wb");
            if (!ctx.file) {
                if (direct_mounted) sd_card_jit_end(direct_suspended);
                result = ESP_FAIL;
                break;
            }
        }

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
            if (ctx.file) fclose(ctx.file);
            if (direct_mounted) sd_card_jit_end(direct_suspended);
            result = proxy_err;
            break;
        }
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            if (ctx.file) fclose(ctx.file);
            if (direct_mounted) sd_card_jit_end(direct_suspended);
            result = ESP_FAIL;
            break;
        }

        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        ESP_LOGI(TAG, "download attempt %d: err=%s http=%d written=%u total=%u",
                 attempt, esp_err_to_name(err), status, (unsigned)ctx.written, (unsigned)ctx.total);

        // Flush whatever tail is still staged before judging completeness.
        if (ctx.buf && ctx.ok && err == ESP_OK && !(s_ctx && s_ctx->cancel_requested)) {
            if (download_flush_buffer(&ctx) != ESP_OK) ctx.ok = false;
        }
        if (ctx.file) { fclose(ctx.file); ctx.file = NULL; }
        if (direct_mounted) sd_card_jit_end(direct_suspended);

        if (ctx.written > 0 || ctx.total > 0) {
            set_status(CLOUD_STORE_STATE_DOWNLOADING, item->name, ctx.written, ctx.total, NULL);
        }

        if (s_ctx && s_ctx->cancel_requested) { result = ESP_FAIL; break; }
        if (err != ESP_OK || !ctx.ok || (status != 200 && status != 206)) {
            ESP_LOGW(TAG, "Download failed err=%s http=%d written=%u total=%u",
                     esp_err_to_name(err), status, (unsigned)ctx.written, (unsigned)ctx.total);
            vTaskDelay(pdMS_TO_TICKS(400 * attempt));
            continue;
        }
        if ((ctx.total > 0 && ctx.written == ctx.total) || (ctx.total == 0 && ctx.written > 0)) {
            result = ESP_OK;
            break;
        }
        ESP_LOGW("CloudStore", "Incomplete download: %d/%d bytes, retrying", (int)ctx.written, (int)ctx.total);
        vTaskDelay(pdMS_TO_TICKS(400 * attempt));
    }

    free(ctx.buf);
    return result;
}

static esp_err_t install_item(const cloud_store_item_t *item) {
    if (!safe_id(item->id)) return ESP_ERR_INVALID_ARG;
    if (!sd_card_manager.is_initialized && !sd_card_needs_jit_mount()) {
        return ESP_ERR_NOT_FOUND;
    }

    const char *ext = (item->type == CLOUD_STORE_TYPE_APP) ? "gapp"
                    : (item->type == CLOUD_STORE_TYPE_SCRIPT) ? "gsb.tmp"
                    : "gtheme.tmp";
    char filename[CLOUD_STORE_ID_MAX + 16];
    snprintf(filename, sizeof(filename), "%s.%s", item->id, ext);
    char download_path[CLOUD_STORE_ID_MAX + 80];
    const char *base_dir = (item->type == CLOUD_STORE_TYPE_APP) ? CLOUD_DOWNLOAD_DIR
                         : (item->type == CLOUD_STORE_TYPE_SCRIPT) ? CLOUD_DOWNLOAD_DIR
                         : CLOUD_THEMES_DIR;
    if (!join_path(download_path, sizeof(download_path), base_dir, filename)) return ESP_ERR_INVALID_SIZE;

    // Phase 1 (dirs): brief SD access. On JIT boards this mounts + suspends the
    // display just long enough to ensure the staging dirs exist; on other
    // boards the SD is persistently mounted and the jit calls are no-ops.
    {
        bool suspended = false;
        if (!sd_card_jit_begin(&suspended, true)) return ESP_ERR_INVALID_STATE;
        mkdir_if_missing(CLOUD_DOWNLOAD_DIR);
        mkdir_if_missing(CLOUD_THEMES_DIR);
        mkdir_if_missing(CLOUD_SCRIPTS_DIR);
        sd_card_jit_end(suspended);
    }

    // Phase 2 (download): on JIT boards this runs with the display LIVE
    // (buffered to RAM, flushed to SD in slices), so the progress bar animates
    // instead of freezing while the SD holds the shared bus.
    set_status(CLOUD_STORE_STATE_DOWNLOADING, item->name, 0, 0, NULL);
    esp_err_t err = download_with_retries(item, download_path);
    if (err != ESP_OK) return err;

    // s_ctx->status is a single polled struct, not a queue: without this,
    // small/fast downloads finish and flip straight to INSTALLING inside one
    // scheduler slice, so the UI's 300ms poll never observes the 100%
    // download frame and the bar looks like it jumped from 0% to done.
    vTaskDelay(pdMS_TO_TICKS(350));

    // Phase 3 (extract/finalize): needs the SD held for the whole operation, so
    // on JIT boards the display is suspended here (this phase can't animate).
    // The reload/rescan below take their own nested SD mount, which is safe --
    // sd_card_mount_for_flush is depth-counted under this outer mount.
    bool suspended = false;
    if (!sd_card_jit_begin(&suspended, false)) return ESP_ERR_INVALID_STATE;
    set_status(CLOUD_STORE_STATE_INSTALLING, item->name, 0, 0, NULL);

    // Verify the download actually wrote data — a CDN 404 or empty response
    // can report HTTP 200 with zero body bytes. Check while SD is mounted.
    {
        struct stat dl_st;
        if (stat(download_path, &dl_st) != 0 || dl_st.st_size == 0) {
            ESP_LOGW(TAG, "downloaded file missing or empty for %s", item->id);
            unlink(download_path);
            sd_card_jit_end(suspended);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "downloaded %s: %ld bytes", item->id, (long)dl_st.st_size);
    }

    if (item->type == CLOUD_STORE_TYPE_APP) {
        err = plugin_installer_install_gapp(download_path);
        if (err == ESP_OK) plugin_manager_reload();
    } else if (item->type == CLOUD_STORE_TYPE_SCRIPT) {
        char script_dir[128];
        int n = snprintf(script_dir, sizeof(script_dir), "%s/%s", CLOUD_SCRIPTS_DIR, item->id);
        if (n <= 0 || (size_t)n >= sizeof(script_dir)) {
            err = ESP_ERR_INVALID_SIZE;
            ESP_LOGW(TAG, "script dir path too long for %s", item->id);
        } else {
            mkdir_if_missing(script_dir);
            char final_gsb[160];
            snprintf(final_gsb, sizeof(final_gsb), "%s/%s.gsb", script_dir, item->id);
            unlink(final_gsb);
            if (rename(download_path, final_gsb) != 0) {
                err = ESP_FAIL;
                ESP_LOGW(TAG, "rename failed for script %s: errno=%d", item->id, errno);
            } else {
                char manifest_path[160];
                snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", script_dir);
                FILE *mf = fopen(manifest_path, "w");
                if (mf) {
                    fprintf(mf, "{\"id\":\"%s\",\"name\":\"%s\",\"entry\":\"%s.gsb\"}",
                            item->id, item->name, item->id);
                    fclose(mf);
                }
                ESP_LOGI(TAG, "script installed: %s -> %s", item->id, final_gsb);
                err = ESP_OK;
            }
        }
    } else {
        char final_name[CLOUD_STORE_ID_MAX + 16];
        snprintf(final_name, sizeof(final_name), "%s.gtheme", item->id);
        char final_path[CLOUD_STORE_ID_MAX + 80];
        if (!join_path(final_path, sizeof(final_path), CLOUD_THEMES_DIR, final_name)) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            unlink(final_path);
            if (rename(download_path, final_path) != 0) {
                err = ESP_FAIL;
            } else {
                asset_pack_rescan_installed();
                err = ESP_OK;
            }
        }
    }
    sd_card_jit_end(suspended);
    return err;
}

bool cloud_store_is_available(void) {
    return sd_card_manager.is_initialized;
}

bool cloud_store_apps_available(void) {
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
}

static void refresh_task(void *arg) {
    (void)arg;
    cloud_store_pause_ap_if_needed();
    esp_err_t err = refresh_manifest();
    xSemaphoreTake(s_ctx->mutex, portMAX_DELAY);
    s_ctx->status.state = err == ESP_OK ? CLOUD_STORE_STATE_READY : CLOUD_STORE_STATE_FAILED;
    strncpy(s_ctx->status.active_name, "Cloud Store", sizeof(s_ctx->status.active_name) - 1);
    s_ctx->status.active_name[sizeof(s_ctx->status.active_name) - 1] = '\0';
    s_ctx->status.bytes_done = 0;
    s_ctx->status.bytes_total = 0;
    if (err == ESP_OK) {
        s_ctx->status.error[0] = '\0';
    } else {
        strncpy(s_ctx->status.error, "Could not load cloud manifest", sizeof(s_ctx->status.error) - 1);
        s_ctx->status.error[sizeof(s_ctx->status.error) - 1] = '\0';
    }
    s_ctx->task_running = false;
    xSemaphoreGive(s_ctx->mutex);
    vTaskDelete(NULL);
}

esp_err_t cloud_store_refresh_async(void) {
    if (!ensure_ctx()) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_ctx->mutex, portMAX_DELAY);
    if (s_ctx->task_running) {
        xSemaphoreGive(s_ctx->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx->task_running = true;
    s_ctx->status.state = CLOUD_STORE_STATE_FETCHING;
    strncpy(s_ctx->status.active_name, "Cloud Store", sizeof(s_ctx->status.active_name) - 1);
    s_ctx->status.active_name[sizeof(s_ctx->status.active_name) - 1] = '\0';
    s_ctx->status.bytes_done = 0;
    s_ctx->status.bytes_total = 0;
    s_ctx->status.error[0] = '\0';
    xSemaphoreGive(s_ctx->mutex);
    cloud_store_pause_ap_if_needed();
    BaseType_t rc = xTaskCreate(refresh_task, "cloud_refresh", 8192, NULL, 5, NULL);
    if (rc != pdPASS) {
        xSemaphoreTake(s_ctx->mutex, portMAX_DELAY);
        s_ctx->task_running = false;
        s_ctx->status.state = CLOUD_STORE_STATE_FAILED;
        strncpy(s_ctx->status.error, "Could not start cloud refresh", sizeof(s_ctx->status.error) - 1);
        s_ctx->status.error[sizeof(s_ctx->status.error) - 1] = '\0';
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
        // install_item now owns SD access per phase (dirs / buffered download /
        // extract), so the display isn't held down for the whole install on
        // JIT boards -- that's what lets the download progress bar animate.
        xSemaphoreTake(s_ctx->mutex, portMAX_DELAY);
        s_ctx->status.active_type = item.type;
        xSemaphoreGive(s_ctx->mutex);
        ESP_LOGI(TAG, "installing %s from %s", item.id, item.download_url);
        cloud_store_pause_ap_if_needed();
        esp_err_t err = install_item(&item);
        bool cancelled = s_ctx->cancel_requested;
        if (cancelled) {
            ESP_LOGI(TAG, "install cancelled for %s", item.id);
            // Return to the loaded catalog rather than an error state; the UI
            // shows its own "cancelled" toast when it requests the cancel.
            set_status(CLOUD_STORE_STATE_READY, "Cloud Store", 0, 0, NULL);
        } else if (err == ESP_OK) {
            ESP_LOGI(TAG, "install succeeded for %s", item.id);
            set_status(CLOUD_STORE_STATE_DONE, item.name, 0, 0, NULL);
        } else {
            ESP_LOGW(TAG, "install failed for %s: %s", item.id, esp_err_to_name(err));
            const char *msg = (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_STATE)
                ? "SD card required" : esp_err_to_name(err);
            set_status(CLOUD_STORE_STATE_FAILED, item.name, 0, 0, msg);
        }
        // Drop the partial download on cancel/failure so we don't leave broken
        // files on the SD card (a half-written .gapp can also trip the plugin
        // scanner). Success paths keep/rename the file, so they're excluded.
        // The SD isn't held here anymore, so take a brief mount for the unlink.
        if (cancelled || err != ESP_OK) {
            bool suspended = false;
            if (sd_card_jit_begin(&suspended, false)) {
                char tmp_name[CLOUD_STORE_ID_MAX + 16];
                char tmp_path[CLOUD_STORE_ID_MAX + 80];
                if (item.type == CLOUD_STORE_TYPE_ASSET_PACK) {
                    snprintf(tmp_name, sizeof(tmp_name), "%s.gtheme.tmp", item.id);
                    if (join_path(tmp_path, sizeof(tmp_path), CLOUD_THEMES_DIR, tmp_name)) unlink(tmp_path);
                } else if (item.type == CLOUD_STORE_TYPE_SCRIPT) {
                    snprintf(tmp_name, sizeof(tmp_name), "%s.gsb.tmp", item.id);
                    if (join_path(tmp_path, sizeof(tmp_path), CLOUD_DOWNLOAD_DIR, tmp_name)) unlink(tmp_path);
                } else {
                    snprintf(tmp_name, sizeof(tmp_name), "%s.gapp", item.id);
                    if (join_path(tmp_path, sizeof(tmp_path), CLOUD_DOWNLOAD_DIR, tmp_name)) unlink(tmp_path);
                }
                sd_card_jit_end(suspended);
            }
        }
    }
    xSemaphoreTake(s_ctx->mutex, portMAX_DELAY);
    s_ctx->task_running = false;
    xSemaphoreGive(s_ctx->mutex);
    vTaskDelete(NULL);
}

esp_err_t cloud_store_install_async(cloud_store_item_type_t type, const char *id) {
    if (!sd_card_manager.is_initialized && !sd_card_needs_jit_mount()) return ESP_ERR_NOT_FOUND;
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
