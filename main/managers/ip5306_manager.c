#include "managers/ip5306_manager.h"

#ifdef CONFIG_USE_IP5306_POWER_MANAGER

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "i2c_shared.h"
#include <stddef.h>

static const char *TAG = "IP5306";

#define IP5306_I2C_PORT       I2C_NUM_0
#define IP5306_I2C_SDA_PIN   ((gpio_num_t)CONFIG_I2C_MANAGER_0_SDA)
#define IP5306_I2C_SCL_PIN   ((gpio_num_t)CONFIG_I2C_MANAGER_0_SCL)
#define IP5306_SCL_SPEED_HZ  400000
#define IP5306_TIMEOUT_MS    100

#define IP5306_REG_SYS_CTL0      0x00
#define IP5306_REG_CHARGER_CTL0  0x20
#define IP5306_REG_CHARGER_CTL1  0x21
#define IP5306_REG_CHARGER_CTL2  0x22
#define IP5306_REG_CHARGER_CTL3  0x23
#define IP5306_REG_CHG_DIG_CTL0  0x24
#define IP5306_REG_READ0         0x70
#define IP5306_REG_READ1         0x71

#define IP5306_CHARGER_ENABLE_BIT  (1U << 4)
#define IP5306_CHARGE_STATUS_BIT   (1U << 3)
#define IP5306_CHARGE_FULL_BIT     (1U << 3)

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_initialized;

static esp_err_t ip5306_read8_locked(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, value, 1,
                                       IP5306_TIMEOUT_MS);
}

static esp_err_t ip5306_read8(uint8_t reg, uint8_t *value)
{
    if (!s_bus || !s_dev || !value) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!i2c_shared_bus_lock(s_bus, IP5306_TIMEOUT_MS)) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ip5306_read8_locked(reg, value);
    i2c_shared_bus_unlock(s_bus);
    return ret;
}

static esp_err_t ip5306_modify8(uint8_t reg, uint8_t mask, uint8_t value)
{
    if (!s_bus || !s_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!i2c_shared_bus_lock(s_bus, IP5306_TIMEOUT_MS)) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t current = 0;
    esp_err_t ret = ip5306_read8_locked(reg, &current);
    if (ret == ESP_OK) {
        uint8_t next = (uint8_t)((current & (uint8_t)~mask) | (value & mask));
        if (next != current) {
            const uint8_t write_data[] = {reg, next};
            ret = i2c_master_transmit(s_dev, write_data, sizeof(write_data),
                                      IP5306_TIMEOUT_MS);
        }
    }

    i2c_shared_bus_unlock(s_bus);
    return ret;
}

static esp_err_t ip5306_configure(void)
{
    /*
     * Only charger fields are changed. SYS_CTL1 and the button-related bits in
     * SYS_CTL0 are intentionally left alone so the IP5306 keeps its native
     * short/double/long press behavior.
     */
    static const struct {
        uint8_t reg;
        uint8_t mask;
        uint8_t value;
    } settings[] = {
        {IP5306_REG_SYS_CTL0,     IP5306_CHARGER_ENABLE_BIT, IP5306_CHARGER_ENABLE_BIT},
        {IP5306_REG_CHARGER_CTL0, 0x03, 0x03}, /* 4.2 V charge-stop voltage */
        {IP5306_REG_CHARGER_CTL1, 0xC0, 0x40}, /* 400 mA charge-stop detection */
        {IP5306_REG_CHARGER_CTL2, 0x0F, 0x02}, /* 4.2 V target, vendor-recommended +28 mV */
        {IP5306_REG_CHARGER_CTL3, 0x20, 0x20}, /* constant-current loop at VIN */
        {IP5306_REG_CHG_DIG_CTL0, 0x1F, CONFIG_IP5306_CHARGE_CURRENT_CODE},
    };

    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i) {
        esp_err_t ret = ip5306_modify8(settings[i].reg, settings[i].mask,
                                       settings[i].value);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure register 0x%02X: %s",
                     settings[i].reg, esp_err_to_name(ret));
            return ret;
        }
    }

    return ESP_OK;
}

bool ip5306_manager_init(void)
{
    if (s_initialized) {
        return true;
    }

    esp_err_t ret = i2c_shared_get_or_create_bus(
        IP5306_I2C_PORT, IP5306_I2C_SDA_PIN, IP5306_I2C_SCL_PIN, true,
        &s_bus, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get I2C bus: %s", esp_err_to_name(ret));
        return false;
    }

    ret = i2c_shared_get_device(s_bus, CONFIG_IP5306_I2C_ADDRESS,
                                IP5306_SCL_SPEED_HZ, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to attach device at 0x%02X: %s",
                 CONFIG_IP5306_I2C_ADDRESS, esp_err_to_name(ret));
        return false;
    }

    uint8_t read0 = 0;
    ret = ip5306_read8(IP5306_REG_READ0, &read0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No IP5306 response at 0x%02X: %s",
                 CONFIG_IP5306_I2C_ADDRESS, esp_err_to_name(ret));
        return false;
    }

    ret = ip5306_configure();
    if (ret != ESP_OK) {
        return false;
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "Initialized at 0x%02X on I2C0 SDA=%d SCL=%d; charge current code=0x%02X",
             CONFIG_IP5306_I2C_ADDRESS, CONFIG_I2C_MANAGER_0_SDA,
             CONFIG_I2C_MANAGER_0_SCL, CONFIG_IP5306_CHARGE_CURRENT_CODE);
    return true;
}

bool ip5306_manager_get_status(ip5306_status_t *status)
{
    if (!s_initialized || !status) {
        return false;
    }

    uint8_t read0 = 0;
    uint8_t read1 = 0;
    if (ip5306_read8(IP5306_REG_READ0, &read0) != ESP_OK ||
        ip5306_read8(IP5306_REG_READ1, &read1) != ESP_OK) {
        return false;
    }

    status->is_charging = (read0 & IP5306_CHARGE_STATUS_BIT) != 0;
    status->is_full = (read1 & IP5306_CHARGE_FULL_BIT) != 0;
    return true;
}

#else

bool ip5306_manager_init(void)
{
    return false;
}

bool ip5306_manager_get_status(ip5306_status_t *status)
{
    (void)status;
    return false;
}

#endif
