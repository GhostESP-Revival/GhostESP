#include "managers/plugin_icon.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "PluginIcon";

#if CONFIG_IDF_TARGET_ESP32P4
/* Plugin manifests live on removable storage.  Keep a malformed or
 * accidentally full-size image from consuming the P4's PSRAM and starving
 * ESP-Hosted/Wi-Fi.  The launcher renders icons at roughly 50 px, so 128 px
 * leaves room for high-DPI artwork while keeping each icon bounded. */
#define P4_PLUGIN_ICON_MAX_DIMENSION 128u

static bool p4_plugin_icon_size_allowed(uint16_t width, uint16_t height,
                                        size_t bytes_per_pixel)
{
    if (width <= P4_PLUGIN_ICON_MAX_DIMENSION &&
        height <= P4_PLUGIN_ICON_MAX_DIMENSION) {
        return true;
    }

    ESP_LOGW(TAG, "Rejecting oversized P4 plugin icon %ux%u (%u bytes/px)",
             (unsigned)width, (unsigned)height, (unsigned)bytes_per_pixel);
    return false;
}
#endif

const lv_img_dsc_t *plugin_icon_load_rgb565(const char *path, uint16_t width, uint16_t height) {
    if (!path || width == 0 || height == 0) return NULL;
#if CONFIG_IDF_TARGET_ESP32P4
    if (!p4_plugin_icon_size_allowed(width, height, 2u)) return NULL;
#endif
    size_t data_size = (size_t)width * (size_t)height * 2u;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *data = heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) data = malloc(data_size);
    if (!data) { fclose(f); return NULL; }
    size_t n = fread(data, 1, data_size, f);
    fclose(f);
    if (n != data_size) {
        free(data);
        ESP_LOGW(TAG, "Icon size mismatch for %s", path);
        return NULL;
    }
    lv_img_dsc_t *dsc = heap_caps_calloc(1, sizeof(*dsc), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dsc) dsc = calloc(1, sizeof(*dsc));
    if (!dsc) {
        free(data);
        return NULL;
    }
    dsc->header.always_zero = 0;
    dsc->header.w = width;
    dsc->header.h = height;
    dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
    dsc->data_size = data_size;
    dsc->data = data;
    return dsc;
}

const lv_img_dsc_t *plugin_icon_load_rgb565a8(const char *path, uint16_t width, uint16_t height) {
    if (!path || width == 0 || height == 0) return NULL;
#if CONFIG_IDF_TARGET_ESP32P4
    if (!p4_plugin_icon_size_allowed(width, height, 3u)) return NULL;
#endif
    size_t data_size = (size_t)width * (size_t)height * 3u;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *data = heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) data = malloc(data_size);
    if (!data) { fclose(f); return NULL; }
    size_t n = fread(data, 1, data_size, f);
    fclose(f);
    if (n != data_size) {
        free(data);
        ESP_LOGW(TAG, "Icon size mismatch for %s", path);
        return NULL;
    }
    lv_img_dsc_t *dsc = heap_caps_calloc(1, sizeof(*dsc), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dsc) dsc = calloc(1, sizeof(*dsc));
    if (!dsc) {
        free(data);
        return NULL;
    }
    dsc->header.always_zero = 0;
    dsc->header.w = width;
    dsc->header.h = height;
    dsc->header.cf = LV_IMG_CF_RGB565A8;
    dsc->data_size = data_size;
    dsc->data = data;
    return dsc;
}

const lv_img_dsc_t *plugin_icon_load_true_color_alpha(const char *path, uint16_t width, uint16_t height) {
    if (!path || width == 0 || height == 0) return NULL;
#if CONFIG_IDF_TARGET_ESP32P4
    if (!p4_plugin_icon_size_allowed(width, height, 3u)) return NULL;
#endif
    size_t data_size = (size_t)width * (size_t)height * 3u;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *data = heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) data = malloc(data_size);
    if (!data) { fclose(f); return NULL; }
    size_t n = fread(data, 1, data_size, f);
    fclose(f);
    if (n != data_size) {
        free(data);
        ESP_LOGW(TAG, "Icon size mismatch for %s", path);
        return NULL;
    }
    lv_img_dsc_t *dsc = heap_caps_calloc(1, sizeof(*dsc), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dsc) dsc = calloc(1, sizeof(*dsc));
    if (!dsc) {
        free(data);
        return NULL;
    }
    dsc->header.always_zero = 0;
    dsc->header.w = width;
    dsc->header.h = height;
    dsc->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    dsc->data_size = data_size;
    dsc->data = data;
    return dsc;
}

void plugin_icon_free(const lv_img_dsc_t *icon) {
    if (!icon) return;
    free((void *)icon->data);
    free((void *)icon);
}
