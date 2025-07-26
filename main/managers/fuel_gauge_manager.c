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
#define BQ27220_UNSEAL          0x0080
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

static esp_err_t bq27220_write_word(uint8_t reg, uint16_t data) {
    uint8_t write_data[3];
    write_data[0] = reg;
    write_data[1] = data & 0xFF;
    write_data[2] = (data >> 8) & 0xFF;
    
    return i2c_master_write_to_device(I2C_MASTER_NUM, BQ27220_I2C_ADDRESS,
                                      write_data, 3,
                                      pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
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
    esp_err_t ret = i2c_master_write_read_device(I2C_MASTER_NUM, BQ27220_I2C_ADDRESS,
                                                 &reg, 1, &data, 1,
                                                 pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
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
    // Add validation and fallback logic to prevent stuck SOC
    int new_percentage = last_data.percentage;
    if (soc_byte != 0xFF && soc_byte <= 100) {
        // Validate that the new SOC makes sense compared to voltage
        // This helps detect when the fuel gauge is reporting incorrect values
        if (voltage != 0xFFFF) {
            // Basic voltage-based SOC estimation as fallback
            int voltage_based_soc = 0;
            if (voltage >= 4100) {
                voltage_based_soc = 100;
            } else if (voltage >= 3700) {
                voltage_based_soc = 75 + ((voltage - 3700) * 25) / 400;
            } else if (voltage >= 3500) {
                voltage_based_soc = 25 + ((voltage - 3500) * 50) / 200;
            } else if (voltage >= 3300) {
                voltage_based_soc = 5 + ((voltage - 3300) * 20) / 200;
            } else {
                voltage_based_soc = 0;
            }
            
            // If the reported SOC differs significantly from voltage-based estimation,
            // it might be incorrect. Use voltage-based estimation with some weighting.
            int diff = abs((int)soc_byte - voltage_based_soc);
            if (diff > 30) {
                // Large discrepancy - use voltage-based estimation
                ESP_LOGW(TAG, "Large SOC discrepancy detected: reported=%d%%, voltage-based=%d%%", soc_byte, voltage_based_soc);
                new_percentage = voltage_based_soc;
            } else {
                // Reasonable agreement - use reported SOC
                new_percentage = soc_byte;
            }
        } else {
            new_percentage = soc_byte;
        }
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
        bool charging_flag = flag_valid && ((flags & 0x0100) != 0);
        bool charging_current = current_valid && (data->current_ma > 0);
        charging = charging_flag || charging_current;
        
        // Additional check: if we're at 100% and not charging, but current is positive,
        // we might be in a charging state that the fuel gauge hasn't detected yet
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
