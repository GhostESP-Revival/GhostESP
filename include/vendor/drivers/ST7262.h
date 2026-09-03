/*
 * SPDX-FileCopyrightText: 2023-2024
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LCD_ST7262_H
#define LCD_ST7262_H

#include "esp_err.h"
#ifdef CONFIG_USE_7_INCHER
#include "esp_lcd_types.h"
#endif
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the ST7262 LCD panel.
 *
 * This function initializes the ST7262 LCD panel connected via the RGB
 * interface.
 *
 * @return
 *      - ESP_OK on success
 *      - Appropriate error code on failure
 */
esp_err_t lcd_st7262_init(void);

/**
 * @brief Deinitialize the ST7262 LCD panel.
 *
 * This function deinitializes the ST7262 LCD panel and releases any allocated
 * resources.
 *
 * @return
 *      - ESP_OK on success
 *      - Appropriate error code on failure
 */
esp_err_t lcd_st7262_deinit(void);

/**
 * @brief Get the LCD panel handle.
 *
 * This function returns the handle to the LCD panel, which can be used for
 * further operations.
 *
 * @return
 *      - esp_lcd_panel_handle_t on success
 *      - NULL on failure
 */
#ifdef CONFIG_USE_7_INCHER
esp_lcd_panel_handle_t lcd_st7262_get_panel_handle(void);
#endif

/**
 * @brief Initialize LVGL display driver for the ST7262 LCD panel.
 *
 * This function sets up the LVGL display driver and registers the flush
 * callback.
 *
 * @return
 *      - ESP_OK on success
 *      - Appropriate error code on failure
 */
esp_err_t lcd_st7262_lvgl_init(void);

#ifdef CONFIG_CROWPANEL_ADVANCE_RGB_LCD
typedef struct {
  uint32_t requested_pclk_hz;
  uint32_t frames;
  int64_t started_us;
  uint32_t frame_min_us, frame_max_us;
  uint32_t long_frames, short_frames, vsync_during_flush;
  uint32_t flushes, flush_errors, flush_max_us;
  uint64_t flush_total_us, pixels;
  uint32_t presented_frames, present_wait_timeouts;
  uint32_t render_max_us;
  uint64_t render_total_us;
  int last_x1, last_y1, last_x2, last_y2;
} crowpanel_rgb_stats_t;

// Runtime diagnostics only; clock changes are not saved to NVS.
esp_err_t lcd_st7262_set_pclk_mhz(uint32_t mhz);
void lcd_st7262_get_rgb_stats(crowpanel_rgb_stats_t *stats);
void lcd_st7262_reset_rgb_stats(void);
// Read-only, asynchronous register snapshot; not a hardware underrun counter.
esp_err_t lcd_st7262_get_hw_stats(uint32_t regs[16]);
// Legacy lifetime bounce counters. Factory-direct mode returns zeroes; retained
// for diagnostics and for non-factory RGB configurations in the adapted driver.
esp_err_t lcd_st7262_get_bounce_stats(uint32_t stats[7]);
#endif

#endif

#ifdef __cplusplus
}
#endif
