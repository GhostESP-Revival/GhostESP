#include "core/screen_mirror.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include <stdlib.h>

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
#include "driver/usb_serial_jtag.h"
#define MIRROR_USE_JTAG 1
#else
#define MIRROR_USE_JTAG 0
#endif

static const char *TAG = "ScreenMirror";
static bool s_mirror_enabled = false;
static uint32_t s_frame_count = 0;

#define CHUNK_BUF_SIZE 1024
static uint8_t *s_chunk_buf = NULL;
static uint16_t s_checksum;
// Mirror pixel format tradeoffs:
// - 8-bit RGB332: 1.0 B/px (fastest, most banding)
// - 12-bit packed RGB444: 1.5 B/px (good compromise, much closer to 16-bit)
// - 16-bit RGB565: 2.0 B/px (best quality, slowest)
#if MIRROR_USE_JTAG
#ifndef USE_8BIT_MIRROR
#define USE_8BIT_MIRROR 0
#endif
#ifndef USE_12BIT_MIRROR
#define USE_12BIT_MIRROR 1
#endif
#else
#ifndef USE_8BIT_MIRROR
#define USE_8BIT_MIRROR 1
#endif
#ifndef USE_12BIT_MIRROR
#define USE_12BIT_MIRROR 0
#endif
#endif

#ifndef CONFIG_ESP_CONSOLE_UART_NUM
#define MIRROR_UART_NUM UART_NUM_0
#else
#define MIRROR_UART_NUM CONFIG_ESP_CONSOLE_UART_NUM
#endif

static inline void checksum_reset(void) {
    s_checksum = 0;
}

static inline void checksum_add(uint8_t byte) {
    s_checksum = (uint16_t)((s_checksum + byte) & 0xFFFF);
}

