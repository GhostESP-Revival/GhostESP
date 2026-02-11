#include "sdkconfig.h"
#include "managers/status_display_manager.h"

#ifdef CONFIG_WITH_STATUS_DISPLAY

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

#include <string.h>
#include <ctype.h>
#include <stdio.h>

#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/task.h"
#include "i2c_bus_lock.h"
#include "managers/settings_manager.h"
#include "managers/status_display_animations.h"

static esp_err_t status_display_send(uint8_t control, const uint8_t *data, size_t len);

#define STATUS_DISPLAY_I2C_PORT CONFIG_STATUS_DISPLAY_I2C_PORT
#define STATUS_DISPLAY_ADDR CONFIG_STATUS_DISPLAY_I2C_ADDRESS
#define STATUS_CMD 0x00
#define STATUS_DATA 0x40

#if CONFIG_STATUS_DISPLAY_ROTATE_180
#define STATUS_SEGMENT_REMAP_CMD 0xA1
#define STATUS_COM_SCAN_CMD 0xC8
#else
#define STATUS_SEGMENT_REMAP_CMD 0xA0
#define STATUS_COM_SCAN_CMD 0xC0
#endif

static const char *TAG = "StatusDisplay";

static SemaphoreHandle_t s_mutex;
static bool s_ready;
static bool s_i2c_configured;
static bool s_i2c_installed;
static uint8_t *s_buffer;
#define STATUS_BUFFER_SIZE (128 * 8)
static char s_line1[24];
static char s_line2[24];
static const int SCALE_Y = 2; // simple vertical scaling factor
#if defined(CONFIG_USE_IO_EXPANDER)
static TickType_t s_next_flush_allowed_tick;
static const TickType_t STATUS_DISPLAY_MIN_FLUSH_INTERVAL_TICKS = pdMS_TO_TICKS(200);
#endif
// idle animation settings
static TimerHandle_t s_idle_timer;
static TickType_t s_last_update_tick;
static const TickType_t ANIM_INTERVAL_TICKS = pdMS_TO_TICKS(150);
static int s_anim_frame;
static TaskHandle_t s_anim_task;
static TickType_t s_next_anim_allowed_tick;
static TickType_t s_oom_backoff_until;
static bool s_oom_logged;
// static int s_i2c_error_streak; // unused

static const uint8_t font_5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5f,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7f,0x14,0x7f,0x14}, {0x24,0x2a,0x7f,0x2a,0x12}, {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1c,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1c,0x00}, {0x14,0x08,0x3e,0x08,0x14}, {0x08,0x08,0x3e,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02}, {0x3e,0x51,0x49,0x45,0x3e}, {0x00,0x42,0x7f,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4b,0x31}, {0x18,0x14,0x12,0x7f,0x10},
    {0x27,0x45,0x45,0x45,0x39}, {0x3c,0x4a,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1e}, {0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00}, {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06}, {0x32,0x49,0x79,0x41,0x3e},
    {0x7e,0x11,0x11,0x11,0x7e}, {0x7f,0x49,0x49,0x49,0x36}, {0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c}, {0x7f,0x49,0x49,0x49,0x41}, {0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a}, {0x7f,0x08,0x08,0x08,0x7f}, {0x00,0x41,0x7f,0x41,0x00},
    {0x20,0x40,0x41,0x3f,0x01}, {0x7f,0x08,0x14,0x22,0x41}, {0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f}, {0x7f,0x04,0x08,0x10,0x7f}, {0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,0x09,0x09,0x09,0x06}, {0x3e,0x41,0x51,0x21,0x5e}, {0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7f,0x01,0x01}, {0x3f,0x40,0x40,0x40,0x3f},
    {0x1f,0x20,0x40,0x20,0x1f}, {0x3f,0x40,0x38,0x40,0x3f}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7f,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7f,0x00}, {0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40}, {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78},
    {0x7f,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7f},
    {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7e,0x09,0x01,0x02}, {0x0c,0x52,0x52,0x52,0x3e},
    {0x7f,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7d,0x40,0x00}, {0x20,0x40,0x44,0x3d,0x00},
    {0x7f,0x10,0x28,0x44,0x00}, {0x00,0x41,0x7f,0x40,0x00}, {0x7c,0x04,0x18,0x04,0x78},
    {0x7c,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, {0x7c,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7c}, {0x7c,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3f,0x44,0x40,0x20}, {0x3c,0x40,0x40,0x20,0x7c}, {0x1c,0x20,0x40,0x20,0x1c},
    {0x3c,0x40,0x30,0x40,0x3c}, {0x44,0x28,0x10,0x28,0x44}, {0x0c,0x50,0x50,0x50,0x3c},
    {0x44,0x64,0x54,0x4c,0x44}, {0x00,0x08,0x36,0x41,0x00}, {0x00,0x00,0x7f,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00}, {0x10,0x08,0x08,0x10,0x08}, {0x00,0x06,0x09,0x09,0x06}
};

