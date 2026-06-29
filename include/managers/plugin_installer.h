#ifndef PLUGIN_INSTALLER_H
#define PLUGIN_INSTALLER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLUGIN_INSTALLER_ERROR_MAX 160

typedef enum {
    PLUGIN_INSTALLER_KEEP_DATA = 0,
    PLUGIN_INSTALLER_DELETE_DATA,
} plugin_installer_data_mode_t;

esp_err_t plugin_installer_install_package(const char *package_path);
esp_err_t plugin_installer_install_gapp(const char *gapp_path);
esp_err_t plugin_installer_extract_gapp_to_dir(const char *gapp_path, const char *dst_dir);
esp_err_t plugin_installer_uninstall_app(const char *app_id, plugin_installer_data_mode_t data_mode);
const char *plugin_installer_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
