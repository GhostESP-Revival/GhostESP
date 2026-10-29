/* Attaky board bring-up.
 *
 * Everything here is gated on CONFIG_ATTAKY_TARGET so the file compiles to
 * nothing on every other board profile.
 *
 * Pin / address facts (Attaky Cyber Deck Plus 1.0 datasheet + public hardware
 * catalog; the two agree on every module-level fact used here):
 *
 *   Shared main I2C bus   SCL IO1, SDA IO2, 400 kHz
 *                         0x38 FT6636 touch         (Core, always)
 *                         0x48 STK3311-X light      (Core, unused by this port)
 *                         0x58 AW9523 interrupt aggregator (Core, always)
 *                         0x59 AW9523 buttons/LED/CTP reset (Core, always)
 *                         0x6B QMI8658A IMU         (Core, accelerometer app)
 *                         0x36 MAX17048 gauge       (Power module)
 *                         0x5A/0x5B AW9523 keyboard (Keyboard module)
 *                         0x22/0x26 TCA9555 gamepad (Gamepad module)
 *
 * AW9523B P1 bank carries P10-P17; on the Core's 0x59 those are the RGB LED
 * (P10 red / P11 green / P12 blue, active low) and the touch reset CTP_RESET
 * (P13, active low). They must stay outputs driving high - see the P1 note on
 * the button map in components/io_manager/io_manager.c.
 *
 * The AW9523B drives its P-pins at power-on (the exact pattern follows the
 * AD0/AD1 strap), so every instance gets a software reset followed by explicit
 * direction writes before anything reads it.
 */

#include "vendor/attaky/attaky_board.h"

#if defined(CONFIG_ATTAKY_TARGET)

#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_shared.h"

static const char *TAG = "ATTAKY";

/* ---- Bus ---------------------------------------------------------- */
#define ATTAKY_I2C_PORT       0
#define ATTAKY_I2C_SDA        CONFIG_I2C_MANAGER_0_SDA
#define ATTAKY_I2C_SCL        CONFIG_I2C_MANAGER_0_SCL
#define ATTAKY_I2C_HZ         400000
#define ATTAKY_I2C_TIMEOUT_MS 100

/* ---- Device addresses --------------------------------------------- */
#define ATTAKY_ADDR_TOUCH    0x38
#define ATTAKY_ADDR_AGG      0x58
#define ATTAKY_ADDR_BUTTONS  0x59
#define ATTAKY_ADDR_GAUGE    0x36
#define ATTAKY_ADDR_KB_LEFT  0x5A
#define ATTAKY_ADDR_KB_RIGHT 0x5B
#define ATTAKY_ADDR_GP_LEFT  0x22
#define ATTAKY_ADDR_GP_RIGHT 0x26

/* ---- AW9523B registers -------------------------------------------- */
#define AW9523_REG_OUTPUT_P0   0x02
#define AW9523_REG_OUTPUT_P1   0x03
#define AW9523_REG_CONFIG_P0   0x04
#define AW9523_REG_CONFIG_P1   0x05
#define AW9523_REG_ID          0x10
#define AW9523_REG_LEDMODE_P0  0x12
#define AW9523_REG_LEDMODE_P1  0x13
#define AW9523_REG_SOFT_RESET  0x7F
#define AW9523_ID_VALUE        0x23

/* Core 0x59: P0 buttons (all inputs), P1 = LED R/G/B + CTP reset (outputs,
 * driven high = off/released) with P14-P17 left as inputs. */
#define ATTAKY_BTN_DIR_P0      0xFF
#define ATTAKY_BTN_DIR_P1      0xF0
#define ATTAKY_BTN_OUT_P0      0xFF
#define ATTAKY_BTN_OUT_P1      0x0F
#define ATTAKY_CTP_RESET_BIT   0x08   /* P13 -> bit 3 of the P1 register */

static i2c_master_bus_handle_t s_bus = NULL;
static bool s_present[ATTAKY_MODULE_COUNT];
static bool s_initialized = false;

static esp_err_t reg_write(uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t payload[2] = {reg, val};
    return i2c_shared_transmit_to_addr(s_bus, addr, ATTAKY_I2C_HZ, payload,
                                       sizeof(payload), ATTAKY_I2C_TIMEOUT_MS);
}

static esp_err_t reg_read(uint8_t addr, uint8_t reg, uint8_t *val)
{
    return i2c_shared_transmit_receive_from_addr(s_bus, addr, ATTAKY_I2C_HZ,
                                                 &reg, 1, val, 1,
                                                 ATTAKY_I2C_TIMEOUT_MS);
}

/* True when the device answers at all (ACK on a 1-byte read). */
static bool addr_acks(uint8_t addr)
{
    uint8_t scratch = 0;
    return i2c_shared_transmit_receive_from_addr(s_bus, addr, ATTAKY_I2C_HZ,
                                                 &scratch, 1, &scratch, 1,
                                                 ATTAKY_I2C_TIMEOUT_MS) == ESP_OK;
}