static esp_err_t status_display_send(uint8_t control, const uint8_t *data, size_t len) {
    if (!data || !len) return ESP_OK;
    TickType_t now = xTaskGetTickCount();
    if (s_oom_backoff_until && now < s_oom_backoff_until) {
        return ESP_ERR_NO_MEM;
    }
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) {
        if (!s_oom_logged) {
            ESP_LOGW(TAG, "i2c_cmd_link_create failed (OOM), backing off");
            s_oom_logged = true;
        }
        s_oom_backoff_until = now + pdMS_TO_TICKS(2000);
        return ESP_ERR_NO_MEM;
    }
    s_oom_backoff_until = 0;
    s_oom_logged = false;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (STATUS_DISPLAY_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, control, true);
    i2c_master_write(cmd, (uint8_t *)data, len, true);
    i2c_master_stop(cmd);
    bool locked = i2c_bus_lock(STATUS_DISPLAY_I2C_PORT, 120);
    if (!locked) {
        i2c_cmd_link_delete(cmd);
        ESP_LOGD(TAG, "status display i2c busy, skipping ctrl=0x%02X", control);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_cmd_begin(STATUS_DISPLAY_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_bus_unlock(STATUS_DISPLAY_I2C_PORT);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "i2c write failed ctrl=0x%02X len=%u err=%s", control, (unsigned)len, esp_err_to_name(err));
    } else {
        ESP_LOGD(TAG, "i2c write ok ctrl=0x%02X len=%u", control, (unsigned)len);
    }
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t status_display_write_command(uint8_t command) {
    return status_display_send(STATUS_CMD, &command, 1);
}

static void status_display_flush(void) {
    if (!s_buffer) return;
#if defined(CONFIG_USE_IO_EXPANDER)
    TickType_t now = xTaskGetTickCount();
    if (now < s_next_flush_allowed_tick) {
        return;
    }
    s_next_flush_allowed_tick = now + STATUS_DISPLAY_MIN_FLUSH_INTERVAL_TICKS;
#endif
    for (uint8_t page = 0; page < 8; ++page) {
        uint8_t setup[] = { (uint8_t)(0xB0 | page), 0x00, 0x10 };
        if (status_display_send(STATUS_CMD, setup, sizeof(setup)) != ESP_OK) return;
        const uint8_t *chunk = &s_buffer[page * 128];
        if (status_display_send(STATUS_DATA, chunk, 128) != ESP_OK) return;
#if defined(CONFIG_USE_IO_EXPANDER)
        // brief pause lets other IO expander clients grab the bus between 1kB bursts
        vTaskDelay(pdMS_TO_TICKS(5));
#endif
    }
}

static void status_display_clear_buffer(void) {
    if (s_buffer) memset(s_buffer, 0, STATUS_BUFFER_SIZE);
}

static void status_display_plot_pixel(int x, int y, bool on) {
    if (!s_buffer || x < 0 || x >= 128 || y < 0 || y >= 64) return;
    int index = x + (y / 8) * 128;
    uint8_t bit = 1u << (y & 7);
    if (on) s_buffer[index] |= bit; else s_buffer[index] &= (uint8_t)~bit;
}

static void status_display_draw_char(int x, int y, char c) {
    if (c < 32 || c > 126) c = ' ';
    const uint8_t *glyph = font_5x7[(int)c - 32];
    for (int col = 0; col < 5; ++col) {
        uint8_t column = glyph[col];
        for (int row = 0; row < 7; ++row) {
            bool on = (column >> row) & 0x01;
            // scale vertically by drawing multiple rows per bit
            for (int sy = 0; sy < SCALE_Y; ++sy) {
                status_display_plot_pixel(x + col, y + row * SCALE_Y + sy, on);
            }
        }
    }
}

