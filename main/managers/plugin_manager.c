#include "managers/plugin_manager.h"

#include "managers/plugin_api.h"
#include "managers/plugin_installer.h"
#include "managers/plugin_icon.h"
#include "managers/sd_card_manager.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "core/memory_debug.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PLUGIN_APPS_DIR "/mnt/ghostesp/apps"
#define PLUGIN_PACKAGES_DIR "/mnt/ghostesp/packages"
#define PLUGIN_APP_CACHE_DIR "/mnt/ghostesp/app_cache"
#define PLUGIN_APPDATA_DIR "/mnt/ghostesp/appdata"
#define PLUGIN_CACHE_META_FILE ".source"
#define PLUGIN_MANIFEST_MAX_BYTES 8192

static const char *TAG = "PluginManager";

#ifdef CONFIG_NATIVE_SD_APP_MAX_COUNT
#define PLUGIN_APP_REGISTRY_CAPACITY CONFIG_NATIVE_SD_APP_MAX_COUNT
#else
#define PLUGIN_APP_REGISTRY_CAPACITY PLUGIN_APP_MAX_COUNT
#endif

_Static_assert(PLUGIN_APP_REGISTRY_CAPACITY > 0 &&
               PLUGIN_APP_REGISTRY_CAPACITY <= PLUGIN_APP_MAX_COUNT,
               "native app registry capacity is out of range");
static plugin_app_manifest_t *s_apps = NULL;
static int s_app_count = 0;
static char s_last_error[128];
static plugin_manager_progress_cb_t s_progress_cb = NULL;
static void *s_progress_user = NULL;

typedef struct {
    bool valid;
    uint32_t count;
    uint64_t total_size;
    time_t newest_mtime;
} plugin_package_sig_t;

static plugin_package_sig_t s_package_sigs[2];
static bool s_packages_materialized = false;

static bool read_file_to_buffer(const char *path, char **out_buf);

static bool plugin_manager_has_psram(void) {
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
}

static bool plugin_manager_sd_jit_allowed(void) {
    return sd_card_needs_jit_mount();
}

static bool write_app_state_by_id(const char *id, uint32_t failure_count, bool quarantined, bool launch_pending, const char *last_error) {
    if (!id || id[0] == '\0') return false;
    char state_path[PLUGIN_APP_PATH_MAX];
    int n = snprintf(state_path, sizeof(state_path), "/mnt/ghostesp/appdata/%s/.state.json", id);
    if (n <= 0 || (size_t)n >= sizeof(state_path)) return false;
    FILE *f = fopen(state_path, "wb");
    if (!f) return false;
    cJSON *root = cJSON_CreateObject();
    if (!root) { fclose(f); return false; }
    cJSON_AddNumberToObject(root, "launch_failure_count", failure_count);
    cJSON_AddBoolToObject(root, "quarantined", quarantined);
    cJSON_AddBoolToObject(root, "launch_pending", launch_pending);
    cJSON_AddStringToObject(root, "last_error", last_error ? last_error : "");
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    if (json_str) {
        fputs(json_str, f);
        free(json_str);
    }
    fclose(f);
    return true;
}

static void set_error(const char *fmt, const char *arg) {
    snprintf(s_last_error, sizeof(s_last_error), fmt, arg ? arg : "");
}

static bool is_safe_id(const char *id) {
    if (!id || id[0] == '\0') return false;
    for (const char *p = id; *p; ++p) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) return false;
    }
    return true;
}

static bool join_path(char *dst, size_t dst_len, const char *base, const char *name) {
    if (!dst || dst_len == 0 || !base || !name) return false;
    size_t base_len = strlen(base);
    size_t name_len = strlen(name);
    if (base_len == 0 || name_len == 0 || base_len + 1 + name_len + 1 > dst_len) return false;
    memcpy(dst, base, base_len);
    dst[base_len] = '/';
    memcpy(dst + base_len + 1, name, name_len);
    dst[base_len + 1 + name_len] = '\0';
    return true;
}

static void copy_json_string(cJSON *root, const char *key, char *dst, size_t dst_len) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(dst, item->valuestring, dst_len - 1);
        dst[dst_len - 1] = '\0';
    }
}

