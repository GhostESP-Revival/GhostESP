#include "vendor/drivers/crowpanel_p4_display.h"

#ifdef CONFIG_CROWPANEL_ADVANCED_P4

#include "driver/gpio.h"
#include "esp_idf_version.h"
#include "esp_lcd_panel_rgb.h"
#if !defined(CONFIG_CROWPANEL_P4_PANEL_RGB_800X480)
#include "esp_lcd_ek79007.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#endif
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl_i2c/i2c_manager.h"

#define CROWPANEL_P4_BITS_PER_PIXEL 16

#if defined(CONFIG_CROWPANEL_P4_PANEL_RGB_800X480)
#define CROWPANEL_P4_H_RES 800
#define CROWPANEL_P4_V_RES 480
#define CROWPANEL_P4_TOUCH_RESET_GPIO 36
#else
#define CROWPANEL_P4_H_RES 1024
#define CROWPANEL_P4_V_RES 600
#define CROWPANEL_P4_TOUCH_RESET_GPIO 40
#endif

#define CROWPANEL_P4_TOUCH_INT_GPIO 42
#define CROWPANEL_P4_I2C_PORT CONFIG_LV_I2C_TOUCH_PORT
#define CROWPANEL_P4_STC8_ADDRESS 0x2F
#define CROWPANEL_P4_STC8_SET_GPIO_REGISTER 0x1B
#define CROWPANEL_P4_STC8_SET_PWM_REGISTER 0x20

static const char *TAG = "crowpanel_p4_display";
#if !defined(CONFIG_CROWPANEL_P4_PANEL_RGB_800X480)
static esp_lcd_dsi_bus_handle_t s_dsi_bus;
static esp_lcd_panel_io_handle_t s_dbi_io;
static esp_ldo_channel_handle_t s_ldo3;
#endif
static esp_lcd_panel_handle_t s_panel;
#if !defined(CONFIG_CROWPANEL_P4_PANEL_RGB_800X480)
static SemaphoreHandle_t s_refresh_done;
static bool s_dirty_rows_pending;
static int s_dirty_y1;
static int s_dirty_y2;

static IRAM_ATTR bool crowpanel_p4_refresh_done_cb(esp_lcd_panel_handle_t panel,
                                                   esp_lcd_dpi_panel_event_data_t *event_data,
                                                   void *user_ctx)
{
    (void)panel;
    (void)event_data;
    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx, &task_woken);
    return task_woken == pdTRUE;
}
#endif


