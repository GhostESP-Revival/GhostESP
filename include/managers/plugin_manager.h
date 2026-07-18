#ifndef PLUGIN_MANAGER_H
#define PLUGIN_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLUGIN_APP_MAX_COUNT 32
#define PLUGIN_APP_ID_MAX 32
#define PLUGIN_APP_NAME_MAX 64
#define PLUGIN_APP_VERSION_MAX 24
#define PLUGIN_APP_AUTHOR_MAX 64
#define PLUGIN_APP_TARGET_MAX 16
#define PLUGIN_APP_ENTRY_MAX 64
#define PLUGIN_APP_PATH_MAX 384
#define PLUGIN_APP_DESC_MAX 160
#define PLUGIN_APP_CATEGORY_MAX 32
#define PLUGIN_APP_ICON_MAX 64
#define PLUGIN_APP_STORAGE_SCOPE_MAX 16
#define PLUGIN_APP_ACCENT_COLOR_MAX 8
#define PLUGIN_APP_CHECKSUM_MAX 65

#define PLUGIN_APP_MANIFEST_VERSION 1u
#define PLUGIN_APP_DATA_VERSION_DEFAULT 1u
#define PLUGIN_APP_STORAGE_SCOPE_APP "app"
#define PLUGIN_APP_STORAGE_SCOPE_GHOSTESP "ghostesp"
#define PLUGIN_APP_QUARANTINE_THRESHOLD 3u

typedef uint64_t plugin_permission_t;
typedef uint32_t plugin_feature_t;

#define PLUGIN_PERMISSION_UI           (1ULL << 0)
#define PLUGIN_PERMISSION_STORAGE      (1ULL << 1)
#define PLUGIN_PERMISSION_COMMANDS     (1ULL << 2)
#define PLUGIN_PERMISSION_TASKS        (1ULL << 3)
#define PLUGIN_PERMISSION_WIFI         (1ULL << 4)
#define PLUGIN_PERMISSION_BLE          (1ULL << 5)
#define PLUGIN_PERMISSION_NFC          (1ULL << 6)
#define PLUGIN_PERMISSION_IR           (1ULL << 7)
#define PLUGIN_PERMISSION_SUBGHZ       (1ULL << 8)
#define PLUGIN_PERMISSION_BADUSB       (1ULL << 9)
#define PLUGIN_PERMISSION_RAW_GPIO     (1ULL << 10)
#define PLUGIN_PERMISSION_LVGL         (1ULL << 11)
#define PLUGIN_PERMISSION_RGB          (1ULL << 12)
#define PLUGIN_PERMISSION_UART         (1ULL << 13)
#define PLUGIN_PERMISSION_I2C          (1ULL << 14)
#define PLUGIN_PERMISSION_SPI          (1ULL << 15)
#define PLUGIN_PERMISSION_ADC          (1ULL << 16)
#define PLUGIN_PERMISSION_PWM          (1ULL << 17)
#define PLUGIN_PERMISSION_NETWORK      (1ULL << 18)
#define PLUGIN_PERMISSION_WIFI_CONTROL (1ULL << 19)
#define PLUGIN_PERMISSION_POWER        (1ULL << 20)
#define PLUGIN_PERMISSION_INPUT        (1ULL << 21)
#define PLUGIN_PERMISSION_DISPLAY      (1ULL << 22)
#define PLUGIN_PERMISSION_TIME         (1ULL << 23)
#define PLUGIN_PERMISSION_RANDOM       (1ULL << 24)
#define PLUGIN_PERMISSION_SYSTEM       (1ULL << 25)
#define PLUGIN_PERMISSION_CAMERA       (1ULL << 26)
#define PLUGIN_PERMISSION_USB          (1ULL << 27)
#define PLUGIN_PERMISSION_ETHERNET     (1ULL << 28)
#define PLUGIN_PERMISSION_AUDIO        (1ULL << 29)
#define PLUGIN_PERMISSION_SETTINGS     (1ULL << 30)
#define PLUGIN_PERMISSION_ZIGBEE       (1ULL << 31)
#define PLUGIN_PERMISSION_NRF24        (1ULL << 32)

#define PLUGIN_FEATURE_TOUCHSCREEN     (1U << 0)
#define PLUGIN_FEATURE_DPAD            (1U << 1)
#define PLUGIN_FEATURE_ENCODER         (1U << 2)
#define PLUGIN_FEATURE_KEYBOARD        (1U << 3)

typedef struct {
    char id[PLUGIN_APP_ID_MAX];
    char name[PLUGIN_APP_NAME_MAX];
    char version[PLUGIN_APP_VERSION_MAX];
    char author[PLUGIN_APP_AUTHOR_MAX];
    char target[PLUGIN_APP_TARGET_MAX];
    char entry[PLUGIN_APP_ENTRY_MAX];
    char description[PLUGIN_APP_DESC_MAX];
    char category[PLUGIN_APP_CATEGORY_MAX];
    char icon[PLUGIN_APP_ICON_MAX];
    char icon_format[PLUGIN_APP_CATEGORY_MAX];
    char accent_color[PLUGIN_APP_ACCENT_COLOR_MAX];
    char storage_scope[PLUGIN_APP_STORAGE_SCOPE_MAX];
    char firmware_min[PLUGIN_APP_VERSION_MAX];
    char firmware_max[PLUGIN_APP_VERSION_MAX];
    char checksum[PLUGIN_APP_CHECKSUM_MAX];
    char base_path[PLUGIN_APP_PATH_MAX];
    char entry_path[PLUGIN_APP_PATH_MAX];
    uint32_t api_version;
    uint32_t manifest_version;
    uint32_t package_version;
    uint32_t data_version;
    uint32_t memory_limit;
    uint32_t stack_size;
    plugin_feature_t required_features;
    uint16_t icon_width;
    uint16_t icon_height;
    uint32_t launch_failure_count;
    plugin_permission_t permissions;
    bool requires_psram;
    bool allow_absolute_storage;
    bool quarantined;
    bool valid;
    const lv_img_dsc_t *icon_dsc;
    char error[96];
} plugin_app_manifest_t;

void plugin_manager_init(void);
int plugin_manager_reload(void);
int plugin_manager_count(void);
const plugin_app_manifest_t *plugin_manager_get(int index);
const plugin_app_manifest_t *plugin_manager_find(const char *id);
const lv_img_dsc_t *plugin_manager_get_icon(const plugin_app_manifest_t *app);
bool plugin_manager_target_supported(void);
bool plugin_manager_target_matches(const plugin_app_manifest_t *app);
bool plugin_manager_required_features_supported(const plugin_app_manifest_t *app, char *missing_feature, size_t missing_feature_len);
bool plugin_manager_reset_app_state(const char *id);
const char *plugin_manager_last_error(void);

/* Boot-time progress reporting. Callback is invoked on the calling task at
 * the start of plugin_manager_reload and at completion. Pass NULL to clear. */
typedef void (*plugin_manager_progress_cb_t)(float pct, int files_scanned, int files_total, void *user);
void plugin_manager_set_progress_cb(plugin_manager_progress_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif

#endif
