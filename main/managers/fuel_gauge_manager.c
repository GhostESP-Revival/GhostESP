#include "managers/fuel_gauge_manager.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "esp_pm.h"
#include "io_manager/i2c_bus_lock.h"

#ifdef CONFIG_USE_BQ27220_FUEL_GAUGE

static const char *TAG = "FuelGaugeManager";

#define BQ27220_I2C_ADDRESS     CONFIG_BQ27220_I2C_ADDRESS
#define BQ27220_REG_VOLTAGE     0x08
#define BQ27220_REG_CURRENT     0x0C
#define BQ27220_REG_SOC         0x2C
#define BQ27220_REG_FLAGS       0x0A
#define BQ27220_REG_CAPACITY    0x0E
#define BQ27220_REG_REMAINING   0x10

// BQ27220 Control Registers and Commands
#define BQ27220_REG_CONTROL     0x00
#define BQ27220_REG_TEMPERATURE 0x06
#define BQ27220_REG_FULL_CHARGE_CAPACITY 0x12
#define BQ27220_REG_DESIGN_CAPACITY 0x3C

// BQ27220 Control Commands
#define BQ27220_CONTROL_STATUS  0x0000
#define BQ27220_DEVICE_TYPE     0x0001
#define BQ27220_FW_VERSION      0x0002
#define BQ27220_RESET           0x0041
#define BQ27220_SEAL            0x0020
#define BQ27220_UNSEAL          0x8000
#define BQ27220_IT_ENABLE       0x0021
#define BQ27220_ENTER_CFG_UPDATE 0x0013
#define BQ27220_EXIT_CFG_UPDATE 0x0042

// BQ27220 Configuration Registers
#define BQ27220_REG_DF_VERSION  0x3F
#define BQ27220_REG_BLOCK_DATA  0x40
#define BQ27220_REG_BLOCK_DATA_CHECKSUM 0x60
#define BQ27220_REG_BLOCK_DATA_CONTROL 0x61
#define BQ27220_REG_BLOCK_DATA_CLASS 0x3E
#define BQ27220_REG_DATA_BLOCK  0x3F

#ifdef CONFIG_IDF_TARGET_ESP32S3
#define I2C_MASTER_NUM          I2C_NUM_0
#else
#define I2C_MASTER_NUM          I2C_NUM_0
#endif
#define I2C_MASTER_TIMEOUT_MS   100

static bool is_initialized = false;
static bool i2c_initialized_by_us = false;
static fuel_gauge_data_t last_data = {0};
static volatile bool s_paused = false;

#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t fg_i2c_pm_lock = NULL;
#endif

