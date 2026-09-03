/*
* Copyright © 2021 Sturnus Inc.

* Permission is hereby granted, free of charge, to any person obtaining a copy of this 
* software and associated documentation files (the “Software”), to deal in the Software 
* without restriction, including without limitation the rights to use, copy, modify, merge, 
* publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons 
* to whom the Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all copies or 
* substantial portions of the Software.
* 
* THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, 
* INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR 
* PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE 
* FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, 
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE 
* SOFTWARE.
*/

#include <esp_log.h>
#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include <lvgl.h>
#else
#include <lvgl/lvgl.h>
#endif
#include "gt911.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_i2c/i2c_manager.h"

#define TAG "GT911"

gt911_status_t gt911_status;

static void gt911_hardware_reset(uint8_t dev_addr) {
#if CONFIG_LV_GT911_RESET_PIN >= 0
    const gpio_num_t reset = (gpio_num_t)CONFIG_LV_GT911_RESET_PIN;
    gpio_config_t reset_cfg = {
        .pin_bit_mask = 1ULL << reset,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&reset_cfg));

#if CONFIG_LV_GT911_INT_PIN >= 0
    const gpio_num_t interrupt = (gpio_num_t)CONFIG_LV_GT911_INT_PIN;
    gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << interrupt,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&int_cfg));
#endif

    gpio_set_level(reset, 0);
#if CONFIG_LV_GT911_INT_PIN >= 0
    /* GT911 samples INT during reset: high selects 0x14, low selects 0x5d. */
    gpio_set_level(interrupt, dev_addr == 0x14);
#endif
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(reset, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

#if CONFIG_LV_GT911_INT_PIN >= 0
    int_cfg.mode = GPIO_MODE_INPUT;
    ESP_ERROR_CHECK(gpio_config(&int_cfg));
#endif
    vTaskDelay(pdMS_TO_TICKS(50));
#else
    (void)dev_addr;
#endif
}

//TODO: handle multibyte read and refactor to just one read transaction
esp_err_t gt911_i2c_read(uint8_t slave_addr, uint16_t register_addr, uint8_t *data_buf, uint8_t len) {
    return lvgl_i2c_read(CONFIG_LV_I2C_TOUCH_PORT, slave_addr, register_addr | I2C_REG_16, data_buf, len);
}

esp_err_t gt911_i2c_write8(uint8_t slave_addr, uint16_t register_addr, uint8_t data) {
    uint8_t buffer = data;
    return lvgl_i2c_write(CONFIG_LV_I2C_TOUCH_PORT, slave_addr, register_addr | I2C_REG_16, &buffer, 1);
}

/**
  * @brief  Initialize for GT911 communication via I2C
  * @param  dev_addr: Device address on communication Bus (I2C slave address of GT911).
  * @retval None
  */
void gt911_init(uint8_t dev_addr) {
    if (!gt911_status.inited) {
        gt911_hardware_reset(dev_addr);
        gt911_status.i2c_dev_addr = dev_addr;
        uint8_t data_buf;
        esp_err_t ret;

        ESP_LOGI(TAG, "Checking for GT911 Touch Controller at 0x%02x", dev_addr);
        ret = gt911_i2c_read(dev_addr, GT911_PRODUCT_ID1, &data_buf, 1);
        /* Some GT911 panels power up at 0x14 when INT is not wired. Try the
         * alternate address once so an address-selection strap cannot leave
         * touch completely disabled. */
        if (ret != ESP_OK && dev_addr == 0x5d) {
            const uint8_t alternate_addr = 0x14;
            ESP_LOGW(TAG, "GT911 not responding at 0x5d; probing alternate address 0x14");
            ret = gt911_i2c_read(alternate_addr, GT911_PRODUCT_ID1, &data_buf, 1);
            if (ret == ESP_OK) {
                dev_addr = alternate_addr;
                gt911_status.i2c_dev_addr = dev_addr;
                ESP_LOGI(TAG, "GT911 responded at alternate address 0x14");
            }
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Error reading from device: %s",
                        esp_err_to_name(ret));    // Only show error the first time
            return;
        }

        // Read 4 bytes for Product ID in ASCII
        for (int i = 0; i < GT911_PRODUCT_ID_LEN; i++) {
            gt911_i2c_read(dev_addr, (GT911_PRODUCT_ID1 + i), (uint8_t *)&(gt911_status.product_id[i]), 1);
        }
        ESP_LOGI(TAG, "\tProduct ID: %.*s", GT911_PRODUCT_ID_LEN, gt911_status.product_id);

        gt911_i2c_read(dev_addr, GT911_VENDOR_ID, &data_buf, 1);
        ESP_LOGI(TAG, "\tVendor ID: 0x%02x", data_buf);

        gt911_i2c_read(dev_addr, GT911_X_COORD_RES_L, &data_buf, 1);
        gt911_status.max_x_coord = data_buf;
        gt911_i2c_read(dev_addr, GT911_X_COORD_RES_H, &data_buf, 1);
        gt911_status.max_x_coord |= ((uint16_t)data_buf << 8);
        ESP_LOGI(TAG, "\tX Resolution: %d", gt911_status.max_x_coord);

        gt911_i2c_read(dev_addr, GT911_Y_COORD_RES_L, &data_buf, 1);
        gt911_status.max_y_coord = data_buf;
        gt911_i2c_read(dev_addr, GT911_Y_COORD_RES_H, &data_buf, 1);
        gt911_status.max_y_coord |= ((uint16_t)data_buf << 8);
        ESP_LOGI(TAG, "\tY Resolution: %d", gt911_status.max_y_coord);
        gt911_status.inited = true;
    }
}

