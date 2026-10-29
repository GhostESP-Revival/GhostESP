/* Status LEDs on the Attaky modules that the Core does not own.
 *
 * Five RGB status LEDs exist in a complete build. The Core's own one hangs off
 * the button expander @ 0x59 and is owned by io_manager (it has to share that
 * expander's output shadow with the touch-reset line). This file drives the
 * remaining four, which belong to the modules:
 *
 *   Keyboard, left/right zone   AW9523B @ 0x5A / 0x5B    P17 red, P16 green, P15 blue
 *   Gamepad,  left/right half   TCA9555 @ 0x22 / 0x26    P17 red, P16 green, P15 blue
 *
 * All of them are active-low: the pin sinks and the channel lights.
 *
 * Two hardware facts decide how this is implemented.
 *
 * 1. The AW9523B has a constant-current LED mode with a 256-step dimming
 *    register per pin, so those two sites get real 8-bit brightness. Its DIM
 *    registers are NOT in pin order - see aw9523_dim_reg() below, which follows
 *    "Table 13, 256 step dimming control" (the same layout Adafruit's driver
 *    uses: pins 0-7 -> 0x24+, pins 8-11 -> 0x20+, pins 12-15 -> 0x2C+).
 *    In that mode the brightness comes from DIM, so the pin's output bit is
 *    held low (sink enabled) and "off" is simply DIM = 0.
 *
 * 2. The TCA9555 has no PWM of any kind: its LED pins are plain outputs. Those
 *    two sites are therefore quantised to on/off, and any fade the caller asks
 *    for shows up as the colour stepping over time.
 *
 * The keyboard's scan task owns the P1 output register of 0x5A/0x5B because
 * that is where its matrix rows live. It keeps the three LED bits clear (see
 * attaky_keyboard.c), so this file only ever writes the DIM registers there
 * and never has to fight the scanner for the port.
 */

#include "vendor/attaky/attaky_rgb.h"

#if defined(CONFIG_ATTAKY_TARGET)

#include <stddef.h>

#include "esp_log.h"
#include "i2c_shared.h"
#include "vendor/attaky/attaky_board.h"

static const char *TAG = "ATTAKY_RGB";

#define RGB_I2C_HZ          400000
#define RGB_I2C_TIMEOUT_MS  50

/* Module expander addresses (system-wide table, from the datasheet). */
#define KB_ADDR_LEFT   0x5A
#define KB_ADDR_RIGHT  0x5B
#define GP_ADDR_LEFT   0x22
#define GP_ADDR_RIGHT  0x26

/* AW9523B registers this driver touches. */
#define AW_REG_ID        0x10
#define AW_ID_VALUE      0x23
#define AW_REG_DIM_BLUE  0x2D   /* pin 13 = P15 */
#define AW_REG_DIM_GREEN 0x2E   /* pin 14 = P16 */
#define AW_REG_DIM_RED   0x2F   /* pin 15 = P17 */

/* TCA9555 registers: input port 0 for an ACK, output/config port 1 for the LED. */
#define TCA_REG_INPUT_P0   0x00
#define TCA_REG_OUTPUT_P1  0x03
#define TCA_REG_CONFIG_P1  0x07

/* P17 red, P16 green, P15 blue, active low. Pulling the bit high turns the
 * channel off, which is also how the gamepad driver leaves the expanders. */
#define TCA_LED_MASK   0xE0u
#define TCA_LED_OFF    0xE0u

/* Below roughly a quarter scale the LED reads as off rather than dim; the
 * TCA9555 has nothing in between, so this only picks which side of the line a
 * fading colour lands on. */
#define TCA_ON_LEVEL   0x40u

typedef struct {
    uint8_t addr;
    attaky_module_t module;
    bool dimmable;      /* true = AW9523B constant current, false = TCA9555 */
    bool present;
    uint8_t dim[3];     /* last written red/green/blue (dimmable sites only) */
} rgb_site_t;

static rgb_site_t s_sites[] = {
    {.addr = KB_ADDR_LEFT,  .module = ATTAKY_MODULE_KEYBOARD, .dimmable = true},
    {.addr = KB_ADDR_RIGHT, .module = ATTAKY_MODULE_KEYBOARD, .dimmable = true},
    {.addr = GP_ADDR_LEFT,  .module = ATTAKY_MODULE_GAMEPAD,  .dimmable = false},
    {.addr = GP_ADDR_RIGHT, .module = ATTAKY_MODULE_GAMEPAD,  .dimmable = false},
};

#define SITE_COUNT (sizeof(s_sites) / sizeof(s_sites[0]))

static i2c_master_bus_handle_t s_bus = NULL;
static bool s_probed = false;
static int s_present = 0;

static esp_err_t rgb_write(uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t payload[2] = {reg, val};
    return i2c_shared_transmit_to_addr(s_bus, addr, RGB_I2C_HZ, payload,
                                       sizeof(payload), RGB_I2C_TIMEOUT_MS);
}

static esp_err_t rgb_read(uint8_t addr, uint8_t reg, uint8_t *val)
{
    return i2c_shared_transmit_receive_from_addr(s_bus, addr, RGB_I2C_HZ, &reg, 1,
                                                 val, 1, RGB_I2C_TIMEOUT_MS);
}