static inline uint16_t mirror_color_to_rgb565(lv_color_t color) {
    // derive via lv_color_to32 to avoid any bitfield packing surprises
    lv_color32_t c32;
    c32.full = lv_color_to32(color);
    uint16_t r = (uint16_t)(c32.ch.red >> 3);
    uint16_t g = (uint16_t)(c32.ch.green >> 2);
    uint16_t b = (uint16_t)(c32.ch.blue >> 3);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline uint8_t quantize_round(uint8_t value, uint8_t bits) {
    if (bits >= 8) return value;
    const uint8_t shift = 8 - bits;
    const uint16_t rounded = (uint16_t)value + (1 << (shift - 1));
    const uint16_t scaled = rounded >> shift;
    const uint8_t mask = (1 << bits) - 1;
    return (uint8_t)(scaled > mask ? mask : scaled);
}

static inline bool colors_close(uint8_t a, uint8_t b, uint8_t tolerance) {
    return a > b ? (a - b) <= tolerance : (b - a) <= tolerance;
}

static inline uint8_t mirror_color_remap_rgb332(uint8_t r3, uint8_t g3, uint8_t b2, const lv_color32_t *c32) {
    const uint8_t red = c32->ch.red;
    const uint8_t green = c32->ch.green;
    const uint8_t blue = c32->ch.blue;

    if ((uint16_t)red + green + blue < 60 || (red < 32 && green < 32 && blue < 32)) {
        return 0;
    }

    if (red < 120 && green < 120 && blue < 120 &&
        colors_close(red, green, 20) &&
        colors_close(green, blue, 20)) {
        const uint8_t avg = (red + green + blue) / 3;
        const uint8_t grey3 = quantize_round(avg, 3);
        const uint8_t grey2 = quantize_round(avg, 2);
        return (uint8_t)((grey3 << 5) | (grey3 << 2) | grey2);
    }

    if (green > red + 45 && green > blue + 30) {
        return (uint8_t)((0 << 5) | (7 << 2) | 0);
    }

    return (uint8_t)((r3 << 5) | (g3 << 2) | b2);
}

static inline uint8_t mirror_color_to_rgb332(lv_color_t color) {
    lv_color32_t c32;
    c32.full = lv_color_to32(color);
    uint8_t r3 = quantize_round(c32.ch.red, 3);
    uint8_t g3 = quantize_round(c32.ch.green, 3);
    uint8_t b2 = quantize_round(c32.ch.blue, 2);
    return mirror_color_remap_rgb332(r3, g3, b2, &c32);
}

static inline uint16_t mirror_color_to_rgb444(lv_color_t color) {
    // 12-bit packed RGB444: RRRRGGGGBBBB
    lv_color32_t c32;
    c32.full = lv_color_to32(color);
    uint16_t r4 = quantize_round(c32.ch.red, 4);
    uint16_t g4 = quantize_round(c32.ch.green, 4);
    uint16_t b4 = quantize_round(c32.ch.blue, 4);
    return (uint16_t)((r4 << 8) | (g4 << 4) | b4);
}

static void mirror_write(const void *data, size_t len) {
#if MIRROR_USE_JTAG
    const uint8_t *p = (const uint8_t *)data;
    size_t remaining = len;
    int64_t start = esp_timer_get_time();
    while (remaining > 0) {
        if (esp_timer_get_time() - start > 500000) break;  // 500ms max
        size_t chunk = (remaining > 64) ? 64 : remaining;
        int written = usb_serial_jtag_write_bytes(p, chunk, pdMS_TO_TICKS(50));
        if (written > 0) {
            p += written;
            remaining -= written;
        }
    }
#else
    const uint8_t *p = (const uint8_t *)data;
    size_t remaining = len;
    while (remaining > 0) {
        size_t chunk = remaining > 1024 ? 1024 : remaining;
        int written = uart_write_bytes(MIRROR_UART_NUM, (const char *)p, chunk);
        if (written > 0) {
            p += written;
            remaining -= written;
        } else {
            // retry a few times before a short yield
            static int retries = 0;
            if (++retries > 3) {
                vTaskDelay(pdMS_TO_TICKS(1));
                retries = 0;
            }
        }
    }
    // non-blocking flush hint; return immediately
    uart_wait_tx_done(MIRROR_UART_NUM, 0);
#endif
}

void screen_mirror_init(void) {
    s_mirror_enabled = false;
    s_frame_count = 0;
    ESP_LOGI(TAG, "Screen mirror initialized");
}

void screen_mirror_set_enabled(bool enabled) {
    s_mirror_enabled = enabled;
    if (enabled) {
        s_frame_count = 0;
        screen_mirror_send_info();
        lv_obj_invalidate(lv_scr_act());
        ESP_LOGI(TAG, "Screen mirror enabled");
    } else {
        ESP_LOGI(TAG, "Screen mirror disabled");
    }
}

bool screen_mirror_is_enabled(void) {
    return s_mirror_enabled;
}

void screen_mirror_refresh(void) {
    if (s_mirror_enabled) {
        lv_obj_invalidate(lv_scr_act());
    }
}

void screen_mirror_send_info(void) {
    if (!s_mirror_enabled) return;

    lv_disp_t *disp = lv_disp_get_default();
    if (!disp) return;

    uint16_t w = lv_disp_get_hor_res(disp);
    uint16_t h = lv_disp_get_ver_res(disp);

    mirror_packet_header_t hdr = {
        .marker = MIRROR_MARKER,
        .cmd = MIRROR_CMD_INFO,
        .x1 = w,
        .y1 = h,
        .x2 = 16,
        .y2 = 0,
        .data_len = 0
    };

    mirror_write(&hdr, sizeof(hdr));
}

static uint32_t estimate_rle16(const lv_color_t *pixels, uint32_t pixel_count) {
    uint32_t size = 0;
    uint32_t i = 0;
    const uint32_t raw_size = pixel_count * 2;

    while (i < pixel_count) {
        uint16_t val = mirror_color_to_rgb565(pixels[i]);
        uint8_t count = 1;
        while (i + count < pixel_count && count < 255 && mirror_color_to_rgb565(pixels[i + count]) == val) {
            count++;
        }
        size += 3;
        // The caller selects raw at equality; further runs cannot make RLE smaller.
        if (size >= raw_size) return raw_size;
        i += count;
    }

    return size;
}

static uint32_t estimate_rle8(const lv_color_t *pixels, uint32_t pixel_count) {
    uint32_t size = 0;
    uint32_t i = 0;

    while (i < pixel_count) {
        uint8_t val = mirror_color_to_rgb332(pixels[i]);
        uint8_t count = 1;
        while (i + count < pixel_count && count < 255 && mirror_color_to_rgb332(pixels[i + count]) == val) {
            count++;
        }
        size += 2; // count + value
        // No need to scan the remaining pixels once raw is guaranteed to win.
        if (size >= pixel_count) return pixel_count;
        i += count;
    }

    return size;
}

void screen_mirror_send_area(const lv_area_t *area, lv_color_t *color_p) {
    if (!s_mirror_enabled || !area || !color_p) return;

    if (!s_chunk_buf) {
        s_chunk_buf = (uint8_t *)malloc(CHUNK_BUF_SIZE);
        if (!s_chunk_buf) return;
    }

    uint16_t w = area->x2 - area->x1 + 1;
    uint16_t h = area->y2 - area->y1 + 1;
    uint32_t pixel_count = w * h;
    lv_color_t *pixels = color_p;

#if USE_8BIT_MIRROR
    uint32_t raw_size_8 = pixel_count; // rgb332
    uint32_t estimated_rle_8 = estimate_rle8(pixels, pixel_count);
    bool use_rle = (estimated_rle_8 > 0 && estimated_rle_8 < raw_size_8);
    if (use_rle) {
        mirror_packet_header_t hdr = {
            .marker = MIRROR_MARKER,
            .cmd = MIRROR_CMD_FRAME_8BIT_RLE,
            .x1 = area->x1,
            .y1 = area->y1,
            .x2 = area->x2,
            .y2 = area->y2,
            .data_len = estimated_rle_8
        };

        mirror_write(&hdr, sizeof(hdr));

        uint32_t i = 0;
        uint16_t buf_pos = 0;
        while (i < pixel_count) {
            uint8_t val = mirror_color_to_rgb332(pixels[i]);
            uint8_t count = 1;
            while (i + count < pixel_count && count < 255 && mirror_color_to_rgb332(pixels[i + count]) == val) {
                count++;
            }

            if (buf_pos > CHUNK_BUF_SIZE - 2) {
                mirror_write(s_chunk_buf, buf_pos);
                buf_pos = 0;
            }

            s_chunk_buf[buf_pos++] = count;
            s_chunk_buf[buf_pos++] = val;

            i += count;
        }

        if (buf_pos > 0) {
            mirror_write(s_chunk_buf, buf_pos);
        }
    } else {
        mirror_packet_header_t hdr = {
            .marker = MIRROR_MARKER,
            .cmd = MIRROR_CMD_FRAME_8BIT,
            .x1 = area->x1,
            .y1 = area->y1,
            .x2 = area->x2,
            .y2 = area->y2,
            .data_len = raw_size_8
        };

        mirror_write(&hdr, sizeof(hdr));

        uint32_t i = 0;
        uint16_t buf_pos = 0;

        while (i < pixel_count) {
            uint8_t val = mirror_color_to_rgb332(pixels[i++]);
            s_chunk_buf[buf_pos++] = val;

            if (buf_pos >= CHUNK_BUF_SIZE) {
                mirror_write(s_chunk_buf, buf_pos);
                buf_pos = 0;
            }
        }

        if (buf_pos > 0) {
            mirror_write(s_chunk_buf, buf_pos);
        }
    }
#elif USE_12BIT_MIRROR
    // 12-bit packed RGB444: 2 pixels -> 3 bytes, last odd pixel -> 2 bytes.
    // data_len = floor(n/2)*3 + (n%2 ? 2 : 0)
    uint32_t raw_size_12 = (pixel_count / 2) * 3 + ((pixel_count & 1) ? 2 : 0);

    mirror_packet_header_t hdr = {
        .marker = MIRROR_MARKER,
        .cmd = MIRROR_CMD_FRAME_12BIT,
        .x1 = area->x1,
        .y1 = area->y1,
        .x2 = area->x2,
        .y2 = area->y2,
        .data_len = raw_size_12
    };

    mirror_write(&hdr, sizeof(hdr));

    uint32_t i = 0;
    uint16_t buf_pos = 0;
    while (i < pixel_count) {
        if (i + 1 < pixel_count) {
            uint16_t p0 = mirror_color_to_rgb444(pixels[i]);
            uint16_t p1 = mirror_color_to_rgb444(pixels[i + 1]);

            // Pack: [p0(11:4)], [p0(3:0)<<4 | p1(11:8)], [p1(7:0)]
            if (buf_pos > CHUNK_BUF_SIZE - 3) {
                mirror_write(s_chunk_buf, buf_pos);
                buf_pos = 0;
            }
            s_chunk_buf[buf_pos++] = (uint8_t)(p0 >> 4);
            s_chunk_buf[buf_pos++] = (uint8_t)(((p0 & 0x0F) << 4) | ((p1 >> 8) & 0x0F));
            s_chunk_buf[buf_pos++] = (uint8_t)(p1 & 0xFF);
            i += 2;
        } else {
            uint16_t p0 = mirror_color_to_rgb444(pixels[i]);
            if (buf_pos > CHUNK_BUF_SIZE - 2) {
                mirror_write(s_chunk_buf, buf_pos);
                buf_pos = 0;
            }
            s_chunk_buf[buf_pos++] = (uint8_t)(p0 >> 4);
            s_chunk_buf[buf_pos++] = (uint8_t)((p0 & 0x0F) << 4);
            i += 1;
        }
    }

    if (buf_pos > 0) {
        mirror_write(s_chunk_buf, buf_pos);
    }
#else
    uint32_t raw_size_16 = pixel_count * 2;
    uint32_t estimated_rle_16 = estimate_rle16(pixels, pixel_count);
    uint32_t raw_size = raw_size_16;
    uint32_t estimated_rle = estimated_rle_16;
    if (estimated_rle > 0 && estimated_rle < raw_size) {
        mirror_packet_header_t hdr = {
            .marker = MIRROR_MARKER,
            .cmd = MIRROR_CMD_FRAME_RLE,
            .x1 = area->x1,
            .y1 = area->y1,
            .x2 = area->x2,
            .y2 = area->y2,
            .data_len = estimated_rle
        };

        mirror_write(&hdr, sizeof(hdr));

        uint32_t i = 0;
        uint16_t buf_pos = 0;
        checksum_reset();

        while (i < pixel_count) {
            uint16_t val = mirror_color_to_rgb565(pixels[i]);
            uint8_t count = 1;
            while (i + count < pixel_count && count < 255 && mirror_color_to_rgb565(pixels[i + count]) == val) {
                count++;
            }

            if (buf_pos > CHUNK_BUF_SIZE - 3) {
                mirror_write(s_chunk_buf, buf_pos);
                buf_pos = 0;
            }

            s_chunk_buf[buf_pos++] = count;
            s_chunk_buf[buf_pos++] = (uint8_t)(val >> 8);
            s_chunk_buf[buf_pos++] = (uint8_t)(val & 0xFF);
            checksum_add(count);
            checksum_add((uint8_t)(val >> 8));
            checksum_add((uint8_t)(val & 0xFF));

            i += count;
        }

        if (buf_pos > 0) {
            mirror_write(s_chunk_buf, buf_pos);
        }

        uint16_t cs = s_checksum;
        mirror_write(&cs, sizeof(cs));
    } else {
        mirror_packet_header_t hdr = {
            .marker = MIRROR_MARKER,
            .cmd = MIRROR_CMD_FRAME,
            .x1 = area->x1,
            .y1 = area->y1,
            .x2 = area->x2,
            .y2 = area->y2,
            .data_len = raw_size
        };

        mirror_write(&hdr, sizeof(hdr));

        uint32_t i = 0;
        uint16_t buf_pos = 0;
        checksum_reset();

        while (i < pixel_count) {
            uint16_t val = mirror_color_to_rgb565(pixels[i++]);
            s_chunk_buf[buf_pos++] = (uint8_t)(val & 0xFF);
            s_chunk_buf[buf_pos++] = (uint8_t)(val >> 8);
            checksum_add((uint8_t)(val & 0xFF));
            checksum_add((uint8_t)(val >> 8));

            if (buf_pos >= CHUNK_BUF_SIZE) {
                mirror_write(s_chunk_buf, buf_pos);
                buf_pos = 0;
            }
        }

        if (buf_pos > 0) {
            mirror_write(s_chunk_buf, buf_pos);
        }

        uint16_t cs = s_checksum;
        mirror_write(&cs, sizeof(cs));
    }
#endif

    uint32_t end_marker = MIRROR_END_MARKER;
    mirror_write(&end_marker, sizeof(end_marker));
    s_frame_count++;
}
