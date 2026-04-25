#pragma once

#include "lvgl.h"
#include <stddef.h>
#include <stdint.h>

/**
 * Decode JPEG bytes (baseline or progressive) to uncompressed RGB565 for LVGL (LV_COLOR_DEPTH 16).
 * On success: frees @p jpeg_data and returns a new lv_img_dsc_t (free dsc and dsc->data in free_tile_ram_slot).
 * On failure: leaves @p jpeg_data intact and returns NULL.
 */
lv_img_dsc_t *offline_map_try_decode_jpeg_to_rgb565_dsc(uint8_t *jpeg_data, size_t len, const char *path_for_log);
