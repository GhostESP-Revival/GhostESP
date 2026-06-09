#include "gui/asset_pack.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gui/screen_layout.h"
#include "managers/display_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/views/options_screen.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "lgfx/utility/lgfx_miniz.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#define ACTIVE_PACK_DIR "/mnt/ghostesp/themes/active"
#define ACTIVE_MANIFEST ACTIVE_PACK_DIR "/manifest.json"
#define ACTIVE_GTHEME "/mnt/ghostesp/themes/active.gtheme"
#define THEMES_DIR "/mnt/ghostesp/themes"
#define THEME_CACHE_DIR THEMES_DIR "/.cache"
#define ASSET_PACK_NVS_NS "asset_pack"
#define ASSET_PACK_NVS_ACTIVE "active"
#define ASSET_PACK_MAX_ICONS 32
#define ASSET_PACK_ICON_CACHE_MAX 32
#define ASSET_PACK_ICON_CACHE_INTERNAL 2
#define ASSET_PACK_PATH_MAX 128
#define ASSET_PACK_MANIFEST_MAX (16 * 1024)
#define ASSET_PACK_ICON_MAX_RAW (128 * 128 * 3)
/* Internal-RAM-only icon cap: 32x32 RGB565A8 = 3 KB. Keeps the worst-case
 * icon cache footprint to 2 * 3 KB = 6 KB on devices without PSRAM. */
#define ASSET_PACK_ICON_MAX_RAW_INTERNAL (32 * 32 * 3)
/* Internal-RAM-only bg tile cap: 32x32 RGB565A8 = 3 KB (matches icon cap). */
#define ASSET_PACK_BG_MAX_RAW_INTERNAL (32 * 32 * 3)
#define ASSET_PACK_BG_FULLSCREEN_MAX_RAW (320 * 240 * 2)
#define ASSET_PACK_GTHEME_MAX_FILE (256 * 1024)
#define ASSET_PACK_EXTRACT_BUF_SIZE 4096
#define ASSET_PACK_GTHEME_MAX_FILES 128
#define ASSET_PACK_NAME_MAX 32
#define ASSET_PACK_FAILED_PATH_MAX 16
#define ASSET_PACK_BG_CANDIDATE_MAX 3

#define GIMG_FORMAT_RGB565 1
#define GIMG_FORMAT_RGB565A8 2
/* 16-color indexed. Payload layout: [16 * lv_color32_t palette = 64 B]
 * followed by packed 4-bit pixel indices (low nibble = even pixel).
 * LVGL renders natively via LV_IMG_CF_INDEXED_4BIT. */
#define GIMG_FORMAT_INDEXED_4BPP 3
#define GIMG_COMP_NONE 0
#define GIMG_COMP_DEFLATE_RAW 1

static const char *TAG = "AssetPack";

typedef struct {
    char *name;
    char *small;
    char *large;
    char *xl;
} asset_icon_entry_t;

typedef struct {
    /* FNV-1a 64-bit hash of the resolved file path within the active pack
     * (e.g. "icons/wifi.gimg"). Keyed by path rather than icon name so
     * multiple icon entries pointing at the same file share a single
     * decoded image in the cache. */
    uint64_t key_hash;
    lv_img_dsc_t dsc;
    uint8_t *data;
    uint32_t last_used;
} asset_icon_cache_entry_t;

static bool s_loaded = false;
static bool s_loading_attempted = false;
static uint32_t s_version = 0;
static bool s_has_psram = false;
static bool s_has_color[6];
static uint32_t s_colors[6];
static asset_icon_entry_t *s_icons = NULL;
static int s_icon_count = 0;
/* Icon cache slot array is heap-allocated in detect_psram_and_configure() and
 * sized to s_icon_cache_size, so internal-RAM builds only pay for the slots
 * they actually use (2 instead of 32). */
static asset_icon_cache_entry_t *s_icon_cache = NULL;
static int s_icon_cache_size = ASSET_PACK_ICON_CACHE_INTERNAL;
static uint32_t s_cache_tick = 0;
static char s_bg_tile[ASSET_PACK_PATH_MAX];
static bool s_bg_scale_to_fill = false;
static char s_bg_candidates[ASSET_PACK_BG_CANDIDATE_MAX][ASSET_PACK_PATH_MAX];
static bool s_bg_candidate_scale[ASSET_PACK_BG_CANDIDATE_MAX];
static int s_bg_candidate_count = 0;
static int s_bg_candidate_index = 0;
static char s_app_icon_key[ASSET_PACK_NAME_MAX];
static char s_pack_dir[ASSET_PACK_PATH_MAX] = ACTIVE_PACK_DIR;
static char s_active_name[ASSET_PACK_NAME_MAX] = "active";
static bool s_active_is_archive = false;
static lv_img_dsc_t s_bg_tile_dsc;
static uint8_t *s_bg_tile_data = NULL;
static lv_img_dsc_t s_bg_fullscreen_dsc;
static uint8_t *s_bg_fullscreen_data = NULL;

/* Last-resolved icon short-circuit cache. Avoids walking s_icon_cache on
 * every call when the same menu item is repeatedly queried (carousel/grid). */
static const lv_img_dsc_t *s_last_icon = NULL;
static const char *s_last_icon_key = NULL;
static uint32_t s_last_icon_version = 0;

/* Installed-packs name list is heap-allocated in scan_installed_packs(). */
static char (*s_installed_names)[ASSET_PACK_NAME_MAX] = NULL;
static int s_installed_count = 0;
static bool s_skip_icon_preload_once = false;
static volatile bool s_defer_icon_loads = false;
static volatile bool s_deferred_icon_preload_running = false;
static uint64_t s_failed_path_hashes[ASSET_PACK_FAILED_PATH_MAX];
static asset_pack_progress_cb_t s_progress_cb = NULL;
static void *s_progress_user = NULL;
static uint32_t s_extract_total = 0;
static uint32_t s_extract_current = 0;

static esp_err_t extract_gtheme_to_dir(const char *archive_path, const char *out_dir);
static void scan_installed_packs(void);
static bool mkdir_parent_dirs(const char *path);
static void start_deferred_icon_preload(void);
static uint64_t hash_path(const char *path);

void asset_pack_set_progress_cb(asset_pack_progress_cb_t cb, void *user) {
    s_progress_cb = cb;
    s_progress_user = user;
}

static void emit_progress(float pct, const char *stage) {
    if (s_progress_cb) s_progress_cb(pct, stage, s_progress_user);
}

static void detect_psram_and_configure(void) {
    if (s_icon_cache != NULL) return;
    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    int target_size;
    if (psram_size > 0) {
        s_has_psram = true;
        target_size = ASSET_PACK_ICON_CACHE_MAX;
    } else {
        s_has_psram = false;
        target_size = ASSET_PACK_ICON_CACHE_INTERNAL;
    }
    size_t bytes = (size_t)target_size * sizeof(asset_icon_cache_entry_t);
    s_icon_cache = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_icon_cache) s_icon_cache = malloc(bytes);
    if (!s_icon_cache) {
        ESP_LOGE(TAG, "no memory for icon cache (%u bytes); caching disabled", (unsigned)bytes);
        s_icon_cache_size = 0;
        return;
    }
    memset(s_icon_cache, 0, bytes);
    s_icon_cache_size = target_size;
    ESP_LOGI(TAG, "%s, icon cache=%d slots (%u bytes), bg=tile",
             s_has_psram ? "PSRAM detected" : "no PSRAM",
             s_icon_cache_size, (unsigned)bytes);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static uint64_t checksum_bytes(const uint8_t *data, size_t len) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static bool parse_color(const cJSON *obj, const char *key, uint32_t *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    const char *s = item->valuestring;
    if (s[0] == '#') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    char *end = NULL;
    unsigned long value = strtoul(s, &end, 16);
    if (!end || *end != '\0' || value > 0xFFFFFFUL) return false;
    *out = (uint32_t)value;
    return true;
}

static bool join_pack_path(char *dst, size_t dst_len, const char *rel) {
    if (!dst || !rel || rel[0] == '\0' || rel[0] == '/' || strchr(rel, '\\') || strstr(rel, "..")) return false;
    int n = snprintf(dst, dst_len, "%s/%s", s_pack_dir, rel);
    return n > 0 && (size_t)n < dst_len;
}

static bool join_base_path(char *dst, size_t dst_len, const char *base, const char *rel) {
    if (!base || !dst || !rel || rel[0] == '\0' || rel[0] == '/' || strchr(rel, '\\') || strstr(rel, "..")) return false;
    int n = snprintf(dst, dst_len, "%s/%s", base, rel);
    return n > 0 && (size_t)n < dst_len;
}

