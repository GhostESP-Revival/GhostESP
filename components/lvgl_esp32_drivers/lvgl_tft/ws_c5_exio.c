/**
 * @file ws_c5_exio.c
 *
 * Waveshare ESP32-C5-Touch-LCD-3.5 I/O expander bring-up. See ws_c5_exio.h.
 */
#include "ws_c5_exio.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_i2c/i2c_manager.h"

#define TAG "WS_C5_EXIO"

#define EXIO_ADDR       0x24
#define EXIO_MODE_REG   0x02  /* direction / mode */
#define EXIO_OUTPUT_REG 0x03  /* output latch */
#define EXIO_PWM_REG    0x05  /* backlight PWM */

#define EXIO_TP_RST     0     /* FT6336 reset, active low */
#define EXIO_LCD_RST    1     /* ST7796 reset, active low */
#define EXIO_POWER      5     /* panel power / backlight enable */

#define EXIO_SD_CS_GPIO 9     /* microSD CS on the shared display SPI bus */

static uint8_t s_output;
static bool s_done;
static esp_err_t s_result = ESP_FAIL;

static esp_err_t exio_write(uint8_t reg, uint8_t value)
{
    return lvgl_i2c_write(CONFIG_LV_I2C_TOUCH_PORT, EXIO_ADDR, reg, &value, 1);
}

static esp_err_t exio_set(uint8_t bit, bool level)
{
    if (level) {
        s_output |= (uint8_t)(1U << bit);
    } else {
        s_output &= (uint8_t)~(1U << bit);
    }
    return exio_write(EXIO_OUTPUT_REG, s_output);
}

#define EXIO_STEP(expr, ms)                     \
    do {                                        \
        if (ret == ESP_OK) ret = (expr);        \
        if ((ms) > 0) vTaskDelay(pdMS_TO_TICKS(ms)); \
    } while (0)

esp_err_t ws_c5_exio_init(void)
{
    if (s_done) {
        return s_result;
    }
    s_done = true;

    /* Keep the SD card deselected while the panel comes up on the shared bus. */
    gpio_reset_pin(EXIO_SD_CS_GPIO);
    gpio_set_direction(EXIO_SD_CS_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(EXIO_SD_CS_GPIO, 1);

    esp_err_t ret = lvgl_i2c_init(CONFIG_LV_I2C_TOUCH_PORT);
    const uint8_t used = (uint8_t)((1U << EXIO_TP_RST) | (1U << EXIO_LCD_RST) | (1U << EXIO_POWER));

    /* Bring the rail up together with both resets released; pulsing the touch
     * reset while its rail is off leaves the FT6336 unresponsive. */
    EXIO_STEP(exio_write(EXIO_MODE_REG, 0xFF), 0);
    EXIO_STEP(exio_write(EXIO_OUTPUT_REG, 0x00), 0);
    EXIO_STEP(exio_write(EXIO_PWM_REG, 0x00), 0);
    EXIO_STEP(exio_write(EXIO_MODE_REG, used), 0);

    s_output = (uint8_t)((1U << EXIO_TP_RST) | (1U << EXIO_LCD_RST));
    EXIO_STEP(exio_write(EXIO_OUTPUT_REG, s_output), 50);
    s_output = 0x00;
    EXIO_STEP(exio_write(EXIO_OUTPUT_REG, s_output), 50);
    s_output = used;
    EXIO_STEP(exio_write(EXIO_OUTPUT_REG, s_output), 50);

    /* Power is on; pulse each reset with the rail up. */
    EXIO_STEP(exio_set(EXIO_LCD_RST, false), 20);
    EXIO_STEP(exio_set(EXIO_LCD_RST, true), 120);
    EXIO_STEP(exio_set(EXIO_TP_RST, false), 20);
    EXIO_STEP(exio_set(EXIO_TP_RST, true), 300);  /* FT6336 boot */
    EXIO_STEP(exio_write(EXIO_PWM_REG, 0xFF), 0);

    s_result = ret;
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "I/O expander 0x%02x ready", EXIO_ADDR);
    } else {
        ESP_LOGE(TAG, "I/O expander 0x%02x bring-up failed: %s", EXIO_ADDR, esp_err_to_name(ret));
    }
    return ret;
}

bool ws_c5_exio_ready(void)
{
    return s_done && s_result == ESP_OK;
}

esp_err_t ws_c5_exio_set_backlight(uint8_t duty)
{
    if (!ws_c5_exio_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    return exio_write(EXIO_PWM_REG, duty);
}
