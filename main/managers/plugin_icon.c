#include "managers/plugin_icon.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "PluginIcon";

const lv_img_dsc_t *plugin_icon_load_rgb565(const char *path, uint16_t width, uint16_t height) {
    if (!path || width == 0 || height == 0) return NULL;
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
