#include "vendor/drivers/qmi8658.h"

#if defined(CONFIG_ACCEL_USE_QMI8658) && defined(CONFIG_ACCEL_USE_BMI270)
#error "CONFIG_ACCEL_USE_BMI270 and CONFIG_ACCEL_USE_QMI8658 are both set: a board carries one accelerometer, and the accelerometer screen compiles the BMI270 path first. Turn one off in sdkconfig."
#endif

#ifdef CONFIG_ACCEL_USE_QMI8658

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus_lock.h"
#include <stdbool.h>

static const char *TAG = "QMI8658";

#ifndef CONFIG_ACCEL_I2C_PORT
#define CONFIG_ACCEL_I2C_PORT 0
#endif
#ifndef CONFIG_ACCEL_I2C_ADDR
#define CONFIG_ACCEL_I2C_ADDR 0x6B
#endif

#define QMI8658_I2C_PORT    CONFIG_ACCEL_I2C_PORT
#define QMI8658_I2C_ADDR    CONFIG_ACCEL_I2C_ADDR
#define QMI8658_I2C_FREQ_HZ 400000

/* Register map (subset), QMI8658A datasheet rev A. */
#define QMI8658_REG_WHO_AM_I 0x00
#define QMI8658_REG_CTRL1    0x02
#define QMI8658_REG_CTRL2    0x03
#define QMI8658_REG_CTRL3    0x04
#define QMI8658_REG_CTRL5    0x06
#define QMI8658_REG_CTRL7    0x08
#define QMI8658_REG_STATUS0  0x2E
#define QMI8658_REG_AX_L     0x35
#define QMI8658_REG_RESET    0x60

#define QMI8658_WHO_AM_I_VALUE 0x05
#define QMI8658_RESET_CMD      0xB0

/* CTRL1: bit6 ADDR_AI - register address auto-increment, without which the
 * 6-byte burst read repeats one byte. Bits 7 and 5:0 stay 0: bit5 would flip
 * the read data to big-endian and bit0 (SensorDisable) would stop the
 * internal high-speed oscillator and freeze the data registers. The accel and
 * gyro are enabled through CTRL7, not here. */
#define QMI8658_CTRL1_ADDR_AI 0x40

/* CTRL2/CTRL3: bits[6:4] full scale, bits[3:0] output data rate. */
#define QMI8658_CTRL2_ACC_FS_4G     0x10  /* aFS 001 = +/-4 G (8192 LSB/G) */
#define QMI8658_CTRL2_ACC_ODR_125HZ 0x06  /* aODR 0110 = 125 Hz */
#define QMI8658_CTRL3_GYR_FS_512DPS 0x50  /* gFS 101 = +/-512 dps */
#define QMI8658_CTRL3_GYR_ODR_112HZ 0x06  /* gODR 0110 = 112.1 Hz */

/* CTRL5 = 0x00 disables both low-pass filters. Deliberate: the filter's
 * default 2.66 %-of-ODR bandwidth is a few Hz, which smooths a 5-15 Hz shake
 * into the noise floor. */
#define QMI8658_CTRL5_LPF_OFF 0x00

/* CTRL7: bit0 aEN, bit1 gEN. */
#define QMI8658_CTRL7_ACC_EN 0x01
#define QMI8658_CTRL7_GYR_EN 0x02

/* Accel burst: AX_L/AX_H .. AZ_L/AZ_H, little-endian int16 per axis. */
#define QMI8658_ACCEL_BYTES 6

static i2c_master_dev_handle_t s_dev;
static bool s_initialized;

static esp_err_t qmi8658_device(void)
{
    if (s_dev) return ESP_OK;
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t ret = i2c_master_get_bus_handle(QMI8658_I2C_PORT, &bus);
    if (ret != ESP_OK) return ret;
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMI8658_I2C_ADDR,
        .scl_speed_hz = QMI8658_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(bus, &cfg, &s_dev);
}

static esp_err_t qmi8658_write_reg(uint8_t reg, uint8_t value)
{
    esp_err_t ret = qmi8658_device();
    if (ret != ESP_OK) return ret;
    if (!i2c_bus_lock(QMI8658_I2C_PORT, 100)) return ESP_ERR_TIMEOUT;
    uint8_t buf[2] = {reg, value};
    ret = i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
    i2c_bus_unlock(QMI8658_I2C_PORT);
    return ret;
}

