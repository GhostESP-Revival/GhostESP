#include "managers/plugin_loader.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "managers/ghostchi_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/views/plugin_runner_view.h"
#include "sdkconfig.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if CONFIG_ENABLE_NATIVE_SD_APPS
#include "esp_dlfcn.h"
#endif

static const char *TAG = "PluginLoader";
static plugin_loaded_app_t *s_loaded = NULL;
static char s_last_error[160];

static bool ensure_loaded_slot(void) {
    if (!s_loaded) s_loaded = calloc(1, sizeof(*s_loaded));
    return s_loaded != NULL;
}

static bool plugin_loader_sd_jit_allowed(void) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    return strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0 ||
           strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething2") == 0;
#else
    return false;
#endif
}

static bool plugin_loader_sd_begin(bool *display_was_suspended) {
    if (display_was_suspended) *display_was_suspended = false;
    if (sd_card_manager.is_initialized) return false;
    if (!plugin_loader_sd_jit_allowed()) return false;
    return sd_card_mount_for_flush(display_was_suspended) == ESP_OK;
}

static void plugin_loader_sd_end(bool mounted_here, bool display_was_suspended) {
    if (mounted_here) sd_card_unmount_after_flush(display_was_suspended);
}

static bool state_path_for_manifest(const plugin_app_manifest_t *manifest, char *out, size_t out_len) {
    if (!manifest || !out || out_len == 0) return false;
    int n = snprintf(out, out_len, "/mnt/ghostesp/appdata/%s/.state.json", manifest->id);
    return n > 0 && (size_t)n < out_len;
}

static uint32_t read_state_failure_count(const plugin_app_manifest_t *manifest) {
    char state_path[PLUGIN_APP_PATH_MAX];
    if (!state_path_for_manifest(manifest, state_path, sizeof(state_path))) return 0;
    FILE *f = fopen(state_path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long size = ftell(f);
    if (size <= 0 || size > 1024) { fclose(f); return 0; }
    rewind(f);
    char *buf = calloc(1, (size_t)size + 1);
    if (!buf) { fclose(f); return 0; }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) { free(buf); return 0; }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return 0;
    cJSON *count = cJSON_GetObjectItemCaseSensitive(root, "launch_failure_count");
    uint32_t value = cJSON_IsNumber(count) && count->valueint > 0 ? (uint32_t)count->valueint : 0;
    cJSON_Delete(root);
    return value;
}

static void write_app_state(const plugin_app_manifest_t *manifest, uint32_t failure_count, bool quarantined, bool launch_pending, const char *last_error) {
    char state_path[PLUGIN_APP_PATH_MAX];
    if (!state_path_for_manifest(manifest, state_path, sizeof(state_path))) return;
    sd_card_create_directory("/mnt/ghostesp/appdata");
    char app_dir[PLUGIN_APP_PATH_MAX];
    int dir_n = snprintf(app_dir, sizeof(app_dir), "/mnt/ghostesp/appdata/%s", manifest->id);
    if (dir_n > 0 && (size_t)dir_n < sizeof(app_dir)) sd_card_create_directory(app_dir);
    FILE *f = fopen(state_path, "wb");
    if (!f) return;
    cJSON *root = cJSON_CreateObject();
    if (!root) { fclose(f); return; }
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
}

static void record_app_failure(const plugin_app_manifest_t *manifest, const char *error) {
    if (!manifest) return;
    uint32_t count = read_state_failure_count(manifest) + 1;
    write_app_state(manifest, count, false, false, error);
}

static void record_app_running(const plugin_app_manifest_t *manifest) {
    if (manifest) write_app_state(manifest, read_state_failure_count(manifest), false, true, "");
}

static void record_app_clean_exit(const plugin_app_manifest_t *manifest) {
    if (manifest) write_app_state(manifest, 0, false, false, "");
}

static esp_err_t fail_err(esp_err_t err, const char *message) {
    snprintf(s_last_error, sizeof(s_last_error), "%s", message ? message : "plugin loader error");
    ESP_LOGE(TAG, "%s", s_last_error);
    return err;
}

#define APP_FIELD_END(field) (offsetof(ghostesp_app_t, field) + sizeof(((ghostesp_app_t *)0)->field))

static bool app_has_field(const ghostesp_app_t *app, size_t field_end) {
    return app && app->struct_size >= field_end;
}

