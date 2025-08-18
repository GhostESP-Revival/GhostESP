#include "i80_display.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

static const char *TAG = "I80_Display";
static esp_lcd_i80_bus_handle_t i80_bus = NULL;
static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;

esp_err_t i80_display_init(void)
{
    esp_err_t ret = ESP_OK;

    // Configure RD pin as output high (not used but may need to be set)
    gpio_config_t rd_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << I80_BUS_RD_GPIO
    };
    gpio_config(&rd_gpio_config);
    gpio_set_level(I80_BUS_RD_GPIO, 1); // Set RD high (inactive)
    
    ESP_LOGI(TAG, "Configuring I80 bus with pins: WR=%d, DC=%d, D0-D7=%d-%d, CS=%d, RESET=%d", 
             I80_BUS_WR_GPIO, I80_BUS_DC_GPIO, I80_BUS_D0_GPIO, I80_BUS_D7_GPIO, 
             I80_PANEL_CS_GPIO, I80_PANEL_RESET_GPIO);

    esp_lcd_i80_bus_config_t bus_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = I80_BUS_DC_GPIO,
        .wr_gpio_num = I80_BUS_WR_GPIO,
        .data_gpio_nums = {
            I80_BUS_D0_GPIO,
            I80_BUS_D1_GPIO,
            I80_BUS_D2_GPIO,
            I80_BUS_D3_GPIO,
            I80_BUS_D4_GPIO,
            I80_BUS_D5_GPIO,
            I80_BUS_D6_GPIO,
            I80_BUS_D7_GPIO,
        },
        .bus_width = 8,
        .max_transfer_bytes = I80_LCD_H_RES * 100 * sizeof(uint16_t),
        .dma_burst_size = 64,
        .psram_trans_align = 64,
        .sram_trans_align = 4,
    };
    ret = esp_lcd_new_i80_bus(&bus_config, &i80_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I80 bus: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "I80 bus created successfully");

    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = I80_PANEL_CS_GPIO,
        .pclk_hz = I80_LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
        .lcd_cmd_bits = I80_LCD_CMD_BITS,
        .lcd_param_bits = I80_LCD_PARAM_BITS,
        .flags = {
            .cs_active_high = 0,
            .reverse_color_bits = 0,
            .swap_color_bytes = 0,
            .pclk_active_neg = 0,
            .pclk_idle_low = 0,
        },
    };
    ret = esp_lcd_new_panel_io_i80(i80_bus, &io_config, &io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create panel IO: %s", esp_err_to_name(ret));
        esp_lcd_del_i80_bus(i80_bus);
        return ret;
    }
    ESP_LOGI(TAG, "Panel IO created successfully");

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = I80_PANEL_RESET_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ST7789 panel: %s", esp_err_to_name(ret));
        esp_lcd_del_i80_bus(i80_bus);
        return ret;
    }
    ESP_LOGI(TAG, "ST7789 panel created successfully");

    ret = esp_lcd_panel_reset(panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset panel: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Panel reset completed");

    ret = esp_lcd_panel_init(panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize panel: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Panel initialization completed");

    ret = esp_lcd_panel_invert_color(panel_handle, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to invert color: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Color inversion set");

    // Configure for TDisplay S3 landscape orientation (correct orientation)
    ret = esp_lcd_panel_mirror(panel_handle, true, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure panel mirroring: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Panel mirroring configured (X-axis mirrored)");

    // Swap X/Y coordinates for 90° clockwise rotation (landscape mode)
    ret = esp_lcd_panel_swap_xy(panel_handle, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set panel rotation: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Panel rotation set for 90° clockwise landscape");

    ret = esp_lcd_panel_disp_on_off(panel_handle, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to turn on display: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Display turned on");

    // Backlight is now controlled by LEDC in display_manager
    // No need to configure GPIO here

    ESP_LOGI(TAG, "I80 display initialized successfully");
    return ESP_OK;
}

void i80_display_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    if (!panel_handle) {
        ESP_LOGE(TAG, "Panel handle is NULL in flush callback!");
        lv_disp_flush_ready(drv);
        return;
    }

    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1 + 35; // TDisplay S3 Y offset
    int offsety2 = area->y2 + 35;

    // Debug first few flush calls
    static int flush_count = 0;
    if (flush_count < 5) {
        ESP_LOGI(TAG, "Flush #%d: area(%d,%d,%d,%d) -> offset(%d,%d,%d,%d)", 
                 flush_count, area->x1, area->y1, area->x2, area->y2,
                 offsetx1, offsety1, offsetx2, offsety2);
        flush_count++;
    }

    esp_err_t ret = esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_p);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to draw bitmap: %s", esp_err_to_name(ret));
    }
    
    lv_disp_flush_ready(drv);
}

esp_err_t i80_display_deinit(void)
{
    esp_err_t ret = ESP_OK;

    if (panel_handle) {
        ret = esp_lcd_panel_del(panel_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to delete panel: %s", esp_err_to_name(ret));
        }
        panel_handle = NULL;
    }

    if (io_handle) {
        ret = esp_lcd_panel_io_del(io_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to delete panel IO: %s", esp_err_to_name(ret));
        }
        io_handle = NULL;
    }

    if (i80_bus) {
        ret = esp_lcd_del_i80_bus(i80_bus);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to delete I80 bus: %s", esp_err_to_name(ret));
        }
        i80_bus = NULL;
    }

    ESP_LOGI(TAG, "I80 display deinitialized");
    return ret;
}
