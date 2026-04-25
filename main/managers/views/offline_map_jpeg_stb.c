#include "managers/views/offline_map_jpeg_stb.h"

#include "esp_log.h"
#include "lvgl.h"
#include <stdlib.h>
#include <string.h>

/* RISC-V ESP-IDF: TLS in .flash.tdata next to .rodata can trigger
 * "The gap between .flash.tdata and .flash.rodata_noload must not exist". */
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 4096
#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb_image.h"

static const char *TAG = "OfflineMap";

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
    return NULL;
  }

  if (w > 2047 || h > 2047) {
    ESP_LOGW(TAG, "map: JPEG %s too large for LVGL header (%dx%d)", path_for_log ? path_for_log : "?", w, h);
    stbi_image_free(rgba);
    return NULL;
  }

  const size_t np = (size_t)w * (size_t)h;
  uint16_t *rgb565 = (uint16_t *)malloc(np * sizeof(uint16_t));
  if (!rgb565) {
    stbi_image_free(rgba);
    return NULL;
  }

  for (size_t i = 0; i < np; i++) {
    const stbi_uc *p = rgba + i * 4U;
    lv_color_t c = lv_color_make(p[0], p[1], p[2]);
    rgb565[i] = c.full;
  }
  stbi_image_free(rgba);
  free(jpeg_data);

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

  ESP_LOGI(TAG, "map: JPEG -> RGB565 %s %dx%d (%u px)", path_for_log ? path_for_log : "?", w, h, (unsigned)d->data_size);
  return d;
#endif
}
