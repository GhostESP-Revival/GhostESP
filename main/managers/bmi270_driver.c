#include "managers/bmi270_driver.h"

#ifdef CONFIG_ACCEL_USE_BMI270

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus_lock.h"
#include "i2c_shared.h"
#include <stdbool.h>
#include <stddef.h>

#include "vendor/m5/BMI270_config.inl"

static const char *TAG = "BMI270";
static i2c_master_dev_handle_t s_dev;
static bool s_initialized;

static esp_err_t bmi270_device(void)
{
    if (s_dev) return ESP_OK;
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t ret = i2c_master_get_bus_handle(CONFIG_ACCEL_I2C_PORT, &bus);
#ifdef CONFIG_IS_ATOMS3R
    if (ret != ESP_OK && CONFIG_ACCEL_I2C_PORT == 0) {
        bool created = false;
        ret = i2c_shared_get_or_create_bus(I2C_NUM_0, GPIO_NUM_45, GPIO_NUM_0,
                                           true, &bus, &created);
        if (ret == ESP_OK && created) {
            ESP_LOGI(TAG, "Created AtomS3R BMI270 I2C bus 0 (SDA=45, SCL=0)");
        }
    }
#endif
    if (ret != ESP_OK) return ret;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_ACCEL_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(bus, &cfg, &s_dev);
}

static esp_err_t bmi270_write(uint8_t reg, const uint8_t *data, size_t len)
{
    esp_err_t ret = bmi270_device();
    if (ret != ESP_OK) return ret;
    if (!i2c_bus_lock(CONFIG_ACCEL_I2C_PORT, 100)) return ESP_ERR_TIMEOUT;
    uint8_t buffer[33];
    if (len > sizeof(buffer) - 1) {
        i2c_bus_unlock(CONFIG_ACCEL_I2C_PORT);
        return ESP_ERR_INVALID_SIZE;
    }
    buffer[0] = reg;
    for (size_t i = 0; i < len; ++i) buffer[i + 1] = data[i];
    ret = i2c_master_transmit(s_dev, buffer, len + 1, 100);
    i2c_bus_unlock(CONFIG_ACCEL_I2C_PORT);
    return ret;
}

static esp_err_t bmi270_read(uint8_t reg, uint8_t *data, size_t len)
{
    esp_err_t ret = bmi270_device();
    if (ret != ESP_OK) return ret;
    if (!i2c_bus_lock(CONFIG_ACCEL_I2C_PORT, 100)) return ESP_ERR_TIMEOUT;
    ret = i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 100);
    i2c_bus_unlock(CONFIG_ACCEL_I2C_PORT);
    return ret;
}

static esp_err_t bmi270_write_reg(uint8_t reg, uint8_t value)
{
    return bmi270_write(reg, &value, 1);
}

