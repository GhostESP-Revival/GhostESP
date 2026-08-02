#include "managers/ghostscript_manager.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "managers/sd_card_manager.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static const char *TAG = "GhostScriptMgr";
static char *s_last_error;

static void read_state(ghostscript_manifest_t *manifest);

static void clear_error(void) {
    free(s_last_error);
    s_last_error = NULL;
}

static char *error_buffer(void) {
    clear_error();
    return s_last_error = malloc(GHOSTSCRIPT_ERROR_MAX);
}

static void set_error(const char *msg) {
    char *buffer = error_buffer();
    if (buffer) snprintf(buffer, GHOSTSCRIPT_ERROR_MAX, "%s", msg ? msg : "GhostScript error");
}

static void set_path_error(const char *operation, const char *path) {
    char *buffer = error_buffer();
    if (buffer) {
        snprintf(buffer, GHOSTSCRIPT_ERROR_MAX, "%s %.72s: %s",
                 operation ? operation : "SD operation failed", path ? path : "",
                 strerror(errno));
    }
}

const char *ghostscript_manager_last_error(void) {
    return s_last_error ? s_last_error : "";
}

bool ghostscript_manager_sd_begin(bool *display_was_suspended) {
    if (display_was_suspended) *display_was_suspended = false;
    if (!sd_card_jit_begin(display_was_suspended, false)) {
        set_error("SD mount failed");
        ESP_LOGW(TAG, "SD JIT mount failed");
        return false;
    }
    return true;
}

void ghostscript_manager_sd_end(bool display_was_suspended) {
    sd_card_jit_end(display_was_suspended);
}

static bool join_path(char *out, size_t out_len, const char *base, const char *name) {
    if (!out || out_len == 0 || !base || !name || !base[0] || !name[0]) return false;
    size_t bl = strlen(base);
    size_t nl = strlen(name);
    if (bl + 1 + nl + 1 > out_len) return false;
    memcpy(out, base, bl);
    out[bl] = '/';
    memcpy(out + bl + 1, name, nl);
    out[bl + 1 + nl] = '\0';
    return true;
}

static const char *basename_of(const char *path) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash && slash[1] ? slash + 1 : (path ? path : "script");
}

static void sanitize_id_from_name(const char *name, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    size_t j = 0;
    for (size_t i = 0; name && name[i] && j + 1 < out_len; ++i) {
        char c = name[i];
        if (c == '.') break;
        if (isalnum((unsigned char)c) || c == '_' || c == '-') out[j++] = c;
        else if (c == ' ' || c == '.') out[j++] = '_';
    }
    if (j == 0 && out_len > 1) out[j++] = 's';
    out[j] = '\0';
}

bool ghostscript_manager_is_safe_id(const char *id) {
    if (!id || !id[0]) return false;
    for (const char *p = id; *p; ++p) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) return false;
    }
    return true;
}

bool ghostscript_manager_is_script_file(const char *name) {
    if (!name) return false;
    const char *dot = strrchr(name, '.');
    return dot && strcasecmp(dot, GHOSTSCRIPT_EXT) == 0;
}