static void status_display_draw_char_rot90_right(int x, int y, char c)
{
    if (c < 32 || c > 126) c = ' ';
    const uint8_t *glyph = font_5x7[(int)c - 32];
    const int w = 5;
    const int h = 7;
    for (int col = 0; col < w; ++col) {
        uint8_t column = glyph[col];
        for (int row = 0; row < h; ++row) {
            bool on = (column >> row) & 0x01;
            if (!on) continue;
            int X = x + (h - 1 - row);
            int Y = y + col * SCALE_Y;
            for (int sy = 0; sy < SCALE_Y; ++sy) {
                status_display_plot_pixel(X, Y + sy, true);
            }
        }
    }
}

static void status_display_draw_text(int x, int y, const char *text) {
    int cursor = x;
    while (*text && cursor < 128 - 6) {
        status_display_draw_char(cursor, y, *text);
        cursor += 6;
        ++text;
    }
}

static void status_display_render_locked(const char *line_one, const char *line_two) {
    status_display_clear_buffer();
    int len1 = (int)strlen(line_one);
    int len2 = (int)strlen(line_two);
    int w1 = len1 * 6;
    int w2 = len2 * 6;
    if (w1 > 128) w1 = 128;
    if (w2 > 128) w2 = 128;
    int x1 = (128 - w1) / 2;
    int x2 = (128 - w2) / 2;
    if (x1 < 0) x1 = 0;
    if (x2 < 0) x2 = 0;
    int line_height = 7 * SCALE_Y;
    int gap = 6;
    int total_h = line_height * 2 + gap;
    int y_base = (64 - total_h) / 2;
    if (y_base < 0) y_base = 0;
    status_display_draw_text(x1, y_base, line_one);
    status_display_draw_text(x2, y_base + line_height + gap, line_two);
    status_display_flush();
}

static void status_display_render(const char *line_one, const char *line_two) {
    if (!s_ready) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    status_display_render_locked(line_one, line_two);
    xSemaphoreGive(s_mutex);
}

static void anim_clear(void *user)
{
    (void)user;
    status_display_clear_buffer();
}

static void anim_flush(void *user)
{
    (void)user;
    status_display_flush();
}

static void anim_plot_pixel(void *user, int x, int y, bool on)
{
    (void)user;
    status_display_plot_pixel(x, y, on);
}

static void anim_draw_text(void *user, int x, int y, const char *text)
{
    (void)user;
    status_display_draw_text(x, y, text);
}

static void anim_draw_char_rot90_right(void *user, int x, int y, char c)
{
    (void)user;
    status_display_draw_char_rot90_right(x, y, c);
}

static bool status_idle_delay_elapsed(TickType_t now)
{
    uint32_t timeout_ms = settings_get_status_idle_timeout_ms(&G_Settings);
    if (timeout_ms == 0 || timeout_ms == UINT32_MAX) {
        return false;
    }
    TickType_t required = pdMS_TO_TICKS(timeout_ms);
    return (now - s_last_update_tick) >= required;
}

static void status_display_idle_timer_cb(TimerHandle_t t) {
    (void)t;
    // timer runs in FreeRTOS timer task context; keep it light and just notify
    if (s_anim_task) {
        xTaskNotifyGive(s_anim_task);
    }
}

static void status_display_anim_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        TickType_t now = xTaskGetTickCount();
        if (!status_idle_delay_elapsed(now)) {
            status_display_animations_reset();
            continue;
        }
        if (now < s_next_anim_allowed_tick) continue;
        s_next_anim_allowed_tick = now + ANIM_INTERVAL_TICKS;

        s_anim_frame++;

        IdleAnimation anim = settings_get_status_idle_animation(&G_Settings);

        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            StatusAnimGfx gfx = {
                .clear = anim_clear,
                .flush = anim_flush,
                .plot_pixel = anim_plot_pixel,
                .draw_text = anim_draw_text,
                .draw_char_rot90_right = anim_draw_char_rot90_right,
                .width = 128,
                .height = 64,
                .scale_y = SCALE_Y,
                .font_char_width = 5,
                .font_char_height = 7,
                .user = NULL,
            };

            status_display_clear_buffer();
            status_display_animations_step(anim, now, s_anim_frame, &gfx);
            status_display_flush();
            xSemaphoreGive(s_mutex);
        }
    }
}

static void status_display_sanitize(char *dst, size_t dst_len, const char *src) {
    if (!dst_len) return;
    size_t pos = 0;
    while (src && *src && pos < dst_len - 1) {
        char c = *src++;
        if (!isprint((unsigned char)c)) c = ' ';
        dst[pos++] = c;
    }
    dst[pos] = '\0';
}