/* Configure one AW9523B: software reset, GPIO (not LED-sink) mode on both
 * banks, explicit directions, explicit output values. */
static esp_err_t aw9523_configure(uint8_t addr, uint8_t dir_p0, uint8_t dir_p1,
                                  uint8_t out_p0, uint8_t out_p1)
{
    esp_err_t ret = reg_write(addr, AW9523_REG_SOFT_RESET, 0x00);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(2));   /* reset settle; datasheet gives no figure */

    /* 0xFF = GPIO mode on every pin. The two mode registers are written
     * together because sources disagree on which bank sits in 0x12 vs 0x13
     * (the catalog puts P0 in 0x12; the driver-notes doc swaps them); writing
     * both makes the outcome independent of that. */
    if (reg_write(addr, AW9523_REG_LEDMODE_P0, 0xFF) != ESP_OK ||
        reg_write(addr, AW9523_REG_LEDMODE_P1, 0xFF) != ESP_OK) {
        ESP_LOGW(TAG, "0x%02X: LEDMODE write failed", addr);
    }

    if (reg_write(addr, AW9523_REG_CONFIG_P0, dir_p0) != ESP_OK ||
        reg_write(addr, AW9523_REG_CONFIG_P1, dir_p1) != ESP_OK) {
        ESP_LOGE(TAG, "0x%02X: direction write failed", addr);
        return ESP_FAIL;
    }
    if (reg_write(addr, AW9523_REG_OUTPUT_P0, out_p0) != ESP_OK ||
        reg_write(addr, AW9523_REG_OUTPUT_P1, out_p1) != ESP_OK) {
        ESP_LOGE(TAG, "0x%02X: output write failed", addr);
        return ESP_FAIL;
    }

    uint8_t id = 0;
    if (reg_read(addr, AW9523_REG_ID, &id) == ESP_OK && id != AW9523_ID_VALUE) {
        ESP_LOGW(TAG, "0x%02X: expander ID 0x%02X (expected 0x%02X)", addr, id,
                 AW9523_ID_VALUE);
    }
    return ESP_OK;
}

esp_err_t attaky_board_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    memset(s_present, 0, sizeof(s_present));

    bool created = false;
    esp_err_t ret = i2c_shared_get_or_create_bus(ATTAKY_I2C_PORT, ATTAKY_I2C_SDA,
                                                 ATTAKY_I2C_SCL, true, &s_bus,
                                                 &created);
    if (ret != ESP_OK || s_bus == NULL) {
        ESP_LOGE(TAG, "shared I2C bus unavailable on SDA=%d SCL=%d: %s",
                 ATTAKY_I2C_SDA, ATTAKY_I2C_SCL, esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "main I2C bus %s (SDA=%d SCL=%d)", created ? "created" : "reused",
             ATTAKY_I2C_SDA, ATTAKY_I2C_SCL);

    /* Core interrupt aggregator (0x58): every pin is an input. Its P-pins
     * power up driving, and several of them are wired to the open-drain
     * interrupt outputs of other chips, so leaving them as outputs would fight
     * those lines and read as a stuck-low bus. */
    s_present[ATTAKY_MODULE_TOUCH] = addr_acks(ATTAKY_ADDR_TOUCH);

    if (addr_acks(ATTAKY_ADDR_AGG)) {
        if (aw9523_configure(ATTAKY_ADDR_AGG, 0xFF, 0xFF, 0x00, 0xFF) == ESP_OK) {
            s_present[ATTAKY_MODULE_EXPANDER_AGG] = true;
        }
    } else {
        ESP_LOGW(TAG, "Core expander 0x58 did not answer");
    }

    /* Core buttons / RGB LED / touch reset (0x59). */
    if (addr_acks(ATTAKY_ADDR_BUTTONS)) {
        if (aw9523_configure(ATTAKY_ADDR_BUTTONS, ATTAKY_BTN_DIR_P0,
                             ATTAKY_BTN_DIR_P1, ATTAKY_BTN_OUT_P0,
                             ATTAKY_BTN_OUT_P1) == ESP_OK) {
            s_present[ATTAKY_MODULE_EXPANDER_BTN] = true;
        }
    } else {
        ESP_LOGE(TAG, "Core expander 0x59 did not answer; buttons will not work");
    }

    /* Capacitive touch reset is driven from expander 0x59 P13 and is
     * active-low. Pulse it low, then release, before display_manager's
     * touch_driver_init() talks to the controller at 0x38. */
    if (s_present[ATTAKY_MODULE_EXPANDER_BTN]) {
        uint8_t out = 0;
        if (reg_read(ATTAKY_ADDR_BUTTONS, AW9523_REG_OUTPUT_P1, &out) == ESP_OK) {
            reg_write(ATTAKY_ADDR_BUTTONS, AW9523_REG_OUTPUT_P1,
                      (uint8_t)(out & ~ATTAKY_CTP_RESET_BIT));
            vTaskDelay(pdMS_TO_TICKS(10));
            reg_write(ATTAKY_ADDR_BUTTONS, AW9523_REG_OUTPUT_P1,
                      (uint8_t)(out | ATTAKY_CTP_RESET_BIT));
            vTaskDelay(pdMS_TO_TICKS(60));   /* controller boot time */
            ESP_LOGI(TAG, "touch reset released (expander 0x59 P13)");
        } else {
            ESP_LOGW(TAG, "could not read 0x59 output register to pulse the touch reset");
        }
    }

    /* Optional modules: presence only. Each module's own driver configures its
     * chips (the keyboard needs row-drive/column-sense directions, the gamepad
     * only needs inputs). */
#if defined(CONFIG_ATTAKY_HAS_POWER)
    s_present[ATTAKY_MODULE_FUEL_GAUGE] = addr_acks(ATTAKY_ADDR_GAUGE);
#endif
#if defined(CONFIG_ATTAKY_HAS_KEYBOARD)
    {
        bool left = addr_acks(ATTAKY_ADDR_KB_LEFT);
        bool right = addr_acks(ATTAKY_ADDR_KB_RIGHT);
        s_present[ATTAKY_MODULE_KEYBOARD] = left && right;
        if (left != right) {
            ESP_LOGW(TAG, "Keyboard halves disagree: 0x5A=%s 0x5B=%s",
                     left ? "ok" : "absent", right ? "ok" : "absent");
        }
    }
#endif
#if defined(CONFIG_ATTAKY_HAS_GAMEPAD)
    {
        bool left = addr_acks(ATTAKY_ADDR_GP_LEFT);
        bool right = addr_acks(ATTAKY_ADDR_GP_RIGHT);
        s_present[ATTAKY_MODULE_GAMEPAD] = left && right;
        if (left != right) {
            ESP_LOGW(TAG, "Gamepad halves disagree: 0x22=%s 0x26=%s",
                     left ? "ok" : "absent", right ? "ok" : "absent");
        }
    }
#endif

    s_initialized = true;
    return s_present[ATTAKY_MODULE_EXPANDER_BTN] ? ESP_OK : ESP_FAIL;
}