bool ghostscript_manager_permission_from_string(const char *value, uint32_t *out) {
    uint32_t perm = 0;
    if (!value) return false;
    if (strcmp(value, "ui") == 0) perm = PLUGIN_PERMISSION_UI;
    else if (strcmp(value, "storage") == 0) perm = PLUGIN_PERMISSION_STORAGE;
    else if (strcmp(value, "commands") == 0 || strcmp(value, "command") == 0) perm = PLUGIN_PERMISSION_COMMANDS;
    else if (strcmp(value, "tasks") == 0) perm = PLUGIN_PERMISSION_TASKS;
    else if (strcmp(value, "wifi") == 0) perm = PLUGIN_PERMISSION_WIFI;
    else if (strcmp(value, "ble") == 0) perm = PLUGIN_PERMISSION_BLE;
    else if (strcmp(value, "nfc") == 0) perm = PLUGIN_PERMISSION_NFC;
    else if (strcmp(value, "ir") == 0 || strcmp(value, "infrared") == 0) perm = PLUGIN_PERMISSION_IR;
    else if (strcmp(value, "subghz") == 0) perm = PLUGIN_PERMISSION_SUBGHZ;
    else if (strcmp(value, "badusb") == 0) perm = PLUGIN_PERMISSION_BADUSB;
    else if (strcmp(value, "raw_gpio") == 0 || strcmp(value, "gpio") == 0) perm = PLUGIN_PERMISSION_RAW_GPIO;
    else if (strcmp(value, "lvgl") == 0) perm = PLUGIN_PERMISSION_LVGL;
    else if (strcmp(value, "rgb") == 0 || strcmp(value, "led") == 0 || strcmp(value, "leds") == 0) perm = PLUGIN_PERMISSION_RGB;
    else if (strcmp(value, "uart") == 0 || strcmp(value, "serial") == 0) perm = PLUGIN_PERMISSION_UART;
    else if (strcmp(value, "i2c") == 0) perm = PLUGIN_PERMISSION_I2C;
    else if (strcmp(value, "spi") == 0) perm = PLUGIN_PERMISSION_SPI;
    else if (strcmp(value, "adc") == 0) perm = PLUGIN_PERMISSION_ADC;
    else if (strcmp(value, "pwm") == 0) perm = PLUGIN_PERMISSION_PWM;
    else if (strcmp(value, "network") == 0 || strcmp(value, "http") == 0) perm = PLUGIN_PERMISSION_NETWORK;
    else if (strcmp(value, "wifi_control") == 0 || strcmp(value, "wifi.control") == 0) perm = PLUGIN_PERMISSION_WIFI_CONTROL;
    else if (strcmp(value, "power") == 0 || strcmp(value, "battery") == 0) perm = PLUGIN_PERMISSION_POWER;
    else if (strcmp(value, "input") == 0 || strcmp(value, "buttons") == 0) perm = PLUGIN_PERMISSION_INPUT;
    else if (strcmp(value, "display") == 0 || strcmp(value, "backlight") == 0) perm = PLUGIN_PERMISSION_DISPLAY;
    else if (strcmp(value, "time") == 0) perm = PLUGIN_PERMISSION_TIME;
    else if (strcmp(value, "random") == 0) perm = PLUGIN_PERMISSION_RANDOM;
    else if (strcmp(value, "system") == 0) perm = PLUGIN_PERMISSION_SYSTEM;
    else if (strcmp(value, "camera") == 0) perm = PLUGIN_PERMISSION_CAMERA;
    else if (strcmp(value, "usb") == 0) perm = PLUGIN_PERMISSION_USB;
    else if (strcmp(value, "ethernet") == 0 || strcmp(value, "eth") == 0) perm = PLUGIN_PERMISSION_ETHERNET;
    else if (strcmp(value, "audio") == 0 || strcmp(value, "mic") == 0) perm = PLUGIN_PERMISSION_AUDIO;
    else if (strcmp(value, "settings") == 0) perm = PLUGIN_PERMISSION_SETTINGS;
    else if (strcmp(value, "zigbee") == 0) perm = PLUGIN_PERMISSION_ZIGBEE;
    if (!perm) return false;
    if (out) *out = perm;
    return true;
}

static bool read_small_file(const char *path, char **out, size_t max_bytes) {
    if (out) *out = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f);
    if (sz < 0 || (size_t)sz > max_bytes) { fclose(f); return false; }
    rewind(f);
    char *buf = calloc(1, (size_t)sz + 1);
    if (!buf) { fclose(f); return false; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) { free(buf); return false; }
    *out = buf;
    return true;
}

static uint32_t clamp_memory_limit(uint32_t requested) {
    uint32_t limit = requested ? requested : GHOSTSCRIPT_DEFAULT_MEMORY_LIMIT;
    uint32_t max = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0 ? GHOSTSCRIPT_MAX_MEMORY_LIMIT_PSRAM : GHOSTSCRIPT_MAX_MEMORY_LIMIT_NO_PSRAM;
    if (limit > max) limit = max;
    if (limit < 8192) limit = 8192;
    return limit;
}

