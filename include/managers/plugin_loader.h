#ifndef PLUGIN_LOADER_H
#define PLUGIN_LOADER_H

#include "managers/plugin_api.h"
#include "managers/plugin_manager.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLUGIN_APP_STATE_EMPTY = 0,
    PLUGIN_APP_STATE_LOADED,
    PLUGIN_APP_STATE_RUNNING,
    PLUGIN_APP_STATE_PAUSED,
    PLUGIN_APP_STATE_FAILED,
} plugin_app_state_t;

typedef struct {
    const plugin_app_manifest_t *manifest;
    const ghostesp_app_t *app;
    void *handle;
    bool running;
    plugin_app_state_t state;
    plugin_permission_t permissions;
    uint32_t last_tick_ms;
    char app_data_path[PLUGIN_APP_PATH_MAX];
} plugin_loaded_app_t;

esp_err_t plugin_loader_load(const char *id, plugin_loaded_app_t **out_app);
esp_err_t plugin_loader_start(plugin_loaded_app_t *loaded);
esp_err_t plugin_loader_pause(plugin_loaded_app_t *loaded);
esp_err_t plugin_loader_resume(plugin_loaded_app_t *loaded);
esp_err_t plugin_loader_stop(plugin_loaded_app_t *loaded);
esp_err_t plugin_loader_tick(plugin_loaded_app_t *loaded, uint32_t elapsed_ms);
esp_err_t plugin_loader_unload(plugin_loaded_app_t *loaded);
plugin_loaded_app_t *plugin_loader_current(void);
const char *plugin_loader_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