void status_display_init(void) {
    if (s_ready) return;

    ESP_LOGI(TAG, "initializing status display on I2C port %d addr 0x%02X", STATUS_DISPLAY_I2C_PORT, STATUS_DISPLAY_ADDR);

    // Configure power control pin (Vext) if specified
#if CONFIG_STATUS_DISPLAY_POWER_PIN >= 0
    gpio_reset_pin(CONFIG_STATUS_DISPLAY_POWER_PIN);
    gpio_set_direction(CONFIG_STATUS_DISPLAY_POWER_PIN, GPIO_MODE_OUTPUT);
    // Set LOW to power on display (Heltec V3 Vext pin behavior)
    gpio_set_level(CONFIG_STATUS_DISPLAY_POWER_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10)); // Small delay for power to stabilize
    ESP_LOGI(TAG, "power control pin %d set LOW (display ON)", CONFIG_STATUS_DISPLAY_POWER_PIN);
#endif

    // Configure reset pin if specified
#if CONFIG_STATUS_DISPLAY_RESET_PIN >= 0
    gpio_reset_pin(CONFIG_STATUS_DISPLAY_RESET_PIN);
    gpio_set_direction(CONFIG_STATUS_DISPLAY_RESET_PIN, GPIO_MODE_OUTPUT);
    // Reset sequence: LOW -> delay -> HIGH
    gpio_set_level(CONFIG_STATUS_DISPLAY_RESET_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(CONFIG_STATUS_DISPLAY_RESET_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "reset pin %d sequence completed", CONFIG_STATUS_DISPLAY_RESET_PIN);
#endif

    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            ESP_LOGE(TAG, "failed to create mutex");
            return;
        }
    }

    if (!s_buffer) {
#if CONFIG_SPIRAM
        s_buffer = heap_caps_malloc(STATUS_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_buffer) {
            ESP_LOGI(TAG, "buffer allocated in PSRAM");
        } else
#endif
        {
            s_buffer = heap_caps_malloc(STATUS_BUFFER_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (!s_buffer) {
            ESP_LOGE(TAG, "failed to allocate display buffer");
            return;
        }
    }

#if defined(CONFIG_USE_IO_EXPANDER)
    // share existing IO expander bus; do not (re)configure or (re)install the driver
    s_i2c_configured = false;
    s_i2c_installed = false;
#else
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = CONFIG_STATUS_DISPLAY_SDA_PIN,
        .scl_io_num = CONFIG_STATUS_DISPLAY_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000
    };

    esp_err_t err = i2c_param_config(STATUS_DISPLAY_I2C_PORT, &conf);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(err));
        return;
    }

    bool configured_by_us = (err == ESP_OK);
    if (configured_by_us) {
        ESP_LOGI(TAG, "configured I2C port %d", STATUS_DISPLAY_I2C_PORT);
    } else {
        ESP_LOGW(TAG, "I2C port %d already configured, sharing driver", STATUS_DISPLAY_I2C_PORT);
    }

    err = i2c_driver_install(STATUS_DISPLAY_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(err));
        if (configured_by_us) {
            i2c_driver_delete(STATUS_DISPLAY_I2C_PORT);
        }
        return;
    }

    s_i2c_configured = configured_by_us;
    s_i2c_installed = (err == ESP_OK);
    if (s_i2c_installed) {
        ESP_LOGI(TAG, "installed I2C driver for port %d", STATUS_DISPLAY_I2C_PORT);
    }
