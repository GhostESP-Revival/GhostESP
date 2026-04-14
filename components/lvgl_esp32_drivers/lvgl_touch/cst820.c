#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include <lvgl.h>
#else
#include <lvgl/lvgl.h>
#endif
#include "cst820.h"
#include "i2c_shared.h"

#define TAG "CST820"
#define I2C_MASTER_TIMEOUT_MS 1000
#define I2C_MASTER_FREQ_HZ 400000

static i2c_master_bus_handle_t s_cst820_bus = NULL;
static i2c_master_dev_handle_t s_cst820_dev = NULL;
static bool s_cst820_bus_owned = false;

esp_err_t cst820_i2c_read(uint8_t reg_addr, uint8_t *data, size_t len) {
    return i2c_master_transmit_receive(s_cst820_dev, &reg_addr, 1, data, len, I2C_MASTER_TIMEOUT_MS);
}

esp_err_t cst820_i2c_write(uint8_t reg_addr, uint8_t data) {
    uint8_t payload[2] = { reg_addr, data };
    return i2c_master_transmit(s_cst820_dev, payload, sizeof(payload), I2C_MASTER_TIMEOUT_MS);
}

void cst820_init(void) {
    ESP_ERROR_CHECK(i2c_shared_get_or_create_bus(0, CYD28_TouchC_SDA, CYD28_TouchC_SCL,
                                                 true, &s_cst820_bus, &s_cst820_bus_owned));
    if (s_cst820_dev == NULL) {
        ESP_ERROR_CHECK(i2c_shared_add_device(s_cst820_bus, I2C_ADDR_CST820, I2C_MASTER_FREQ_HZ, &s_cst820_dev));
    }

    gpio_set_direction(CYD28_TouchC_INT, GPIO_MODE_OUTPUT);
    gpio_set_level(CYD28_TouchC_INT, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(CYD28_TouchC_INT, 0);
    vTaskDelay(pdMS_TO_TICKS(1));

    gpio_set_direction(CYD28_TouchC_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(CYD28_TouchC_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(CYD28_TouchC_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_ERROR_CHECK(cst820_i2c_write(0xFE, 0xFF));
}
static void convert_raw_xy(int16_t raw_x, int16_t raw_y, int16_t *x, int16_t *y) {
#if CONFIG_USE_TDISPLAY_S3
    *x = raw_y;
    *y = 170 - raw_x;
    // rotate 90 degrees
    ESP_LOGI(TAG, "Raw: x=%d, y=%d, Converted: x=%d, y=%d", raw_x, raw_y, *x, *y);
#else
    *x = raw_x;
    *y = raw_y;
#endif
}

bool cst820_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    static int16_t last_x = 0;
    static int16_t last_y = 0;
    uint8_t touch_points = 0;

    if (cst820_i2c_read(0x02, &touch_points, 1) != ESP_OK) {
        data->point.x = last_x;
        data->point.y = last_y;
        data->state = LV_INDEV_STATE_REL;
        return false;
    }

    if (!touch_points) {
        data->point.x = last_x;
        data->point.y = last_y;
        data->state = LV_INDEV_STATE_REL;
        return false;
    }

    uint8_t touch_data[4];
    if (cst820_i2c_read(0x03, touch_data, 4) != ESP_OK) {
        return false;
    }

    int16_t raw_x = ((touch_data[0] & 0x0f) << 8) | touch_data[1];
    int16_t raw_y = ((touch_data[2] & 0x0f) << 8) | touch_data[3];
    
    int16_t x, y;
    convert_raw_xy(raw_x, raw_y, &x, &y);

    last_x = x;
    last_y = y;
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PR;

    ESP_LOGV(TAG, "Touch: x=%d, y=%d", x, y);
    return false;
}