/**
  * @brief  Get the touch screen X and Y positions values. Ignores multi touch
  * @param  drv:
  * @param  data: Store data here
  * @retval Always false
  */
bool gt911_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    uint8_t touch_pnt_cnt;
    static int16_t last_x = 0;
    static int16_t last_y = 0;
    static lv_indev_state_t last_state = LV_INDEV_STATE_REL;
    // Jitter filter state (raw space, before scaling). It must be reset when
    // a contact ends; otherwise a new tap inherits the previous tap's anchor.
#ifndef CONFIG_CROWPANEL_ADVANCED_P4
    static int16_t filt_x = 0, filt_y = 0;
    static bool filt_inited = false;
#endif
    uint8_t status_reg;

    esp_err_t err = gt911_i2c_read(gt911_status.i2c_dev_addr, GT911_STATUS_REG,
                                   &status_reg, 1);
    if (err != ESP_OK || !(status_reg & 0x80)) {
        data->point.x = last_x;
        data->point.y = last_y;
        data->state = last_state;
        return false;
    }
    touch_pnt_cnt = status_reg & 0x0F;
    if (touch_pnt_cnt != 1) {
        gt911_i2c_write8(gt911_status.i2c_dev_addr, GT911_STATUS_REG, 0x00);
        data->point.x = last_x;
        data->point.y = last_y;
        last_state = LV_INDEV_STATE_REL;
#ifndef CONFIG_CROWPANEL_ADVANCED_P4
        filt_inited = false;
#endif
        data->state = last_state;
        return false;
    }

    // Read one coherent coordinate sample. Four separate I2C transactions can
    // mix bytes from adjacent GT911 reports and unnecessarily delay releases.
    uint8_t point[4];
    err = gt911_i2c_read(gt911_status.i2c_dev_addr, GT911_PT1_X_COORD_L,
                         point, sizeof(point));
    if (err != ESP_OK) goto retain;
    uint16_t raw_x = point[0] | ((uint16_t)point[1] << 8);
    uint16_t raw_y = point[2] | ((uint16_t)point[3] << 8);

#ifndef CONFIG_CROWPANEL_ADVANCED_P4
    // Keep the legacy jitter filter on smaller panels. The large P4 has
    // enough coordinate precision that filtering is more likely to make a
    // small control feel unresponsive than to improve it.
    if (!filt_inited) { filt_x = raw_x; filt_y = raw_y; filt_inited = true; }
    const int16_t kDead = 3;
    if (abs((int)raw_x - filt_x) > kDead) filt_x = raw_x;
    if (abs((int)raw_y - filt_y) > kDead) filt_y = raw_y;
    raw_x = (uint16_t)filt_x;
    raw_y = (uint16_t)filt_y;
#endif

    // Apply factory transforms in raw space, then scale to display res
#if CONFIG_LV_GT911_INVERT_X
    raw_x = gt911_status.max_x_coord ? (gt911_status.max_x_coord - raw_x) : raw_x;
#endif
#if CONFIG_LV_GT911_INVERT_Y
    raw_y = gt911_status.max_y_coord ? (gt911_status.max_y_coord - raw_y) : raw_y;
#endif
    uint16_t max_x = gt911_status.max_x_coord;
    uint16_t max_y = gt911_status.max_y_coord;
#if CONFIG_LV_GT911_SWAPXY
    { uint16_t t = raw_x; raw_x = raw_y; raw_y = t; }
    { uint16_t t = max_x; max_x = max_y; max_y = t; }
#endif
    // GT911 resolution values describe the coordinate range, while LVGL
    // addresses pixels from 0 through resolution - 1. Clamp before scaling
    // so edge taps cannot produce an out-of-bounds LVGL coordinate.
    if (max_x > 0 && raw_x >= max_x) raw_x = max_x - 1;
    if (max_y > 0 && raw_y >= max_y) raw_y = max_y - 1;
    if (drv && drv->disp && drv->disp->driver) {
        last_x = max_x > 1
                     ? (int32_t)raw_x * (drv->disp->driver->hor_res - 1) /
                           (max_x - 1)
                     : raw_x;
        last_y = max_y > 1
                     ? (int32_t)raw_y * (drv->disp->driver->ver_res - 1) /
                           (max_y - 1)
                     : raw_y;
    } else {
        last_x = raw_x;
        last_y = raw_y;
    }

    gt911_i2c_write8(gt911_status.i2c_dev_addr, GT911_STATUS_REG, 0x00);
    data->point.x = last_x;
    data->point.y = last_y;
    last_state = LV_INDEV_STATE_PR;
    data->state = last_state;
    return false;

retain:
    data->point.x = last_x;
    data->point.y = last_y;
    data->state = last_state;
    return false;
}