static plugin_permission_t permission_from_string(const char *value) {
    if (!value) return 0;
    if (strcmp(value, "ui") == 0) return PLUGIN_PERMISSION_UI;
    if (strcmp(value, "storage") == 0) return PLUGIN_PERMISSION_STORAGE;
    if (strcmp(value, "commands") == 0 || strcmp(value, "command") == 0) return PLUGIN_PERMISSION_COMMANDS;
    if (strcmp(value, "tasks") == 0) return PLUGIN_PERMISSION_TASKS;
    if (strcmp(value, "wifi") == 0) return PLUGIN_PERMISSION_WIFI;
    if (strcmp(value, "ble") == 0) return PLUGIN_PERMISSION_BLE;
    if (strcmp(value, "nfc") == 0) return PLUGIN_PERMISSION_NFC;
    if (strcmp(value, "ir") == 0 || strcmp(value, "infrared") == 0) return PLUGIN_PERMISSION_IR;
    if (strcmp(value, "subghz") == 0) return PLUGIN_PERMISSION_SUBGHZ;
    if (strcmp(value, "badusb") == 0) return PLUGIN_PERMISSION_BADUSB;
    if (strcmp(value, "raw_gpio") == 0) return PLUGIN_PERMISSION_RAW_GPIO;
    if (strcmp(value, "lvgl") == 0) return PLUGIN_PERMISSION_LVGL;
    if (strcmp(value, "rgb") == 0 || strcmp(value, "led") == 0 || strcmp(value, "leds") == 0) return PLUGIN_PERMISSION_RGB;
    if (strcmp(value, "uart") == 0 || strcmp(value, "serial") == 0) return PLUGIN_PERMISSION_UART;
    if (strcmp(value, "i2c") == 0) return PLUGIN_PERMISSION_I2C;
    if (strcmp(value, "spi") == 0) return PLUGIN_PERMISSION_SPI;
    if (strcmp(value, "adc") == 0) return PLUGIN_PERMISSION_ADC;
    if (strcmp(value, "pwm") == 0 || strcmp(value, "ledc") == 0) return PLUGIN_PERMISSION_PWM;
    if (strcmp(value, "network") == 0 || strcmp(value, "http") == 0) return PLUGIN_PERMISSION_NETWORK;
    if (strcmp(value, "wifi_control") == 0) return PLUGIN_PERMISSION_WIFI_CONTROL;
    if (strcmp(value, "power") == 0 || strcmp(value, "battery") == 0) return PLUGIN_PERMISSION_POWER;
    if (strcmp(value, "input") == 0 || strcmp(value, "buttons") == 0) return PLUGIN_PERMISSION_INPUT;
    if (strcmp(value, "display") == 0 || strcmp(value, "backlight") == 0) return PLUGIN_PERMISSION_DISPLAY;
    if (strcmp(value, "time") == 0) return PLUGIN_PERMISSION_TIME;
    if (strcmp(value, "random") == 0) return PLUGIN_PERMISSION_RANDOM;
    if (strcmp(value, "system") == 0) return PLUGIN_PERMISSION_SYSTEM;
    if (strcmp(value, "camera") == 0) return PLUGIN_PERMISSION_CAMERA;
    if (strcmp(value, "usb") == 0) return PLUGIN_PERMISSION_USB;
    if (strcmp(value, "ethernet") == 0 || strcmp(value, "eth") == 0) return PLUGIN_PERMISSION_ETHERNET;
    if (strcmp(value, "audio") == 0 || strcmp(value, "mic") == 0 || strcmp(value, "microphone") == 0) return PLUGIN_PERMISSION_AUDIO;
    if (strcmp(value, "settings") == 0) return PLUGIN_PERMISSION_SETTINGS;
    if (strcmp(value, "zigbee") == 0 || strcmp(value, "ieee802154") == 0) return PLUGIN_PERMISSION_ZIGBEE;
    if (strcmp(value, "nrf24") == 0) return PLUGIN_PERMISSION_NRF24;
    if (strcmp(value, "espnow") == 0 || strcmp(value, "esp_now") == 0) return PLUGIN_PERMISSION_ESPNOW;
    return 0;
}

static plugin_feature_t feature_from_string(const char *value) {
    if (!value) return 0;
    if (strcmp(value, "touchscreen") == 0 || strcmp(value, "touch") == 0) return PLUGIN_FEATURE_TOUCHSCREEN;
    if (strcmp(value, "dpad") == 0 || strcmp(value, "joystick") == 0) return PLUGIN_FEATURE_DPAD;
    if (strcmp(value, "encoder") == 0) return PLUGIN_FEATURE_ENCODER;
    if (strcmp(value, "keyboard") == 0 || strcmp(value, "physical_keyboard") == 0) return PLUGIN_FEATURE_KEYBOARD;
    return 0;
}

