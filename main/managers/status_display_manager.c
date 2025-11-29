#include "sdkconfig.h"
#include "managers/status_display_manager.h"

#ifdef CONFIG_WITH_STATUS_DISPLAY

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

#include <string.h>
#include <ctype.h>
#include <stdio.h>

#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "i2c_bus_lock.h"
#include "managers/settings_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/ap_manager.h"

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
static uint8_t s_buffer[128 * 8];
static char s_line1[24];
static char s_line2[24];
static const int SCALE_Y = 2; // simple vertical scaling factor
// idle animation settings
static TimerHandle_t s_idle_timer;
static TickType_t s_last_update_tick;
static const TickType_t ANIM_INTERVAL_TICKS = pdMS_TO_TICKS(150);

static bool status_idle_delay_elapsed(TickType_t now)
{
    uint32_t timeout_ms = settings_get_status_idle_timeout_ms(&G_Settings);
    if (timeout_ms == 0 || timeout_ms == UINT32_MAX) {
        return false; // never start
    }
    TickType_t required = pdMS_TO_TICKS(timeout_ms);
    return (now - s_last_update_tick) >= required;
}
static int s_anim_frame;
static TaskHandle_t s_anim_task;
static TickType_t s_next_anim_allowed_tick;
// static int s_i2c_error_streak; // unused

// conway's life state
#define LIFE_COLS 32
#define LIFE_ROWS 16
#define LIFE_CELL_SIZE 4
static uint8_t s_life_grid[LIFE_ROWS][LIFE_COLS];
static uint8_t s_life_next[LIFE_ROWS][LIFE_COLS];
static bool s_life_active;