bool attaky_board_module_present(attaky_module_t module)
{
    if ((int)module < 0 || module >= ATTAKY_MODULE_COUNT) {
        return false;
    }
    return s_present[module];
}

void *attaky_board_i2c_bus(void)
{
    return (void *)s_bus;
}

const char *attaky_milestone_name(int milestone)
{
    switch (milestone) {
        case 1: return "boot";
        case 2: return "display";
        case 3: return "storage";
        case 4: return "core-buttons";
        case 5: return "keyboard";
        case 6: return "gamepad";
        case 7: return "radio-audio";
        case 8: return "power-sensors";
        default: return "boot";
    }
}

void attaky_board_emit_banner(void)
{
    /* Chain-integration shim: the exact lines firmware-deploy matches to prove
     * the ported app booted, followed by the milestone marker on its own line. */
#if defined(CONFIG_ATTAKY_MILESTONE)
    int milestone = CONFIG_ATTAKY_MILESTONE;
#else
    int milestone = 0;
#endif
    printf("\n");
    printf("[INFO] [APP] %s v%s on %s\n", ATTAKY_PORT_FIRMWARE_NAME,
           ATTAKY_PORT_VERSION, ATTAKY_PORT_BUILD_SKU);
    printf("[INFO] [APP] Build: %s %s / toolchain: %s\n", __DATE__, __TIME__,
           ATTAKY_PORT_TOOLCHAIN);
    printf("[INFO] [APP] milestone: M%d (%s)\n", milestone,
           attaky_milestone_name(milestone));
    printf("[INFO] [APP] upstream: GhostESP v2.2.2 (GPL-3.0) \n");
    fflush(stdout);
}

void attaky_board_emit_presence(void)
{
    printf("[INFO] [HW] attaky: display=st7789 touch=%s exp=0x58:%s/0x59:%s "
           "gauge=%s keyboard=%s gamepad=%s\n",
           s_present[ATTAKY_MODULE_TOUCH] ? "0x38" : "absent",
           s_present[ATTAKY_MODULE_EXPANDER_AGG] ? "ok" : "absent",
           s_present[ATTAKY_MODULE_EXPANDER_BTN] ? "ok" : "absent",
           s_present[ATTAKY_MODULE_FUEL_GAUGE] ? "0x36" : "absent",
           s_present[ATTAKY_MODULE_KEYBOARD] ? "0x5A+0x5B" : "absent",
           s_present[ATTAKY_MODULE_GAMEPAD] ? "0x22+0x26" : "absent");
    printf("[INFO] [HW] attaky: media-io storage is mounted by the SD manager "
           "(SPI3 then SPI2); a Core-only device logs it absent and keeps running\n");
    fflush(stdout);
}

#endif /* CONFIG_ATTAKY_TARGET */