static const char *feature_name(plugin_feature_t feature) {
    switch (feature) {
        case PLUGIN_FEATURE_TOUCHSCREEN: return "touchscreen";
        case PLUGIN_FEATURE_DPAD: return "D-pad";
        case PLUGIN_FEATURE_ENCODER: return "encoder";
        case PLUGIN_FEATURE_KEYBOARD: return "keyboard";
        default: return "required input";
    }
}

static const char *feature_id(plugin_feature_t feature) {
    switch (feature) {
        case PLUGIN_FEATURE_TOUCHSCREEN: return "touchscreen";
        case PLUGIN_FEATURE_DPAD: return "dpad";
        case PLUGIN_FEATURE_ENCODER: return "encoder";
        case PLUGIN_FEATURE_KEYBOARD: return "keyboard";
        default: return "";
    }
}

static bool has_gapp_extension(const char *name) {
    if (!name) return false;
    const char *dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ".gapp") == 0;
}

static uint32_t fnv1a32(const char *value) {
    uint32_t hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)value; p && *p; ++p) {
        hash ^= *p;
        hash *= 16777619u;
    }
    return hash;
}

static void package_cache_name(char *dst, size_t dst_len, const char *source_path, const char *file_name) {
    size_t out = 0;
    if (!dst || dst_len == 0) return;
    size_t suffix_len = 9;
    size_t limit = dst_len > suffix_len ? dst_len - suffix_len : 1;
    size_t stem_len = strlen(file_name);
    const char *dot = strrchr(file_name, '.');
    if (dot && strcasecmp(dot, ".gapp") == 0) stem_len = (size_t)(dot - file_name);
    for (size_t i = 0; i < stem_len && out + 1 < limit; ++i) {
        char c = file_name[i];
        dst[out++] = (isalnum((unsigned char)c) || c == '_' || c == '-') ? c : '_';
    }
    if (out == 0 && out + 1 < limit) dst[out++] = '_';
    if (dst_len >= suffix_len) {
        snprintf(dst + out, dst_len - out, "-%08lx", (unsigned long)fnv1a32(source_path));
    } else {
        dst[out] = '\0';
    }
}

static bool write_cache_source(const char *cache_path, const char *source_path, const struct stat *source_st) {
    char meta_path[PLUGIN_APP_PATH_MAX];
    if (!join_path(meta_path, sizeof(meta_path), cache_path, PLUGIN_CACHE_META_FILE)) return false;
    FILE *f = fopen(meta_path, "wb");
    if (!f) return false;
    bool ok = fprintf(f, "%s\n%ld\n%ld\n", source_path, (long)source_st->st_size, (long)source_st->st_mtime) > 0;
    fclose(f);
    return ok;
}

static bool cache_source_current(const char *cache_path) {
    char meta_path[PLUGIN_APP_PATH_MAX];
    if (!join_path(meta_path, sizeof(meta_path), cache_path, PLUGIN_CACHE_META_FILE)) return false;
    FILE *f = fopen(meta_path, "rb");
    if (!f) return false;

    char source_path[PLUGIN_APP_PATH_MAX];
    long recorded_size = -1;
    long recorded_mtime = -1;
    bool ok = fgets(source_path, sizeof(source_path), f) != NULL &&
              fscanf(f, "%ld\n%ld", &recorded_size, &recorded_mtime) == 2;
    fclose(f);
    if (!ok) return false;

    source_path[strcspn(source_path, "\r\n")] = '\0';
    struct stat st;
    if (stat(source_path, &st) != 0 || S_ISDIR(st.st_mode)) return false;
    return recorded_size == (long)st.st_size && recorded_mtime == (long)st.st_mtime;
}

static bool package_cache_current(const char *cache_path) {
    if (!cache_source_current(cache_path)) return false;
    char manifest_path[PLUGIN_APP_PATH_MAX];
    struct stat st;
    return join_path(manifest_path, sizeof(manifest_path), cache_path, "manifest.json") && stat(manifest_path, &st) == 0;
}

static plugin_package_sig_t package_dir_signature(const char *dir_path) {
    plugin_package_sig_t sig = { .valid = true };
    DIR *dir = opendir(dir_path);
    if (!dir) {
        sig.valid = false;
        return sig;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || !has_gapp_extension(entry->d_name)) continue;
        char package_path[PLUGIN_APP_PATH_MAX];
        if (!join_path(package_path, sizeof(package_path), dir_path, entry->d_name)) continue;
        struct stat st;
        if (stat(package_path, &st) != 0 || S_ISDIR(st.st_mode)) continue;
        sig.count++;
        sig.total_size += (uint64_t)st.st_size;
        if (st.st_mtime > sig.newest_mtime) sig.newest_mtime = st.st_mtime;
    }
    closedir(dir);
    return sig;
}

