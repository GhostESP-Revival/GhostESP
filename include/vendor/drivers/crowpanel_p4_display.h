#ifndef CROWPANEL_ADVANCED_P4_DISPLAY_H
#define CROWPANEL_ADVANCED_P4_DISPLAY_H

#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t crowpanel_p4_display_init(void);
esp_err_t crowpanel_p4_display_touch_reset(void);
esp_err_t crowpanel_p4_display_set_backlight(uint8_t percentage);
esp_err_t crowpanel_p4_display_get_frame_buffers(void **fb0, void **fb1);
void crowpanel_p4_display_mark_dirty_rows(int y1, int y2);
void crowpanel_p4_display_flush_cb(lv_disp_drv_t *drv,
                                   const lv_area_t *area,
                                   lv_color_t *color_p);

#ifdef __cplusplus
}
#endif

#endif