static esp_err_t validate_app_descriptor(const plugin_app_manifest_t *manifest, const ghostesp_app_t *app) {
    if (!app) return fail_err(ESP_ERR_INVALID_RESPONSE, "app init returned null descriptor");
    if (app->api_version != GHOSTESP_APP_API_VERSION) {
        return fail_err(ESP_ERR_INVALID_VERSION, "app descriptor API version mismatch");
    }
    if (app->struct_size < APP_FIELD_END(on_tick)) {
        return fail_err(ESP_ERR_INVALID_SIZE, "app descriptor struct too small; rebuild with current SDK");
    }
    if (!app->id || app->id[0] == '\0' || strcmp(app->id, manifest->id) != 0) {
        return fail_err(ESP_ERR_INVALID_RESPONSE, "app descriptor id mismatch");
    }
    if (!app->name || app->name[0] == '\0') {
        return fail_err(ESP_ERR_INVALID_RESPONSE, "app descriptor missing name");
    }
    return ESP_OK;
}

plugin_loaded_app_t *plugin_loader_current(void) {
    return (s_loaded && s_loaded->manifest) ? s_loaded : NULL;
}

esp_err_t plugin_loader_load(const char *id, plugin_loaded_app_t **out_app) {
    int64_t load_start_us = esp_timer_get_time();
    size_t internal_free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (out_app) *out_app = NULL;
    if (!id) return fail_err(ESP_ERR_INVALID_ARG, "missing app id");
    if (!ensure_loaded_slot()) return fail_err(ESP_ERR_NO_MEM, "failed to allocate plugin loader slot");

    if (s_loaded->manifest) {
        plugin_loader_unload(s_loaded);
    }

    const plugin_app_manifest_t *manifest = plugin_manager_find(id);
    if (!manifest) return fail_err(ESP_ERR_NOT_FOUND, "app not found");
    if (!plugin_manager_target_supported()) return fail_err(ESP_ERR_NOT_SUPPORTED, "native SD apps disabled or unsupported target");
    if (!plugin_manager_target_matches(manifest)) return fail_err(ESP_ERR_NOT_SUPPORTED, "app target does not match firmware target");
    if (manifest->memory_limit > 0 && heap_caps_get_free_size(MALLOC_CAP_8BIT) < manifest->memory_limit) {
        return fail_err(ESP_ERR_NO_MEM, "not enough free heap for app memory limit");
    }
    if (manifest->requires_psram && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) == 0) {
        return fail_err(ESP_ERR_NOT_SUPPORTED, "app requires PSRAM");
    }

    bool display_was_suspended = false;
    bool mounted_here = plugin_loader_sd_begin(&display_was_suspended);
    if (!sd_card_manager.is_initialized) {
        return fail_err(ESP_ERR_INVALID_STATE, "storage is not mounted");
    }

#if CONFIG_ENABLE_NATIVE_SD_APPS
    struct stat entry_st;
    if (stat(manifest->entry_path, &entry_st) == 0) {
        size_t largest_8bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        size_t largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_LOGI(TAG, "Loading %s size=%ld free8=%u largest8=%u freepsram=%u largestpsram=%u",
                 manifest->id,
                 (long)entry_st.st_size,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)largest_8bit,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)largest_psram);
        if (entry_st.st_size > 0 && largest_8bit < (size_t)entry_st.st_size && largest_psram < (size_t)entry_st.st_size) {
            ESP_LOGW(TAG, "Largest heap block is smaller than ELF file; dlopen may fail");
        }
    }
    void *handle = dlopen(manifest->entry_path, RTLD_NOW);
    if (!handle) {
        const char *err = dlerror();
        snprintf(s_last_error, sizeof(s_last_error), "dlopen failed: %s", err ? err : "unknown");
        record_app_failure(manifest, s_last_error);
        ESP_LOGE(TAG, "%s", s_last_error);
        plugin_loader_sd_end(mounted_here, display_was_suspended);
        return ESP_FAIL;
    }

    ghostesp_app_init_fn init_fn = (ghostesp_app_init_fn)dlsym(handle, "ghostesp_app_init");
    if (!init_fn) {
        const char *err = dlerror();
        snprintf(s_last_error, sizeof(s_last_error), "dlsym failed: %s", err ? err : "ghostesp_app_init missing");
        dlclose(handle);
        record_app_failure(manifest, s_last_error);
        ESP_LOGE(TAG, "%s", s_last_error);
        plugin_loader_sd_end(mounted_here, display_was_suspended);
        return ESP_FAIL;
    }

    const ghostesp_api_t *api = plugin_api_get(manifest->id,
                                               manifest->permissions,
                                               manifest->memory_limit,
                                               manifest->allow_absolute_storage);
    if (!api) {
        dlclose(handle);
        record_app_failure(manifest, "plugin API unavailable");
        plugin_loader_sd_end(mounted_here, display_was_suspended);
        return fail_err(ESP_ERR_INVALID_STATE, "plugin API unavailable");
    }

    const ghostesp_app_t *app = init_fn(api);
    esp_err_t validate_err = validate_app_descriptor(manifest, app);
    if (validate_err != ESP_OK) {
        dlclose(handle);
        record_app_failure(manifest, s_last_error);
        plugin_loader_sd_end(mounted_here, display_was_suspended);
        return validate_err;
    }

    memset(s_loaded, 0, sizeof(*s_loaded));
    s_loaded->manifest = manifest;
    s_loaded->app = app;
    s_loaded->handle = handle;
    s_loaded->state = PLUGIN_APP_STATE_LOADED;
    s_loaded->permissions = manifest->permissions;
    snprintf(s_loaded->app_data_path, sizeof(s_loaded->app_data_path), "/mnt/ghostesp/appdata/%s", manifest->id);
    if (out_app) *out_app = s_loaded;
    s_last_error[0] = '\0';
    ESP_LOGI(TAG, "Loaded SD app %s in %lld ms free8=%u largest8=%u freepsram=%u largestpsram=%u",
             manifest->id,
             (long long)((esp_timer_get_time() - load_start_us) / 1000),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    size_t internal_free_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "App %s internal SRAM: before=%u after=%u consumed=%d bytes "
             "(flash-XIP keeps .text out of internal RAM; expect a small delta)",
             manifest->id,
             (unsigned)internal_free_before,
             (unsigned)internal_free_after,
             (int)((long)internal_free_before - (long)internal_free_after));
    plugin_loader_sd_end(mounted_here, display_was_suspended);
    return ESP_OK;