#define STAR_COUNT 96
typedef struct {
    int x;
    int y;
    int dx;
    int dy;
    uint8_t trail;
} Star;
static Star s_stars[STAR_COUNT];
static bool s_starfield_inited;
// ghost sprite (24x30), 1bpp, row-major MSB-first
static const int GHOST_W = 24;
static const int GHOST_H = 30;
static const uint8_t ghostidle_bits[] = {
    0x00, 0x3f, 0x00, 0x00, 0xc0, 0xc0, 0x01, 0x00, 0x20, 0x02, 0x00, 0x10, 0x02, 0x00, 0x10, 0x02,
    0x00, 0x08, 0x02, 0x0c, 0xc8, 0x02, 0x0c, 0xc8, 0x04, 0x1c, 0xc8, 0x04, 0x00, 0x08, 0x04, 0x01,
    0x88, 0x04, 0x03, 0x88, 0x04, 0x00, 0x08, 0x04, 0x1c, 0x0e, 0x04, 0x62, 0x31, 0x08, 0x82, 0x41,
    0x08, 0x0c, 0x02, 0x08, 0x30, 0x0c, 0x10, 0x40, 0x30, 0x10, 0x00, 0x10, 0x10, 0x00, 0x10, 0x10,
    0x00, 0x20, 0x20, 0x00, 0x40, 0x20, 0x01, 0x80, 0x4e, 0x06, 0x00, 0xf1, 0xf0, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xff, 0x80
};
static int s_ghost_x;
static int s_ghost_dir = 1; // 1:right, -1:left

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
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (STATUS_DISPLAY_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, control, true);
    i2c_master_write(cmd, (uint8_t *)data, len, true);
    i2c_master_stop(cmd);
    bool locked = i2c_bus_lock(STATUS_DISPLAY_I2C_PORT, 120);
    if (!locked) {
        i2c_cmd_link_delete(cmd);
        ESP_LOGW(TAG, "status display i2c busy, skipping ctrl=0x%02X", control);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_cmd_begin(STATUS_DISPLAY_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_bus_unlock(STATUS_DISPLAY_I2C_PORT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c write failed ctrl=0x%02X len=%u err=%s", control, (unsigned)len, esp_err_to_name(err));
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
    for (uint8_t page = 0; page < 8; ++page) {
        uint8_t setup[] = { (uint8_t)(0xB0 | page), 0x00, 0x10 };
        if (status_display_send(STATUS_CMD, setup, sizeof(setup)) != ESP_OK) return;
        const uint8_t *chunk = &s_buffer[page * 128];
        if (status_display_send(STATUS_DATA, chunk, 128) != ESP_OK) return;
    }
}

static bool hud_get_wifi_ssid(char *out, size_t len)
{
    if (!out || len == 0) return false;
    wifi_ap_record_t ap;
    memset(&ap, 0, sizeof(ap));
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    if (err != ESP_OK) return false;
    size_t slen = strnlen((const char *)ap.ssid, sizeof(ap.ssid));
    if (slen == 0) return false;
    if (slen >= len) slen = len - 1;
    memcpy(out, ap.ssid, slen);
    out[slen] = '\0';
    return true;
}

static void status_display_clear_buffer(void) {
    memset(s_buffer, 0, sizeof(s_buffer));
}

static void status_display_plot_pixel(int x, int y, bool on) {
    if (x < 0 || x >= 128 || y < 0 || y >= 64) return;
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

static void status_display_plot_pixel_rot90_right(int x, int y, bool on)
{
    int ry = x;
    int rx = 127 - y;
    if (ry < 0 || ry >= 64) return;
    if (rx < 0 || rx >= 128) return;
    status_display_plot_pixel(rx, ry, on);
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

static void draw_sprite_msb(int x, int y, const uint8_t *bits, int w, int h, bool flip_h)
{
    // bits are packed MSB-first per byte, row-major left-to-right
    int bytes_per_row = (w + 7) / 8;
    for (int row = 0; row < h; ++row) {
        const uint8_t *rowptr = bits + row * bytes_per_row;
        for (int col = 0; col < w; ++col) {
            int byte_idx = col >> 3;
            int bit_idx = 7 - (col & 7);
            bool on = ((rowptr[byte_idx] >> bit_idx) & 1) != 0;
            if (!on) continue;
            int draw_x = flip_h ? (x + (w - 1 - col)) : (x + col);
            status_display_plot_pixel(draw_x, y + row, true);
        }
    }
}

typedef struct {
    int ram_used_pct;
    int ram_free_kb;
    int ram_total_kb;
    int cpu_used_pct; // based on internal (non-PSRAM) heap usage
    bool sd_ok;
} HudStats;

static void hud_collect_stats(HudStats *out)
{
    if (!out) return;
    // overall 8-bit heap (may include PSRAM depending on config)
    size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t total_bytes = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    out->ram_free_kb = (int)(free_bytes / 1024);
    out->ram_total_kb = (int)(total_bytes / 1024);
    int used_pct = 0;
    if (total_bytes > 0 && free_bytes <= total_bytes) {
        used_pct = (int)(((total_bytes - free_bytes) * 100) / total_bytes);
    }
    if (used_pct < 0) used_pct = 0;
    if (used_pct > 100) used_pct = 100;
    out->ram_used_pct = used_pct;

    // "CPU" meter based on internal (non-PSRAM) heap usage
    size_t ifree_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t itotal_bytes = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int cpu_pct = 0;
    if (itotal_bytes > 0 && ifree_bytes <= itotal_bytes) {
        cpu_pct = (int)(((itotal_bytes - ifree_bytes) * 100) / itotal_bytes);
    }
    if (cpu_pct < 0) cpu_pct = 0;
    if (cpu_pct > 100) cpu_pct = 100;
    out->cpu_used_pct = cpu_pct;

    // SD status (mounted or not)
    out->sd_ok = sd_card_manager.is_initialized;
}

static int hud_get_webui_sta_count(bool *ap_enabled, bool *server_running)
{
    if (ap_enabled) {
        *ap_enabled = settings_get_ap_enabled(&G_Settings);
    }
    bool srv = false;
    ap_manager_get_status(&srv, NULL, NULL);
    if (server_running) {
        *server_running = srv;
    }
    if (!srv) return 0;

    wifi_sta_list_t sta_list;
    memset(&sta_list, 0, sizeof(sta_list));
    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) {
        return 0;
    }
    return sta_list.num;
}

static void draw_hline(int x0, int x1, int y)
{
    if (y < 0 || y >= 64) return;
    if (x0 > x1) { int tmp = x0; x0 = x1; x1 = tmp; }
    if (x1 < 0 || x0 >= 128) return;
    if (x0 < 0) x0 = 0;
    if (x1 > 127) x1 = 127;
    for (int x = x0; x <= x1; ++x) status_display_plot_pixel(x, y, true);
}

static void draw_vline(int x, int y0, int y1)
{
    if (x < 0 || x >= 128) return;
    if (y0 > y1) { int tmp = y0; y0 = y1; y1 = tmp; }
    if (y1 < 0 || y0 >= 64) return;
    if (y0 < 0) y0 = 0;
    if (y1 > 63) y1 = 63;
    for (int y = y0; y <= y1; ++y) status_display_plot_pixel(x, y, true);
}

static void draw_bar(int x, int y, int w, int h, int pct)
{
    if (w <= 0 || h <= 0) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int filled = (w * pct) / 100;
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            bool on = (yy == 0 || yy == h - 1 || xx == 0 || xx == w - 1 || xx < filled);
            if (on) status_display_plot_pixel(x + xx, y + yy, true);
        }
    }
}

static void status_display_draw_hud(TickType_t now, int phase)
{
    HudStats stats;
    hud_collect_stats(&stats);

    char buf[32];
    uint32_t hud_phase = (uint32_t)phase;
    uint32_t top_mode = (hud_phase / 8U) % 3U; // rotate every few frames: 0=AP,1=SD,2=WebUI

    if (top_mode == 0) {
        char ssid[20];
        if (hud_get_wifi_ssid(ssid, sizeof(ssid))) {
            snprintf(buf, sizeof(buf), "AP  %.16s", ssid);
        } else {
            snprintf(buf, sizeof(buf), "AP Not connected");
        }
    } else if (top_mode == 1) {
        if (!stats.sd_ok) {
            snprintf(buf, sizeof(buf), "SD FAIL");
        } else {
            snprintf(buf, sizeof(buf), "SD OK");
        }
    } else {
        bool ap_enabled = false;
        bool server_running = false;
        int sta_count = hud_get_webui_sta_count(&ap_enabled, &server_running);
        if (!ap_enabled) {
            snprintf(buf, sizeof(buf), "WebUI: OFF");
        } else if (!server_running) {
            snprintf(buf, sizeof(buf), "WebUI: STOP");
        } else {
            snprintf(buf, sizeof(buf), "WebUI: %d STA", sta_count);
        }
    }
    status_display_draw_text(2, 2, buf);

    snprintf(buf, sizeof(buf), "RAM %3d%%",
             stats.ram_used_pct);
    status_display_draw_text(2, 18, buf);

    snprintf(buf, sizeof(buf), "CPU %3d%%", stats.cpu_used_pct);
    status_display_draw_text(2, 34, buf);
    // CPU bar just above the bottom edge
    draw_bar(8, 56, 112, 5, stats.cpu_used_pct);

    // Vertical scrolling "GhostESP: Revival" text along the right edge, rotated
    const char *vert = "GhostESP: Revival";
    int len = (int)strlen(vert);
    int char_h = 5 * SCALE_Y;
    int letter_gap = 1;
    int seq_gap = char_h / 2;
    int step = char_h + letter_gap;
    int total_h = len * step + seq_gap;
    if (total_h <= 0) return;
    int base_x = 127 - 6;    // leave a small margin from the right edge
    int scroll = (phase * 2) % total_h;
    for (int repeat = 0; repeat < 2; ++repeat) {
        int start = -scroll + repeat * total_h;
        for (int i = 0; i < len; ++i) {
            int y = start + i * step;
            if (y > 63) break;
            if (y + char_h < 0) continue;
            status_display_draw_char_rot90_right(base_x, y, vert[i]);
        }
    }
}

// draw an idle animation frame (a moving dot on the bottom row) - unused
// static void status_display_draw_idle_frame(void) {
//     if (!s_ready) return;
//     if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
//     // preserve current lines while drawing animation
//     status_display_render_locked(s_line1, s_line2);
//     // draw dot at bottom row
//     int y = 64 - 2; // near bottom
//     int range = 120; // travel range
//     int x = 4 + (s_anim_frame % range);
//     status_display_plot_pixel(x, y, true);
//     status_display_flush();
//     xSemaphoreGive(s_mutex);
// }

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
            s_life_active = false;
            continue;
        }
        if (now < s_next_anim_allowed_tick) continue;
        s_next_anim_allowed_tick = now + ANIM_INTERVAL_TICKS;

        s_anim_frame++;

        IdleAnimation anim = settings_get_status_idle_animation(&G_Settings);

        if (anim == IDLE_ANIM_GAME_OF_LIFE) {
            if (!s_life_active) {
                uint32_t seed = (uint32_t)now;
                seed ^= (uint32_t)((uintptr_t)&now);
                seed = seed * 1664525u + 1013904223u;
                for (int r = 0; r < LIFE_ROWS; ++r) {
                    for (int c = 0; c < LIFE_COLS; ++c) {
                        seed = seed * 1664525u + 1013904223u;
                        s_life_grid[r][c] = (seed >> 28) & 1;
                    }
                }
                s_life_active = true;
            } else {
                bool any_alive = false;
                bool any_change = false;
                for (int r = 0; r < LIFE_ROWS; ++r) {
                    for (int c = 0; c < LIFE_COLS; ++c) {
                        int live_neighbors = 0;
                        for (int dr = -1; dr <= 1; ++dr) {
                            for (int dc = -1; dc <= 1; ++dc) {
                                if (dr == 0 && dc == 0) continue;
                                int rr = (r + dr + LIFE_ROWS) % LIFE_ROWS;
                                int cc = (c + dc + LIFE_COLS) % LIFE_COLS;
                                live_neighbors += s_life_grid[rr][cc] ? 1 : 0;
                            }
                        }
                        if (s_life_grid[r][c]) {
                            s_life_next[r][c] = (live_neighbors == 2 || live_neighbors == 3) ? 1 : 0;
                        } else {
                            s_life_next[r][c] = (live_neighbors == 3) ? 1 : 0;
                        }
                        if (s_life_next[r][c]) {
                            any_alive = true;
                        }
                        if (s_life_next[r][c] != s_life_grid[r][c]) {
                            any_change = true;
                        }
                    }
                }
                if (!any_alive || !any_change) {
                    s_life_active = false;
                } else {
                    for (int r = 0; r < LIFE_ROWS; ++r) memcpy(s_life_grid[r], s_life_next[r], LIFE_COLS);
                }
            }
        } else if (anim == IDLE_ANIM_STARFIELD) {
            if (!s_starfield_inited) {
                uint32_t seed = (uint32_t)now ^ (uint32_t)((uintptr_t)&now);
                for (int i = 0; i < STAR_COUNT; ++i) {
                    seed = seed * 1664525u + 1013904223u;
                    int cx = 64;
                    int cy = 32;
                    int jitter_x = ((int)(seed & 0x0F)) - 8;
                    seed = seed * 1664525u + 1013904223u;
                    int jitter_y = ((int)(seed & 0x0F)) - 8;
                    s_stars[i].x = cx + jitter_x;
                    s_stars[i].y = cy + jitter_y;

                    int dx = 0;
                    int dy = 0;
                    int mag2 = 0;
                    do {
                        seed = seed * 1664525u + 1013904223u;
                        dx = (int)(((seed >> 28) & 0x07) - 3);
                        seed = seed * 1664525u + 1013904223u;
                        dy = (int)(((seed >> 28) & 0x07) - 3);
                        mag2 = dx * dx + dy * dy;
                    } while (mag2 < 2 || mag2 > 18);

                    int vx = s_stars[i].x - cx;
                    int vy = s_stars[i].y - cy;
                    if (vx * dx + vy * dy <= 0) {
                        dx = -dx;
                        dy = -dy;
                    }

                    s_stars[i].dx = dx;
                    s_stars[i].dy = dy;

                    seed = seed * 1664525u + 1013904223u;
                    s_stars[i].trail = (uint8_t)(5 + (seed % 3));
                }
                s_starfield_inited = true;
            } else {
                for (int i = 0; i < STAR_COUNT; ++i) {
                    s_stars[i].x += s_stars[i].dx;
                    s_stars[i].y += s_stars[i].dy;
                    if (s_stars[i].x < 0 || s_stars[i].x >= 128 ||
                        s_stars[i].y < 0 || s_stars[i].y >= 64) {
                        uint32_t seed = (uint32_t)now ^ (uint32_t)((uintptr_t)&s_stars[i]);
                        seed = seed * 1664525u + 1013904223u;
                        int cx = 64;
                        int cy = 32;
                        int jitter_x = ((int)(seed & 0x0F)) - 8;
                        seed = seed * 1664525u + 1013904223u;
                        int jitter_y = ((int)(seed & 0x0F)) - 8;
                        s_stars[i].x = cx + jitter_x;
                        s_stars[i].y = cy + jitter_y;

                        int dx = 0;
                        int dy = 0;
                        int mag2 = 0;
                        do {
                            seed = seed * 1664525u + 1013904223u;
                            dx = (int)(((seed >> 28) & 0x07) - 3);
                            seed = seed * 1664525u + 1013904223u;
                            dy = (int)(((seed >> 28) & 0x07) - 3);
                            mag2 = dx * dx + dy * dy;
                        } while (mag2 < 2 || mag2 > 18);

                        int vx = s_stars[i].x - cx;
                        int vy = s_stars[i].y - cy;
                        if (vx * dx + vy * dy <= 0) {
                            dx = -dx;
                            dy = -dy;
                        }

                        s_stars[i].dx = dx;
                        s_stars[i].dy = dy;

                        seed = seed * 1664525u + 1013904223u;
                        s_stars[i].trail = (uint8_t)(5 + (seed % 3));
                    }
                }
            }
        } else {
            // ghost sprite horizontal walk with gentle vertical float
            int speed = 4; // pixels per tick
            if (s_ghost_dir > 0) {
                s_ghost_x += speed;
                if (s_ghost_x + GHOST_W >= 128) { s_ghost_x = 128 - GHOST_W; s_ghost_dir = -1; }
            } else {
                s_ghost_x -= speed;
                if (s_ghost_x <= 0) { s_ghost_x = 0; s_ghost_dir = 1; }
            }
        }

        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            status_display_clear_buffer();
            if (anim == IDLE_ANIM_GAME_OF_LIFE) {
                for (int r = 0; r < LIFE_ROWS; ++r) {
                    for (int c = 0; c < LIFE_COLS; ++c) {
                        if (!s_life_grid[r][c]) continue;
                        int sx = c * LIFE_CELL_SIZE;
                        int sy = r * LIFE_CELL_SIZE;
                        for (int yy = 0; yy < LIFE_CELL_SIZE; ++yy) {
                            for (int xx = 0; xx < LIFE_CELL_SIZE; ++xx) {
                                status_display_plot_pixel(sx + xx, sy + yy, true);
                            }
                        }
                    }
                }
            } else if (anim == IDLE_ANIM_STARFIELD) {
                for (int i = 0; i < STAR_COUNT; ++i) {
                    int x = s_stars[i].x;
                    int y = s_stars[i].y;
                    int dx = s_stars[i].dx;
                    int dy = s_stars[i].dy;
                    int len = s_stars[i].trail;
                    for (int t = 0; t < len; ++t) {
                        int px = x - dx * t;
                        int py = y - dy * t;
                        status_display_plot_pixel(px, py, true);
                    }
                }
            } else if (anim == IDLE_ANIM_HUD) {
                int phase = s_anim_frame;
                status_display_draw_hud(now, phase);
            } else {
                bool flip = (s_ghost_dir < 0);
                int base_y = 64 - GHOST_H - 6;
                if (base_y < 0) base_y = 0;
                int phase = s_anim_frame & 0x1F;
                int tri = phase < 16 ? phase : (32 - phase);
                int y_offset = tri - 8;
                if (y_offset < -6) y_offset = -6;
                if (y_offset > 6) y_offset = 6;
                int y = base_y + y_offset;
                draw_sprite_msb(s_ghost_x, y, ghostidle_bits, GHOST_W, GHOST_H, flip);
            }
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

    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            ESP_LOGE(TAG, "failed to create mutex");
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
    s_ghost_x = 0;
    s_ghost_dir = 1;
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
    s_life_active = false;
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


