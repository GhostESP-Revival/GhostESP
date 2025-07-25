#include "managers/fuel_gauge_manager.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#ifdef CONFIG_USE_BQ27220_FUEL_GAUGE

static const char *TAG = "FuelGaugeManager";

#define BQ27220_I2C_ADDRESS     CONFIG_BQ27220_I2C_ADDRESS
#define BQ27220_REG_VOLTAGE     0x08
#define BQ27220_REG_CURRENT     0x0C
#define BQ27220_REG_SOC         0x2C
#define BQ27220_REG_FLAGS       0x0A
#define BQ27220_REG_CAPACITY    0x0E
#define BQ27220_REG_REMAINING   0x10

#ifdef CONFIG_IDF_TARGET_ESP32S3
#define I2C_MASTER_NUM          I2C_NUM_1
#else
#define I2C_MASTER_NUM          I2C_NUM_0
#endif
#define I2C_MASTER_TIMEOUT_MS   100

static bool is_initialized = false;
static bool i2c_initialized_by_us = false;
static fuel_gauge_data_t last_data = {0};

static uint16_t bq27220_read_word(uint8_t reg) {
    uint8_t data[2] = {0};

    esp_err_t ret = i2c_master_write_read_device(I2C_MASTER_NUM, BQ27220_I2C_ADDRESS,
                                                 &reg, 1, data, 2,
                                                 pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));

    if (ret != ESP_OK) {
        return 0xFFFF;
    }

    return (data[1] << 8) | data[0];
}

static uint8_t bq27220_read_byte(uint8_t reg) {
    uint8_t data = 0xFF;
    esp_err_t ret = i2c_master_write_read_device(I2C_MASTER_NUM, BQ27220_I2C_ADDRESS,
                                                 &reg, 1, &data, 1,
                                                 pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    if (ret != ESP_OK) {
        return 0xFF;
    }
    return data;
}

static esp_err_t fuel_gauge_i2c_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = CONFIG_BQ27220_I2C_SDA_PIN,
        .scl_io_num = CONFIG_BQ27220_I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
        .clk_flags = 0,
    };

    esp_err_t ret = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure I2C parameters: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_driver_install(I2C_MASTER_NUM, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGI(TAG, "I2C driver already installed on port %d", I2C_MASTER_NUM);
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Failed to install I2C driver: %s", esp_err_to_name(ret));
        return ret;
    }

    i2c_initialized_by_us = true;
    ESP_LOGI(TAG, "I2C initialized successfully on port %d", I2C_MASTER_NUM);
    return ESP_OK;
}

bool fuel_gauge_manager_init(void) {
    if (is_initialized) {
        return true;
    }

    ESP_LOGI(TAG, "Initializing BQ27220 fuel gauge");
    ESP_LOGI(TAG, "Configuration: Address=0x%02X, SDA=%d, SCL=%d, I2C_PORT=%d",
             BQ27220_I2C_ADDRESS, CONFIG_BQ27220_I2C_SDA_PIN, CONFIG_BQ27220_I2C_SCL_PIN, I2C_MASTER_NUM);

    esp_err_t ret = fuel_gauge_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C");
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    uint16_t voltage = bq27220_read_word(BQ27220_REG_VOLTAGE);

    if (voltage == 0xFFFF) {
        ESP_LOGE(TAG, "Failed to communicate with BQ27220 - check wiring and I2C config");
        return false;
    }

    if (voltage == 0) {
        ESP_LOGW(TAG, "BQ27220 reports 0V - battery may be disconnected");
    }

    ESP_LOGI(TAG, "BQ27220 detected, voltage: %d mV", voltage);

    memset(&last_data, 0, sizeof(last_data));
    last_data.is_initialized = true;
    is_initialized = true;

    ESP_LOGI(TAG, "BQ27220 fuel gauge initialized successfully");
    return true;
}