#else
    plugin_loader_sd_end(mounted_here, display_was_suspended);
    return fail_err(ESP_ERR_NOT_SUPPORTED, "native SD apps are not compiled in");
#endif
}

esp_err_t plugin_loader_start(plugin_loaded_app_t *loaded) {
    if (!loaded || !loaded->app) return ESP_ERR_INVALID_ARG;
    bool display_was_suspended = false;
    bool mounted_here = plugin_loader_sd_begin(&display_was_suspended);
    record_app_running(loaded->manifest);
    if (!loaded->running && loaded->app->on_start) loaded->app->on_start();
    plugin_loader_sd_end(mounted_here, display_was_suspended);
    loaded->running = true;
    loaded->state = PLUGIN_APP_STATE_RUNNING;
    loaded->last_tick_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    ghostchi_manager_add_xp(5);
    return ESP_OK;
}

esp_err_t plugin_loader_pause(plugin_loaded_app_t *loaded) {
    if (!loaded || !loaded->app) return ESP_ERR_INVALID_ARG;
    if (loaded->state == PLUGIN_APP_STATE_RUNNING && app_has_field(loaded->app, APP_FIELD_END(on_pause)) && loaded->app->on_pause) {
        loaded->app->on_pause();
    }
    if (loaded->running) loaded->state = PLUGIN_APP_STATE_PAUSED;
    return ESP_OK;
}

esp_err_t plugin_loader_resume(plugin_loaded_app_t *loaded) {
    if (!loaded || !loaded->app) return ESP_ERR_INVALID_ARG;
    if (loaded->state == PLUGIN_APP_STATE_PAUSED && app_has_field(loaded->app, APP_FIELD_END(on_resume)) && loaded->app->on_resume) {
        loaded->app->on_resume();
    }
    if (loaded->running) {
        loaded->state = PLUGIN_APP_STATE_RUNNING;
        loaded->last_tick_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    }
    return ESP_OK;
}

esp_err_t plugin_loader_stop(plugin_loaded_app_t *loaded) {
    if (!loaded || !loaded->app) return ESP_ERR_INVALID_ARG;
    bool display_was_suspended = false;
    bool mounted_here = plugin_loader_sd_begin(&display_was_suspended);
    if (loaded->running && loaded->app->on_stop) loaded->app->on_stop();
    loaded->running = false;
    loaded->state = PLUGIN_APP_STATE_LOADED;
    record_app_clean_exit(loaded->manifest);
    plugin_loader_sd_end(mounted_here, display_was_suspended);
    return ESP_OK;
}

esp_err_t plugin_loader_tick(plugin_loaded_app_t *loaded, uint32_t elapsed_ms) {
    if (!loaded || !loaded->app) return ESP_ERR_INVALID_ARG;
    if (loaded->state != PLUGIN_APP_STATE_RUNNING || !loaded->running) return ESP_OK;
    if (loaded->app->on_tick) loaded->app->on_tick(elapsed_ms);
    return ESP_OK;
}

esp_err_t plugin_loader_unload(plugin_loaded_app_t *loaded) {
    if (!loaded || !loaded->manifest) return ESP_OK;
    plugin_runner_stop_tick();
    plugin_loader_stop(loaded);
#if CONFIG_ENABLE_NATIVE_SD_APPS
    if (loaded->handle) dlclose(loaded->handle);
#endif
    plugin_api_release();
    memset(loaded, 0, sizeof(*loaded));
    return ESP_OK;
}

const char *plugin_loader_last_error(void) {
    return s_last_error;
}