static bool safe_pack_id(const char *id) {
    if (!id || id[0] == '\0' || strlen(id) >= sizeof(s_active_name)) return false;
    for (const char *p = id; *p; ++p) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) return false;
    }
    return true;
}

static bool pack_cache_dir(const char *name, char *path, size_t path_len) {
    if (!safe_pack_id(name)) return false;
    int n = snprintf(path, path_len, "%s/%s", THEME_CACHE_DIR, name);
    return n > 0 && (size_t)n < path_len;
}

static bool pack_cache_meta_path(const char *name, char *path, size_t path_len) {
    if (!safe_pack_id(name)) return false;
    int n = snprintf(path, path_len, "%s/%s/.extract", THEME_CACHE_DIR, name);
    return n > 0 && (size_t)n < path_len;
}

static void set_pack_dir_for_name(const char *name, bool archive) {
    snprintf(s_active_name, sizeof(s_active_name), "%s", name && name[0] ? name : "active");
    s_active_is_archive = archive;
    if (archive) {
        if (!pack_cache_dir(s_active_name, s_pack_dir, sizeof(s_pack_dir))) {
            snprintf(s_pack_dir, sizeof(s_pack_dir), "%s", ACTIVE_PACK_DIR);
        }
    } else {
        snprintf(s_pack_dir, sizeof(s_pack_dir), "%s/%s", THEMES_DIR, s_active_name);
    }
}

