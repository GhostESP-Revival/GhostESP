/**
 * @file tsc2007.c
 */

#include "tsc2007.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// TSC2007 Address
static uint8_t s_tsc2007_addr = 0x4B;

// I2C Port (assumed 0 based on user context)
#define I2C_PORT_NUM 0

static const char *TAG = "TSC2007";

#include "i2c_bus_lock.h"

static bool tsc2007_i2c_read_cmd(uint8_t func, uint16_t *res) {
    uint8_t cmd = (func << 4) | (1 << 2) | (0 << 1); // Func, ADON_IRQOFF, 12-bit
    uint8_t data[2] = {0};
    
    // Acquire shared lock from io_manager
    if (!i2c_bus_lock(I2C_PORT_NUM, 50)) {
        return false;
    }
    
    i2c_cmd_handle_t link = i2c_cmd_link_create();
    i2c_master_start(link);
    i2c_master_write_byte(link, (s_tsc2007_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(link, cmd, true);
    i2c_master_stop(link);
    
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT_NUM, link, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(link);

    if (ret != ESP_OK) {
        i2c_bus_unlock(I2C_PORT_NUM);
        return false;
    }
    
    link = i2c_cmd_link_create();
    i2c_master_start(link);
    i2c_master_write_byte(link, (s_tsc2007_addr << 1) | I2C_MASTER_READ, true);
    i2c_master_read(link, data, 2, I2C_MASTER_LAST_NACK);
    i2c_master_stop(link);

    ret = i2c_master_cmd_begin(I2C_PORT_NUM, link, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(link);
    
    i2c_bus_unlock(I2C_PORT_NUM);

    if (ret == ESP_OK) {
        *res = ((data[0] << 4) | (data[1] >> 4));
        return true;
    }
    return false;
}

void tsc2007_init(void) {
    uint8_t addresses[] = {0x4B, 0x48};
    bool found = false;
    uint16_t dummy;
    
    for (int i = 0; i < 2; i++) {
        s_tsc2007_addr = addresses[i];
        // Try to read Z1 (func 14) to check presence
        if (tsc2007_i2c_read_cmd(14, &dummy)) {
            ESP_LOGI(TAG, "TSC2007 Init (Found at Addr: 0x%02X)", s_tsc2007_addr);
            found = true;
            break;
        }
    }
    
    if (!found) {
        ESP_LOGE(TAG, "TSC2007 not found at 0x4B or 0x48");
        // Fallback or leave as last attempted
    }
}

#define TOUCH_X_MIN 300
#define TOUCH_X_MAX 3800
#define TOUCH_Y_MIN 300
#define TOUCH_Y_MAX 3800

static int32_t map(int32_t x, int32_t in_min, int32_t in_max, int32_t out_min, int32_t out_max) {
    if (x < in_min) x = in_min;
    if (x > in_max) x = in_max;
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

#define AVG_SAMPLES 4
static int16_t avg_x[AVG_SAMPLES] = {0};
static int16_t avg_y[AVG_SAMPLES] = {0};
static uint8_t avg_idx = 0;

bool tsc2007_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    static int16_t last_x = 0;
    static int16_t last_y = 0;
    uint16_t x = 0, y = 0, z1 = 0;
    
    // MEASURE_X = 12
    // MEASURE_Y = 13
    // MEASURE_Z1 = 14
    
    bool valid_x = tsc2007_i2c_read_cmd(12, &x);
    bool valid_y = tsc2007_i2c_read_cmd(13, &y);
    bool valid_z = tsc2007_i2c_read_cmd(14, &z1);
    
    // Increased threshold slightly to prevent ghost touches
    if (valid_x && valid_y && valid_z && z1 > 200) { 
        // Add to simple moving average buffer
        avg_x[avg_idx] = x;
        avg_y[avg_idx] = y;
        avg_idx = (avg_idx + 1) % AVG_SAMPLES;
        
        // Calculate average
        int32_t sum_x = 0;
        int32_t sum_y = 0;
        for(int i=0; i<AVG_SAMPLES; i++) {
            if(avg_x[i] == 0) { // Fill buffer if empty
                 sum_x += x;
                 sum_y += y;
            } else {
                 sum_x += avg_x[i];
                 sum_y += avg_y[i];
            }
        }
        x = sum_x / AVG_SAMPLES;
        y = sum_y / AVG_SAMPLES;
        
        // Map with calibration margins
        lv_coord_t scaled_x = map(x, TOUCH_X_MIN, TOUCH_X_MAX, 0, LV_HOR_RES);
        lv_coord_t scaled_y = map(y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, LV_VER_RES);

        // Invert Y-axis
        scaled_y = (LV_VER_RES - 1) - scaled_y;

        // Clamp coordinates
        if (scaled_x < 0) scaled_x = 0;
        if (scaled_x >= LV_HOR_RES) scaled_x = LV_HOR_RES - 1;
        if (scaled_y < 0) scaled_y = 0;
        if (scaled_y >= LV_VER_RES) scaled_y = LV_VER_RES - 1;

        data->state = LV_INDEV_STATE_PR;
        data->point.x = scaled_x;
        data->point.y = scaled_y;
        last_x = scaled_x;
        last_y = scaled_y;
    } else {
        // Reset average buffer on release to avoid trailing
        for(int i=0; i<AVG_SAMPLES; i++) {
            avg_x[i] = 0;
            avg_y[i] = 0;
        }
        data->state = LV_INDEV_STATE_REL;
        data->point.x = last_x;
        data->point.y = last_y;
    }
    
    return false; // No buffering
}
