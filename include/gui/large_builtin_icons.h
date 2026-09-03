#pragma once

#include "lvgl.h"

/* Returns a P4-only, higher-resolution 128x128 alpha copy of a built-in icon.
 * Other displays and non-built-in/large asset-pack images are returned
 * unchanged. SVG artwork is kept as the source of truth and rasterized into
 * this LVGL-native format at build/runtime cache fill. The returned
 * descriptor is cached for the lifetime of the app. */
const lv_img_dsc_t *gui_large_builtin_icon(const lv_img_dsc_t *source);