static esp_err_t bmi270_aux_write(uint8_t reg, uint8_t value)
{
    esp_err_t ret = bmi270_write_reg(0x4F, value);
    if (ret == ESP_OK) ret = bmi270_write_reg(0x4E, reg);
    for (int i = 0; ret == ESP_OK && i < 3; ++i) {
        uint8_t status = 0;
        ret = bmi270_read(0x03, &status, 1);
        if (ret != ESP_OK || !(status & 0x04)) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ret;
}

esp_err_t bmi270_init(void)
{
    if (s_initialized) return ESP_OK;
    uint8_t chip_id = 0;
    if (bmi270_read(0x00, &chip_id, 1) != ESP_OK || chip_id != 0x24) {
        ESP_LOGE(TAG, "BMI270 not found (ID: 0x%02X)", chip_id);
        return ESP_ERR_NOT_FOUND;
    }

    /* Register sequence copied from M5Unified::BMI270_Class::begin. */
    bmi270_write_reg(0x7E, 0xB6);
    vTaskDelay(pdMS_TO_TICKS(10));
    bmi270_write_reg(0x7C, 0x00);
    vTaskDelay(pdMS_TO_TICKS(1));

    for (size_t offset = 0; offset < sizeof(bmi270_config_file); offset += 32) {
        size_t len = sizeof(bmi270_config_file) - offset;
        if (len > 32) len = 32;
        uint8_t address[2] = {
            (uint8_t)((offset >> 1) & 0x0F),
            (uint8_t)(offset >> 5),
        };
        if (bmi270_write(0x5B, address, 2) != ESP_OK ||
            bmi270_write(0x5E, &bmi270_config_file[offset], len) != ESP_OK) {
            ESP_LOGE(TAG, "BMI270 configuration upload failed at %u", (unsigned)offset);
            return ESP_FAIL;
        }
    }
    if (bmi270_write_reg(0x59, 0x01) != ESP_OK) return ESP_FAIL;
    bmi270_write_reg(0x58, 0xFF);
    /* Keep the scale explicit: 0x00 is the BMI270 +/-2 G range. */
    if (bmi270_write_reg(0x41, 0x00) != ESP_OK) return ESP_FAIL;
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t status = 0;
    bool ready = false;
    for (int i = 0; i < 20; ++i) {
        if (bmi270_read(0x21, &status, 1) == ESP_OK && status == 0x01) {
            ready = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!ready) {
        ESP_LOGE(TAG, "BMI270 configuration did not become ready (0x%02X)", status);
        return ESP_FAIL;
    }

    /* The firmware is loaded but the accelerometer is still suspended. Give it
       an output data rate and switch acc_en on, otherwise the data registers
       (0x0C) read all-zero forever and the gauge/numbers never move. */
    bmi270_write_reg(0x40, 0xA8); /* ACC_CONF: ODR 100Hz, normal BW, perf mode */
    bmi270_write_reg(0x7C, 0x02); /* PWR_CONF: advanced power save off */
    bmi270_write_reg(0x7D, 0x04); /* PWR_CTRL: acc_en */
    vTaskDelay(pdMS_TO_TICKS(5));

    s_initialized = true;
    ESP_LOGI(TAG, "BMI270 initialized with official firmware");
    return ESP_OK;
}

esp_err_t bmi270_read_accel(int16_t *x, int16_t *y, int16_t *z)
{
    uint8_t data[6];
    esp_err_t ret = bmi270_read(0x0C, data, sizeof(data));
    if (ret != ESP_OK) return ret;
    int16_t raw_x = (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
    int16_t raw_y = (int16_t)((uint16_t)data[2] | ((uint16_t)data[3] << 8));
    /* M5Unified's official AtomS3R axis correction: order Y/X/Z, invert Y. */
    *x = raw_y;
    *y = (int16_t)-raw_x;
    *z = (int16_t)((uint16_t)data[4] | ((uint16_t)data[5] << 8));
    return ESP_OK;
}

esp_err_t bmi270_read_mag(int16_t *x, int16_t *y, int16_t *z)
{
    static bool aux_configured;
    if (bmi270_init() != ESP_OK) return ESP_FAIL;
    if (!aux_configured) {
        /* Register sequence copied from M5Unified::BMI270_Class::begin. */
        if (bmi270_write_reg(0x6B, 0x20) != ESP_OK ||
            bmi270_write_reg(0x7C, 0x00) != ESP_OK ||
            bmi270_write_reg(0x7D, 0x0E) != ESP_OK ||
            bmi270_write_reg(0x4C, 0x80) != ESP_OK ||
            bmi270_write_reg(0x4B, (uint8_t)(0x10 << 1)) != ESP_OK ||
            bmi270_aux_write(0x4B, 0x83) != ESP_OK ||
            bmi270_aux_write(0x4C, 0x38) != ESP_OK ||
            bmi270_write_reg(0x4C, 0x4F) != ESP_OK ||
            bmi270_write_reg(0x4D, 0x42) != ESP_OK ||
            bmi270_write_reg(0x7D, 0x0F) != ESP_OK) {
            ESP_LOGE(TAG, "BMI270 BMM150 AUX setup failed");
            return ESP_FAIL;
        }
        aux_configured = true;
    }
    uint8_t data[6];
    esp_err_t ret = bmi270_read(0x04, data, sizeof(data));
    if (ret != ESP_OK) return ret;
    *x = (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
    *y = (int16_t)((uint16_t)data[2] | ((uint16_t)data[3] << 8));
    *z = (int16_t)((uint16_t)data[4] | ((uint16_t)data[5] << 8));
    *x = (int16_t)(*x >> 2);
    *y = (int16_t)(*y >> 2);
    *z = (int16_t)(*z & 0xFFFE);
    /* M5Unified's official AtomS3R magnetometer correction. */
    *x = (int16_t)-*x;
    *z = (int16_t)-*z;
    return ESP_OK;
}

#else
esp_err_t bmi270_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t bmi270_read_accel(int16_t *x, int16_t *y, int16_t *z)
{
    (void)x; (void)y; (void)z;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t bmi270_read_mag(int16_t *x, int16_t *y, int16_t *z)
{
    (void)x; (void)y; (void)z;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
