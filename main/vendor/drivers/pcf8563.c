#include "vendor/drivers/pcf8563.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "RTC_DRIVER";

static i2c_port_t rtc_i2c_port;
static uint8_t rtc_address;
static rtc_chip_type_t rtc_chip;

static uint8_t _bcd_to_dec(uint8_t val) {
  return ((val / 16 * 10) + (val % 16));
}

static uint8_t _dec_to_bcd(uint8_t val) {
  return ((val / 10 * 16) + (val % 10));
}

// Generic RTC initialization
esp_err_t rtc_init(i2c_port_t i2c_port, uint8_t addr, rtc_chip_type_t chip_type) {
  rtc_i2c_port = i2c_port;
  rtc_address = addr;
  rtc_chip = chip_type;
  
  ESP_LOGI(TAG, "RTC initialized: chip=%d, addr=0x%02X, port=%d", chip_type, addr, i2c_port);
  
  // For DS1307, ensure clock is running (clear CH bit)
  if (chip_type == RTC_CHIP_DS1307 || chip_type == RTC_CHIP_DS3231) {
    uint8_t sec_reg;
    esp_err_t ret = i2c_master_write_read_device(rtc_i2c_port, rtc_address, 
                                                  (uint8_t[]){DS1307_SEC_REG}, 1, 
                                                  &sec_reg, 1, pdMS_TO_TICKS(1000));
    if (ret == ESP_OK && (sec_reg & DS1307_CH_MASK)) {
      // Clear clock halt bit
      sec_reg &= ~DS1307_CH_MASK;
      uint8_t data[] = {DS1307_SEC_REG, sec_reg};
      i2c_master_write_to_device(rtc_i2c_port, rtc_address, data, sizeof(data), pdMS_TO_TICKS(1000));
      ESP_LOGI(TAG, "DS1307 clock started");
    }
  }
  
  return ESP_OK;
}

// Legacy PCF8563 initialization for backward compatibility
esp_err_t pcf8563_init(i2c_port_t i2c_port, uint8_t addr) {
  return rtc_init(i2c_port, addr, RTC_CHIP_PCF8563);
}

static esp_err_t _read_register(uint8_t reg, uint8_t *data, size_t len) {
  return i2c_master_write_read_device(rtc_i2c_port, rtc_address, &reg, 1, data,
                                      len, pdMS_TO_TICKS(1000));
}

static esp_err_t _write_register(uint8_t reg, const uint8_t *data, size_t len) {
  uint8_t buffer[1 + len];
  buffer[0] = reg;
  memcpy(&buffer[1], data, len);
  return i2c_master_write_to_device(rtc_i2c_port, rtc_address, buffer,
                                    sizeof(buffer), pdMS_TO_TICKS(1000));
}

esp_err_t rtc_set_datetime(const RTC_Date *datetime) {
  if (rtc_chip == RTC_CHIP_PCF8563) {
    uint8_t data[7];
    data[0] = _dec_to_bcd(datetime->second) & ~PCF8563_VOL_LOW_MASK;
    data[1] = _dec_to_bcd(datetime->minute);
    data[2] = _dec_to_bcd(datetime->hour);
    data[3] = _dec_to_bcd(datetime->day);
    data[4] = _dec_to_bcd(datetime->month) |
              ((datetime->year < 2000) ? PCF8563_CENTURY_MASK : 0);
    data[5] = _dec_to_bcd(datetime->year % 100);
    data[6] = 0; // weekday (not used)
    return _write_register(PCF8563_SEC_REG, data, 7);
  } else {
    // DS1307/DS3231
    uint8_t data[7];
    data[0] = _dec_to_bcd(datetime->second) & ~DS1307_CH_MASK;
    data[1] = _dec_to_bcd(datetime->minute);
    data[2] = _dec_to_bcd(datetime->hour);
    data[3] = _dec_to_bcd(datetime->day % 7); // DS1307 uses day of week (1-7)
    data[4] = _dec_to_bcd(datetime->day);
    data[5] = _dec_to_bcd(datetime->month);
    data[6] = _dec_to_bcd(datetime->year % 100);
    return _write_register(DS1307_SEC_REG, data, 7);
  }
}

esp_err_t rtc_get_datetime(RTC_Date *datetime) {
  if (rtc_chip == RTC_CHIP_PCF8563) {
    uint8_t data[7];
    esp_err_t ret = _read_register(PCF8563_SEC_REG, data, 7);
    if (ret != ESP_OK) {
      return ret;
    }

    bool century = data[5] & PCF8563_CENTURY_MASK;
    datetime->second = _bcd_to_dec(data[0] & ~PCF8563_VOL_LOW_MASK);
    datetime->minute = _bcd_to_dec(data[1] & PCF8563_MINUTES_MASK);
    datetime->hour = _bcd_to_dec(data[2] & PCF8563_HOUR_MASK);
    datetime->day = _bcd_to_dec(data[3] & PCF8563_DAY_MASK);
    datetime->month = _bcd_to_dec(data[4] & PCF8563_MONTH_MASK);
    datetime->year = (century ? 1900 : 2000) + _bcd_to_dec(data[5]);
  } else {
    uint8_t data[7];
    esp_err_t ret = _read_register(DS1307_SEC_REG, data, 7);
    if (ret != ESP_OK) {
      return ret;
    }

    datetime->second = _bcd_to_dec(data[0] & ~DS1307_CH_MASK);
    datetime->minute = _bcd_to_dec(data[1]);
    datetime->hour = _bcd_to_dec(data[2] & 0x3F);
    datetime->day = _bcd_to_dec(data[4]);
    datetime->month = _bcd_to_dec(data[5] & 0x1F);
    datetime->year = 2000 + _bcd_to_dec(data[6]);
  }
  return ESP_OK;
}

