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

#if CONFIG_USE_TDISPLAY_S3
/* T-Display S3: touch is a CST816 on SDA=18/SCL=17, INT=16, RESET=21.
 * The CYD defaults (INT=21, RST=25) are wrong here: GPIO 25 does not exist
 * on the ESP32-S3, and GPIO 21 is the touch RESET line, not the interrupt. */
#define TOUCH_INT_PIN 16
#define TOUCH_RST_PIN 21
#if defined(CONFIG_I2C_MANAGER_0_ENABLED)
#define TOUCH_SDA_PIN CONFIG_I2C_MANAGER_0_SDA
#define TOUCH_SCL_PIN CONFIG_I2C_MANAGER_0_SCL
#else
#define TOUCH_SDA_PIN 18
#define TOUCH_SCL_PIN 17
#endif
#else
#define TOUCH_INT_PIN CYD28_TouchC_INT
#define TOUCH_RST_PIN CYD28_TouchC_RST
#define TOUCH_SDA_PIN CYD28_TouchC_SDA
#define TOUCH_SCL_PIN CYD28_TouchC_SCL
#endif

esp_err_t cst820_i2c_read(uint8_t reg_addr, uint8_t *data, size_t len) {
    if (s_cst820_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_cst820_dev, &reg_addr, 1, data, len, I2C_MASTER_TIMEOUT_MS);
}

esp_err_t cst820_i2c_write(uint8_t reg_addr, uint8_t data) {
    if (s_cst820_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t payload[2] = { reg_addr, data };
    return i2c_master_transmit(s_cst820_dev, payload, sizeof(payload), I2C_MASTER_TIMEOUT_MS);
}

static void cst820_reset_pins(void) {
    /* CST816-style panels hold the IC in reset until RST is toggled low and
     * released high. Without this the IC never ACKs on I2C. */
    if (GPIO_IS_VALID_GPIO(TOUCH_INT_PIN)) {
        gpio_set_direction(TOUCH_INT_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level(TOUCH_INT_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(1));
        gpio_set_level(TOUCH_INT_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (GPIO_IS_VALID_GPIO(TOUCH_RST_PIN)) {
        gpio_set_direction(TOUCH_RST_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level(TOUCH_RST_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(TOUCH_RST_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

void cst820_init(void) {
    ESP_ERROR_CHECK(i2c_shared_get_or_create_bus(0, TOUCH_SDA_PIN, TOUCH_SCL_PIN,
                                                 true, &s_cst820_bus, &s_cst820_bus_owned));

    cst820_reset_pins();

    if (s_cst820_dev == NULL) {
        /* Some panels (e.g. T-Display S3) carry an FT6x36-compatible IC at
         * 0x38 instead of the CST820 at 0x15. Probe both, they share the
         * same register layout. Retry a few times: the IC can take a moment
         * to come up after the reset pulse. */
        const uint16_t probe_addrs[] = { I2C_ADDR_CST820, 0x38 };
        for (int attempt = 0; attempt < 3 && s_cst820_dev == NULL; attempt++) {
            for (size_t i = 0; i < sizeof(probe_addrs) / sizeof(probe_addrs[0]); i++) {
                if (i2c_master_probe(s_cst820_bus, probe_addrs[i], 100) == ESP_OK) {
                    esp_err_t ret = i2c_shared_add_device(s_cst820_bus, probe_addrs[i],
                                                          I2C_MASTER_FREQ_HZ, &s_cst820_dev);
                    if (ret == ESP_OK) {
                        ESP_LOGI(TAG, "Touch IC found at 0x%02X", probe_addrs[i]);
                        break;
                    }
                }
            }
            if (s_cst820_dev == NULL && attempt < 2) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        if (s_cst820_dev == NULL) {
            ESP_LOGE(TAG, "No touch IC detected on I2C bus (probed 0x15, 0x38)");
        }
    }

    if (s_cst820_dev != NULL) {
        esp_err_t err = cst820_i2c_write(0xFE, 0xFF);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Touch IC soft reset write failed: %s", esp_err_to_name(err));
        }
    }
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