static esp_err_t qmi8658_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = qmi8658_device();
    if (ret != ESP_OK) return ret;
    if (!i2c_bus_lock(QMI8658_I2C_PORT, 100)) return ESP_ERR_TIMEOUT;
    ret = i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 100);
    i2c_bus_unlock(QMI8658_I2C_PORT);
    return ret;
}

static esp_err_t qmi8658_read_accel_raw(int16_t *x, int16_t *y, int16_t *z)
{
    uint8_t data[QMI8658_ACCEL_BYTES];
    esp_err_t ret = qmi8658_read_regs(QMI8658_REG_AX_L, data, sizeof(data));
    if (ret != ESP_OK) return ret;
    *x = (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
    *y = (int16_t)((uint16_t)data[2] | ((uint16_t)data[3] << 8));
    *z = (int16_t)((uint16_t)data[4] | ((uint16_t)data[5] << 8));
    return ESP_OK;
}

esp_err_t qmi8658_init(void)
{
    if (s_initialized) return ESP_OK;

    uint8_t who = 0;
    if (qmi8658_read_regs(QMI8658_REG_WHO_AM_I, &who, 1) != ESP_OK ||
        who != QMI8658_WHO_AM_I_VALUE) {
        ESP_LOGE(TAG, "QMI8658A not found (WHO_AM_I: 0x%02X)", who);
        return ESP_ERR_NOT_FOUND;
    }

    /* Soft reset, then wait out the datasheet's 15 ms turn-on time. */
    if (qmi8658_write_reg(QMI8658_REG_RESET, QMI8658_RESET_CMD) != ESP_OK) {
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    if (qmi8658_write_reg(QMI8658_REG_CTRL1, QMI8658_CTRL1_ADDR_AI) != ESP_OK ||
        qmi8658_write_reg(QMI8658_REG_CTRL2,
                          QMI8658_CTRL2_ACC_FS_4G |
                              QMI8658_CTRL2_ACC_ODR_125HZ) != ESP_OK ||
        qmi8658_write_reg(QMI8658_REG_CTRL3,
                          QMI8658_CTRL3_GYR_FS_512DPS |
                              QMI8658_CTRL3_GYR_ODR_112HZ) != ESP_OK ||
        qmi8658_write_reg(QMI8658_REG_CTRL5, QMI8658_CTRL5_LPF_OFF) != ESP_OK ||
        qmi8658_write_reg(QMI8658_REG_CTRL7,
                          QMI8658_CTRL7_ACC_EN | QMI8658_CTRL7_GYR_EN) != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658A configuration write failed");
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(100));   /* first conversion */

    /* Liveness check: the output registers only move once the part is really
     * converting. Two byte-identical reads a conversion apart mean it is not
     * (a live sensor always shows LSB noise), so treat that as not found
     * instead of letting the UI display a frozen value. */
    int16_t a1[3] = {0};
    int16_t a2[3] = {0};
    if (qmi8658_read_accel_raw(&a1[0], &a1[1], &a1[2]) != ESP_OK) {
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(40));
    if (qmi8658_read_accel_raw(&a2[0], &a2[1], &a2[2]) != ESP_OK) {
        return ESP_FAIL;
    }
    if (a1[0] == a2[0] && a1[1] == a2[1] && a1[2] == a2[2]) {
        ESP_LOGE(TAG, "QMI8658A data registers are frozen (%d/%d/%d) - "
                      "accelerometer not converting",
                 a2[0], a2[1], a2[2]);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t status = 0;
    if (qmi8658_read_regs(QMI8658_REG_STATUS0, &status, 1) == ESP_OK &&
        !(status & 0x01)) {
        ESP_LOGW(TAG, "QMI8658A STATUS0 aDA not set (0x%02X)", status);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "QMI8658A initialized: +/-4 G @ 125 Hz, LPF off (WHO_AM_I 0x%02X)",
             who);
    return ESP_OK;
}

esp_err_t qmi8658_read_accel(int16_t *x, int16_t *y, int16_t *z)
{
    if (!x || !y || !z) return ESP_ERR_INVALID_ARG;
    return qmi8658_read_accel_raw(x, y, z);
}

#else
esp_err_t qmi8658_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t qmi8658_read_accel(int16_t *x, int16_t *y, int16_t *z)
{
    (void)x; (void)y; (void)z;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