static bool package_sig_equal(plugin_package_sig_t a, plugin_package_sig_t b) {
    return a.valid == b.valid && a.count == b.count && a.total_size == b.total_size && a.newest_mtime == b.newest_mtime;
}

static bool materialize_gapp_dir(const char *dir_path) {
    bool all_ok = true;
    DIR *dir = opendir(dir_path);
    if (!dir) return true;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || !has_gapp_extension(entry->d_name)) continue;
        char package_path[PLUGIN_APP_PATH_MAX];
        if (!join_path(package_path, sizeof(package_path), dir_path, entry->d_name)) {
            ESP_LOGW(TAG, "Skipping package with too-long path: %s", entry->d_name);
            continue;
        }
        struct stat st;
        if (stat(package_path, &st) != 0 || S_ISDIR(st.st_mode)) continue;

        char cache_name[PLUGIN_APP_ID_MAX];
        char cache_path[PLUGIN_APP_PATH_MAX];
        package_cache_name(cache_name, sizeof(cache_name), package_path, entry->d_name);
        if (!join_path(cache_path, sizeof(cache_path), PLUGIN_APP_CACHE_DIR, cache_name)) continue;

        if (package_cache_current(cache_path)) continue;

        ESP_LOGI(TAG, "Materializing changed package: %s", entry->d_name);
        esp_err_t err = plugin_installer_extract_gapp_to_dir(package_path, cache_path);
        if (err != ESP_OK) {
            all_ok = false;
            ESP_LOGW(TAG, "Package cache extraction failed for %s: %s", entry->d_name, plugin_installer_last_error());
        } else if (!write_cache_source(cache_path, package_path, &st)) {
            all_ok = false;
            ESP_LOGW(TAG, "Failed to write package cache metadata for %s", entry->d_name);
        }
    }

    closedir(dir);
    return all_ok;
}

static void plugin_manager_materialize_packages(void) {
    sd_card_create_directory(PLUGIN_APPS_DIR);
    sd_card_create_directory(PLUGIN_PACKAGES_DIR);
    sd_card_create_directory(PLUGIN_APP_CACHE_DIR);

    const char *package_dirs[] = { PLUGIN_PACKAGES_DIR, PLUGIN_APPS_DIR };
    plugin_package_sig_t current[2];
    bool changed = !s_packages_materialized;
    for (size_t i = 0; i < sizeof(package_dirs) / sizeof(package_dirs[0]); ++i) {
        current[i] = package_dir_signature(package_dirs[i]);
        if (!package_sig_equal(current[i], s_package_sigs[i])) changed = true;
    }
    if (!changed) return;

    bool ok = materialize_gapp_dir(PLUGIN_PACKAGES_DIR);
    ok = materialize_gapp_dir(PLUGIN_APPS_DIR) && ok;
    if (ok) {
        memcpy(s_package_sigs, current, sizeof(s_package_sigs));
        s_packages_materialized = true;
    }
}

static void ensure_appdata_dir(const char *app_id) {
    if (!app_id || app_id[0] == '\0') return;
    sd_card_create_directory(PLUGIN_APPDATA_DIR);
    char appdata_path[PLUGIN_APP_PATH_MAX];
    if (join_path(appdata_path, sizeof(appdata_path), PLUGIN_APPDATA_DIR, app_id)) {
        sd_card_create_directory(appdata_path);
    }
}

static uint32_t copy_json_u32(cJSON *root, const char *key, uint32_t fallback) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item) || item->valueint < 0) return fallback;
    return (uint32_t)item->valueint;
}

static bool copy_json_bool(cJSON *root, const char *key, bool fallback) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

static void load_app_state(plugin_app_manifest_t *out) {
    if (!out || out->id[0] == '\0') return;
    char state_path[PLUGIN_APP_PATH_MAX];
    int n = snprintf(state_path, sizeof(state_path), "/mnt/ghostesp/appdata/%s/.state.json", out->id);
    if (n <= 0 || (size_t)n >= sizeof(state_path)) return;

    char *buf = NULL;
    if (!read_file_to_buffer(state_path, &buf)) return;
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;
    out->launch_failure_count = copy_json_u32(root, "launch_failure_count", 0);
    bool launch_pending = copy_json_bool(root, "launch_pending", false);
    bool was_quarantined = copy_json_bool(root, "quarantined", false);
    cJSON_Delete(root);

    if (launch_pending) {
        out->launch_failure_count++;
        write_app_state_by_id(out->id, out->launch_failure_count, false, false, "app launch interrupted");
    } else {
        out->quarantined = false;
        if (was_quarantined) write_app_state_by_id(out->id, out->launch_failure_count, false, false, "");
    }
}

