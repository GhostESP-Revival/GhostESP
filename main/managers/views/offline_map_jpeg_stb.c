#include "managers/views/offline_map_jpeg_stb.h"

#include "esp_log.h"
#include "lvgl.h"
#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_SPIRAM)
#include "esp_heap_caps.h"
#endif

static void *offline_map_tile_buf_alloc(size_t bytes) {
#if defined(CONFIG_SPIRAM)
  void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) {
    return p;
  }
#endif
  return malloc(bytes);
}

#if LV_USE_PNG && LV_COLOR_DEPTH == 16
#include "lodepng.h"
#endif

/* RISC-V ESP-IDF: TLS in .flash.tdata next to .rodata can trigger
 * "The gap between .flash.tdata and .flash.rodata_noload must not exist". */
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 4096
#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb_image.h"

static const char *TAG = "OfflineMap";

#if LV_COLOR_DEPTH == 16
/** @param rgba from stb or lodepng (freed here). */
static lv_img_dsc_t *offline_map_rgba_to_rgb565_dsc(unsigned char *rgba, unsigned w, unsigned h,
                                                    const char *path_for_log, const char *kind) {
  if (!rgba || w < 1U || h < 1U) {
    return NULL;
  }
  if (w > 2047U || h > 2047U) {
    ESP_LOGW(TAG, "map: %s %s too large for LVGL header (%ux%u)", kind, path_for_log ? path_for_log : "?", w, h);
    free(rgba);
    return NULL;
  }

  const size_t np = (size_t)w * (size_t)h;
  const size_t rgb_bytes = np * sizeof(uint16_t);
  uint16_t *rgb565 = (uint16_t *)offline_map_tile_buf_alloc(rgb_bytes);
  if (!rgb565) {
    free(rgba);
    return NULL;
  }

  for (size_t i = 0; i < np; i++) {
    const unsigned char *p = rgba + i * 4U;
    lv_color_t c = lv_color_make((uint32_t)p[0], (uint32_t)p[1], (uint32_t)p[2]);
    rgb565[i] = c.full;
  }
  free(rgba);

  lv_img_dsc_t *d = (lv_img_dsc_t *)calloc(1, sizeof(lv_img_dsc_t));
  if (!d) {
    free(rgb565);
    return NULL;
  }
  d->header.cf = LV_IMG_CF_TRUE_COLOR;
  d->header.w = (uint32_t)w;
  d->header.h = (uint32_t)h;
  d->data = (const uint8_t *)(void *)rgb565;
  d->data_size = (uint32_t)(np * sizeof(uint16_t));

  ESP_LOGI(TAG, "map: %s -> RGB565 %s %ux%u (%u px)", kind, path_for_log ? path_for_log : "?", w, h, (unsigned)d->data_size);
  return d;
}
#endif

lv_img_dsc_t *offline_map_try_decode_jpeg_to_rgb565_dsc(uint8_t *jpeg_data, size_t len, const char *path_for_log) {
#if LV_COLOR_DEPTH != 16
  (void)jpeg_data;
  (void)len;
  (void)path_for_log;
  return NULL;
#else
  int w = 0;
  int h = 0;
  int comp = 0;
  stbi_uc *rgba = stbi_load_from_memory(jpeg_data, (int)len, &w, &h, &comp, 4);
  if (!rgba || w < 1 || h < 1) {
    if (path_for_log) {
      ESP_LOGW(TAG, "map: stb JPEG decode failed %s (%s)", path_for_log, stbi_failure_reason());
    }
    free(jpeg_data);
    return NULL;
  }
  lv_img_dsc_t *d = offline_map_rgba_to_rgb565_dsc((unsigned char *)rgba, (unsigned)w, (unsigned)h, path_for_log, "JPEG");
  free(jpeg_data);
  return d;
#endif
}

lv_img_dsc_t *offline_map_try_decode_png_to_rgb565_dsc(uint8_t *png_data, size_t len, const char *path_for_log) {
#if !LV_USE_PNG || LV_COLOR_DEPTH != 16
  (void)png_data;
  (void)len;
  (void)path_for_log;
  return NULL;
#else
  unsigned w = 0;
  unsigned h = 0;
  unsigned char *rgba = NULL;
  unsigned err = lodepng_decode32(&rgba, &w, &h, png_data, len);
  if (err) {
    if (rgba) {
      free(rgba);
    }
    if (path_for_log) {
      ESP_LOGW(TAG, "map: lodepng decode failed %s: %s", path_for_log, lodepng_error_text(err));
    }
    /* Caller still owns png_data (fall back to compressed PNG dsc). */
    return NULL;
  }
  free(png_data);
  return offline_map_rgba_to_rgb565_dsc(rgba, w, h, path_for_log, "PNG");
#endif
}
