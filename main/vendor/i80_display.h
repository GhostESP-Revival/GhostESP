#ifndef I80_DISPLAY_H
#define I80_DISPLAY_H

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_io_i80.h"
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// TDisplay S3 I80 pins
#define I80_BUS_WR_GPIO     8
#define I80_BUS_DC_GPIO     7
#define I80_BUS_D0_GPIO     39
#define I80_BUS_D1_GPIO     40
#define I80_BUS_D2_GPIO     41
#define I80_BUS_D3_GPIO     42
#define I80_BUS_D4_GPIO     45
#define I80_BUS_D5_GPIO     46
#define I80_BUS_D6_GPIO     47
#define I80_BUS_D7_GPIO     48
#define I80_BUS_RD_GPIO     9
#define I80_BUS_BL_GPIO     38
#define I80_PANEL_RESET_GPIO    5
#define I80_PANEL_CS_GPIO       6

// Display specifications for TDisplay S3
#define I80_LCD_PIXEL_CLOCK_HZ  (20 * 1000 * 1000)  // 20MHz
#define I80_LCD_H_RES           320
#define I80_LCD_V_RES           170
#define I80_LCD_CMD_BITS        8
#define I80_LCD_PARAM_BITS      8

esp_err_t i80_display_init(void);
void i80_display_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p);
esp_err_t i80_display_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // I80_DISPLAY_H