static bool read_file_to_buffer(const char *path, char **out_buf) {
    *out_buf = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long size = ftell(f);
    if (size <= 0 || size > PLUGIN_MANIFEST_MAX_BYTES) { fclose(f); return false; }
    rewind(f);
    char *buf = calloc(1, (size_t)size + 1);
    if (!buf) { fclose(f); return false; }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) { free(buf); return false; }
    *out_buf = buf;
    return true;
}

static bool parse_manifest(const char *base_path, plugin_app_manifest_t *out) {
    char manifest_path[PLUGIN_APP_PATH_MAX];
    if (!join_path(manifest_path, sizeof(manifest_path), base_path, "manifest.json")) {
        snprintf(out->error, sizeof(out->error), "manifest path too long");
        return false;
    }

    char *buf = NULL;
    if (!read_file_to_buffer(manifest_path, &buf)) {
        snprintf(out->error, sizeof(out->error), "manifest read failed");
        return false;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        snprintf(out->error, sizeof(out->error), "manifest json invalid");
        return false;
    }

    copy_json_string(root, "id", out->id, sizeof(out->id));
    copy_json_string(root, "name", out->name, sizeof(out->name));
    copy_json_string(root, "version", out->version, sizeof(out->version));
    copy_json_string(root, "author", out->author, sizeof(out->author));
    copy_json_string(root, "target", out->target, sizeof(out->target));
    cJSON *targets = cJSON_GetObjectItemCaseSensitive(root, "targets");
    if (cJSON_IsArray(targets)) {
        cJSON *target = NULL;
        cJSON_ArrayForEach(target, targets) {
            if (!cJSON_IsString(target) || !target->valuestring) continue;
            size_t used = strlen(out->targets);
            if (used + strlen(target->valuestring) + 2 >= sizeof(out->targets)) break;
            if (used > 0) out->targets[used++] = ',';
            strcpy(out->targets + used, target->valuestring);
        }
    }
    copy_json_string(root, "entry", out->entry, sizeof(out->entry));
    copy_json_string(root, "description", out->description, sizeof(out->description));
    copy_json_string(root, "category", out->category, sizeof(out->category));
    copy_json_string(root, "icon", out->icon, sizeof(out->icon));
    copy_json_string(root, "icon_format", out->icon_format, sizeof(out->icon_format));
    copy_json_string(root, "accent_color", out->accent_color, sizeof(out->accent_color));
    copy_json_string(root, "storage_scope", out->storage_scope, sizeof(out->storage_scope));
    copy_json_string(root, "firmware_min", out->firmware_min, sizeof(out->firmware_min));
    copy_json_string(root, "firmware_max", out->firmware_max, sizeof(out->firmware_max));
    copy_json_string(root, "checksum", out->checksum, sizeof(out->checksum));

    out->api_version = copy_json_u32(root, "api_version", 0);
    out->manifest_version = copy_json_u32(root, "manifest_version", PLUGIN_APP_MANIFEST_VERSION);
    out->package_version = copy_json_u32(root, "package_version", 1);
    out->data_version = copy_json_u32(root, "data_version", PLUGIN_APP_DATA_VERSION_DEFAULT);
    out->memory_limit = copy_json_u32(root, "memory_limit", 0);
    out->stack_size = copy_json_u32(root, "stack_size", 0);
    out->tick_interval_ms = copy_json_u32(root, "tick_interval_ms", 0);
    out->icon_width = (uint16_t)copy_json_u32(root, "icon_width", 0);
    out->icon_height = (uint16_t)copy_json_u32(root, "icon_height", 0);
    out->requires_psram = copy_json_bool(root, "requires_psram", false);

    cJSON *permissions = cJSON_GetObjectItemCaseSensitive(root, "permissions");
    if (cJSON_IsArray(permissions)) {
        cJSON *perm = NULL;
        cJSON_ArrayForEach(perm, permissions) {
            if (cJSON_IsString(perm)) {
                plugin_permission_t permission = permission_from_string(perm->valuestring);
                if (!permission) {
                    snprintf(out->error, sizeof(out->error), "unknown permission: %.64s", perm->valuestring);
                    cJSON_Delete(root);
                    return false;
                }
                out->permissions |= permission;
            }
        }
    }

    cJSON *required_features = cJSON_GetObjectItemCaseSensitive(root, "requires_features");
    if (required_features && !cJSON_IsArray(required_features)) {
        snprintf(out->error, sizeof(out->error), "requires_features must be an array");
        cJSON_Delete(root);
        return false;
    }
    if (cJSON_IsArray(required_features)) {
        cJSON *feature = NULL;
        cJSON_ArrayForEach(feature, required_features) {
            if (!cJSON_IsString(feature) || !feature->valuestring) {
                snprintf(out->error, sizeof(out->error), "invalid required feature");
                cJSON_Delete(root);
                return false;
            }
            plugin_feature_t bit = feature_from_string(feature->valuestring);
            if (!bit) {
                snprintf(out->error, sizeof(out->error), "unknown required feature: %.48s", feature->valuestring);
                cJSON_Delete(root);
                return false;
            }
            out->required_features |= bit;
        }
    }

    cJSON_Delete(root);

    strncpy(out->base_path, base_path, sizeof(out->base_path) - 1);
    if (out->storage_scope[0] == '\0') {
        strncpy(out->storage_scope, PLUGIN_APP_STORAGE_SCOPE_APP, sizeof(out->storage_scope) - 1);
    }
    out->allow_absolute_storage = strcmp(out->storage_scope, PLUGIN_APP_STORAGE_SCOPE_GHOSTESP) == 0;
    if (!is_safe_id(out->id)) {
        snprintf(out->error, sizeof(out->error), "invalid id");
        return false;
    }
    ensure_appdata_dir(out->id);
    if (out->name[0] == '\0') strncpy(out->name, out->id, sizeof(out->name) - 1);
    load_app_state(out);
    if (out->entry[0] == '\0') {
        snprintf(out->error, sizeof(out->error), "missing entry");
        return false;
    }
    if (strstr(out->entry, "..") || strchr(out->entry, '/') || strchr(out->entry, '\\')) {
        snprintf(out->error, sizeof(out->error), "invalid entry path");
        return false;
    }
    if (!join_path(out->entry_path, sizeof(out->entry_path), base_path, out->entry)) {
        snprintf(out->error, sizeof(out->error), "entry path too long");
        return false;
    }
    if (out->api_version != GHOSTESP_APP_API_VERSION) {
        snprintf(out->error, sizeof(out->error), "api version mismatch");
        return false;
    }
    if (out->manifest_version != PLUGIN_APP_MANIFEST_VERSION) {
        snprintf(out->error, sizeof(out->error), "manifest version mismatch");
        return false;
    }
    if (out->tick_interval_ms != 0 && (out->tick_interval_ms < 16 || out->tick_interval_ms > 1000)) {
        snprintf(out->error, sizeof(out->error), "tick interval must be 16-1000 ms");
        return false;
    }
    if (strcmp(out->storage_scope, PLUGIN_APP_STORAGE_SCOPE_APP) != 0 &&
        strcmp(out->storage_scope, PLUGIN_APP_STORAGE_SCOPE_GHOSTESP) != 0) {
        snprintf(out->error, sizeof(out->error), "invalid storage scope");
        return false;
    }
    if (out->icon[0] != '\0') {
        if (out->icon[0] == '/' || strstr(out->icon, "..") || strchr(out->icon, '\\')) {
            snprintf(out->error, sizeof(out->error), "invalid icon path");
            return false;
        }
        if (out->icon_format[0] == '\0') strncpy(out->icon_format, "rgb565a8", sizeof(out->icon_format) - 1);
    }
    if (!sd_card_exists(out->entry_path)) {
        snprintf(out->error, sizeof(out->error), "entry missing");
        return false;
    }

    out->valid = true;
    return true;
}