bool fuel_gauge_manager_get_data(fuel_gauge_data_t *data) {
    if (!is_initialized || !data) {
        return false;
    }

    uint16_t voltage = bq27220_read_word(BQ27220_REG_VOLTAGE);
    uint16_t current_raw = bq27220_read_word(BQ27220_REG_CURRENT);
    uint8_t soc_byte = bq27220_read_byte(BQ27220_REG_SOC);
    uint16_t flags = bq27220_read_word(BQ27220_REG_FLAGS);

    // Count successful reads
    int successful_reads = 0;
    if (voltage != 0xFFFF) successful_reads++;
    if (current_raw != 0xFFFF) successful_reads++;
    if (soc_byte != 0xFF) successful_reads++;
    if (flags != 0xFFFF) successful_reads++;

    // If less than half reads successful, use cached data
    if (successful_reads < 2) {
        if (last_data.is_initialized) {
            memcpy(data, &last_data, sizeof(fuel_gauge_data_t));
            return true;
        }
        return false;
    }

    // Fill data structure
    data->voltage_mv = (voltage != 0xFFFF) ? voltage : last_data.voltage_mv;
    // BQ27220 current is signed 16-bit in 2's complement, positive = charging
    data->current_ma = (current_raw != 0xFFFF) ? (int16_t)current_raw : last_data.current_ma;
    // StateOfCharge: single-byte integer percent read directly
    data->percentage = (soc_byte != 0xFF && soc_byte <= 100) ? soc_byte : last_data.percentage;

    // BQ27220 charging detection using flags and current measurement
    bool flag_valid = (flags != 0xFFFF);
    bool current_valid = (current_raw != 0xFFFF);
    bool charging = false;
    if (flag_valid || current_valid) {
        bool charging_flag = flag_valid && ((flags & 0x0100) != 0);
        bool charging_current = current_valid && (data->current_ma > 0);
        charging = charging_flag || charging_current;
    } else {
        // Fallback to last known state if no valid data
        charging = last_data.is_charging;
    }
    data->is_charging = charging;
    data->is_initialized = true;

    // Update cache
    memcpy(&last_data, data, sizeof(fuel_gauge_data_t));

    ESP_LOGI(TAG, "Battery: %d%%, %dmV, %dmA, %s",
             data->percentage, data->voltage_mv, data->current_ma,
             data->is_charging ? "charging" : "discharging");

    return true;
}

int fuel_gauge_manager_get_percentage(void) {
    fuel_gauge_data_t data;
    return fuel_gauge_manager_get_data(&data) ? data.percentage : -1;
}

bool fuel_gauge_manager_is_charging(void) {
    fuel_gauge_data_t data;
    return fuel_gauge_manager_get_data(&data) ? data.is_charging : false;
}

uint16_t fuel_gauge_manager_get_voltage_mv(void) {
    fuel_gauge_data_t data;
    return fuel_gauge_manager_get_data(&data) ? data.voltage_mv : 0;
}

void fuel_gauge_manager_deinit(void) {
    if (is_initialized) {
        if (i2c_initialized_by_us) {
            esp_err_t ret = i2c_driver_delete(I2C_MASTER_NUM);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "I2C driver deleted successfully");
            } else {
                ESP_LOGW(TAG, "Failed to delete I2C driver: %s", esp_err_to_name(ret));
            }
            i2c_initialized_by_us = false;
        }
        is_initialized = false;
        memset(&last_data, 0, sizeof(last_data));
        ESP_LOGI(TAG, "Fuel gauge deinitialized");
    }
}

#else

bool fuel_gauge_manager_init(void) { return false; }
bool fuel_gauge_manager_get_data(fuel_gauge_data_t *data) { return false; }
int fuel_gauge_manager_get_percentage(void) { return -1; }
bool fuel_gauge_manager_is_charging(void) { return false; }
uint16_t fuel_gauge_manager_get_voltage_mv(void) { return 0; }
void fuel_gauge_manager_deinit(void) {}

#endif