#endif

    // quick probe: display off
    if (status_display_write_command(0xAE) != ESP_OK) {
        ESP_LOGE(TAG, "probe failed (driver missing or device NACK)");
        return;
    }

    uint8_t init_cmds[] = {
        // addressing mode: PAGE addressing (matches per-page flush)
        0x20, 0x02, 0xB0, STATUS_COM_SCAN_CMD, 0x00, 0x10, 0x40,
        0x81, 0x8F, STATUS_SEGMENT_REMAP_CMD, 0xA6, 0xA8, 0x3F, 0xA4, 0xD3,
        0x00, 0xD5, 0x80, 0xD9, 0xF1, 0xDA, 0x12, 0xDB,
        0x40, 0x8D, 0x14, 0xAF
    };

    for (size_t i = 0; i < sizeof(init_cmds); ++i) {
        ESP_LOGD(TAG, "send init cmd[%u]=0x%02X", (unsigned)i, init_cmds[i]);
        if (status_display_write_command(init_cmds[i]) != ESP_OK) {
            ESP_LOGE(TAG, "command 0x%02X failed", init_cmds[i]);
            return;
        }
    }

    // clear any garbage before first text
    status_display_clear_buffer();
    status_display_flush();

    s_ready = true;
    status_display_sanitize(s_line1, sizeof(s_line1), "GhostESP: Revival");
    status_display_sanitize(s_line2, sizeof(s_line2), "made with <3");
    status_display_render(s_line1, s_line2);
    // setup idle animation timer
    s_last_update_tick = xTaskGetTickCount();
    s_anim_frame = 0;
    s_idle_timer = xTimerCreate("status_idle", ANIM_INTERVAL_TICKS, pdTRUE, NULL, status_display_idle_timer_cb);
    if (s_idle_timer) {
        xTimerStart(s_idle_timer, 0);
    }
    // create animation worker task
    s_next_anim_allowed_tick = 0;
    if (s_anim_task == NULL) {
        xTaskCreate(status_display_anim_task, "status_anim", 2048, NULL, tskIDLE_PRIORITY + 1, &s_anim_task);
    }
    ESP_LOGI(TAG, "status display ready");
}

bool status_display_is_ready(void) {
    return s_ready;
}

void status_display_set_lines(const char *line_one, const char *line_two) {
    if (!s_ready) {
        ESP_LOGW(TAG, "set_lines called while display not ready");
        return;
    }
    char tmp1[sizeof(s_line1)];
    char tmp2[sizeof(s_line2)];
    status_display_sanitize(tmp1, sizeof(tmp1), line_one ? line_one : "");
    status_display_sanitize(tmp2, sizeof(tmp2), line_two ? line_two : "");
    if (strcmp(tmp1, s_line1) == 0 && strcmp(tmp2, s_line2) == 0) return;
    strcpy(s_line1, tmp1);
    strcpy(s_line2, tmp2);
    status_display_render(s_line1, s_line2);
    // reset idle timer
    s_last_update_tick = xTaskGetTickCount();
    status_display_animations_reset();
}

void status_display_show_attack(const char *attack_name, const char *target) {
    if (!s_ready) {
        ESP_LOGW(TAG, "show_attack ignored (display not ready)");
        return;
    }
    char line_one[sizeof(s_line1)];
    snprintf(line_one, sizeof(line_one), "Attack: %s", attack_name ? attack_name : "?");
    const char *line_two_src = target ? target : "";
    status_display_set_lines(line_one, line_two_src);
}

void status_display_show_status(const char *status_line) {
    if (!s_ready) {
        ESP_LOGW(TAG, "show_status ignored (display not ready)");
        return;
    }
    status_display_set_lines("Status:", status_line ? status_line : "");
}

void status_display_clear(void) {
    if (!s_ready) return;
    status_display_set_lines("", "");
}

void status_display_deinit(void) {
    if (!s_ready) return;
    ESP_LOGI(TAG, "deinitializing status display");
    status_display_clear_buffer();
    status_display_flush();
    s_ready = false;
    if (s_idle_timer) {
        xTimerStop(s_idle_timer, 0);
        xTimerDelete(s_idle_timer, 0);
        s_idle_timer = NULL;
    }
    if (s_anim_task) {
        vTaskDelete(s_anim_task);
        s_anim_task = NULL;
    }
    if (s_i2c_installed) {
        i2c_driver_delete(STATUS_DISPLAY_I2C_PORT);
        s_i2c_installed = false;
    }
    if (s_i2c_configured) {
        s_i2c_configured = false;
    }
#if CONFIG_STATUS_DISPLAY_POWER_PIN >= 0
    // Turn off display by setting power pin HIGH
    gpio_set_level(CONFIG_STATUS_DISPLAY_POWER_PIN, 1);
    ESP_LOGI(TAG, "power control pin %d set HIGH (display OFF)", CONFIG_STATUS_DISPLAY_POWER_PIN);
#endif
}

#else

void status_display_init(void) {}
bool status_display_is_ready(void) { return false; }
void status_display_set_lines(const char *a, const char *b) { (void)a; (void)b; }
void status_display_show_attack(const char *a, const char *b) { (void)a; (void)b; }
void status_display_show_status(const char *s) { (void)s; }
void status_display_clear(void) {}
void status_display_deinit(void) {}

#endif


