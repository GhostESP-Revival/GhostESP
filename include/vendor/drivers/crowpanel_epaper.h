#ifndef CROWPANEL_EPAPER_H
#define CROWPANEL_EPAPER_H

#include "esp_err.h"
#include "lvgl.h"

#if defined(CONFIG_CROWPANEL_EPAPER_579)
#define CROWPANEL_EPAPER_WIDTH  792
#define CROWPANEL_EPAPER_HEIGHT 272
#define CROWPANEL_EPAPER_STORAGE_WIDTH 800
#else
#define CROWPANEL_EPAPER_WIDTH  400
#define CROWPANEL_EPAPER_HEIGHT 300
#define CROWPANEL_EPAPER_STORAGE_WIDTH CROWPANEL_EPAPER_WIDTH
#endif
#define CROWPANEL_EPAPER_FRAME_BYTES ((CROWPANEL_EPAPER_STORAGE_WIDTH / 8) * CROWPANEL_EPAPER_HEIGHT)

/* Factory button labels/pins. These are kept here so the display/input port
 * has one authoritative board definition. */
#define CROWPANEL_EPAPER_HOME_GPIO 2
#define CROWPANEL_EPAPER_EXIT_GPIO 1
#define CROWPANEL_EPAPER_PREV_GPIO 6
#define CROWPANEL_EPAPER_NEXT_GPIO 4
#define CROWPANEL_EPAPER_OK_GPIO   5

/* CrowPanel SSD1683 e-paper display driver.
 *
 * The command order and framebuffer packing follow Elecrow's factory source:
 * x is the MSB-first byte lane, y is the row, and a 1 bit is white.  LVGL
 * renders into a normal RGB565 framebuffer; the flush callback thresholds it
 * into the SSD1683's 1-bit RAM and performs a full or partial update.
 */
esp_err_t crowpanel_epaper_init(void);
void crowpanel_epaper_flush_cb(lv_disp_drv_t *drv,
                               const lv_area_t *area,
                               lv_color_t *color_p);

#endif /* CROWPANEL_EPAPER_H */
