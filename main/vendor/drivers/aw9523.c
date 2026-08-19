/* AW9523B GPIO expander on the M5Stack CoreS3 family.
 *
 * The grove ports (PORT.A/B/C) are not fed from the battery rail directly:
 * an SY7088 boost converter supplies their 5V pins, gated by two expander
 * outputs. P1_7 (BOOST_EN) turns the converter on, P0_1 (BUS_EN) connects it
 * to the ports. Both are inputs after reset, so a unit plugged into PORT.A is
 * unpowered and never ACKs - which reads as a stuck bus (probe timeouts on
 * the loaded port, instant "no devices" on the empty ones).
 *
 * Bit assignments follow M5Unified's Power_Class::_core_s3_output(). Only the
 * two rail bits are touched; the display/touch/audio pins on the same
 * expander keep whatever state the board left them in. */

#include "vendor/drivers/aw9523.h"

#include "i2c_shared.h"
#include <esp_log.h>
#include <string.h>

static const char *TAG = "aw9523";

#define AW9523_I2C_ADDR 0x58
#define AW9523_I2C_FREQ_HZ 400000
#define AW9523_I2C_TIMEOUT_MS 100

#define AW9523_REG_OUTPUT_P0 0x02
#define AW9523_REG_OUTPUT_P1 0x03
#define AW9523_REG_CONFIG_P0 0x04 /* 1 = input, 0 = output */
#define AW9523_REG_CONFIG_P1 0x05
#define AW9523_REG_ID 0x10        /* reads 0x23 */
#define AW9523_REG_GCR 0x11       /* bit4 = P0 push-pull */
#define AW9523_REG_LEDMODE_P0 0x12 /* 1 = GPIO, 0 = LED current sink */
#define AW9523_REG_LEDMODE_P1 0x13

#define AW9523_ID_VALUE 0x23
#define AW9523_P0_BUS_EN (1 << 1)
#define AW9523_P1_BOOST_EN (1 << 7)
#define AW9523_GCR_P0_PUSH_PULL (1 << 4)

/* Internal I2C bus. The CoreS3 pin table (M5Unified _pin_table_i2c_ex_in) is
 * SCL=G11, SDA=G12; the board profile mirrors it in I2C_MANAGER_0. */
#define AW9523_I2C_PORT 0
#ifdef CONFIG_I2C_MANAGER_0_SDA
#define AW9523_SDA_PIN CONFIG_I2C_MANAGER_0_SDA
#else
#define AW9523_SDA_PIN 12
#endif
#ifdef CONFIG_I2C_MANAGER_0_SCL
#define AW9523_SCL_PIN CONFIG_I2C_MANAGER_0_SCL
#else
#define AW9523_SCL_PIN 11
#endif

static i2c_master_bus_handle_t s_bus = NULL;
static bool s_present = false;

static bool aw9523_board_has_expander(void) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    return strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "m5cores3se") == 0;
#else
    return false;
#endif
}

static esp_err_t aw9523_read_reg(uint8_t reg, uint8_t *val) {
    return i2c_shared_transmit_receive_from_addr(s_bus, AW9523_I2C_ADDR,
                                                 AW9523_I2C_FREQ_HZ, &reg, 1,
                                                 val, 1, AW9523_I2C_TIMEOUT_MS);
}

static esp_err_t aw9523_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_shared_transmit_to_addr(s_bus, AW9523_I2C_ADDR,
                                       AW9523_I2C_FREQ_HZ, buf, sizeof(buf),
                                       AW9523_I2C_TIMEOUT_MS);
}

/* Read-modify-write a single register so neighbouring pins are preserved. */
static esp_err_t aw9523_update_reg(uint8_t reg, uint8_t mask, bool set) {
    uint8_t cur = 0;
    esp_err_t err = aw9523_read_reg(reg, &cur);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t next = set ? (uint8_t)(cur | mask) : (uint8_t)(cur & ~mask);
    if (next == cur) {
        return ESP_OK;
    }
    return aw9523_write_reg(reg, next);
}

esp_err_t aw9523_init(void) {
    if (s_present) {
        return ESP_OK;
    }
    if (!aw9523_board_has_expander()) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    bool created = false;
    esp_err_t err = i2c_shared_get_or_create_bus(AW9523_I2C_PORT, AW9523_SDA_PIN,
                                                 AW9523_SCL_PIN, true, &s_bus,
                                                 &created);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Internal I2C bus %d unavailable: %s", AW9523_I2C_PORT,
                 esp_err_to_name(err));
        s_bus = NULL;
        return err;
    }

    uint8_t id = 0;
    err = aw9523_read_reg(AW9523_REG_ID, &id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No AW9523 at 0x%02X on I2C%d (SDA=%d, SCL=%d): %s",
                 AW9523_I2C_ADDR, AW9523_I2C_PORT, AW9523_SDA_PIN,
                 AW9523_SCL_PIN, esp_err_to_name(err));
        return err;
    }
    if (id != AW9523_ID_VALUE) {
        ESP_LOGW(TAG, "Unexpected AW9523 ID 0x%02X (expected 0x%02X)", id,
                 AW9523_ID_VALUE);
    }

    /* GPIO mode (not LED sink) and push-pull for the two rail pins. P1 is
     * push-pull by hardware; only P0 needs the GCR bit. */
    err = aw9523_update_reg(AW9523_REG_LEDMODE_P0, AW9523_P0_BUS_EN, true);
    if (err == ESP_OK) {
        err = aw9523_update_reg(AW9523_REG_LEDMODE_P1, AW9523_P1_BOOST_EN, true);
    }
    if (err == ESP_OK) {
        err = aw9523_update_reg(AW9523_REG_GCR, AW9523_GCR_P0_PUSH_PULL, true);
    }
    /* Direction last: clearing the config bit makes the pin an output, which
     * immediately drives whatever is already in the output register. */
    if (err == ESP_OK) {
        err = aw9523_update_reg(AW9523_REG_CONFIG_P0, AW9523_P0_BUS_EN, false);
    }
    if (err == ESP_OK) {
        err = aw9523_update_reg(AW9523_REG_CONFIG_P1, AW9523_P1_BOOST_EN, false);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AW9523 configuration failed: %s", esp_err_to_name(err));
        return err;
    }

    s_present = true;
    ESP_LOGI(TAG, "AW9523 ready (ID 0x%02X) on I2C%d (SDA=%d, SCL=%d)", id,
             AW9523_I2C_PORT, AW9523_SDA_PIN, AW9523_SCL_PIN);
    return ESP_OK;
}

esp_err_t aw9523_set_port_5v(bool enable) {
    esp_err_t err = aw9523_init();
    if (err != ESP_OK) {
        return err;
    }

    if (enable) {
        /* Boost first, then connect it to the ports. */
        err = aw9523_update_reg(AW9523_REG_OUTPUT_P1, AW9523_P1_BOOST_EN, true);
        if (err == ESP_OK) {
            err = aw9523_update_reg(AW9523_REG_OUTPUT_P0, AW9523_P0_BUS_EN, true);
        }
    } else {
        err = aw9523_update_reg(AW9523_REG_OUTPUT_P0, AW9523_P0_BUS_EN, false);
        if (err == ESP_OK) {
            err = aw9523_update_reg(AW9523_REG_OUTPUT_P1, AW9523_P1_BOOST_EN, false);
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to %s grove port 5V: %s",
                 enable ? "enable" : "disable", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Grove port 5V rail %s", enable ? "enabled" : "disabled");
    return ESP_OK;
}

bool aw9523_is_present(void) { return s_present; }