void plugin_manager_init(void) {
    if (!plugin_manager_has_psram()) {
        s_app_count = 0;
        s_last_error[0] = '\0';
        return;
    }
    if (!s_apps) {
        s_apps = spiram_calloc(PLUGIN_APP_REGISTRY_CAPACITY, sizeof(*s_apps));
        if (!s_apps) {
            snprintf(s_last_error, sizeof(s_last_error), "failed to allocate app registry");
            return;
        }
    }
    s_last_error[0] = '\0';
}

bool plugin_manager_target_supported(void) {
#if CONFIG_ENABLE_NATIVE_SD_APPS
    return plugin_manager_has_psram();
#else
    return false;
#endif
}

bool plugin_manager_target_matches(const plugin_app_manifest_t *app) {
    if (!app) return false;
    if (app->targets[0] != '\0') {
        const char *target = plugin_api_current_target();
        const char *cursor = app->targets;
        while (*cursor) {
            const char *end = strchr(cursor, ',');
            size_t len = end ? (size_t)(end - cursor) : strlen(cursor);
            if (strlen(target) == len && strncmp(cursor, target, len) == 0) return true;
            cursor = end ? end + 1 : cursor + len;
        }
        return false;
    }
    if (app->target[0] == '\0') return true;
    return strcmp(app->target, plugin_api_current_target()) == 0;
}