static uint16_t bq27220_read_word(uint8_t reg) {
    uint8_t data[2] = {0};
#if CONFIG_PM_ENABLE
    if (fg_i2c_pm_lock) esp_pm_lock_acquire(fg_i2c_pm_lock);
#endif
    bool locked = i2c_bus_lock(I2C_MASTER_NUM, I2C_MASTER_TIMEOUT_MS);
    esp_err_t ret = i2c_master_write_read_device(I2C_MASTER_NUM, BQ27220_I2C_ADDRESS,
                                                 &reg, 1, data, 2,
                                                 pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    if (locked) i2c_bus_unlock(I2C_MASTER_NUM);
#if CONFIG_PM_ENABLE
    if (fg_i2c_pm_lock) esp_pm_lock_release(fg_i2c_pm_lock);
#endif

    if (ret != ESP_OK) {
        return 0xFFFF;
    }

    return (data[1] << 8) | data[0];
}

static esp_err_t bq27220_write_word(uint8_t reg, uint16_t data) {
    uint8_t write_data[3];
    write_data[0] = reg;
    write_data[1] = data & 0xFF;
    write_data[2] = (data >> 8) & 0xFF;
#if CONFIG_PM_ENABLE
    if (fg_i2c_pm_lock) esp_pm_lock_acquire(fg_i2c_pm_lock);
#endif
    bool locked = i2c_bus_lock(I2C_MASTER_NUM, I2C_MASTER_TIMEOUT_MS);
    esp_err_t ret = i2c_master_write_to_device(I2C_MASTER_NUM, BQ27220_I2C_ADDRESS,
                                      write_data, 3,
                                      pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    if (locked) i2c_bus_unlock(I2C_MASTER_NUM);
#if CONFIG_PM_ENABLE
    if (fg_i2c_pm_lock) esp_pm_lock_release(fg_i2c_pm_lock);
#endif
    return ret;
}

static esp_err_t bq27220_control_command(uint16_t command) {
    return bq27220_write_word(BQ27220_REG_CONTROL, command);
}

static uint16_t bq27220_get_control_status(void) {
    bq27220_control_command(BQ27220_CONTROL_STATUS);
    vTaskDelay(pdMS_TO_TICKS(5));
    return bq27220_read_word(BQ27220_REG_CONTROL);
}

static bool bq27220_is_sealed(void) {
    uint16_t status = bq27220_get_control_status();
    return (status & 0x2000) != 0;
}

static esp_err_t bq27220_seal_device(void) {
    if (bq27220_is_sealed()) {
        return ESP_OK;
    }

    esp_err_t ret = bq27220_control_command(BQ27220_SEAL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send seal command: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    if (!bq27220_is_sealed()) {
        ESP_LOGW(TAG, "Fuel gauge did not enter sealed state");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t bq27220_unseal_device(void) {
    if (!bq27220_is_sealed()) {
        return ESP_OK;
    }
    
    // Send unseal command twice
    esp_err_t ret = bq27220_control_command(BQ27220_UNSEAL);
    if (ret != ESP_OK) return ret;
    
    vTaskDelay(pdMS_TO_TICKS(5));
    
    ret = bq27220_control_command(BQ27220_UNSEAL);
    if (ret != ESP_OK) return ret;
    
    vTaskDelay(pdMS_TO_TICKS(10));
    
    return ESP_OK;
}

static esp_err_t bq27220_enter_config_update(void) {
    esp_err_t ret = bq27220_unseal_device();
    if (ret != ESP_OK) return ret;
    
    ret = bq27220_control_command(BQ27220_ENTER_CFG_UPDATE);
    if (ret != ESP_OK) return ret;
    
    // Wait for CFGUPDATE mode
    vTaskDelay(pdMS_TO_TICKS(100));
    
    uint16_t flags = bq27220_read_word(BQ27220_REG_FLAGS);
    if ((flags & 0x0020) == 0) {
        ESP_LOGW(TAG, "Failed to enter CFGUPDATE mode");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

static esp_err_t bq27220_exit_config_update(void) {
    esp_err_t ret = bq27220_control_command(BQ27220_EXIT_CFG_UPDATE);
    if (ret != ESP_OK) return ret;
    
    // Wait for exit from CFGUPDATE mode
    vTaskDelay(pdMS_TO_TICKS(100));
    
    uint16_t flags = bq27220_read_word(BQ27220_REG_FLAGS);
    if ((flags & 0x0020) != 0) {
        ESP_LOGW(TAG, "Failed to exit CFGUPDATE mode");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

static uint8_t bq27220_read_byte(uint8_t reg) {
    uint8_t data = 0xFF;
#if CONFIG_PM_ENABLE
    if (fg_i2c_pm_lock) esp_pm_lock_acquire(fg_i2c_pm_lock);
#endif
    bool locked = i2c_bus_lock(I2C_MASTER_NUM, I2C_MASTER_TIMEOUT_MS);
    esp_err_t ret = i2c_master_write_read_device(I2C_MASTER_NUM, BQ27220_I2C_ADDRESS,
                                                  &reg, 1, &data, 1,
                                                  pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    if (locked) i2c_bus_unlock(I2C_MASTER_NUM);
#if CONFIG_PM_ENABLE
    if (fg_i2c_pm_lock) esp_pm_lock_release(fg_i2c_pm_lock);
#endif
    if (ret != ESP_OK) {
        return 0xFF;
    }
    return data;
}

static esp_err_t fuel_gauge_configure(void) {
    // Check if device is sealed first
    bool is_sealed = bq27220_is_sealed();
    ESP_LOGI(TAG, "Device is %s", is_sealed ? "sealed" : "unsealed");
    
    // Enter configuration update mode
    esp_err_t ret = bq27220_enter_config_update();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enter config update mode: %s", esp_err_to_name(ret));
        // Try to read some registers to understand the state
        uint16_t control_status = bq27220_get_control_status();
        uint16_t flags = bq27220_read_word(BQ27220_REG_FLAGS);
        uint16_t voltage = bq27220_read_word(BQ27220_REG_VOLTAGE);
        ESP_LOGE(TAG, "Control status: 0x%04X, Flags: 0x%04X, Voltage: %d mV", control_status, flags, voltage);
        return ret;
    }

    // Read current design capacity
    uint16_t design_capacity = bq27220_read_word(BQ27220_REG_DESIGN_CAPACITY);
    ESP_LOGI(TAG, "Current design capacity: %d mAh", design_capacity);

    // Read current full charge capacity
    uint16_t full_charge_capacity = bq27220_read_word(BQ27220_REG_FULL_CHARGE_CAPACITY);
    ESP_LOGI(TAG, "Current full charge capacity: %d mAh", full_charge_capacity);

    // Configure battery parameters based on Kconfig settings
    // Set design capacity (mAh)
    uint16_t new_design_capacity = CONFIG_BQ27220_DESIGN_CAPACITY;
    if (new_design_capacity != design_capacity) {
        ESP_LOGI(TAG, "Setting design capacity to %d mAh (was %d mAh)", new_design_capacity, design_capacity);
        ret = bq27220_write_word(BQ27220_REG_DESIGN_CAPACITY, new_design_capacity);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write design capacity: %s", esp_err_to_name(ret));
            // Don't return error, continue with other configurations
        }
    }

    // TODO: Add other battery parameters configuration here as needed
    // Based on the BQ27220 datasheet, other important parameters that could be configured include:
    // - Charge termination voltage (important for proper charging)
    // - EDV (End of Discharge Voltage) parameters
    // - CEDV gauging configuration
    // - Temperature compensation parameters
    // These would require additional Kconfig options to be defined
    //
    // Example for setting charge termination voltage (if we had the config option):
    // uint16_t terminate_voltage = CONFIG_BQ27220_TERMINATE_VOLTAGE;
    // ret = bq27220_write_word(0x9290, terminate_voltage); // Charge Termination Voltage address
    // if (ret != ESP_OK) {
    //     ESP_LOGE(TAG, "Failed to write terminate voltage: %s", esp_err_to_name(ret));
    // }

    // Exit configuration update mode
    ret = bq27220_exit_config_update();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to exit config update mode: %s", esp_err_to_name(ret));
        return ret;
    }

    if (is_sealed) {
        esp_err_t seal_ret = bq27220_seal_device();
        if (seal_ret != ESP_OK) {
            ESP_LOGW(TAG, "Unable to reseal fuel gauge: %s", esp_err_to_name(seal_ret));
        }
    }

    esp_err_t it_ret = bq27220_control_command(BQ27220_IT_ENABLE);
    if (it_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enable gauging (IT_ENABLE): %s", esp_err_to_name(it_ret));
    } else {
        ESP_LOGI(TAG, "Impedance Track gauging enabled");
    }

    ESP_LOGI(TAG, "Fuel gauge configuration completed");
    return ESP_OK;
}

static esp_err_t fuel_gauge_reset(void) {
    ESP_LOGI(TAG, "Resetting BQ27220 fuel gauge");
    
    // Send reset command
    esp_err_t ret = bq27220_control_command(BQ27220_RESET);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send reset command: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Wait for reset to complete
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Re-initialize the fuel gauge
    uint16_t voltage = bq27220_read_word(BQ27220_REG_VOLTAGE);
    if (voltage == 0xFFFF) {
        ESP_LOGE(TAG, "Failed to communicate with BQ27220 after reset");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "BQ27220 reset completed, voltage: %d mV", voltage);
    return ESP_OK;
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
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to configure I2C parameters: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_driver_install(I2C_MASTER_NUM, I2C_MODE_MASTER, 0, 0, 0);
    if (ret == ESP_OK) {
        i2c_initialized_by_us = true;
    } else if (ret == ESP_ERR_INVALID_STATE || ret == ESP_FAIL) {
        ESP_LOGI(TAG, "I2C driver already installed or busy on port %d", I2C_MASTER_NUM);
        ret = ESP_OK; // Treat as success
    } else {
        ESP_LOGE(TAG, "Failed to install I2C driver: %s", esp_err_to_name(ret));
    }

#if CONFIG_PM_ENABLE
    if (fg_i2c_pm_lock == NULL) {
        esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "fg_i2c", &fg_i2c_pm_lock);
    }
#endif
    return ret;
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

    vTaskDelay(pdMS_TO_TICKS(150));

    uint16_t voltage = 0xFFFF;
    for (int attempt = 0; attempt < 3; ++attempt) {
        voltage = bq27220_read_word(BQ27220_REG_VOLTAGE);
        if (voltage != 0xFFFF) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (voltage == 0xFFFF) {
        ESP_LOGE(TAG, "Failed to communicate with BQ27220 - check wiring and I2C config");
        return false;
    }

    if (voltage == 0) {
        ESP_LOGW(TAG, "BQ27220 reports 0V - battery may be disconnected");
    }

    ESP_LOGI(TAG, "BQ27220 detected, voltage: %d mV", voltage);

    // Check device type
    bq27220_control_command(BQ27220_DEVICE_TYPE);
    vTaskDelay(pdMS_TO_TICKS(5));
    uint16_t device_type = bq27220_read_word(BQ27220_REG_CONTROL);
    ESP_LOGI(TAG, "BQ27220 device type: 0x%04X", device_type);

    // Configure the fuel gauge with proper battery parameters
    ret = fuel_gauge_configure();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Fuel gauge configuration failed, continuing with default settings");
    }

    // Check if SOC is stuck at a suspicious value (like 54%)
    uint8_t initial_soc = bq27220_read_byte(BQ27220_REG_SOC);
    if (initial_soc != 0xFF && initial_soc <= 100) {
        ESP_LOGI(TAG, "Initial SOC reading: %d%%", initial_soc);
        
        // If SOC is at a suspicious value, consider resetting the fuel gauge
        if (initial_soc == 54 || initial_soc == 55 || initial_soc == 53) {
            ESP_LOGW(TAG, "Suspicious initial SOC value detected (%d%%), consider resetting fuel gauge", initial_soc);
            // Optionally reset the fuel gauge if needed
            // fuel_gauge_reset();
        }
    }

    memset(&last_data, 0, sizeof(last_data));
    last_data.is_initialized = true;
    is_initialized = true;

    ESP_LOGI(TAG, "BQ27220 fuel gauge initialized successfully");
    return true;
}

void fuel_gauge_manager_set_paused(bool paused) {
    s_paused = paused;
}

bool fuel_gauge_manager_get_data(fuel_gauge_data_t *data) {
    if (!is_initialized || !data) {
        return false;
    }

    if (s_paused) {
        if (last_data.is_initialized) {
            memcpy(data, &last_data, sizeof(fuel_gauge_data_t));
            return true;
        }
        return false;
    }

    uint16_t voltage = bq27220_read_word(BQ27220_REG_VOLTAGE);
    uint16_t current_raw = bq27220_read_word(BQ27220_REG_CURRENT);
    uint8_t soc_byte = 0xFF;
    for (int attempt = 0; attempt < 3; ++attempt) {
        soc_byte = bq27220_read_byte(BQ27220_REG_SOC);
        if (soc_byte != 0xFF) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
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

    // Quick charging hint for SOC handling (before smoothing below)
    bool charging_hint = false;
    if (flags != 0xFFFF) {
        charging_hint = ((flags & 0x0001) == 0);
    }
    if (!charging_hint && current_raw != 0xFFFF) {
        int16_t cur = (int16_t)current_raw;
        if (cur > 10) charging_hint = true;
    }

    // Fill data structure
    data->voltage_mv = (voltage != 0xFFFF) ? voltage : last_data.voltage_mv;
    // BQ27220 current is signed 16-bit in 2's complement; treat positive as charging on this board
    int16_t interpreted_current = (current_raw != 0xFFFF) ? (int16_t)current_raw : last_data.current_ma;
    // Treat near-zero noise as 0 to avoid false charge/discharge flips
    if (interpreted_current > -10 && interpreted_current < 10) {
        interpreted_current = 0;
    }
    data->current_ma = interpreted_current;
    
    // StateOfCharge: prefer reported SOC if valid; fallback to voltage estimate only when invalid
    static TickType_t last_soc_warn = 0;
    int new_percentage = last_data.percentage;
    bool soc_valid = (soc_byte != 0xFF && soc_byte <= 100);
    int voltage_based_soc = -1;
    if (voltage != 0xFFFF) {
        if (voltage >= 4200) {
            voltage_based_soc = 100;
        } else if (voltage >= 4000) {
            voltage_based_soc = 75 + ((voltage - 4000) * 25) / 200;
        } else if (voltage >= 3800) {
            voltage_based_soc = 40 + ((voltage - 3800) * 35) / 200;
        } else if (voltage >= 3600) {
            voltage_based_soc = 10 + ((voltage - 3600) * 30) / 200;
        } else if (voltage >= 3300) {
            voltage_based_soc = (voltage - 3300) * 10 / 300;
        } else {
            voltage_based_soc = 0;
        }
    }

    if (soc_valid) {
        new_percentage = soc_byte;
        if (soc_byte == 100) {
            if (voltage_based_soc >= 0 && voltage_based_soc < 98) {
                // If SOC reads 100% but voltage estimate is clearly lower, trust voltage
                new_percentage = voltage_based_soc;
            } else if (charging_hint) {
                // While charging, creep upward instead of jumping to 100
                int target = (voltage_based_soc >= 0) ? voltage_based_soc : new_percentage;
                if (last_data.is_initialized) {
                    int min_step = last_data.percentage + 1;
                    if (target < min_step) target = min_step;
                }
                if (target > 99) target = 99;
                new_percentage = target;
            }
        }
    } else if (voltage_based_soc >= 0) {
        new_percentage = voltage_based_soc;
    }
    
    // Apply hysteresis to prevent SOC jumping around
    if (last_data.is_initialized) {
        int diff = abs(new_percentage - last_data.percentage);
        if (diff > 5) {
            // Large change - allow it but log it
            ESP_LOGD(TAG, "Large SOC change: %d%% -> %d%%", last_data.percentage, new_percentage);
        } else if (diff > 0) {
            // Small change - smooth it
            if (new_percentage > last_data.percentage) {
                new_percentage = last_data.percentage + 1;
            } else if (new_percentage < last_data.percentage) {
                new_percentage = last_data.percentage - 1;
            }
        }
    }
    
    data->percentage = new_percentage;

    // BQ27220 charging detection using flags and current measurement
    bool flag_valid = (flags != 0xFFFF);
    bool current_valid = (current_raw != 0xFFFF);
    bool charging = false;
    if (flag_valid || current_valid) {
        // BatteryStatus: DSG is bit0 (1 = discharging), CHG bit may be unreliable; prefer !DSG
        bool dsg_bit = flag_valid && ((flags & 0x0001) != 0);
        bool charging_flag = flag_valid ? !dsg_bit : false;
        // Current fallback: positive current means charging on this board, use a small threshold to ignore noise
        bool charging_current = current_valid && (data->current_ma > 10);
        charging = charging_flag || charging_current;
        
        // Additional check: if we're at 100% and not charging, but current shows charge, assume charging
        if (data->percentage >= 99 && !charging && charging_current) {
            charging = true;
        }
    } else {
        // Fallback to last known state if no valid data
        charging = last_data.is_charging;
    }
    data->is_charging = charging;
    data->is_initialized = true;

    // Update cache
    memcpy(&last_data, data, sizeof(fuel_gauge_data_t));

    ESP_LOGD(TAG, "Battery: %d%%, %dmV, %dmA, %s",
             data->percentage, data->voltage_mv, data->current_ma,
             data->is_charging ? "charging" : "discharging");
    ESP_LOGI(TAG, "FG raw: flags=0x%04X soc=%d%% current_ma=%d voltage=%dmV",
             flags, soc_byte, data->current_ma, data->voltage_mv);

    return true;
}

int fuel_gauge_manager_get_percentage(void) {
    return (is_initialized && last_data.is_initialized) ? last_data.percentage : -1;
}

bool fuel_gauge_manager_is_charging(void) {
    return (is_initialized && last_data.is_initialized) ? last_data.is_charging : false;
}

uint16_t fuel_gauge_manager_get_voltage_mv(void) {
    return (is_initialized && last_data.is_initialized) ? last_data.voltage_mv : 0;
}

esp_err_t fuel_gauge_manager_reset(void) {
    if (!is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    bool was_paused = s_paused;
    s_paused = true;

    esp_err_t ret = fuel_gauge_reset();
    if (ret == ESP_OK) {
        memset(&last_data, 0, sizeof(last_data));
        esp_err_t cfg_ret = fuel_gauge_configure();
        if (cfg_ret != ESP_OK) {
            ESP_LOGW(TAG, "Fuel gauge reconfiguration after reset failed: %s", esp_err_to_name(cfg_ret));
        }
    }

    s_paused = was_paused;
    return ret;
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

#elif defined(CONFIG_USE_MAX17048_FUEL_GAUGE)

static const char *TAG = "MAX17048";

#define MAX17048_I2C_ADDRESS     CONFIG_MAX17048_I2C_ADDRESS
#define MAX17048_REG_VCELL       0x02
#define MAX17048_REG_SOC         0x04
#define MAX17048_REG_MODE        0x06
#define MAX17048_REG_VERSION     0x08
#define MAX17048_REG_CONFIG      0x0C
#define MAX17048_REG_CRATE       0x16
#define MAX17048_REG_STATUS      0x1A
#define MAX17048_REG_CMD         0xFE

#ifdef CONFIG_IDF_TARGET_ESP32S3
#define I2C_MASTER_NUM          I2C_NUM_0
#else
#define I2C_MASTER_NUM          I2C_NUM_0
#endif
#define I2C_MASTER_TIMEOUT_MS   250
#define I2C_LOCK_TIMEOUT_MS     500

static bool is_initialized = false;
static bool i2c_initialized_by_us = false;
static fuel_gauge_data_t last_data = {0};
static volatile bool s_paused = false;

#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t fg_i2c_pm_lock = NULL;
#endif

static uint16_t max17048_read_word(uint8_t reg) {
    uint8_t data[2] = {0};
#if CONFIG_PM_ENABLE
    if (fg_i2c_pm_lock) esp_pm_lock_acquire(fg_i2c_pm_lock);
#endif

    uint16_t result = 0xFFFF;
    for (int retry = 0; retry < 3; retry++) {
        if (i2c_bus_lock(I2C_MASTER_NUM, I2C_LOCK_TIMEOUT_MS)) {
            esp_err_t ret = i2c_master_write_read_device(I2C_MASTER_NUM, MAX17048_I2C_ADDRESS,
                                                         &reg, 1, data, 2,
                                                         pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
            i2c_bus_unlock(I2C_MASTER_NUM);
            if (ret == ESP_OK) {
                result = (uint16_t)((data[0] << 8) | data[1]);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

#if CONFIG_PM_ENABLE
    if (fg_i2c_pm_lock) esp_pm_lock_release(fg_i2c_pm_lock);
#endif
    return result;
}

static esp_err_t max17048_write_word(uint8_t reg, uint16_t data) {
    uint8_t write_data[3];
    write_data[0] = reg;
    write_data[1] = (data >> 8) & 0xFF;
    write_data[2] = data & 0xFF;
#if CONFIG_PM_ENABLE
    if (fg_i2c_pm_lock) esp_pm_lock_acquire(fg_i2c_pm_lock);
#endif

    esp_err_t ret = ESP_ERR_TIMEOUT;
    for (int retry = 0; retry < 3; retry++) {
        if (i2c_bus_lock(I2C_MASTER_NUM, I2C_LOCK_TIMEOUT_MS)) {
            ret = i2c_master_write_to_device(I2C_MASTER_NUM, MAX17048_I2C_ADDRESS,
                                              write_data, 3,
                                              pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
            i2c_bus_unlock(I2C_MASTER_NUM);
            if (ret == ESP_OK) break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

#if CONFIG_PM_ENABLE
    if (fg_i2c_pm_lock) esp_pm_lock_release(fg_i2c_pm_lock);
#endif
    return ret;
}

static esp_err_t fuel_gauge_i2c_init(void) {
    // First try to install the driver to check if it's already installed
    esp_err_t ret = i2c_driver_install(I2C_MASTER_NUM, I2C_MODE_MASTER, 0, 0, 0);
    if (ret == ESP_OK) {
        // We installed it - now configure parameters
        i2c_initialized_by_us = true;
        i2c_config_t conf = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = CONFIG_MAX17048_I2C_SDA_PIN,
            .scl_io_num = CONFIG_MAX17048_I2C_SCL_PIN,
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .master.clk_speed = 100000,
            .clk_flags = 0,
        };

        ret = i2c_param_config(I2C_MASTER_NUM, &conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure I2C parameters: %s", esp_err_to_name(ret));
            i2c_driver_delete(I2C_MASTER_NUM);
            i2c_initialized_by_us = false;
            return ret;
        }
    } else if (ret == ESP_ERR_INVALID_STATE || ret == ESP_FAIL) {
        ESP_LOGI(TAG, "I2C driver already installed on port %d", I2C_MASTER_NUM);
        i2c_initialized_by_us = false;
        ret = ESP_OK; // Treat as success - use existing driver
    } else {
        ESP_LOGE(TAG, "Failed to install I2C driver: %s", esp_err_to_name(ret));
        return ret;
    }

#if CONFIG_PM_ENABLE
    if (fg_i2c_pm_lock == NULL) {
        esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "fg_i2c", &fg_i2c_pm_lock);
    }
#endif
    return ret;
}

bool fuel_gauge_manager_init(void) {
    if (is_initialized) {
        return true;
    }

    ESP_LOGI(TAG, "Initializing MAX17048 fuel gauge");
    ESP_LOGI(TAG, "Configuration: Address=0x%02X, SDA=%d, SCL=%d, I2C_PORT=%d",
             MAX17048_I2C_ADDRESS, CONFIG_MAX17048_I2C_SDA_PIN, CONFIG_MAX17048_I2C_SCL_PIN, I2C_MASTER_NUM);

    esp_err_t ret = fuel_gauge_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C");
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(150));

    uint16_t version = max17048_read_word(MAX17048_REG_VERSION);
    if (version == 0xFFFF || (version & 0xFFF0) != 0x0010) {
        // Retry once for MAX17048 which might be slow to wake
        vTaskDelay(pdMS_TO_TICKS(50));
        version = max17048_read_word(MAX17048_REG_VERSION);
    }

    if (version == 0xFFFF) {
        ESP_LOGE(TAG, "Failed to communicate with MAX17048 - check wiring and I2C config");
        // CLEANUP: If we installed the driver, remove it so others can use the port cleanly
        if (i2c_initialized_by_us) {
            i2c_driver_delete(I2C_MASTER_NUM);
            i2c_initialized_by_us = false;
        }
        return false;
    }

    ESP_LOGI(TAG, "MAX17048 detected, version: 0x%04X", version);
    
    // Perform a full software reset to clear any stale ModelGauge data
    max17048_write_word(MAX17048_REG_CMD, 0x5400);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Set RCOMP to standard LiPo value (0x97) to stabilize ModelGauge
    // CONFIG register is at 0x0C. High byte is RCOMP.
    uint16_t config_reg = max17048_read_word(MAX17048_REG_CONFIG);
    if (config_reg != 0xFFFF) {
        uint16_t new_config = (config_reg & 0x00FF) | 0x9700;
        max17048_write_word(MAX17048_REG_CONFIG, new_config);
        
        // Verify write
        uint16_t verify = max17048_read_word(MAX17048_REG_CONFIG);
        if ((verify & 0xFF00) == 0x9700) {
            ESP_LOGI(TAG, "RCOMP set successfully: 0x%04X", verify);
        } else {
            ESP_LOGW(TAG, "RCOMP write mismatch! Got: 0x%04X", verify);
        }
    }

    // Quick start to update SOC immediately after reset/config
    uint16_t current_mode = max17048_read_word(MAX17048_REG_MODE);
    if (current_mode != 0xFFFF) {
        max17048_write_word(MAX17048_REG_MODE, current_mode | 0x4000); // Set QuickStart bit
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));

    memset(&last_data, 0, sizeof(last_data));
    last_data.is_initialized = true;
    is_initialized = true;

    ESP_LOGI(TAG, "MAX17048 fuel gauge initialized successfully");
    return true;
}

void fuel_gauge_manager_set_paused(bool paused) {
    s_paused = paused;
}

bool fuel_gauge_manager_get_data(fuel_gauge_data_t *data) {
    if (!is_initialized || !data) {
        return false;
    }

    if (s_paused) {
        if (last_data.is_initialized) {
            memcpy(data, &last_data, sizeof(fuel_gauge_data_t));
            return true;
        }
        return false;
    }

    uint16_t vcell = max17048_read_word(MAX17048_REG_VCELL);
    uint16_t soc_raw = max17048_read_word(MAX17048_REG_SOC);
    uint16_t crate_raw = max17048_read_word(MAX17048_REG_CRATE);

    if (vcell == 0xFFFF || soc_raw == 0xFFFF) {
        if (last_data.is_initialized) {
            memcpy(data, &last_data, sizeof(fuel_gauge_data_t));
            return true;
        }
        return false;
    }

    // VCELL: 1 unit = 78.125uV.  (vcell * 78.125) / 1000 = mV
    data->voltage_mv = (uint16_t)((float)vcell * 78.125f / 1000.0f);
    
    // SOC Jump detection: 
    // If we read > 110%, it's likely an uninitialized/reset state (like 0xFE / 254%).
    // If it's between 100-110%, it's just a full battery calibration offset; cap to 100.
    uint8_t current_perc = (uint8_t)(soc_raw >> 8);
    if (current_perc > 110) {
        if (!last_data.is_initialized) {
            // Hard fallback for first-run crazy data: force a QuickStart
            ESP_LOGW(TAG, "Invalid initial SOC 0x%04X. Forcing QuickStart.", soc_raw);
            uint16_t m = max17048_read_word(MAX17048_REG_MODE);
            if (m != 0xFFFF) max17048_write_word(MAX17048_REG_MODE, m | 0x4000);
            data->percentage = (data->voltage_mv > 3700) ? 50 : 0; 
        } else {
            ESP_LOGW(TAG, "SOC jump detected (0x%04X), ignoring glitch", soc_raw);
            data->percentage = last_data.percentage;
        }
    } else {
        // Capped valid range
        uint8_t filtered_perc = (current_perc > 100) ? 100 : current_perc;
        
        // Glitch rejection: if last reading was high and this is 0, it's almost certainly an I2C error
        if (last_data.is_initialized && last_data.percentage > 50 && filtered_perc == 0) {
            ESP_LOGW(TAG, "SOC dropped from %d%% to 0%%, rejecting glitch", last_data.percentage);
            data->percentage = last_data.percentage;
        } else {
            data->percentage = filtered_perc;
        }
        
        // Logically verify SOC against voltage
        // If voltage > 4.1V but SOC < 20%, the algorithm is clearly lost
        if (data->voltage_mv > 4100 && data->percentage < 20) {
            ESP_LOGW(TAG, "SOC/Voltage discrepancy (%d mV but %d%%). Triggering QuickStart.", 
                     data->voltage_mv, data->percentage);
            uint16_t m = max17048_read_word(MAX17048_REG_MODE);
            if (m != 0xFFFF) max17048_write_word(MAX17048_REG_MODE, m | 0x4000);
        }
    }

    // CRATE: charge/discharge rate in %/hr. 1 unit = 0.208 %/hr
    int16_t signed_crate = (int16_t)crate_raw;
    data->current_ma = (int16_t)((float)signed_crate * 0.208f); 
    
    // Charging detection refinement:
    // If voltage is > 4.15V, we are likely charging even if signed_crate is negative (noise/ModelGauge error)
    // unless it's a very large discharge.
    bool voltage_high = (data->voltage_mv > 4150);
    data->is_charging = (signed_crate > 2) || (voltage_high && signed_crate > -50);
    
    data->is_initialized = true;

    // Log the raw and processed values for debugging
    ESP_LOGD(TAG, "VCELL: %d (%d mV), SOC: 0x%04X (%d%%), CRATE: %d (%d %%/hr), charging: %s",
             vcell, data->voltage_mv, soc_raw, data->percentage, 
             signed_crate, (int)(signed_crate * 0.208f),
             data->is_charging ? "YES" : "NO");

    uint16_t config = max17048_read_word(MAX17048_REG_CONFIG);
    uint16_t mode = max17048_read_word(MAX17048_REG_MODE);
    ESP_LOGD(TAG, "CONFIG: 0x%04X, MODE: 0x%04X", config, mode);

    // Update cache
    memcpy(&last_data, data, sizeof(fuel_gauge_data_t));

    return true;
}

int fuel_gauge_manager_get_percentage(void) {
    return (is_initialized && last_data.is_initialized) ? last_data.percentage : -1;
}

bool fuel_gauge_manager_is_charging(void) {
    return (is_initialized && last_data.is_initialized) ? last_data.is_charging : false;
}

uint16_t fuel_gauge_manager_get_voltage_mv(void) {
    return (is_initialized && last_data.is_initialized) ? last_data.voltage_mv : 0;
}

esp_err_t fuel_gauge_manager_reset(void) {
    if (!is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    // RESET command: write 0x5400 to CMD register (Note: high byte 0x54, low byte 0x00?
    // Snippet says write(REG::CMD, 0x5400) which sends 0x54 then 0x00)
    max17048_write_word(MAX17048_REG_CMD, 0x5400);
    vTaskDelay(pdMS_TO_TICKS(100));
    is_initialized = false;
    return fuel_gauge_manager_init() ? ESP_OK : ESP_FAIL;
}

void fuel_gauge_manager_deinit(void) {
    if (is_initialized) {
        if (i2c_initialized_by_us) {
            i2c_driver_delete(I2C_MASTER_NUM);
            i2c_initialized_by_us = false;
        }
        is_initialized = false;
        memset(&last_data, 0, sizeof(last_data));
    }
}

#else

bool fuel_gauge_manager_init(void) { return false; }
bool fuel_gauge_manager_get_data(fuel_gauge_data_t *data) { return false; }
int fuel_gauge_manager_get_percentage(void) { return -1; }
bool fuel_gauge_manager_is_charging(void) { return false; }
uint16_t fuel_gauge_manager_get_voltage_mv(void) { return 0; }
void fuel_gauge_manager_deinit(void) {}
void fuel_gauge_manager_set_paused(bool paused) { (void)paused; }
esp_err_t fuel_gauge_manager_reset(void) { return ESP_ERR_NOT_SUPPORTED; }

#endif