/* Probe once: which module LEDs actually answered, and leave every one of them
 * dark so a fresh boot does not flash colour before the first colour is set. */
static void rgb_probe(void)
{
    if (s_probed) {
        return;
    }
    s_probed = true;

    s_bus = (i2c_master_bus_handle_t)attaky_board_i2c_bus();
    if (s_bus == NULL) {
        ESP_LOGW(TAG, "shared I2C bus not available; module LEDs stay off");
        return;
    }

    for (size_t i = 0; i < SITE_COUNT; i++) {
        rgb_site_t *site = &s_sites[i];
        if (!attaky_board_module_present(site->module)) {
            continue;
        }

        uint8_t value = 0;
        if (site->dimmable) {
            if (rgb_read(site->addr, AW_REG_ID, &value) != ESP_OK) {
                ESP_LOGD(TAG, "0x%02X: no answer, LED skipped", site->addr);
                continue;
            }
            if (value != AW_ID_VALUE) {
                ESP_LOGW(TAG, "0x%02X: unexpected expander ID 0x%02X", site->addr, value);
            }
            /* Dark first: after the mode switch the DIM registers carry the
             * brightness, and their power-on value is not zero. */
            esp_err_t err = rgb_write(site->addr, AW_REG_DIM_RED, 0);
            if (err == ESP_OK) err = rgb_write(site->addr, AW_REG_DIM_GREEN, 0);
            if (err == ESP_OK) err = rgb_write(site->addr, AW_REG_DIM_BLUE, 0);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "0x%02X: LED dark-on-boot write failed: %s",
                         site->addr, esp_err_to_name(err));
                continue;
            }
            site->dim[0] = site->dim[1] = site->dim[2] = 0;
        } else {
            if (rgb_read(site->addr, TCA_REG_INPUT_P0, &value) != ESP_OK) {
                ESP_LOGD(TAG, "0x%02X: no answer, LED skipped", site->addr);
                continue;
            }
            /* P15..P17 must be outputs or the LED pins float. The gamepad
             * driver already configures them that way; repeat it here so the
             * order of the two inits cannot leave them undriven. */
            if (rgb_read(site->addr, TCA_REG_CONFIG_P1, &value) != ESP_OK) {
                continue;
            }
            uint8_t config = (uint8_t)(value & ~TCA_LED_MASK);
            if (config != value) {
                if (rgb_write(site->addr, TCA_REG_CONFIG_P1, config) != ESP_OK) {
                    continue;
                }
            }
            if (rgb_write(site->addr, TCA_REG_OUTPUT_P1, TCA_LED_OFF) != ESP_OK) {
                continue;
            }
        }

        site->present = true;
        s_present++;
    }

    ESP_LOGI(TAG, "%d module status LED(s) online", s_present);
}

esp_err_t attaky_rgb_set(uint8_t red, uint8_t green, uint8_t blue)
{
    rgb_probe();
    if (s_present == 0) {
        return ESP_OK;   /* nothing attached: not an error */
    }

    esp_err_t result = ESP_OK;

    for (size_t i = 0; i < SITE_COUNT; i++) {
        rgb_site_t *site = &s_sites[i];
        if (!site->present) {
            continue;
        }

        if (site->dimmable) {
            const uint8_t wanted[3] = {red, green, blue};
            const uint8_t regs[3] = {AW_REG_DIM_RED, AW_REG_DIM_GREEN, AW_REG_DIM_BLUE};
            for (int ch = 0; ch < 3; ch++) {
                if (site->dim[ch] == wanted[ch]) {
                    continue;   /* unchanged: keep the bus quiet during effects */
                }
                esp_err_t err = rgb_write(site->addr, regs[ch], wanted[ch]);
                if (err == ESP_OK) {
                    site->dim[ch] = wanted[ch];
                } else {
                    result = err;
                }
            }
            continue;
        }

        /* TCA9555: no dimming, so each channel is lit or dark. */
        uint8_t value = TCA_LED_OFF;
        if (red >= TCA_ON_LEVEL)   value = (uint8_t)(value & ~(1u << 7));   /* P17 */
        if (green >= TCA_ON_LEVEL) value = (uint8_t)(value & ~(1u << 6));   /* P16 */
        if (blue >= TCA_ON_LEVEL)  value = (uint8_t)(value & ~(1u << 5));   /* P15 */

        /* Read back instead of caching: the gamepad driver writes this port
         * once at init, and a cached copy would skip a needed re-assert if the
         * two inits ever ran in the other order. */
        uint8_t current = 0;
        if (rgb_read(site->addr, TCA_REG_OUTPUT_P1, &current) != ESP_OK) {
            esp_err_t err = rgb_write(site->addr, TCA_REG_OUTPUT_P1, value);
            if (err != ESP_OK) {
                result = err;
            }
            continue;
        }
        if ((current & TCA_LED_MASK) == (value & TCA_LED_MASK)) {
            continue;
        }
        uint8_t next = (uint8_t)((current & (uint8_t)~TCA_LED_MASK) | (value & TCA_LED_MASK));
        esp_err_t err = rgb_write(site->addr, TCA_REG_OUTPUT_P1, next);
        if (err != ESP_OK) {
            result = err;
        }
    }

    return result;
}

#endif /* CONFIG_ATTAKY_TARGET */
