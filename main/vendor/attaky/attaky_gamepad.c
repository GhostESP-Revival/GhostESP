/* Control: Gamepad (Bottom) 1.0 - two TCA9555 expanders.
 *
 * Button->pin map, from the Gamepad (Bottom) datasheet (the catalog agrees):
 *
 *   left half @ 0x22            right half @ 0x26
 *   P00 FN_L1                   P00 R2 (top + front)
 *   P01 FN_L2                   P01 R1 (top + front)
 *   P02 Down                    P02 X
 *   P03 Right                   P03 A
 *   P04 Left                    P04 B
 *   P05 Up                      P05 Y
 *   P06 L1  (top + front)       P06 FN_R2
 *   P07 L2  (top + front)       P07 FN_R1
 *
 * Every button is active-low with the expander's internal pull-up; L1/L2/R1/R2
 * each have two physical buttons wired to one pin, so they read as one input.
 * Per-half RGB LEDs sit on P15/P16/P17 (red/green/blue, active low). This
 * driver configures those three pins as outputs and leaves them dark; the
 * colour is owned by attaky_rgb, which writes the port without touching the
 * button inputs.
 */

#include "vendor/attaky/attaky_gamepad.h"

#if defined(CONFIG_ATTAKY_TARGET) && defined(CONFIG_ATTAKY_HAS_GAMEPAD)

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_shared.h"
#include "managers/display_manager.h"
#include "vendor/attaky/attaky_board.h"
#include "vendor/m5/m5_keyboard_def.h"

static const char *TAG = "ATTAKY_GP";

#define GP_I2C_HZ         400000
#define GP_I2C_TIMEOUT_MS 100
#define GP_ADDR_LEFT      0x22
#define GP_ADDR_RIGHT     0x26
#define GP_SCAN_MS        15

/* TCA9555 registers. */
#define TCA9555_REG_INPUT_P0  0x00
#define TCA9555_REG_OUTPUT_P0 0x02
#define TCA9555_REG_OUTPUT_P1 0x03
#define TCA9555_REG_CONFIG_P0 0x06
#define TCA9555_REG_CONFIG_P1 0x07

/* Joystick indices the display manager's navigation path expects. */
#define GP_JOY_LEFT   0
#define GP_JOY_SELECT 1
#define GP_JOY_UP     2
#define GP_JOY_RIGHT  3
#define GP_JOY_DOWN   4

/* Back key accepted by the shared view helpers (LV_KEY_ESC). */
#define GP_KEY_BACK   27

typedef enum {
    GP_OUT_JOYSTICK,
    GP_OUT_KEYBOARD,
} gp_output_t;

typedef struct {
    uint8_t half;      /* 0 = left (0x22), 1 = right (0x26) */
    uint8_t bit;       /* bit inside that half's P0 input byte */
    gp_output_t output;
    uint8_t value;     /* joystick index or key code */
    const char *name;
} gp_button_t;

/* Mapped buttons. Anything not listed is read but emitted as a debug log
 * (see kGpUnmapped) until a feature claims it. */
static const gp_button_t kGpButtons[] = {
    {0, 5, GP_OUT_JOYSTICK, GP_JOY_UP,     "Up"},
    {0, 2, GP_OUT_JOYSTICK, GP_JOY_DOWN,   "Down"},
    {0, 4, GP_OUT_JOYSTICK, GP_JOY_LEFT,   "Left"},
    {0, 3, GP_OUT_JOYSTICK, GP_JOY_RIGHT,  "Right"},
    {1, 3, GP_OUT_JOYSTICK, GP_JOY_SELECT, "A"},
    {1, 4, GP_OUT_KEYBOARD, GP_KEY_BACK,   "B"},
    {1, 2, GP_OUT_KEYBOARD, KEY_TAB,       "X"},
    {1, 5, GP_OUT_KEYBOARD, KEY_BACKSPACE, "Y"},
    {1, 1, GP_OUT_KEYBOARD, KEY_ENTER,     "R1"},
};

static const struct {
    uint8_t half;
    uint8_t bit;
    const char *name;
} kGpUnmapped[] = {
    {0, 6, "L1"}, {0, 7, "L2"}, {1, 0, "R2"},
    {0, 0, "FN_L1"}, {0, 1, "FN_L2"}, {1, 6, "FN_R2"}, {1, 7, "FN_R1"},
};

static const uint8_t kAddrs[2] = {GP_ADDR_LEFT, GP_ADDR_RIGHT};

static i2c_master_bus_handle_t s_bus = NULL;
static TaskHandle_t s_task = NULL;
static bool s_active = false;

static esp_err_t gp_write(uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t payload[2] = {reg, val};
    return i2c_shared_transmit_to_addr(s_bus, addr, GP_I2C_HZ, payload,
                                       sizeof(payload), GP_I2C_TIMEOUT_MS);
}

/* One burst read returns P0 and P1 (registers auto-increment). */
static esp_err_t gp_read_inputs(uint8_t addr, uint8_t *p0, uint8_t *p1)
{
    uint8_t reg = TCA9555_REG_INPUT_P0;
    uint8_t values[2] = {0xFF, 0xFF};
    esp_err_t ret = i2c_shared_transmit_receive_from_addr(s_bus, addr, GP_I2C_HZ,
                                                          &reg, 1, values,
                                                          sizeof(values),
                                                          GP_I2C_TIMEOUT_MS);
    if (ret == ESP_OK) {
        *p0 = values[0];
        *p1 = values[1];
    }
    return ret;
}