static void fill_common_defaults(ghostscript_manifest_t *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->version, sizeof(out->version), "1.0.0");
    /* Scripts must explicitly request every privileged capability. */
    out->permissions = 0;
    out->memory_limit = GHOSTSCRIPT_DEFAULT_MEMORY_LIMIT;
    out->instruction_budget = 10000;
    out->timeout_ms = 30000;
}

bool ghostscript_manager_make_single_file_manifest(const char *path, ghostscript_manifest_t *out) {
    if (!path || !out || !ghostscript_manager_is_script_file(path)) return false;
    bool display_was_suspended = false;
    if (!ghostscript_manager_sd_begin(&display_was_suspended)) return false;
    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode) || st.st_size > GHOSTSCRIPT_FILE_MAX_BYTES) {
        ghostscript_manager_sd_end(display_was_suspended);
        return false;
    }
    fill_common_defaults(out);
    const char *base = basename_of(path);
    sanitize_id_from_name(base, out->id, sizeof(out->id));
    snprintf(out->name, sizeof(out->name), "%s", base);
    char *dot = strrchr(out->name, '.');
    if (dot) *dot = '\0';
    snprintf(out->base_path, sizeof(out->base_path), "%s", path);
    snprintf(out->entry_path, sizeof(out->entry_path), "%s", path);
    snprintf(out->data_path, sizeof(out->data_path), "%s/%s", GHOSTSCRIPT_DATA_DIR, out->id);
    out->memory_limit = clamp_memory_limit(out->memory_limit);
    out->valid = true;
    ghostscript_manager_sd_end(display_was_suspended);
    read_state(out);
    return true;
}

