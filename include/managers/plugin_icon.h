#ifndef PLUGIN_ICON_H
#define PLUGIN_ICON_H

#include "lvgl.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const lv_img_dsc_t *plugin_icon_load_rgb565(const char *path, uint16_t width, uint16_t height);
const lv_img_dsc_t *plugin_icon_load_rgb565a8(const char *path, uint16_t width, uint16_t height);
void plugin_icon_free(const lv_img_dsc_t *icon);

#ifdef __cplusplus
}
#endif

#endif