esp_err_t crowpanel_p4_display_init(void)
{
#if defined(CONFIG_CROWPANEL_P4_PANEL_RGB_800X480)
    const esp_lcd_rgb_panel_config_t panel_config = {
        .data_width = 16,
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
        .psram_trans_align = 64,
#else
        .dma_burst_size = 64,
#endif
        .num_fbs = 2,
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_gpio_nums = {
            8, 7, 6, 5, 4, 14, 13, 12,
            11, 10, 9, 19, 18, 17, 16, 15,
        },
        .pclk_gpio_num = 3,
        .vsync_gpio_num = 41,
        .hsync_gpio_num = 40,
        .de_gpio_num = 2,
        .disp_gpio_num = -1,
        .timings = {
            .pclk_hz = 18 * 1000 * 1000,
            .h_res = CROWPANEL_P4_H_RES,
            .v_res = CROWPANEL_P4_V_RES,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .hsync_pulse_width = 4,
            .vsync_back_porch = 16,
            .vsync_front_porch = 16,
            .vsync_pulse_width = 4,
            .flags = {
                .pclk_active_neg = true,
                .pclk_idle_high = true,
            },
        },
        .flags.fb_in_psram = true,
    };

    esp_err_t err = esp_lcd_new_rgb_panel(&panel_config, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create 5-inch RGB panel: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_reset(s_panel);
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(s_panel);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize 5-inch RGB panel: %s", esp_err_to_name(err));
        goto fail;
    }

    ESP_LOGI(TAG, "CrowPanel Advanced P4 5-inch display initialized (%dx%d)",
             CROWPANEL_P4_H_RES, CROWPANEL_P4_V_RES);
    return ESP_OK;
#else
    const esp_ldo_channel_config_t ldo3_config = {
        .chan_id = 3,
        .voltage_mv = 2500,
    };
    esp_err_t err = esp_ldo_acquire_channel(&ldo3_config, &s_ldo3);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to enable MIPI PHY LDO3: %s", esp_err_to_name(err));
        return err;
    }

    /* LDO4 (3.3V) is owned by the SD card manager (sd_pwr_ctrl_by_on_chip_ldo).
     * The display only needs LDO3 for the MIPI DSI PHY; matching the Elecrow
     * factory BSP which does NOT acquire LDO4 for the display. */

    gpio_config_t backlight_power_config = {
        .pin_bit_mask = 1ULL << 29,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&backlight_power_config);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_set_level(29, 1);
    if (err != ESP_OK) {
        return err;
    }

    const esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = 900,
    };
    err = esp_lcd_new_dsi_bus(&bus_config, &s_dsi_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create DSI bus: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_config, &s_dbi_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create DSI DBI I/O: %s", esp_err_to_name(err));
        goto fail;
    }

    const esp_lcd_dpi_panel_config_t dpi_config = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 51,
        .virtual_channel = 0,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 2,
        .video_timing = {
            .h_size = CROWPANEL_P4_H_RES,
            .v_size = CROWPANEL_P4_V_RES,
            .hsync_back_porch = 160,
            .hsync_pulse_width = 70,
            .hsync_front_porch = 160,
            .vsync_back_porch = 23,
            .vsync_pulse_width = 10,
            .vsync_front_porch = 12,
        },
    };

    ek79007_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = s_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = CROWPANEL_P4_BITS_PER_PIXEL,
        .vendor_config = &vendor_config,
    };

    err = esp_lcd_new_panel_ek79007(s_dbi_io, &panel_config, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create EK79007 panel: %s", esp_err_to_name(err));
        goto fail;
    }
    err = esp_lcd_panel_reset(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to reset EK79007 panel: %s", esp_err_to_name(err));
        goto fail;
    }
    err = esp_lcd_panel_init(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize EK79007 panel: %s", esp_err_to_name(err));
        goto fail;
    }

    s_refresh_done = xSemaphoreCreateBinary();
    if (!s_refresh_done) {
        err = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "failed to create display VSYNC semaphore");
        goto fail;
    }
    const esp_lcd_dpi_panel_event_callbacks_t dpi_callbacks = {
        .on_refresh_done = crowpanel_p4_refresh_done_cb,
    };
    err = esp_lcd_dpi_panel_register_event_callbacks(s_panel, &dpi_callbacks, s_refresh_done);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to register display VSYNC callback: %s", esp_err_to_name(err));
        goto fail;
    }

    ESP_LOGI(TAG, "CrowPanel Advanced P4 MIPI display initialized (%dx%d)",
             CROWPANEL_P4_H_RES, CROWPANEL_P4_V_RES);
    return ESP_OK;
#endif

fail:
#if !defined(CONFIG_CROWPANEL_P4_PANEL_RGB_800X480)
    if (s_refresh_done) {
        vSemaphoreDelete(s_refresh_done);
        s_refresh_done = NULL;
    }
#endif
    if (s_panel) {
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }
#if !defined(CONFIG_CROWPANEL_P4_PANEL_RGB_800X480)
    if (s_dbi_io) {
        esp_lcd_panel_io_del(s_dbi_io);
        s_dbi_io = NULL;
    }
    if (s_dsi_bus) {
        esp_lcd_del_dsi_bus(s_dsi_bus);
        s_dsi_bus = NULL;
    }
#endif
    return err;
}