bool ghostscript_manager_load_manifest(const char *path, ghostscript_manifest_t *out) {
    if (!path || !out) return false;
    bool display_was_suspended = false;
    if (!ghostscript_manager_sd_begin(&display_was_suspended)) return false;
    char manifest_path[GHOSTSCRIPT_PATH_MAX];
    struct stat st;
    bool path_is_dir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    if (path_is_dir) {
        if (!join_path(manifest_path, sizeof(manifest_path), path, "manifest.json")) {
            ghostscript_manager_sd_end(display_was_suspended);
            return false;
        }
    } else {
        snprintf(manifest_path, sizeof(manifest_path), "%s", path);
    }
    char *json = NULL;
    if (!read_small_file(manifest_path, &json, 8192)) {
        set_error("manifest read failed");
        ghostscript_manager_sd_end(display_was_suspended);
        return false;
    }
    ghostscript_manager_sd_end(display_was_suspended);
    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) { set_error("manifest parse failed"); return false; }
    fill_common_defaults(out);
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    cJSON *entry = cJSON_GetObjectItemCaseSensitive(root, "entry");
    if (!cJSON_IsString(id) || !ghostscript_manager_is_safe_id(id->valuestring) || !cJSON_IsString(name) || !cJSON_IsString(entry)) {
        cJSON_Delete(root);
        set_error("manifest missing id/name/entry");
        return false;
    }
    snprintf(out->id, sizeof(out->id), "%s", id->valuestring);
    snprintf(out->name, sizeof(out->name), "%s", name->valuestring);
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (cJSON_IsString(v)) snprintf(out->version, sizeof(out->version), "%s", v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(root, "author");
    if (cJSON_IsString(v)) snprintf(out->author, sizeof(out->author), "%s", v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(root, "description");
    if (cJSON_IsString(v)) snprintf(out->description, sizeof(out->description), "%s", v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(root, "category");
    if (cJSON_IsString(v)) snprintf(out->category, sizeof(out->category), "%s", v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(root, "memory_limit");
    if (cJSON_IsNumber(v)) out->memory_limit = (uint32_t)v->valueint;
    v = cJSON_GetObjectItemCaseSensitive(root, "instruction_budget");
    if (cJSON_IsNumber(v) && v->valueint > 0) out->instruction_budget = (uint32_t)v->valueint;
    v = cJSON_GetObjectItemCaseSensitive(root, "timeout_ms");
    if (cJSON_IsNumber(v) && v->valueint > 0) out->timeout_ms = (uint32_t)v->valueint;
    v = cJSON_GetObjectItemCaseSensitive(root, "storage_scope");
    out->allow_absolute_storage = cJSON_IsString(v) && strcmp(v->valuestring, PLUGIN_APP_STORAGE_SCOPE_GHOSTESP) == 0;
    v = cJSON_GetObjectItemCaseSensitive(root, "permissions");
    if (cJSON_IsArray(v)) {
        out->permissions = 0;
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, v) {
            uint32_t perm;
            if (cJSON_IsString(item) && ghostscript_manager_permission_from_string(item->valuestring, &perm)) out->permissions |= perm;
        }
    }
    if (path_is_dir) snprintf(out->base_path, sizeof(out->base_path), "%s", path);
    else {
        snprintf(out->base_path, sizeof(out->base_path), "%s", manifest_path);
        char *slash = strrchr(out->base_path, '/');
        if (slash) *slash = '\0';
    }
    if (!join_path(out->entry_path, sizeof(out->entry_path), out->base_path, entry->valuestring)) {
        cJSON_Delete(root);
        set_error("entry path too long");
        return false;
    }
    snprintf(out->data_path, sizeof(out->data_path), "%s/%s", GHOSTSCRIPT_DATA_DIR, out->id);
    out->memory_limit = clamp_memory_limit(out->memory_limit);
    out->valid = true;
    cJSON_Delete(root);
    read_state(out);
    return true;
}

static int browser_entry_compare(const void *a, const void *b) {
    const ghostscript_browser_entry_t *ea = (const ghostscript_browser_entry_t *)a;
    const ghostscript_browser_entry_t *eb = (const ghostscript_browser_entry_t *)b;
    int result = strcasecmp(ea->name, eb->name);
    return result != 0 ? result : strcmp(ea->path, eb->path);
}

int ghostscript_manager_list(const char *dir, int offset, ghostscript_browser_entry_t *out, int max_entries, bool *has_more) {
    if (has_more) *has_more = false;
    if (!dir || !out || max_entries <= 0) return 0;
    if (offset < 0) offset = 0;
    bool display_was_suspended = false;
    if (!ghostscript_manager_sd_begin(&display_was_suspended)) return 0;
    DIR *d = opendir(dir);
    if (!d) {
        set_path_error("Cannot open scripts directory", dir);
        ESP_LOGW(TAG, "%s", ghostscript_manager_last_error());
        ghostscript_manager_sd_end(display_was_suspended);
        return 0;
    }
    clear_error();
    ghostscript_browser_entry_t *entries = NULL;
    int count = 0;
    int capacity = 0;
    bool allocation_failed = false;
    int read_errno = 0;
    struct dirent *ent;
    while (true) {
        errno = 0;
        ent = readdir(d);
        if (!ent) {
            read_errno = errno;
            break;
        }
        if (ent->d_name[0] == '.') continue;
        char full[GHOSTSCRIPT_PATH_MAX];
        if (!join_path(full, sizeof(full), dir, ent->d_name)) continue;
        struct stat st;
        if (stat(full, &st) != 0) continue;
        bool is_dir = S_ISDIR(st.st_mode);
        bool is_script = !is_dir && ghostscript_manager_is_script_file(ent->d_name);
        bool has_manifest = false;
        if (is_dir) {
            char mp[GHOSTSCRIPT_PATH_MAX];
            has_manifest = join_path(mp, sizeof(mp), full, "manifest.json") && stat(mp, &st) == 0;
        }
        if (!is_dir && !is_script) continue;
        if (count == capacity) {
            int next_capacity = capacity ? capacity * 2 : 8;
            ghostscript_browser_entry_t *grown = heap_caps_realloc_prefer(
                entries, (size_t)next_capacity * sizeof(*entries), 2,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (!grown) {
                allocation_failed = true;
                break;
            }
            entries = grown;
            capacity = next_capacity;
        }
        ghostscript_browser_entry_t *entry = &entries[count];
        memset(entry, 0, sizeof(*entry));
        strncpy(entry->name, ent->d_name, sizeof(entry->name) - 1);
        strncpy(entry->path, full, sizeof(entry->path) - 1);
        entry->is_dir = is_dir;
        entry->is_script = is_script;
        entry->has_manifest = has_manifest;
        count++;
    }
    closedir(d);
    if (read_errno != 0) {
        free(entries);
        errno = read_errno;
        set_path_error("Cannot read scripts directory", dir);
        ghostscript_manager_sd_end(display_was_suspended);
        return 0;
    }
    if (allocation_failed) {
        free(entries);
        set_error("Not enough memory to list scripts");
        ghostscript_manager_sd_end(display_was_suspended);
        return 0;
    }
    if (count > 1) qsort(entries, (size_t)count, sizeof(*entries), browser_entry_compare);
    int available = count - offset;
    if (available < 0) available = 0;
    int result_count = available < max_entries ? available : max_entries;
    if (result_count > 0) {
        memcpy(out, entries + offset, (size_t)result_count * sizeof(*out));
    }
    if (has_more) *has_more = available > result_count;
    free(entries);
    ghostscript_manager_sd_end(display_was_suspended);
    return result_count;
}

void ghostscript_manager_init(void) {
    clear_error();
    bool display_was_suspended = false;
    if (!ghostscript_manager_sd_begin(&display_was_suspended)) return;
    sd_card_create_directory("/mnt/ghostesp");
    sd_card_create_directory(GHOSTSCRIPT_ROOT_DIR);
    sd_card_create_directory(GHOSTSCRIPT_DATA_DIR);
    ghostscript_manager_sd_end(display_was_suspended);
}

static bool state_path(const ghostscript_manifest_t *manifest, char *out, size_t out_len) {
    if (!manifest || !manifest->id[0]) return false;
    int n = snprintf(out, out_len, "%s/%s/.state.json", GHOSTSCRIPT_DATA_DIR, manifest->id);
    return n > 0 && (size_t)n < out_len;
}

static void read_state(ghostscript_manifest_t *manifest) {
    if (!manifest || !manifest->id[0]) return;
    char path[GHOSTSCRIPT_PATH_MAX];
    if (!state_path(manifest, path, sizeof(path))) return;
    bool display_was_suspended = false;
    if (!ghostscript_manager_sd_begin(&display_was_suspended)) return;
    char *json = NULL;
    if (!read_small_file(path, &json, 1024)) {
        ghostscript_manager_sd_end(display_was_suspended);
        return;
    }
    ghostscript_manager_sd_end(display_was_suspended);
    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) return;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "launch_failure_count");
    if (cJSON_IsNumber(v) && v->valueint > 0) manifest->launch_failure_count = (uint32_t)v->valueint;
    v = cJSON_GetObjectItemCaseSensitive(root, "last_error");
    if (cJSON_IsString(v)) snprintf(manifest->error, sizeof(manifest->error), "%s", v->valuestring);
    cJSON_Delete(root);
}

static void write_state(const ghostscript_manifest_t *manifest, uint32_t failures, const char *error) {
    if (!manifest) return;
    bool display_was_suspended = false;
    if (!ghostscript_manager_sd_begin(&display_was_suspended)) return;
    sd_card_create_directory(GHOSTSCRIPT_DATA_DIR);
    sd_card_create_directory(manifest->data_path);
    char path[GHOSTSCRIPT_PATH_MAX];
    if (!state_path(manifest, path, sizeof(path))) {
        ghostscript_manager_sd_end(display_was_suspended);
        return;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        ghostscript_manager_sd_end(display_was_suspended);
        return;
    }
    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddNumberToObject(root, "launch_failure_count", failures);
        cJSON_AddStringToObject(root, "last_error", error ? error : "");
        char *json = cJSON_PrintUnformatted(root);
        if (json) {
            fprintf(f, "%s\n", json);
            free(json);
        }
        cJSON_Delete(root);
    }
    fclose(f);
    ghostscript_manager_sd_end(display_was_suspended);
}

void ghostscript_manager_record_failure(const ghostscript_manifest_t *manifest, const char *error) {
    uint32_t failures = manifest ? manifest->launch_failure_count + 1 : 1;
    write_state(manifest, failures, error);
    ESP_LOGW(TAG, "Script %s failed: %s", manifest ? manifest->id : "?", error ? error : "unknown");
}

void ghostscript_manager_record_clean_exit(const ghostscript_manifest_t *manifest) {
    write_state(manifest, 0, "");
}