static void save_active_name(const char *name) {
    if (!safe_pack_id(name)) return;
    nvs_handle_t nvs;
    if (nvs_open(ASSET_PACK_NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, ASSET_PACK_NVS_ACTIVE, name);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static bool load_saved_active_name(char out[32]) {
    nvs_handle_t nvs;
    size_t len = 32;
    if (nvs_open(ASSET_PACK_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return false;
    esp_err_t err = nvs_get_str(nvs, ASSET_PACK_NVS_ACTIVE, out, &len);
    nvs_close(nvs);
    return err == ESP_OK && safe_pack_id(out);
}

static bool path_is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool path_is_file(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

static esp_err_t asset_sd_begin(bool *mounted_here, bool *display_suspended) {
    if (mounted_here) *mounted_here = false;
    if (display_suspended) *display_suspended = false;

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        esp_err_t err = sd_card_mount_for_flush(display_suspended);
        if (err == ESP_OK && mounted_here) *mounted_here = true;
        return err;
    }
#endif

    if (sd_card_manager.is_initialized) return ESP_OK;
    esp_err_t err = sd_card_mount_for_flush(display_suspended);
    if (err == ESP_OK && mounted_here) *mounted_here = true;
    return err;
}

static void asset_sd_end(bool mounted_here, bool display_suspended) {
    if (mounted_here) sd_card_unmount_after_flush(display_suspended);
}

static bool pack_dir_manifest_path(const char *name, char *path, size_t path_len) {
    if (!safe_pack_id(name)) return false;
    int n = snprintf(path, path_len, "%s/%s/manifest.json", THEMES_DIR, name);
    return n > 0 && (size_t)n < path_len;
}

static bool pack_archive_path(const char *name, char *path, size_t path_len) {
    if (!safe_pack_id(name)) return false;
    int n = snprintf(path, path_len, "%s/%s.gtheme", THEMES_DIR, name);
    return n > 0 && (size_t)n < path_len;
}

static bool archive_cache_matches(const char *name, const struct stat *archive_st) {
    if (!name || !archive_st) return false;

    char manifest_path[192];
    if (!join_base_path(manifest_path, sizeof(manifest_path), s_pack_dir, "manifest.json") ||
        !path_is_file(manifest_path)) {
        return false;
    }

    char meta_path[192];
    if (!pack_cache_meta_path(name, meta_path, sizeof(meta_path))) return false;
    FILE *f = fopen(meta_path, "r");
    if (!f) return false;
    unsigned long cached_size = 0;
    unsigned long cached_mtime = 0;
    int matched = fscanf(f, "%lu %lu", &cached_size, &cached_mtime);
    fclose(f);
    return matched == 2 &&
           cached_size == (unsigned long)archive_st->st_size &&
           cached_mtime == (unsigned long)archive_st->st_mtime;
}

static void write_archive_cache_meta(const char *name, const struct stat *archive_st) {
    if (!name || !archive_st) return;
    char meta_path[192];
    if (!pack_cache_meta_path(name, meta_path, sizeof(meta_path))) return;
    if (!mkdir_parent_dirs(meta_path)) return;
    FILE *f = fopen(meta_path, "w");
    if (!f) return;
    fprintf(f, "%lu %lu\n", (unsigned long)archive_st->st_size, (unsigned long)archive_st->st_mtime);
    fclose(f);
}

static void invalidate_active_archive_cache(void) {
    if (!s_active_is_archive) return;
    char meta_path[192];
    if (!pack_cache_meta_path(s_active_name, meta_path, sizeof(meta_path))) return;
    (void)unlink(meta_path);
}

static bool pack_exists(const char *name, bool *archive) {
    char path[192];
    if (pack_dir_manifest_path(name, path, sizeof(path)) && path_is_file(path)) {
        if (archive) *archive = false;
        return true;
    }
    if (pack_archive_path(name, path, sizeof(path)) && path_is_file(path)) {
        if (archive) *archive = true;
        return true;
    }
    return false;
}

static void consider_pack_candidate(const char *name, const char *current, char first[32], char next[32]) {
    if (!safe_pack_id(name)) return;
    if (first[0] == '\0' || strcmp(name, first) < 0) snprintf(first, 32, "%s", name);
    if (current && current[0] && strcmp(name, current) > 0 && (next[0] == '\0' || strcmp(name, next) < 0)) {
        snprintf(next, 32, "%s", name);
    }
}

static bool find_selectable_pack(const char *current, char out[32]) {
    out[0] = '\0';
    if (!sd_card_manager.is_initialized) {
        ESP_LOGW(TAG, "SD card is not mounted; cannot scan asset packs");
        return false;
    }
    DIR *dir = opendir(THEMES_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "could not open theme directory: %s (errno=%d)", THEMES_DIR, errno);
        return false;
    }

    char first[32] = {0};
    char next[32] = {0};
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (!name || name[0] == '.') continue;

        char path[192];
        int n = snprintf(path, sizeof(path), "%s/%s", THEMES_DIR, name);
        if (n <= 0 || (size_t)n >= sizeof(path)) continue;

        if (path_is_dir(path)) {
            char manifest[192];
            if (pack_dir_manifest_path(name, manifest, sizeof(manifest)) && path_is_file(manifest)) {
                ESP_LOGI(TAG, "found asset pack folder: %s", name);
                consider_pack_candidate(name, current, first, next);
            } else if (strcmp(name, "active") != 0) {
                ESP_LOGW(TAG, "ignoring theme folder without manifest: %s", name);
            }
            continue;
        }

        if (!path_is_file(path)) continue;
        size_t len = strlen(name);
        const char *ext = ".gtheme";
        size_t ext_len = strlen(ext);
        if (len <= ext_len || strcasecmp(name + len - ext_len, ext) != 0) continue;
        if (len - ext_len >= sizeof(first)) continue;
        char id[32];
        memcpy(id, name, len - ext_len);
        id[len - ext_len] = '\0';
        ESP_LOGI(TAG, "found asset pack archive: %s", name);
        consider_pack_candidate(id, current, first, next);
    }
    closedir(dir);

    const char *chosen = next[0] ? next : first;
    if (!chosen[0]) return false;
    snprintf(out, 32, "%s", chosen);
    return true;
}

static esp_err_t select_pack_for_load(void) {
    if (!sd_card_manager.is_initialized) {
        ESP_LOGW(TAG, "cannot load asset pack: SD not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    char name[32] = "active";
    char archive_path_buf[192];
    bool archive = false;

    char saved[32];
    if (load_saved_active_name(saved)) snprintf(name, sizeof(name), "%s", saved);
    if (strcmp(name, "None") == 0) {
        ESP_LOGI(TAG, "asset pack disabled (None)");
        return ESP_ERR_NOT_FOUND;
    }
    if (!pack_exists(name, &archive)) {
        if (!find_selectable_pack(NULL, name) || !pack_exists(name, &archive)) {
            ESP_LOGW(TAG, "selected asset pack not found and no fallback pack found");
            return ESP_ERR_NOT_FOUND;
        }
        save_active_name(name);
    }

    set_pack_dir_for_name(name, archive);
    ESP_LOGI(TAG, "selected asset pack '%s' (%s)", name, archive ? "archive" : "folder");
    if (!archive) return ESP_OK;
    if (!pack_archive_path(name, archive_path_buf, sizeof(archive_path_buf))) {
        ESP_LOGE(TAG, "asset pack archive path too long for '%s'", name);
        return ESP_ERR_INVALID_ARG;
    }

    struct stat archive_st;
    if (stat(archive_path_buf, &archive_st) != 0) return ESP_ERR_NOT_FOUND;
    if (archive_cache_matches(name, &archive_st)) {
        ESP_LOGI(TAG, "archive cache hit for '%s' (size=%lu mtime=%lu), skipping extraction",
                 name, (unsigned long)archive_st.st_size, (unsigned long)archive_st.st_mtime);
        return ESP_OK;
    }

    esp_err_t err = extract_gtheme_to_dir(archive_path_buf, s_pack_dir);
    if (err == ESP_OK) {
        write_archive_cache_meta(name, &archive_st);
    } else {
        ESP_LOGE(TAG, "failed to extract %s: %s", archive_path_buf, esp_err_to_name(err));
    }
    return err;
}

static bool archive_path_safe(const char *name) {
    if (!name || name[0] == '\0' || name[0] == '/' || strchr(name, '\\') || strchr(name, ':')) return false;
    if (strstr(name, "..")) return false;
    return true;
}

static int mkdir_if_missing(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    return mkdir(path, 0775);
}

static bool mkdir_recursive_for_dir(const char *path) {
    if (!path || path[0] == '\0') return false;
    char tmp[192];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir_if_missing(tmp) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
    }
    return mkdir_if_missing(tmp) == 0 || errno == EEXIST;
}

static bool mkdir_parent_dirs(const char *path) {
    char tmp[192];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash) return true;
    *slash = '\0';
    return mkdir_recursive_for_dir(tmp);
}

static bool read_exact(FILE *f, void *buf, size_t len) {
    return len == 0 || fread(buf, 1, len, f) == len;
}

static bool write_bytes_to_file(const char *path, const uint8_t *data, size_t len) {
    if (!mkdir_parent_dirs(path)) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

static bool stream_archive_file(FILE *in, const char *out_path, uint32_t size, uint64_t expected_hash) {
    if (!mkdir_parent_dirs(out_path)) return false;
    FILE *out = fopen(out_path, "wb");
    if (!out) return false;

    uint8_t *buf = heap_caps_malloc(ASSET_PACK_EXTRACT_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(ASSET_PACK_EXTRACT_BUF_SIZE);
    if (!buf) {
        fclose(out);
        return false;
    }
    uint64_t hash = 14695981039346656037ULL;
    uint32_t remaining = size;
    bool ok = true;
    while (remaining > 0) {
        size_t want = remaining > ASSET_PACK_EXTRACT_BUF_SIZE ? ASSET_PACK_EXTRACT_BUF_SIZE : remaining;
        if (fread(buf, 1, want, in) != want) {
            ok = false;
            break;
        }
        for (size_t i = 0; i < want; ++i) {
            hash ^= buf[i];
            hash *= 1099511628211ULL;
        }
        if (fwrite(buf, 1, want, out) != want) {
            ok = false;
            break;
        }
        remaining -= (uint32_t)want;
    }
    fclose(out);
    free(buf);
    if (!ok || hash != expected_hash) {
        unlink(out_path);
        return false;
    }
    return true;
}

static bool read_file_text(const char *path, char **out, size_t max_bytes) {
    *out = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long size = ftell(f);
    if (size <= 0 || (size_t)size > max_bytes) { fclose(f); return false; }
    rewind(f);
    char *buf = calloc(1, (size_t)size + 1);
    if (!buf) { fclose(f); return false; }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) { free(buf); return false; }
    *out = buf;
    return true;
}

static void free_img_dsc(lv_img_dsc_t *dsc, uint8_t **data) {
    if (*data) {
        free(*data);
        *data = NULL;
    }
    memset(dsc, 0, sizeof(*dsc));
}

static void free_icon_table(void) {
    if (!s_icons) return;
    for (int i = 0; i < s_icon_count; ++i) {
        free(s_icons[i].name);
        free(s_icons[i].small);
        free(s_icons[i].large);
        free(s_icons[i].xl);
    }
    free(s_icons);
    s_icons = NULL;
}

static void clear_runtime(void) {
    if (s_icon_cache) {
        for (int i = 0; i < s_icon_cache_size; ++i) {
            free_img_dsc(&s_icon_cache[i].dsc, &s_icon_cache[i].data);
            s_icon_cache[i].key_hash = 0;
            s_icon_cache[i].last_used = 0;
        }
    }
    free_img_dsc(&s_bg_tile_dsc, &s_bg_tile_data);
    free_img_dsc(&s_bg_fullscreen_dsc, &s_bg_fullscreen_data);
    s_last_icon = NULL;
    s_last_icon_key = NULL;
    s_last_icon_version = 0;
    memset(s_has_color, 0, sizeof(s_has_color));
    memset(s_colors, 0, sizeof(s_colors));
    memset(s_failed_path_hashes, 0, sizeof(s_failed_path_hashes));
    free_icon_table();
    memset(s_bg_tile, 0, sizeof(s_bg_tile));
    s_bg_scale_to_fill = false;
    memset(s_bg_candidates, 0, sizeof(s_bg_candidates));
    memset(s_bg_candidate_scale, 0, sizeof(s_bg_candidate_scale));
    s_bg_candidate_count = 0;
    s_bg_candidate_index = 0;
    memset(s_app_icon_key, 0, sizeof(s_app_icon_key));
    s_icon_count = 0;
    s_defer_icon_loads = false;
    s_loaded = false;
    s_version++;
}

static bool path_load_failed(const char *path) {
    uint64_t h = hash_path(path);
    if (!h) return false;
    for (int i = 0; i < ASSET_PACK_FAILED_PATH_MAX; ++i) {
        if (s_failed_path_hashes[i] == h) return true;
    }
    return false;
}

static void remember_failed_path(const char *path) {
    uint64_t h = hash_path(path);
    if (!h || path_load_failed(path)) return;
    for (int i = 0; i < ASSET_PACK_FAILED_PATH_MAX; ++i) {
        if (!s_failed_path_hashes[i]) {
            s_failed_path_hashes[i] = h;
            return;
        }
    }
    s_failed_path_hashes[0] = h;
}

static bool alloc_icon_table(void) {
    size_t bytes = ASSET_PACK_MAX_ICONS * sizeof(asset_icon_entry_t);
    s_icons = heap_caps_calloc(ASSET_PACK_MAX_ICONS, sizeof(asset_icon_entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_icons) s_icons = calloc(ASSET_PACK_MAX_ICONS, sizeof(asset_icon_entry_t));
    if (!s_icons) {
        ESP_LOGE(TAG, "no memory for icon metadata (%u bytes)", (unsigned)bytes);
        return false;
    }
    return true;
}

static char *asset_strdup(const char *s) {
    if (!s || !s[0]) return NULL;
    size_t len = strlen(s) + 1;
    char *out = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) out = malloc(len);
    if (!out) return NULL;
    memcpy(out, s, len);
    return out;
}

static void add_bg_candidate(const char *path, bool scale_to_fill) {
    if (!path || !path[0] || s_bg_candidate_count >= ASSET_PACK_BG_CANDIDATE_MAX) return;
    snprintf(s_bg_candidates[s_bg_candidate_count], sizeof(s_bg_candidates[s_bg_candidate_count]), "%s", path);
    s_bg_candidate_scale[s_bg_candidate_count] = scale_to_fill;
    if (s_bg_candidate_count == 0) {
        snprintf(s_bg_tile, sizeof(s_bg_tile), "%s", path);
        s_bg_scale_to_fill = scale_to_fill;
    }
    s_bg_candidate_count++;
}

static bool select_next_bg_candidate(void) {
    while (++s_bg_candidate_index < s_bg_candidate_count) {
        snprintf(s_bg_tile, sizeof(s_bg_tile), "%s", s_bg_candidates[s_bg_candidate_index]);
        s_bg_scale_to_fill = s_bg_candidate_scale[s_bg_candidate_index];
        free_img_dsc(&s_bg_tile_dsc, &s_bg_tile_data);
        free_img_dsc(&s_bg_fullscreen_dsc, &s_bg_fullscreen_data);
        return true;
    }
    return false;
}

static lv_img_cf_t gimg_cf(uint8_t fmt) {
    if (fmt == GIMG_FORMAT_RGB565A8) return LV_IMG_CF_RGB565A8;
    if (fmt == GIMG_FORMAT_INDEXED_4BPP) return LV_IMG_CF_INDEXED_4BIT;
    return LV_IMG_CF_TRUE_COLOR;
}

static esp_err_t load_gimg(const char *path, bool psram_only, uint32_t max_raw, lv_img_dsc_t *out_dsc, uint8_t **out_data) {
    *out_data = NULL;
    memset(out_dsc, 0, sizeof(*out_dsc));

    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    uint8_t hdr[32];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || memcmp(hdr, "GIMG", 4) != 0) {
        fclose(f);
        return ESP_ERR_INVALID_VERSION;
    }

    uint16_t version = rd16(hdr + 4);
    uint16_t header_size = rd16(hdr + 6);
    uint16_t width = rd16(hdr + 8);
    uint16_t height = rd16(hdr + 10);
    uint8_t fmt = hdr[12];
    uint8_t comp = hdr[13];
    uint32_t raw_size = rd32(hdr + 16);
    uint32_t payload_size = rd32(hdr + 20);
    uint64_t expected_hash = rd64(hdr + 24);

    if (version != 1 || header_size < sizeof(hdr) || width == 0 || height == 0 || raw_size == 0 || raw_size > max_raw || payload_size > max_raw) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    if (fmt != GIMG_FORMAT_RGB565 && fmt != GIMG_FORMAT_RGB565A8 && fmt != GIMG_FORMAT_INDEXED_4BPP) {
        fclose(f);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (comp != GIMG_COMP_NONE && comp != GIMG_COMP_DEFLATE_RAW) {
        fclose(f);
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* Internal RAM cannot afford the payload+raw double-buffer during inflate. */
    if (!s_has_psram && comp == GIMG_COMP_DEFLATE_RAW) {
        fclose(f);
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* Indexed 4bpp deflate isn't supported — 4bpp data doesn't compress well
     * and keeping the codec simple means one consistent uncompressed path. */
    if (fmt == GIMG_FORMAT_INDEXED_4BPP && comp != GIMG_COMP_NONE) {
        fclose(f);
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* Indexed 4bpp payload is always [64 B palette][W*H/2 B pixels]. */
    size_t indexed_pixel_bytes = 0;
    if (fmt == GIMG_FORMAT_INDEXED_4BPP) {
        indexed_pixel_bytes = ((size_t)width * (size_t)height + 1) / 2;
        size_t expected_raw = 64 + indexed_pixel_bytes;
        if (raw_size != expected_raw || payload_size != expected_raw) {
            fclose(f);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    if (header_size > sizeof(hdr) && fseek(f, header_size, SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }

    uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    uint8_t *raw = NULL;

    if (comp == GIMG_COMP_NONE) {
        /* Read directly into the raw buffer — no separate payload alloc.
         * Peak RAM during decode is just the raw buffer (not 2x). */
        raw = heap_caps_malloc(raw_size, caps);
        if (!raw && !psram_only) raw = malloc(raw_size);
        if (!raw) { fclose(f); return ESP_ERR_NO_MEM; }
        if (fread(raw, 1, raw_size, f) != raw_size) {
            fclose(f);
            free(raw);
            return ESP_FAIL;
        }
        fclose(f);
    } else {
        /* Deflate path: load compressed payload, then decompress into raw. */
        size_t psize = payload_size ? payload_size : 1;
        uint8_t *payload = heap_caps_malloc(psize, caps);
        if (!payload && !psram_only) payload = malloc(psize);
        if (!payload) { fclose(f); return ESP_ERR_NO_MEM; }

        bool read_ok = fread(payload, 1, payload_size, f) == payload_size;
        fclose(f);
        if (!read_ok) {
            free(payload);
            return ESP_FAIL;
        }

        raw = heap_caps_malloc(raw_size, caps);
        if (!raw && !psram_only) raw = malloc(raw_size);
        if (!raw) {
            free(payload);
            return ESP_ERR_NO_MEM;
        }

        size_t out_len = lgfx_tinfl_decompress_mem_to_mem(raw, raw_size, payload, payload_size, 0);
        free(payload);
        if (out_len == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || out_len != raw_size) {
            free(raw);
            return ESP_FAIL;
        }
    }

    if (checksum_bytes(raw, raw_size) != expected_hash) {
        free(raw);
        return ESP_ERR_INVALID_CRC;
    }

    if (fmt == GIMG_FORMAT_INDEXED_4BPP) {
        /* Layout: [16 * lv_color32_t palette (64 B)][W*H/2 packed pixels].
         * The cache's data pointer owns the single combined buffer; palette
         * and pixel data both live inside it. */
        out_dsc->header.cf = gimg_cf(fmt);
        out_dsc->header.always_zero = 0;
        out_dsc->header.reserved = 0;
        out_dsc->header.w = width;
        out_dsc->header.h = height;
        out_dsc->data_size = raw_size;
        out_dsc->data = raw;
        *out_data = raw;
        ESP_LOGD(TAG, "load_gimg OK: %s %ux%u fmt=indexed4 raw=%lu data=%p",
                 path, width, height, (unsigned long)raw_size,
                 (void *)out_dsc->data);
        return ESP_OK;
    }

#if LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP
    uint32_t color_bytes = raw_size;
    if (fmt == GIMG_FORMAT_RGB565A8) {
        color_bytes = (uint32_t)width * (uint32_t)height * 2;
    }
    if ((color_bytes & 1) != 0 || color_bytes > raw_size) {
        free(raw);
        return ESP_ERR_INVALID_SIZE;
    }
    for (uint32_t i = 0; i < color_bytes; i += 2) {
        uint8_t tmp = raw[i];
        raw[i] = raw[i + 1];
        raw[i + 1] = tmp;
    }
#endif

    out_dsc->header.cf = gimg_cf(fmt);
    out_dsc->header.always_zero = 0;
    out_dsc->header.reserved = 0;
    out_dsc->header.w = width;
    out_dsc->header.h = height;
    out_dsc->data_size = raw_size;
    out_dsc->data = raw;
    *out_data = raw;
    ESP_LOGD(TAG, "load_gimg OK: %s %ux%u fmt=%u comp=%u raw=%lu payload=%lu cf=%lu data=%p",
             path, width, height, fmt, comp, (unsigned long)raw_size, (unsigned long)payload_size,
             (unsigned long)out_dsc->header.cf, (void *)raw);
    return ESP_OK;
}

static esp_err_t extract_gtheme_to_dir(const char *archive_path, const char *out_dir) {
    if (!out_dir || out_dir[0] == '\0') return ESP_ERR_INVALID_ARG;
    struct stat st;
    if (stat(archive_path, &st) != 0) {
        ESP_LOGE(TAG, "gtheme not found: %s (errno=%d)", archive_path, errno);
        return ESP_ERR_NOT_FOUND;
    }
    if (!mkdir_recursive_for_dir(out_dir)) {
        ESP_LOGE(TAG, "failed to create theme extraction directory: %s (errno=%d)", out_dir, errno);
        return ESP_FAIL;
    }

    FILE *f = fopen(archive_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "failed to open gtheme: %s (errno=%d)", archive_path, errno);
        return ESP_FAIL;
    }

    uint8_t hdr[24];
    if (!read_exact(f, hdr, 12) || memcmp(hdr, "GAPP", 4) != 0 || rd16(hdr + 4) != 1) {
        fclose(f);
        ESP_LOGE(TAG, "invalid gtheme header: %s magic=%02X %02X %02X %02X version=%u",
                 archive_path, hdr[0], hdr[1], hdr[2], hdr[3], rd16(hdr + 4));
        if (hdr[0] == 'P' && hdr[1] == 'K') {
            ESP_LOGE(TAG, "zip-style .gtheme archives are not supported on firmware; rebuild with current gbt asset pack --archive");
        }
        return ESP_ERR_INVALID_VERSION;
    }

    uint32_t file_count = rd32(hdr + 8);
    if (file_count == 0 || file_count > ASSET_PACK_GTHEME_MAX_FILES) {
        fclose(f);
        ESP_LOGE(TAG, "invalid gtheme file count: %lu", (unsigned long)file_count);
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "extracting gtheme %s (%lu files)", archive_path, (unsigned long)file_count);
    s_extract_total = file_count;
    s_extract_current = 0;
    emit_progress(30.0f, "Extracting theme");

    for (uint32_t i = 0; i < file_count; ++i) {
        s_extract_current = i;
        if (s_progress_cb && file_count > 0) {
            float pct = 30.0f + ((float)i * 20.0f) / (float)file_count;
            emit_progress(pct, "Extracting theme");
        }
        if (!read_exact(f, hdr, sizeof(hdr)) || memcmp(hdr, "FILE", 4) != 0) {
            fclose(f);
            ESP_LOGE(TAG, "invalid gtheme entry header at index %lu", (unsigned long)i);
            return ESP_FAIL;
        }

        uint16_t method = rd16(hdr + 4);
        uint16_t name_len = rd16(hdr + 6);
        uint32_t raw_size = rd32(hdr + 8);
        uint32_t payload_size = rd32(hdr + 12);
        uint64_t expected_hash = rd64(hdr + 16);
        if (name_len == 0 || name_len >= ASSET_PACK_PATH_MAX || raw_size > ASSET_PACK_GTHEME_MAX_FILE || payload_size > ASSET_PACK_GTHEME_MAX_FILE) {
            fclose(f);
            ESP_LOGE(TAG, "invalid gtheme entry size at index %lu: name=%u raw=%lu payload=%lu",
                     (unsigned long)i, name_len, (unsigned long)raw_size, (unsigned long)payload_size);
            return ESP_ERR_INVALID_SIZE;
        }

        char name[ASSET_PACK_PATH_MAX];
        if (!read_exact(f, name, name_len)) {
            fclose(f);
            ESP_LOGE(TAG, "failed to read gtheme entry name at index %lu", (unsigned long)i);
            return ESP_FAIL;
        }
        name[name_len] = '\0';
        if (!archive_path_safe(name)) {
            fclose(f);
            ESP_LOGE(TAG, "unsafe gtheme entry path: %s", name);
            return ESP_ERR_INVALID_ARG;
        }

        char out_path[192];
        if (!join_base_path(out_path, sizeof(out_path), out_dir, name)) {
            fclose(f);
            ESP_LOGE(TAG, "archive output path too long/invalid: %s", name);
            return ESP_ERR_INVALID_ARG;
        }
        ESP_LOGD(TAG, "extracting gtheme entry %s method=%u raw=%lu payload=%lu",
                 name, method, (unsigned long)raw_size, (unsigned long)payload_size);

        esp_err_t err = ESP_OK;
        if (method == 0) {
            if (payload_size != raw_size) {
                err = ESP_ERR_INVALID_SIZE;
            } else if (!stream_archive_file(f, out_path, raw_size, expected_hash)) {
                err = ESP_FAIL;
            }
            if (err != ESP_OK) {
                fclose(f);
                ESP_LOGE(TAG, "failed to stream gtheme entry %s: %s", name, esp_err_to_name(err));
                return err;
            }
            continue;
        }

        uint8_t *payload = heap_caps_malloc(payload_size ? payload_size : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!payload) payload = malloc(payload_size ? payload_size : 1);
        if (!payload) { fclose(f); ESP_LOGE(TAG, "no memory for gtheme payload %s", name); return ESP_ERR_NO_MEM; }
        if (!read_exact(f, payload, payload_size)) {
            free(payload);
            fclose(f);
            ESP_LOGE(TAG, "failed to read gtheme payload for %s", name);
            return ESP_FAIL;
        }

        uint8_t *raw = NULL;
        if (method == 1) {
            raw = heap_caps_malloc(raw_size ? raw_size : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!raw) raw = malloc(raw_size ? raw_size : 1);
            if (!raw) err = ESP_ERR_NO_MEM;
            else {
                size_t out_len = lgfx_tinfl_decompress_mem_to_mem(raw, raw_size, payload, payload_size, 0);
                if (out_len == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || out_len != raw_size) err = ESP_FAIL;
            }
        } else {
            err = ESP_ERR_NOT_SUPPORTED;
        }

        if (err == ESP_OK && checksum_bytes(raw, raw_size) != expected_hash) err = ESP_ERR_INVALID_CRC;
        if (err == ESP_OK && !write_bytes_to_file(out_path, raw, raw_size)) {
            err = ESP_FAIL;
        }

        if (raw) free(raw);
        free(payload);
        if (err != ESP_OK) {
            fclose(f);
            ESP_LOGE(TAG, "failed to extract gtheme entry %s: %s", name, esp_err_to_name(err));
            return err;
        }
    }

    fclose(f);
    ESP_LOGI(TAG, "extracted %s to %s", archive_path, out_dir);
    return ESP_OK;
}

esp_err_t asset_pack_extract_active_gtheme(void) {
    return extract_gtheme_to_dir(ACTIVE_GTHEME, ACTIVE_PACK_DIR);
}

static asset_icon_entry_t *find_icon(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < s_icon_count; ++i) {
        if (s_icons[i].name && strcmp(s_icons[i].name, name) == 0) return &s_icons[i];
    }
    return NULL;
}

static void add_icon_rel_candidate(const char **rels, int *count, const char *rel) {
    if (!rel || !rel[0] || !rels || !count || *count >= 3) return;
    for (int i = 0; i < *count; ++i) {
        if (strcmp(rels[i], rel) == 0) return;
    }
    rels[(*count)++] = rel;
}

static int icon_rel_candidates(const asset_icon_entry_t *entry, const char **rels, int max_count) {
    if (!entry || !rels || max_count < 3) return 0;
    int count = 0;
    bool small = !s_has_psram || LV_MIN(LV_HOR_RES, LV_VER_RES) < 200;
    if (small) {
        add_icon_rel_candidate(rels, &count, entry->small);
        add_icon_rel_candidate(rels, &count, entry->xl);
        add_icon_rel_candidate(rels, &count, entry->large);
    } else {
        add_icon_rel_candidate(rels, &count, entry->large);
        add_icon_rel_candidate(rels, &count, entry->xl);
        add_icon_rel_candidate(rels, &count, entry->small);
    }
    return count;
}

static uint64_t hash_path(const char *path) {
    if (!path) return 0;
    return checksum_bytes((const uint8_t *)path, strlen(path));
}

static asset_icon_cache_entry_t *cache_find_path(const char *path) {
    if (!path || !s_icon_cache) return NULL;
    uint64_t h = hash_path(path);
    for (int i = 0; i < s_icon_cache_size; ++i) {
        if (s_icon_cache[i].key_hash && s_icon_cache[i].data && s_icon_cache[i].key_hash == h) {
            return &s_icon_cache[i];
        }
    }
    return NULL;
}

static asset_icon_cache_entry_t *cache_slot_for_evict(void) {
    if (!s_icon_cache || s_icon_cache_size <= 0) return NULL;
    asset_icon_cache_entry_t *oldest = &s_icon_cache[0];
    for (int i = 0; i < s_icon_cache_size; ++i) {
        if (s_icon_cache[i].key_hash == 0) return &s_icon_cache[i];
        if (s_icon_cache[i].last_used < oldest->last_used) oldest = &s_icon_cache[i];
    }
    free_img_dsc(&oldest->dsc, &oldest->data);
    oldest->key_hash = 0;
    return oldest;
}

static void preload_loaded_assets(void) {
    if (!s_has_psram) {
        ESP_LOGI(TAG, "asset preload skipped: no PSRAM");
        return;
    }

    if (s_bg_tile[0] && !s_bg_tile_data) {
        char path[192];
        if (join_pack_path(path, sizeof(path), s_bg_tile)) {
            esp_err_t err = load_gimg(path, true, ASSET_PACK_BG_FULLSCREEN_MAX_RAW, &s_bg_tile_dsc, &s_bg_tile_data);
            if (err != ESP_OK) {
                if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_VERSION || err == ESP_ERR_INVALID_CRC) {
                    invalidate_active_archive_cache();
                }
                ESP_LOGW(TAG, "background preload failed (%s): %s", path, esp_err_to_name(err));
            }
        }
    }

    if (s_skip_icon_preload_once) {
        s_skip_icon_preload_once = false;
        s_defer_icon_loads = true;
        ESP_LOGI(TAG, "preloaded asset pack '%s': bg=%s icons=deferred", s_active_name,
                 s_bg_tile_data ? "yes" : "no");
        start_deferred_icon_preload();
        return;
    }

    int loaded = 0;
    int deduped = 0;
    for (int i = 0; i < s_icon_count; ++i) {
        const char *rels[3] = {0};
        int rel_count = icon_rel_candidates(&s_icons[i], rels, 3);
        bool icon_done = false;
        for (int r = 0; r < rel_count && !icon_done; ++r) {
            char path[192];
            if (!join_pack_path(path, sizeof(path), rels[r])) continue;
            if (path_load_failed(path)) continue;

            /* Skip paths already loaded; multiple icon names can resolve to the
             * same file and we want a single decoded image in the cache. */
            if (cache_find_path(path)) {
                deduped++;
                icon_done = true;
                continue;
            }

            asset_icon_cache_entry_t *slot = cache_slot_for_evict();
            if (!slot) break;

            uint32_t icon_max = s_has_psram ? ASSET_PACK_ICON_MAX_RAW : ASSET_PACK_ICON_MAX_RAW_INTERNAL;
            esp_err_t err = load_gimg(path, false, icon_max, &slot->dsc, &slot->data);
            if (err != ESP_OK) {
                remember_failed_path(path);
                if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_VERSION || err == ESP_ERR_INVALID_CRC) {
                    invalidate_active_archive_cache();
                }
                ESP_LOGW(TAG, "icon preload failed for %s (%s): %s", s_icons[i].name ? s_icons[i].name : "?", path, esp_err_to_name(err));
                continue;
            }
            slot->key_hash = hash_path(path);
            slot->last_used = ++s_cache_tick;
            loaded++;
            icon_done = true;
        }
    }

    if (deduped > 0) {
        ESP_LOGI(TAG, "preloaded asset pack '%s': bg=%s unique_icons=%d entries=%d deduped=%d",
                 s_active_name,
                 s_bg_tile_data ? "yes" : "no", loaded, s_icon_count, deduped);
    } else {
        ESP_LOGI(TAG, "preloaded asset pack '%s': bg=%s icons=%d/%d", s_active_name,
                 s_bg_tile_data ? "yes" : "no", loaded, s_icon_count);
    }
}

static void deferred_icon_preload_task(void *arg) {
    uint32_t version = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(750));

    bool loaded_icons = false;
    bool mounted_here = false;
    bool display_suspended = false;
    esp_err_t mount_err = asset_sd_begin(&mounted_here, &display_suspended);
    if (mount_err == ESP_OK) {
        if (version == s_version && s_loaded) {
            preload_loaded_assets();
            loaded_icons = true;
        }
        asset_sd_end(mounted_here, display_suspended);
    }

    bool stale = version != s_version;
    s_deferred_icon_preload_running = false;
    if (!stale && loaded_icons) {
        s_defer_icon_loads = false;
        s_version++;
        ESP_LOGI(TAG, "deferred icon preload complete for '%s' (v%lu)",
                 s_active_name, (unsigned long)s_version);
    } else if (!stale && s_defer_icon_loads) {
        start_deferred_icon_preload();
    } else if (stale && s_defer_icon_loads) {
        start_deferred_icon_preload();
    }
    vTaskDelete(NULL);
}

static void start_deferred_icon_preload(void) {
    if (s_deferred_icon_preload_running) return;
    s_deferred_icon_preload_running = true;
    BaseType_t ok = xTaskCreate(deferred_icon_preload_task, "icon_preload", 4096,
                                (void *)(uintptr_t)s_version, 4, NULL);
    if (ok != pdPASS) {
        s_deferred_icon_preload_running = false;
        s_defer_icon_loads = false;
        ESP_LOGW(TAG, "failed to create deferred icon preload task");
    }
}

static esp_err_t asset_pack_load_active_impl(void) {
    s_extract_total = 0;
    s_extract_current = 0;
    emit_progress(0.0f, "Scanning packs");
    detect_psram_and_configure();
    scan_installed_packs();
    emit_progress(10.0f, "Selecting pack");
    s_loading_attempted = true;
    esp_err_t select_err = select_pack_for_load();
    if (select_err != ESP_OK) {
        clear_runtime();
        return select_err;
    }
    emit_progress(50.0f, "Parsing manifest");

    char manifest_path[192];
    int manifest_len = snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", s_pack_dir);
    if (manifest_len <= 0 || (size_t)manifest_len >= sizeof(manifest_path)) {
        clear_runtime();
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st;
    if (stat(manifest_path, &st) != 0) {
        clear_runtime();
        return ESP_ERR_NOT_FOUND;
    }

    char *text = NULL;
    if (!read_file_text(manifest_path, &text, ASSET_PACK_MANIFEST_MAX)) {
        clear_runtime();
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        clear_runtime();
        ESP_LOGE(TAG, "invalid active asset pack manifest");
        return ESP_FAIL;
    }

    clear_runtime();

    const cJSON *colors = cJSON_GetObjectItemCaseSensitive(root, "colors");
    if (cJSON_IsObject(colors)) {
        static const char *keys[] = {"accent", "background", "surface", "surface_alt", "text", "text_muted"};
        for (int i = 0; i < 6; ++i) {
            s_has_color[i] = parse_color(colors, keys[i], &s_colors[i]);
        }
    }

    const cJSON *icons = cJSON_GetObjectItemCaseSensitive(root, "icons");
    if (cJSON_IsObject(icons)) {
        if (!alloc_icon_table()) {
            cJSON_Delete(root);
            clear_runtime();
            return ESP_ERR_NO_MEM;
        }
        const cJSON *icon = NULL;
        cJSON_ArrayForEach(icon, icons) {
            if (!icon->string || !cJSON_IsObject(icon) || s_icon_count >= ASSET_PACK_MAX_ICONS) continue;
            asset_icon_entry_t *entry = &s_icons[s_icon_count];
            entry->name = asset_strdup(icon->string);
            if (!entry->name) continue;
            const cJSON *small = cJSON_GetObjectItemCaseSensitive(icon, "s");
            const cJSON *large = cJSON_GetObjectItemCaseSensitive(icon, "l");
            const cJSON *xl = cJSON_GetObjectItemCaseSensitive(icon, "xl");
            if (cJSON_IsString(small) && small->valuestring) entry->small = asset_strdup(small->valuestring);
            if (cJSON_IsString(large) && large->valuestring) entry->large = asset_strdup(large->valuestring);
            if (cJSON_IsString(xl) && xl->valuestring) entry->xl = asset_strdup(xl->valuestring);
            if (!entry->small && !entry->large && !entry->xl) {
                free(entry->name);
                memset(entry, 0, sizeof(*entry));
                continue;
            }
            s_icon_count++;
        }
    }

    const cJSON *bg_tile = cJSON_GetObjectItemCaseSensitive(root, "bg_tile");
    if (cJSON_IsString(bg_tile) && bg_tile->valuestring) {
        add_bg_candidate(bg_tile->valuestring, false);
    }

    const cJSON *backgrounds = cJSON_GetObjectItemCaseSensitive(root, "backgrounds");
    if (cJSON_IsObject(backgrounds)) {
        const char *keys[] = {s_has_psram ? "full" : "half", "tiny", "tile"};
        s_bg_candidate_count = 0;
        s_bg_candidate_index = 0;
        memset(s_bg_tile, 0, sizeof(s_bg_tile));
        s_bg_scale_to_fill = false;
        for (int i = 0; i < 3; ++i) {
            const cJSON *bg = cJSON_GetObjectItemCaseSensitive(backgrounds, keys[i]);
            if (cJSON_IsString(bg) && bg->valuestring) {
                add_bg_candidate(bg->valuestring, strcmp(keys[i], "tile") != 0);
            }
        }
    }

    const cJSON *app_icon = cJSON_GetObjectItemCaseSensitive(root, "app_icon");
    if (cJSON_IsString(app_icon) && app_icon->valuestring) {
        snprintf(s_app_icon_key, sizeof(s_app_icon_key), "%s", app_icon->valuestring);
    }

    cJSON_Delete(root);
    s_loaded = true;
    s_version++;
    emit_progress(60.0f, "Loading icons");
    preload_loaded_assets();
    emit_progress(90.0f, "Baking background");
    ESP_LOGI(TAG, "loaded asset pack '%s': %d icon mappings (v%lu)", s_active_name, s_icon_count, (unsigned long)s_version);
    return ESP_OK;
}

esp_err_t asset_pack_load_active(void) {
    bool mounted_here = false;
    bool display_suspended = false;
    esp_err_t mount_err = asset_sd_begin(&mounted_here, &display_suspended);
    if (mount_err != ESP_OK) return ESP_ERR_INVALID_STATE;
    s_skip_icon_preload_once = true;
    esp_err_t err = asset_pack_load_active_impl();
    s_skip_icon_preload_once = false;
    if (err == ESP_OK) {
        (void)asset_pack_get_background_fullscreen();
        emit_progress(100.0f, "Asset pack ready");
    }
    asset_sd_end(mounted_here, display_suspended);
    return err;
}

bool asset_pack_is_loaded(void) {
    return s_loaded;
}

uint32_t asset_pack_get_version(void) {
    return s_version;
}

const char *asset_pack_active_name(void) {
    return s_active_name;
}

bool asset_pack_has_psram(void) {
    return s_has_psram;
}

int asset_pack_get_installed_count(void) {
    return s_installed_count;
}

const char *asset_pack_get_installed_name(int index) {
    if (index < 0 || index >= s_installed_count) return NULL;
    return s_installed_names[index];
}

int asset_pack_get_current_index(void) {
    if (strcmp(s_active_name, "None") == 0) return 0;
    for (int i = 1; i < s_installed_count; ++i) {
        if (strcmp(s_installed_names[i], s_active_name) == 0) return i;
    }
    return 0;
}

static void scan_installed_packs(void) {
    s_installed_count = 0;
    if (!s_installed_names) {
        size_t bytes = (size_t)ASSET_PACK_INSTALLED_MAX * ASSET_PACK_NAME_MAX;
        s_installed_names = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_installed_names) s_installed_names = malloc(bytes);
        if (!s_installed_names) {
            ESP_LOGE(TAG, "no memory for installed pack names; pack list unavailable");
            return;
        }
    }
    memset(s_installed_names, 0, (size_t)ASSET_PACK_INSTALLED_MAX * ASSET_PACK_NAME_MAX);

    snprintf(s_installed_names[0], ASSET_PACK_NAME_MAX, "None");
    s_installed_count = 1;

    DIR *dir = opendir(THEMES_DIR);
    if (!dir) return;

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL && s_installed_count < ASSET_PACK_INSTALLED_MAX) {
        const char *name = entry->d_name;
        if (!name || name[0] == '.') continue;
        if (strcmp(name, "active") == 0) continue;
        if (strlen(name) >= ASSET_PACK_NAME_MAX) continue;

        char path[192];
        int n = snprintf(path, sizeof(path), "%s/%s", THEMES_DIR, name);
        if (n <= 0 || (size_t)n >= sizeof(path)) continue;

        if (path_is_dir(path)) {
            char manifest[192];
            if (pack_dir_manifest_path(name, manifest, sizeof(manifest)) && path_is_file(manifest)) {
                size_t nlen = strlen(name);
                memcpy(s_installed_names[s_installed_count], name, nlen);
                s_installed_names[s_installed_count][nlen] = '\0';
                s_installed_count++;
            }
            continue;
        }

        if (!path_is_file(path)) continue;
        size_t len = strlen(name);
        const char *ext = ".gtheme";
        size_t ext_len = strlen(ext);
        if (len <= ext_len || strcasecmp(name + len - ext_len, ext) != 0) continue;
        if (len - ext_len >= ASSET_PACK_NAME_MAX) continue;
        char id[ASSET_PACK_NAME_MAX];
        memcpy(id, name, len - ext_len);
        id[len - ext_len] = '\0';
        if (safe_pack_id(id)) {
            snprintf(s_installed_names[s_installed_count], ASSET_PACK_NAME_MAX, "%s", id);
            s_installed_count++;
        }
    }
    closedir(dir);

    for (int i = 0; i < s_installed_count - 1; ++i) {
        for (int j = i + 1; j < s_installed_count; ++j) {
            if (strcmp(s_installed_names[i], s_installed_names[j]) > 0) {
                char tmp[ASSET_PACK_NAME_MAX];
                memcpy(tmp, s_installed_names[i], ASSET_PACK_NAME_MAX);
                memcpy(s_installed_names[i], s_installed_names[j], ASSET_PACK_NAME_MAX);
                memcpy(s_installed_names[j], tmp, ASSET_PACK_NAME_MAX);
            }
        }
    }

    ESP_LOGI(TAG, "found %d installed asset packs", s_installed_count);
}

esp_err_t asset_pack_select_by_index(int index) {
    if (index < 0 || index >= s_installed_count) return ESP_ERR_INVALID_ARG;

    if (index == 0) {
        save_active_name("None");
        clear_runtime();
        return ESP_OK;
    }

    const char *name = s_installed_names[index];
    save_active_name(name);

    bool mounted_here = false;
    bool display_suspended = false;
    esp_err_t mount_err = asset_sd_begin(&mounted_here, &display_suspended);
    if (mount_err != ESP_OK) return ESP_ERR_INVALID_STATE;

    bool archive = false;
    if (!pack_exists(name, &archive)) {
        asset_sd_end(mounted_here, display_suspended);
        return ESP_ERR_NOT_FOUND;
    }

    clear_runtime();

    set_pack_dir_for_name(name, archive);
    s_skip_icon_preload_once = true;
    esp_err_t err = asset_pack_load_active_impl();
    s_skip_icon_preload_once = false;
    asset_sd_end(mounted_here, display_suspended);
    return err;
}

bool asset_pack_get_color(int slot, uint32_t *out_color) {
    if (!s_loaded || !out_color || slot < 0 || slot >= 6 || !s_has_color[slot]) return false;
    *out_color = s_colors[slot];
    return true;
}

const lv_img_dsc_t *asset_pack_get_icon(const char *name, const lv_img_dsc_t *fallback) {
    if (!s_loaded || !name) return fallback;
    if (s_defer_icon_loads) return fallback;

    /* Fast path: same name pointer + same pack version -> return the
     * previously resolved desc without any work. The name argument is
     * always a compile-time string literal from the menu/app item tables,
     * so pointer identity is sufficient and stable. */
    if (s_last_icon && s_last_icon_key == name && s_last_icon_version == s_version) {
        return s_last_icon;
    }

    asset_icon_entry_t *entry = find_icon(name);
    const char *rels[3] = {0};
    int rel_count = icon_rel_candidates(entry, rels, 3);
    if (rel_count == 0) {
        s_last_icon = fallback;
        s_last_icon_key = name;
        s_last_icon_version = s_version;
        return fallback;
    }

    /* Cache lookup by resolved path. Multiple icon names that resolve to
     * the same file share a single decoded image in the cache. */
    for (int r = 0; r < rel_count; ++r) {
        char path[192];
        if (!join_pack_path(path, sizeof(path), rels[r])) continue;
        if (path_load_failed(path)) continue;
        asset_icon_cache_entry_t *cached = cache_find_path(path);
        if (cached) {
            cached->last_used = ++s_cache_tick;
            s_last_icon = &cached->dsc;
            s_last_icon_key = name;
            s_last_icon_version = s_version;
            return &cached->dsc;
        }
    }

    asset_icon_cache_entry_t *slot = cache_slot_for_evict();
    if (!slot) return fallback;
    bool mounted_here = false;
    bool display_suspended = false;
    esp_err_t mount_err = asset_sd_begin(&mounted_here, &display_suspended);
    if (mount_err != ESP_OK) return fallback;
    uint32_t icon_max = s_has_psram ? ASSET_PACK_ICON_MAX_RAW : ASSET_PACK_ICON_MAX_RAW_INTERNAL;
    char loaded_path[192] = {0};
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (int r = 0; r < rel_count; ++r) {
        char path[192];
        if (!join_pack_path(path, sizeof(path), rels[r])) continue;
        if (path_load_failed(path)) continue;
        err = load_gimg(path, false, icon_max, &slot->dsc, &slot->data);
        if (err == ESP_OK) {
            snprintf(loaded_path, sizeof(loaded_path), "%s", path);
            break;
        }
        remember_failed_path(path);
        if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_VERSION || err == ESP_ERR_INVALID_CRC) {
            invalidate_active_archive_cache();
        }
        ESP_LOGW(TAG, "icon load failed for %s (%s): %s", name, path, esp_err_to_name(err));
    }
    asset_sd_end(mounted_here, display_suspended);
    if (err != ESP_OK) {
        return fallback;
    }
    slot->key_hash = hash_path(loaded_path);
    slot->last_used = ++s_cache_tick;
    s_last_icon = &slot->dsc;
    s_last_icon_key = name;
    s_last_icon_version = s_version;
    return &slot->dsc;
}

const lv_img_dsc_t *asset_pack_get_app_icon(const lv_img_dsc_t *fallback) {
    if (!s_loaded || !s_app_icon_key[0]) return fallback;
    return asset_pack_get_icon(s_app_icon_key, fallback);
}

const lv_img_dsc_t *asset_pack_get_background_tile(void) {
    if (!s_loaded || !s_bg_tile[0]) return NULL;
    if (s_bg_tile_data) return &s_bg_tile_dsc;

    while (s_bg_tile[0]) {
        char path[192];
        if (!join_pack_path(path, sizeof(path), s_bg_tile)) return NULL;
        if (path_load_failed(path)) {
            if (select_next_bg_candidate()) continue;
            return NULL;
        }

        /* PSRAM: cap is fullscreen, psram_only=true.
         * Internal: cap is the selected low-RAM variant, psram_only=false so
         * the loader falls back to internal heap. */
        uint32_t bg_max = s_has_psram ? ASSET_PACK_BG_FULLSCREEN_MAX_RAW
                                      : ASSET_PACK_BG_MAX_RAW_INTERNAL;
        if (!s_has_psram && s_bg_scale_to_fill) bg_max = ASSET_PACK_BG_FULLSCREEN_MAX_RAW;
        bool psram_only = s_has_psram;

        bool mounted_here = false;
        bool display_suspended = false;
        esp_err_t mount_err = asset_sd_begin(&mounted_here, &display_suspended);
        if (mount_err != ESP_OK) return NULL;
        esp_err_t err = load_gimg(path, psram_only, bg_max, &s_bg_tile_dsc, &s_bg_tile_data);
        asset_sd_end(mounted_here, display_suspended);
        if (err != ESP_OK) {
            remember_failed_path(path);
            if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_VERSION || err == ESP_ERR_INVALID_CRC) {
                invalidate_active_archive_cache();
            }
            if (!s_has_psram && err == ESP_ERR_INVALID_SIZE) {
                ESP_LOGD(TAG, "background too large for no-PSRAM device (%s)", path);
                if (select_next_bg_candidate()) continue;
                return NULL;
            }
            ESP_LOGW(TAG, "background tile load failed (%s): %s", path, esp_err_to_name(err));
            if (select_next_bg_candidate()) continue;
            return NULL;
        }
        ESP_LOGI(TAG, "background loaded: %s %ux%u cf=%u scale=%d",
                 path, s_bg_tile_dsc.header.w, s_bg_tile_dsc.header.h,
                 (unsigned)s_bg_tile_dsc.header.cf, s_bg_scale_to_fill ? 1 : 0);
        return &s_bg_tile_dsc;
    }
    return NULL;
}

bool asset_pack_background_should_scale(void) {
    return s_bg_scale_to_fill;
}

/* One-time bake of the small tile into a fullscreen RGB565 PSRAM buffer.
 * Result: a single LV_IMG_CF_TRUE_COLOR desc sized to LV_HOR_RES x LV_VER_RES
 * that LVGL can blit with no per-frame tiling math. */
static esp_err_t bake_background_fullscreen(void) {
    if (s_bg_fullscreen_data) return ESP_OK;
    if (!s_bg_tile_data) return ESP_ERR_INVALID_STATE;

    int sw = s_bg_tile_dsc.header.w;
    int sh = s_bg_tile_dsc.header.h;
    int dw = LV_HOR_RES;
    int dh = LV_VER_RES;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return ESP_ERR_INVALID_SIZE;

    size_t size = (size_t)dw * (size_t)dh * 2;
    if (size > ASSET_PACK_BG_FULLSCREEN_MAX_RAW) return ESP_ERR_INVALID_SIZE;

    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return ESP_ERR_NO_MEM;

    const uint8_t *src = s_bg_tile_data;
    for (int y = 0; y < dh; y++) {
        int src_y = y % sh;
        uint8_t *dst_row = buf + (size_t)y * dw * 2;
        const uint8_t *src_row = src + (size_t)src_y * sw * 2;
        int x = 0;
        while (x + sw <= dw) {
            memcpy(dst_row + x * 2, src_row, (size_t)sw * 2);
            x += sw;
        }
        int rem = dw - x;
        if (rem > 0) memcpy(dst_row + x * 2, src_row, (size_t)rem * 2);
    }

    s_bg_fullscreen_dsc = s_bg_tile_dsc;
    s_bg_fullscreen_dsc.header.w = (uint16_t)dw;
    s_bg_fullscreen_dsc.header.h = (uint16_t)dh;
    s_bg_fullscreen_dsc.data_size = (uint32_t)size;
    s_bg_fullscreen_dsc.data = buf;
    s_bg_fullscreen_data = buf;
    ESP_LOGI(TAG, "baked fullscreen bg: %dx%d (%u bytes) from %dx%d tile",
             dw, dh, (unsigned)size, sw, sh);
    return ESP_OK;
}

const lv_img_dsc_t *asset_pack_get_background_fullscreen(void) {
    if (!s_loaded || !s_has_psram) return NULL;
    if (s_bg_fullscreen_data) return &s_bg_fullscreen_dsc;
    if (!asset_pack_get_background_tile()) return NULL;
    if (s_bg_tile_dsc.header.w == (uint16_t)LV_HOR_RES && s_bg_tile_dsc.header.h == (uint16_t)LV_VER_RES) {
        return &s_bg_tile_dsc;
    }
    if (s_bg_scale_to_fill || s_bg_tile_dsc.header.cf != LV_IMG_CF_TRUE_COLOR) {
        return &s_bg_tile_dsc;
    }
    if (bake_background_fullscreen() != ESP_OK) {
        ESP_LOGW(TAG, "fullscreen bg bake failed; callers will fall back to tile");
        return NULL;
    }
    return &s_bg_fullscreen_dsc;
}

typedef struct {
    int index;
} switch_msg_t;

static QueueHandle_t s_switch_queue = NULL;
static TaskHandle_t s_switch_worker = NULL;

static void switch_pack_ui_refresh(void *arg) {
    (void)arg;
    display_manager_update_status_bar_color();
    View *current = display_manager_get_current_view();
    if (current == &options_menu_view) {
        options_menu_refresh_theme();
    } else if (current && current->root && lv_obj_is_valid(current->root)) {
        gui_screen_apply_background(current->root);
    }
}

static esp_err_t switch_pack_run(int index) {
    ESP_LOGI(TAG, "switch_pack: loading pack index %d", index);
    esp_err_t err = asset_pack_select_by_index(index);
    ESP_LOGI(TAG, "switch_pack: result=%s", esp_err_to_name(err));

    if (err == ESP_OK) {
        (void)asset_pack_get_background_fullscreen();
    } else if (err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "asset pack switch failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void switch_pack_worker(void *arg) {
    (void)arg;
    switch_msg_t msg;
    while (true) {
        if (xQueueReceive(s_switch_queue, &msg, portMAX_DELAY) != pdTRUE) continue;
        esp_err_t err = switch_pack_run(msg.index);
        if (err == ESP_OK) {
            display_manager_run_on_lvgl(switch_pack_ui_refresh, NULL);
            ESP_LOGI(TAG, "switch_pack_worker: UI refresh scheduled via version counter");
        }
    }
}

void asset_pack_switch_task(int index) {
    ESP_LOGI(TAG, "asset_pack_switch_task: queue pack index %d", index);
    if (!s_switch_queue) {
        s_switch_queue = xQueueCreate(1, sizeof(switch_msg_t));
        if (!s_switch_queue) {
            ESP_LOGW(TAG, "asset_pack_switch_task: no memory for switch queue; switching inline");
            esp_err_t err = switch_pack_run(index);
            if (err == ESP_OK) switch_pack_ui_refresh(NULL);
            return;
        }
    }

    if (!s_switch_worker) {
        BaseType_t ok = xTaskCreate(switch_pack_worker, "pack_switch", 4096, NULL, 6, &s_switch_worker);
        if (ok != pdPASS) {
            ESP_LOGW(TAG, "asset_pack_switch_task: failed to create worker task; switching inline");
            esp_err_t err = switch_pack_run(index);
            if (err == ESP_OK) switch_pack_ui_refresh(NULL);
            return;
        }
    }

    switch_msg_t msg = {.index = index};
    if (xQueueOverwrite(s_switch_queue, &msg) != pdTRUE) {
        ESP_LOGW(TAG, "asset_pack_switch_task: switch queue send failed; switching inline");
        esp_err_t err = switch_pack_run(index);
        if (err == ESP_OK) switch_pack_ui_refresh(NULL);
    }
}
