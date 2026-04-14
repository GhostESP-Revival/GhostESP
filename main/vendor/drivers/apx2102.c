#include "driver/i2c_master.h"
#include "i2c_shared.h"
#include "vendor/drivers/axp2101.h"
#include <stdio.h>

#define BIT_MASK(bit) (1 << (bit))
#define IS_BIT_SET(value, bit) (((value) & (1 << (bit))) != 0)

bool i2c_initialized = false;
static i2c_master_bus_handle_t s_axp_bus = NULL;
static i2c_master_dev_handle_t s_axp_dev = NULL;
static bool s_axp_bus_owned = false;

#define AXP2101_REG_POWER_LEVEL 0xA4
#define AXP202_MODE_CHGSTATUS 0x01

bool axp202_is_battery_connected(void) {
  if (!i2c_initialized) {
    printf("ERROR [%s]: I2C is not initialized\n", __func__);
    return false;
  }

  uint8_t reg = 0;
  uint8_t reg_addr = AXP202_MODE_CHGSTATUS;

  esp_err_t err = i2c_master_transmit_receive(s_axp_dev, &reg_addr, 1, &reg, 1, 100);
  if (err != ESP_OK) {
    printf("ERROR [%s]: Failed to read register 0x%02X: %s\n", __func__,
           reg_addr, esp_err_to_name(err));
    return false;
  }

  bool battery_connected = IS_BIT_SET(reg, 5);
  printf("INFO [%s]: Battery connection status: %s\n", __func__,
         battery_connected ? "Connected" : "Not Connected");
  return battery_connected;
}

bool axp202_is_charging(void) {
  if (!i2c_initialized) {
    printf("ERROR [%s]: I2C is not initialized\n", __func__);
    return false;
  }

  uint8_t reg = 0;
  uint8_t reg_addr = AXP202_MODE_CHGSTATUS;

  esp_err_t err = i2c_master_transmit_receive(s_axp_dev, &reg_addr, 1, &reg, 1, 100);
  if (err != ESP_OK) {
    printf("ERROR [%s]: Failed to read register 0x%02X: %s\n", __func__,
           reg_addr, esp_err_to_name(err));
    return false;
  }

  bool charging_bit_set = IS_BIT_SET(reg, 6);
  // On this specific AXP variant/hardware, bit 6 of REG 0x01 seems to be inverted.
  // 0 = Charging, 1 = Not Charging. Therefore, we return the inverse.
  bool is_actually_charging = !charging_bit_set; 
  printf("INFO [%s]: Charging status: %s (Register Bit 6: %d)\n", __func__,
         is_actually_charging ? "Charging" : "Not Charging", charging_bit_set);
  return is_actually_charging; 
}

esp_err_t axp2101_init(void) {
  if (i2c_initialized) {
    printf("WARNING [%s]: AXP2101 is already initialized\n", __func__);
    return ESP_OK;
  }

  esp_err_t err = i2c_shared_get_or_create_bus(I2C_MASTER_NUM,
                                               I2C_MASTER_SDA_IO,
                                               I2C_MASTER_SCL_IO,
                                               true,
                                               &s_axp_bus,
                                               &s_axp_bus_owned);
  if (err != ESP_OK) {
    printf("ERROR [%s]: Failed to create/get I2C bus: %s\n", __func__,
           esp_err_to_name(err));
    return err;
  }

  err = i2c_shared_add_device(s_axp_bus, AXP2101_I2C_ADDR, I2C_MASTER_FREQ_HZ, &s_axp_dev);
  if (err != ESP_OK) {
    printf("ERROR [%s]: Failed to attach AXP2101 device: %s\n", __func__,
           esp_err_to_name(err));
    return err;
  }

  i2c_initialized = true;
  printf("INFO [%s]: AXP2101 initialized successfully\n", __func__);
  return ESP_OK;
}

esp_err_t axp2101_deinit(void) {
  if (!i2c_initialized) {
    printf("WARNING [%s]: AXP2101 is not initialized\n", __func__);
    return ESP_OK;
  }

  esp_err_t err = ESP_OK;
  if (s_axp_dev) {
    err = i2c_master_bus_rm_device(s_axp_dev);
    if (err != ESP_OK) {
      printf("ERROR [%s]: Failed to remove AXP2101 device: %s\n", __func__,
             esp_err_to_name(err));
      return err;
    }
    s_axp_dev = NULL;
  }
  if (s_axp_bus_owned && s_axp_bus) {
    err = i2c_del_master_bus(s_axp_bus);
    if (err != ESP_OK) {
      printf("ERROR [%s]: Failed to delete I2C bus: %s\n", __func__,
             esp_err_to_name(err));
      return err;
    }
    s_axp_bus = NULL;
    s_axp_bus_owned = false;
  }

  i2c_initialized = false;
  printf("INFO [%s]: AXP2101 deinitialized successfully\n", __func__);
  return ESP_OK;
}

esp_err_t axp2101_get_power_level(uint8_t *power_level) {
  if (!i2c_initialized) {
    printf("ERROR [%s]: AXP2101 is not initialized\n", __func__);
    return ESP_ERR_INVALID_STATE;
  }

  if (power_level == NULL) {
    printf("ERROR [%s]: Invalid parameter: power_level is NULL\n", __func__);
    return ESP_ERR_INVALID_ARG;
  }

  uint8_t reg_addr = AXP2101_REG_POWER_LEVEL;
  uint8_t data = 0;

  esp_err_t err = i2c_master_transmit_receive(s_axp_dev, &reg_addr, 1, &data, 1, 100);
  if (err != ESP_OK) {
    printf("ERROR [%s]: Failed to read from AXP2101 register 0x%02X: %s\n",
           __func__, reg_addr, esp_err_to_name(err));
    return err;
  }

  if (!(data & BIT_MASK(7))) {
    *power_level = data & (~BIT_MASK(7));
    printf("INFO [%s]: Battery percentage: %d%%\n", __func__, *power_level);
    return ESP_OK;
  } else {
    printf("WARNING [%s]: Battery percentage value is invalid (data: 0x%02X)\n",
           __func__, data);
    return ESP_ERR_INVALID_RESPONSE;
  }
}