esp_err_t rtc_set_alarm(const RTC_Alarm *alarm) {
  if (rtc_chip == RTC_CHIP_PCF8563) {
    uint8_t data[4];
    data[0] = (alarm->minute != 0xFF)
                  ? _dec_to_bcd(alarm->minute) & ~PCF8563_VOL_LOW_MASK
                  : PCF8563_VOL_LOW_MASK;
    data[1] = (alarm->hour != 0xFF)
                  ? _dec_to_bcd(alarm->hour) & ~PCF8563_VOL_LOW_MASK
                  : PCF8563_VOL_LOW_MASK;
    data[2] = (alarm->day != 0xFF)
                  ? _dec_to_bcd(alarm->day) & ~PCF8563_VOL_LOW_MASK
                  : PCF8563_VOL_LOW_MASK;
    data[3] = (alarm->weekday != 0xFF)
                  ? _dec_to_bcd(alarm->weekday) & ~PCF8563_VOL_LOW_MASK
                  : PCF8563_VOL_LOW_MASK;
    return _write_register(PCF8563_ALRM_MIN_REG, data, 4);
  } else {
    // DS1307/DS3231 alarm functionality is more complex and not implemented
    ESP_LOGW(TAG, "Alarm not implemented for DS1307/DS3231");
    return ESP_ERR_NOT_SUPPORTED;
  }
}

esp_err_t rtc_get_alarm(RTC_Alarm *alarm) {
  if (rtc_chip == RTC_CHIP_PCF8563) {
    uint8_t data[4];
    esp_err_t ret = _read_register(PCF8563_ALRM_MIN_REG, data, 4);
    if (ret != ESP_OK) {
      return ret;
    }

    alarm->minute = (data[0] != PCF8563_VOL_LOW_MASK)
                        ? _bcd_to_dec(data[0] & PCF8563_MINUTES_MASK)
                        : 0xFF;
    alarm->hour = (data[1] != PCF8563_VOL_LOW_MASK)
                      ? _bcd_to_dec(data[1] & PCF8563_HOUR_MASK)
                      : 0xFF;
    alarm->day = (data[2] != PCF8563_VOL_LOW_MASK)
                    ? _bcd_to_dec(data[2] & PCF8563_DAY_MASK)
                    : 0xFF;
    alarm->weekday = (data[3] != PCF8563_VOL_LOW_MASK)
                        ? _bcd_to_dec(data[3] & PCF8563_WEEKDAY_MASK)
                        : 0xFF;
    return ESP_OK;
  } else {
    ESP_LOGW(TAG, "Alarm not implemented for DS1307/DS3231");
    return ESP_ERR_NOT_SUPPORTED;
  }
}

esp_err_t rtc_enable_alarm() {
  if (rtc_chip == RTC_CHIP_PCF8563) {
    uint8_t data;
    esp_err_t ret = _read_register(PCF8563_STAT2_REG, &data, 1);
    if (ret != ESP_OK) {
      return ret;
    }
    data |= (1 << 1);
    return _write_register(PCF8563_STAT2_REG, &data, 1);
  } else {
    ESP_LOGW(TAG, "Alarm not implemented for DS1307/DS3231");
    return ESP_ERR_NOT_SUPPORTED;
  }
}

esp_err_t rtc_disable_alarm() {
  if (rtc_chip == RTC_CHIP_PCF8563) {
    uint8_t data;
    esp_err_t ret = _read_register(PCF8563_STAT2_REG, &data, 1);
    if (ret != ESP_OK) {
      return ret;
    }
    data &= ~(1 << 1);
    return _write_register(PCF8563_STAT2_REG, &data, 1);
  } else {
    ESP_LOGW(TAG, "Alarm not implemented for DS1307/DS3231");
    return ESP_ERR_NOT_SUPPORTED;
  }
}

esp_err_t rtc_check_voltage_low(bool *voltage_low) {
  if (rtc_chip == RTC_CHIP_PCF8563) {
    uint8_t data;
    esp_err_t ret = _read_register(PCF8563_SEC_REG, &data, 1);
    if (ret != ESP_OK) {
      return ret;
    }
    *voltage_low = data& PCF8563_VOL_LOW_MASK;
    return ESP_OK;
  } else {
    *voltage_low = false;
    return ESP_OK;
  }
}

// Legacy functions for backward compatibility
esp_err_t pcf8563_set_datetime(const RTC_Date *datetime) {
  return rtc_set_datetime(datetime);
}

esp_err_t pcf8563_get_datetime(RTC_Date *datetime) {
  return rtc_get_datetime(datetime);
}

esp_err_t pcf8563_set_alarm(const RTC_Alarm *alarm) {
  return rtc_set_alarm(alarm);
}

esp_err_t pcf8563_get_alarm(RTC_Alarm *alarm) {
  return rtc_get_alarm(alarm);
}

esp_err_t pcf8563_enable_alarm(void) {
  return rtc_enable_alarm();
}

esp_err_t pcf8563_disable_alarm(void) {
  return rtc_disable_alarm();
}

esp_err_t pcf8563_check_voltage_low(bool *voltage_low) {
  return rtc_check_voltage_low(voltage_low);
}