bool plugin_manager_required_features_supported(const plugin_app_manifest_t *app, char *missing_feature, size_t missing_feature_len) {
    if (missing_feature && missing_feature_len > 0) missing_feature[0] = '\0';
    if (!app) return false;

    const plugin_feature_t features[] = {
        PLUGIN_FEATURE_TOUCHSCREEN,
        PLUGIN_FEATURE_DPAD,
        PLUGIN_FEATURE_ENCODER,
        PLUGIN_FEATURE_KEYBOARD,
    };
    for (size_t i = 0; i < sizeof(features) / sizeof(features[0]); ++i) {
        if (!(app->required_features & features[i])) continue;
        if (plugin_api_feature_supported(feature_id(features[i]))) continue;
        if (missing_feature && missing_feature_len > 0) {
            snprintf(missing_feature, missing_feature_len, "%s", feature_name(features[i]));
        }
        return false;
    }
    return true;
}

int plugin_manager_reload(void) {
    int64_t start_us = esp_timer_get_time();
    if (!plugin_manager_target_supported()) {
        s_app_count = 0;
        s_last_error[0] = '\0';
        if (s_progress_cb) s_progress_cb(100.0f, 0, 0, s_progress_user);
        return 0;
    }
    plugin_manager_init();
    if (!s_apps) return -1;
    for (int i = 0; i < s_app_count; ++i) {
        plugin_icon_free(s_apps[i].icon_dsc);
    }
    s_app_count = 0;
    memset(s_apps, 0, sizeof(*s_apps) * PLUGIN_APP_REGISTRY_CAPACITY);
    s_last_error[0] = '\0';
    if (s_progress_cb) s_progress_cb(0.0f, 0, 0, s_progress_user);

    bool display_was_suspended = false;
    bool mounted_here = false;
    if (!sd_card_manager.is_initialized) {
        if (!plugin_manager_sd_jit_allowed()) {
            set_error("SD not mounted for %s", "apps");
            if (s_progress_cb) s_progress_cb(100.0f, 0, 0, s_progress_user);
            return -1;
        }
        if (sd_card_mount_for_flush(&display_was_suspended) != ESP_OK) {
            set_error("failed to mount SD for %s", "apps");
            if (s_progress_cb) s_progress_cb(100.0f, 0, 0, s_progress_user);
            return -1;
        }
        mounted_here = true;
    }

    int64_t materialize_start_us = esp_timer_get_time();
    plugin_manager_materialize_packages();
    ESP_LOGI(TAG, "Package materialize check took %lld ms", (long long)((esp_timer_get_time() - materialize_start_us) / 1000));

    const char *scan_dirs[] = { PLUGIN_APPS_DIR, PLUGIN_APP_CACHE_DIR };
    for (size_t scan_i = 0; scan_i < sizeof(scan_dirs) / sizeof(scan_dirs[0]) && s_app_count < PLUGIN_APP_REGISTRY_CAPACITY; ++scan_i) {
        DIR *dir = opendir(scan_dirs[scan_i]);
        if (!dir) {
            if (scan_i == 0) set_error("failed to open %s", scan_dirs[scan_i]);
            continue;
        }

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && s_app_count < PLUGIN_APP_REGISTRY_CAPACITY) {
            if (entry->d_name[0] == '.') continue;
            char base_path[PLUGIN_APP_PATH_MAX];
            if (!join_path(base_path, sizeof(base_path), scan_dirs[scan_i], entry->d_name)) {
                ESP_LOGW(TAG, "Skipping app with too-long path: %s", entry->d_name);
                continue;
            }

            struct stat st;
            if (stat(base_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            if (scan_i == 1 && !cache_source_current(base_path)) continue;

            plugin_app_manifest_t *app = &s_apps[s_app_count];
            memset(app, 0, sizeof(*app));
            if (!parse_manifest(base_path, app)) {
                ESP_LOGW(TAG, "Skipping app at %s: %s", base_path, app->error);
                memset(app, 0, sizeof(*app));
                continue;
            }
            s_app_count++;
        }

        closedir(dir);
    }
    for (int i = 0; i < s_app_count; ++i) {
        if (s_apps[i].icon[0] != '\0' && s_apps[i].icon_width > 0 && s_apps[i].icon_height > 0 && !s_apps[i].icon_dsc) {
            char icon_path[PLUGIN_APP_PATH_MAX];
            if (join_path(icon_path, sizeof(icon_path), s_apps[i].base_path, s_apps[i].icon)) {
                if (strcmp(s_apps[i].icon_format, "rgb565a8") == 0) {
                    s_apps[i].icon_dsc = plugin_icon_load_rgb565a8(icon_path, s_apps[i].icon_width, s_apps[i].icon_height);
                } else if (strcmp(s_apps[i].icon_format, "true_color_alpha") == 0) {
                    s_apps[i].icon_dsc = plugin_icon_load_true_color_alpha(icon_path, s_apps[i].icon_width, s_apps[i].icon_height);
                } else {
                    s_apps[i].icon_dsc = plugin_icon_load_rgb565(icon_path, s_apps[i].icon_width, s_apps[i].icon_height);
                }
            }
        }
    }
    if (mounted_here) sd_card_unmount_after_flush(display_was_suspended);
    if (s_progress_cb) s_progress_cb(100.0f, s_app_count, s_app_count, s_progress_user);
    ESP_LOGI(TAG, "Loaded %d SD app manifests in %lld ms", s_app_count, (long long)((esp_timer_get_time() - start_us) / 1000));
    return s_app_count;
}

int plugin_manager_count(void) {
    return s_app_count;
}

const plugin_app_manifest_t *plugin_manager_get(int index) {
    if (!s_apps) return NULL;
    if (index < 0 || index >= s_app_count) return NULL;
    return &s_apps[index];
}

const plugin_app_manifest_t *plugin_manager_find(const char *id) {
    if (!s_apps || !id) return NULL;
    for (int i = 0; i < s_app_count; ++i) {
        if (strcmp(s_apps[i].id, id) == 0) return &s_apps[i];
    }
    return NULL;
}

const lv_img_dsc_t *plugin_manager_get_icon(const plugin_app_manifest_t *app) {
    if (!app || app->icon[0] == '\0' || app->icon_width == 0 || app->icon_height == 0) return NULL;
    plugin_app_manifest_t *mutable_app = NULL;
    for (int i = 0; i < s_app_count; ++i) {
        if (&s_apps[i] == app || strcmp(s_apps[i].id, app->id) == 0) {
            mutable_app = &s_apps[i];
            break;
        }
    }
    if (!mutable_app) return NULL;
    if (mutable_app->icon_dsc) return mutable_app->icon_dsc;

    char icon_path[PLUGIN_APP_PATH_MAX];
    if (!join_path(icon_path, sizeof(icon_path), mutable_app->base_path, mutable_app->icon)) return NULL;
    if (strcmp(mutable_app->icon_format, "rgb565a8") == 0) {
        mutable_app->icon_dsc = plugin_icon_load_rgb565a8(icon_path, mutable_app->icon_width, mutable_app->icon_height);
    } else if (strcmp(mutable_app->icon_format, "true_color_alpha") == 0) {
        mutable_app->icon_dsc = plugin_icon_load_true_color_alpha(icon_path, mutable_app->icon_width, mutable_app->icon_height);
    } else {
        mutable_app->icon_dsc = plugin_icon_load_rgb565(icon_path, mutable_app->icon_width, mutable_app->icon_height);
    }
    return mutable_app->icon_dsc;
}

bool plugin_manager_reset_app_state(const char *id) {
    if (!is_safe_id(id)) {
        set_error("invalid app id: %s", id);
        return false;
    }

    if (!write_app_state_by_id(id, 0, false, false, "")) {
        set_error("failed to reset app state: %s", id);
        return false;
    }

    if (s_apps) {
        for (int i = 0; i < s_app_count; ++i) {
            if (strcmp(s_apps[i].id, id) == 0) {
                s_apps[i].launch_failure_count = 0;
                s_apps[i].quarantined = false;
                break;
            }
        }
    }

    s_last_error[0] = '\0';
    return true;
}

const char *plugin_manager_last_error(void) {
    return s_last_error;
}

void plugin_manager_set_progress_cb(plugin_manager_progress_cb_t cb, void *user) {
    s_progress_cb = cb;
    s_progress_user = user;
}
