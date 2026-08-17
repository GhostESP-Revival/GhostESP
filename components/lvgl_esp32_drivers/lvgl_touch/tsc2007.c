/**
 * @file tsc2007.c
 */

#include "tsc2007.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_shared.h"

// TSC2007 Address
static uint8_t s_tsc2007_addr = 0x4B;
static i2c_master_bus_handle_t s_tsc2007_bus = NULL;

// I2C Port (assumed 0 based on user context)
#define I2C_PORT_NUM 0
#define TSC2007_SCL_SPEED_HZ 400000  // TSC2007 supports up to 400kHz
#define TSC2007_TIMEOUT_MS 50

static const char *TAG = "TSC2007";

static bool tsc2007_i2c_read_cmd(uint8_t func, uint16_t *res) {
    uint8_t cmd = (func << 4) | (1 << 2) | (0 << 1); // Func, ADON_IRQOFF, 12-bit
    uint8_t data[2] = {0};

    if (!s_tsc2007_bus) {
        return false;
    }

    // Uses a cached persistent device handle via the shared I2C layer, so no
    // add/remove churn per transaction (avoids "Wrong I2C status" errors).
    esp_err_t ret = i2c_shared_transmit_receive_from_addr(
        s_tsc2007_bus, s_tsc2007_addr, TSC2007_SCL_SPEED_HZ, &cmd, 1, data, 2, TSC2007_TIMEOUT_MS);

    if (ret == ESP_OK) {
        *res = ((data[0] << 4) | (data[1] >> 4));
        return true;
    }
    return false;
}

void tsc2007_init(void) {
    // The bus is normally created earlier by the IO manager; only create it
    // here as a fallback for configs where it does not exist yet.
    if (i2c_master_get_bus_handle(I2C_PORT_NUM, &s_tsc2007_bus) != ESP_OK) {
        esp_err_t err = i2c_shared_get_or_create_bus(I2C_PORT_NUM, 6, 7, true, &s_tsc2007_bus, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to acquire shared I2C bus: %s", esp_err_to_name(err));
            s_tsc2007_bus = NULL;
            return;
        }
    }

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

    // Hold the shared bus lock for the whole X/Y/Z1 sample so the axes are
    // read consistently and cannot interleave with IO manager button polls.
    bool locked = i2c_shared_bus_lock(s_tsc2007_bus, 100);
    bool valid_x = tsc2007_i2c_read_cmd(12, &x);
    bool valid_y = tsc2007_i2c_read_cmd(13, &y);
    bool valid_z = tsc2007_i2c_read_cmd(14, &z1);
    if (locked) {
        i2c_shared_bus_unlock(s_tsc2007_bus);
    }

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