static void gp_push_joystick(uint8_t index, bool pressed)
{
    InputEvent ev = {0};
    ev.type = INPUT_TYPE_JOYSTICK;
    ev.data.joystick_index = index;
    ev.data.joystick_pressed = pressed;
    if (input_queue == NULL) {
        return;
    }
    xQueueSend((QueueHandle_t)input_queue, &ev, 0);
}

static void gp_push_key(uint8_t value, bool pressed)
{
    InputEvent ev = {0};
    ev.type = INPUT_TYPE_KEYBOARD;
    ev.data.key_value = value;
    ev.is_touch_move = !pressed;   /* upstream convention: set = release */
    if (input_queue == NULL) {
        return;
    }
    xQueueSend((QueueHandle_t)input_queue, &ev, 0);
}

static void gp_emit(uint8_t half, const gp_button_t *btn, bool pressed)
{
    if (btn->output == GP_OUT_JOYSTICK) {
        gp_push_joystick(btn->value, pressed);
    } else {
        gp_push_key(btn->value, pressed);
    }
    if (pressed) {
        ESP_LOGI(TAG, "%s (%s) pressed", btn->name, half == 0 ? "0x22" : "0x26");
    } else {
        ESP_LOGD(TAG, "%s released", btn->name);
    }
}

static void attaky_gamepad_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(GP_SCAN_MS);
    uint8_t prev[2] = {0xFF, 0xFF};
    bool first = true;

    while (1) {
        uint8_t now[2] = {prev[0], prev[1]};
        bool ok = true;
        for (int i = 0; i < 2; i++) {
            uint8_t p0 = 0xFF, p1 = 0xFF;
            if (gp_read_inputs(kAddrs[i], &p0, &p1) != ESP_OK) {
                ok = false;
            }
            now[i] = p0;
        }
        if (!ok) {
            vTaskDelay(period);
            continue;
        }
        if (first) {
            /* Adopt the current state as the baseline: a button held while the
             * device boots must not look like a press. */
            prev[0] = now[0];
            prev[1] = now[1];
            first = false;
            vTaskDelay(period);
            continue;
        }

        for (size_t i = 0; i < sizeof(kGpButtons) / sizeof(kGpButtons[0]); i++) {
            const gp_button_t *btn = &kGpButtons[i];
            const uint8_t mask = (uint8_t)(1u << btn->bit);
            const uint8_t was = prev[btn->half] & mask;
            const uint8_t is = now[btn->half] & mask;
            if (was == is) {
                continue;
            }
            gp_emit(btn->half, btn, is == 0);   /* active low */
        }

        for (size_t i = 0; i < sizeof(kGpUnmapped) / sizeof(kGpUnmapped[0]); i++) {
            const uint8_t mask = (uint8_t)(1u << kGpUnmapped[i].bit);
            const uint8_t was = prev[kGpUnmapped[i].half] & mask;
            const uint8_t is = now[kGpUnmapped[i].half] & mask;
            if (was != is && is == 0) {
                ESP_LOGD(TAG, "%s pressed (no key mapping yet)", kGpUnmapped[i].name);
            }
        }

        prev[0] = now[0];
        prev[1] = now[1];
        vTaskDelay(period);
    }
}

esp_err_t attaky_gamepad_init(void)
{
    if (s_active) {
        return ESP_OK;
    }
    s_bus = (i2c_master_bus_handle_t)attaky_board_i2c_bus();
    if (s_bus == NULL) {
        ESP_LOGE(TAG, "shared I2C bus not available");
        return ESP_ERR_INVALID_STATE;
    }
    if (!attaky_board_module_present(ATTAKY_MODULE_GAMEPAD)) {
        ESP_LOGI(TAG, "no Gamepad module attached; skipping gamepad driver");
        return ESP_ERR_NOT_FOUND;
    }

    for (int i = 0; i < 2; i++) {
        /* P0 is the button bank: all eight pins inputs (the module's own
         * pull-ups hold them high). P1 carries the per-half RGB LED on
         * P15/P16/P17 (blue/green/red); those three are outputs driving high,
         * which is LED off - left as inputs they would float and light
         * unpredictably. P10-P14 are unused and stay inputs. */
        if (gp_write(kAddrs[i], TCA9555_REG_CONFIG_P0, 0xFF) != ESP_OK ||
            gp_write(kAddrs[i], TCA9555_REG_CONFIG_P1, 0x1F) != ESP_OK ||
            gp_write(kAddrs[i], TCA9555_REG_OUTPUT_P0, 0xFF) != ESP_OK ||
            gp_write(kAddrs[i], TCA9555_REG_OUTPUT_P1, 0xE0) != ESP_OK) {
            ESP_LOGE(TAG, "half @ 0x%02X failed to configure", kAddrs[i]);
            return ESP_FAIL;
        }
    }

    if (xTaskCreate(attaky_gamepad_task, "attaky_gp", 4096, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to start gamepad task");
        return ESP_ERR_NO_MEM;
    }
    s_active = true;
    ESP_LOGI(TAG, "Gamepad module ready: halves @ 0x22/0x26, %d ms poll", GP_SCAN_MS);
    return ESP_OK;
}

bool attaky_gamepad_active(void)
{
    return s_active;
}

#endif /* CONFIG_ATTAKY_TARGET && CONFIG_ATTAKY_HAS_GAMEPAD */