esp_err_t crowpanel_p4_display_touch_reset(void)
{
    const gpio_config_t output_config = {
        .pin_bit_mask = 1ULL << CROWPANEL_P4_TOUCH_RESET_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&output_config);
    if (err != ESP_OK) {
        return err;
    }

    const gpio_config_t int_config = {
        .pin_bit_mask = 1ULL << CROWPANEL_P4_TOUCH_INT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&int_config);
    if (err != ESP_OK) {
        return err;
    }

    // Keep INT low during reset so GT911 selects its 0x5D address.
    err = gpio_set_level(CROWPANEL_P4_TOUCH_INT_GPIO, 0);
    if (err == ESP_OK) {
        err = gpio_set_level(CROWPANEL_P4_TOUCH_RESET_GPIO, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    if (err == ESP_OK) {
        err = gpio_set_level(CROWPANEL_P4_TOUCH_RESET_GPIO, 1);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    if (err == ESP_OK) {
        err = gpio_set_direction(CROWPANEL_P4_TOUCH_INT_GPIO, GPIO_MODE_INPUT);
    }
    return err;
}

esp_err_t crowpanel_p4_display_set_backlight(uint8_t percentage)
{
#if defined(CONFIG_CROWPANEL_P4_PANEL_RGB_800X480)
    uint8_t power = percentage > 0 ? 1 : 0;
    esp_err_t err = lvgl_i2c_write(CROWPANEL_P4_I2C_PORT,
                                   CROWPANEL_P4_STC8_ADDRESS,
                                   CROWPANEL_P4_STC8_SET_GPIO_REGISTER,
                                   &power, 1);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t duty = percentage > 100 ? 100 : percentage;
    return lvgl_i2c_write(CROWPANEL_P4_I2C_PORT,
                          CROWPANEL_P4_STC8_ADDRESS,
                          CROWPANEL_P4_STC8_SET_PWM_REGISTER,
                          &duty, 1);
#else
    (void)percentage;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t crowpanel_p4_display_get_frame_buffers(void **fb0, void **fb1)
{
    if (!s_panel || !fb0 || !fb1) {
        return ESP_ERR_INVALID_ARG;
    }
#if defined(CONFIG_CROWPANEL_P4_PANEL_RGB_800X480)
    return esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, fb0, fb1);
#else
    return esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, fb0, fb1);
#endif
}

void crowpanel_p4_display_mark_dirty_rows(int y1, int y2)
{
#if !defined(CONFIG_CROWPANEL_P4_PANEL_RGB_800X480)
    if (y1 < 0) y1 = 0;
    if (y2 >= CROWPANEL_P4_V_RES) y2 = CROWPANEL_P4_V_RES - 1;
    if (y1 > y2) return;

    if (!s_dirty_rows_pending) {
        s_dirty_y1 = y1;
        s_dirty_y2 = y2;
        s_dirty_rows_pending = true;
    } else {
        if (y1 < s_dirty_y1) s_dirty_y1 = y1;
        if (y2 > s_dirty_y2) s_dirty_y2 = y2;
    }
#else
    (void)y1;
    (void)y2;
#endif
}

void crowpanel_p4_display_flush_cb(lv_disp_drv_t *drv,
                                   const lv_area_t *area,
                                   lv_color_t *color_p)
{
    crowpanel_p4_display_mark_dirty_rows(area->y1, area->y2);

    if (!lv_disp_flush_is_last(drv)) {
        lv_disp_flush_ready(drv);
        return;
    }

    // color_p is a DPI-owned full framebuffer in direct mode. Present it once
    // per LVGL refresh, syncing only rows touched by this refresh, then wait
    // for VSYNC before LVGL recycles either buffer.
    int draw_y1 = s_dirty_rows_pending ? s_dirty_y1 : 0;
    int draw_y2 = s_dirty_rows_pending ? s_dirty_y2 + 1 : CROWPANEL_P4_V_RES;
    s_dirty_rows_pending = false;
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, draw_y1,
                                               CROWPANEL_P4_H_RES,
                                               draw_y2,
                                               color_p);
    if (err == ESP_OK && s_refresh_done) {
        xSemaphoreTake(s_refresh_done, 0);
        if (xSemaphoreTake(s_refresh_done, pdMS_TO_TICKS(50)) != pdTRUE) {
            ESP_LOGW(TAG, "display VSYNC timeout");
        }
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "display flush failed: %s", esp_err_to_name(err));
    }
    lv_disp_flush_ready(drv);
}

#endif
