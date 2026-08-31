#include "sdkconfig.h"

#if defined(CONFIG_CROWPANEL_P4_CAMERA)

#if CONFIG_ESP_VIDEO_DISABLE_MIPI_CSI_DRIVER_BACKUP_BUFFER
#error "CrowPanel camera requires the MIPI CSI driver backup buffer; disable ESP_VIDEO_DISABLE_MIPI_CSI_DRIVER_BACKUP_BUFFER"
#endif

#include "managers/views/crowpanel_p4_camera_view.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"

#include "gui/accessibility_fonts.h"
#include "gui/lvgl_safe.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "gui/toast.h"
#include "managers/sd_card_manager.h"
#include "managers/views/sd_browser_screen.h"

#define CAMERA_WIDTH 1024
#define CAMERA_HEIGHT 600
#define CAMERA_PIXEL_BYTES 2
#define CAMERA_BUFFER_COUNT 4
#define CAMERA_PREVIEW_BUFFER_COUNT 3
#define CAMERA_PREVIEW_PERIOD_MS 33
#define CAMERA_TOP_BAR_HEIGHT 64
#define CAMERA_BOTTOM_BAR_HEIGHT 118
#define CAMERA_TAP_SLOP 16
#define CAMERA_FRAME_BYTES (CAMERA_WIDTH * CAMERA_HEIGHT * CAMERA_PIXEL_BYTES)
#define CAMERA_DEVICE ESP_VIDEO_MIPI_CSI_DEVICE_NAME
#define CAMERA_VIDEO_FLAGS (ESP_VIDEO_INIT_FLAGS_MIPI_CSI | ESP_VIDEO_INIT_FLAGS_ISP)
#define CAMERA_SETTING_COUNT 4
#define CAMERA_SETTINGS_BUTTON_COUNT (CAMERA_SETTING_COUNT * 2 + 2)

typedef struct {
    uint32_t id;
    const char *name;
    struct v4l2_query_ext_ctrl range;
    int32_t value;
    bool available;
    lv_obj_t *label;
    lv_obj_t *minus;
    lv_obj_t *plus;
} camera_setting_t;

static camera_setting_t s_camera_settings[CAMERA_SETTING_COUNT] = {
    {.id = V4L2_CID_BRIGHTNESS, .name = "Brightness"},
    {.id = V4L2_CID_CONTRAST, .name = "Contrast"},
    {.id = V4L2_CID_SATURATION, .name = "Saturation"},
    {.id = V4L2_CID_HUE, .name = "Hue"},
};
static lv_obj_t *s_settings_panel;
static lv_obj_t *s_settings_buttons[CAMERA_SETTINGS_BUTTON_COUNT];
static int s_settings_fd = -1;

static const char *TAG = "CrowPanelCamera";

static lv_obj_t *s_root;
static lv_obj_t *s_image;
static lv_obj_t *s_status;
static lv_obj_t *s_shutter;
static lv_obj_t *s_back;
static lv_obj_t *s_gallery;
static lv_obj_t *s_settings;
static lv_obj_t *s_touch_button;
static lv_point_t s_touch_start;
static lv_timer_t *s_refresh_timer;
static SemaphoreHandle_t s_frame_mutex;
static SemaphoreHandle_t s_stream_done;
static SemaphoreHandle_t s_snapshot_done;
static TaskHandle_t s_stream_task;
static bool s_snapshot_busy;
static esp_err_t s_snapshot_result;
static int s_video_fd = -1;
static uint8_t *s_camera_buffers[CAMERA_BUFFER_COUNT];
static uint8_t *s_preview_buffers[CAMERA_PREVIEW_BUFFER_COUNT];
static uint8_t *s_snapshot_buffer;
static int s_ready_buffer = -1;
static int s_display_buffer = -1;
static uint32_t s_captured_frames;
static uint32_t s_preview_frames;
static uint32_t s_displayed_frames;
static int64_t s_last_stats_us;
static uint32_t s_last_stats_frames;
/* Profiling is scoped to this view. Use the hardware clock because LVGL's
 * tick is advanced between render passes, not while a pass is running. */
static lv_disp_drv_t *s_profile_driver;
static void (*s_previous_render_start)(lv_disp_drv_t *);
static void (*s_previous_monitor)(lv_disp_drv_t *, uint32_t, uint32_t);
static void (*s_previous_flush)(lv_disp_drv_t *, const lv_area_t *, lv_color_t *);
static int64_t s_render_start_us;
static uint64_t s_pass_flush_us;
static uint64_t s_render_total_us;
static uint64_t s_flush_total_us;
static uint32_t s_render_max_us;
static uint32_t s_flush_max_us;
static uint32_t s_render_count;
static volatile bool s_stream_running;
static bool s_video_initialized;
static lv_img_dsc_t s_preview_images[CAMERA_PREVIEW_BUFFER_COUNT];

static void camera_render_start(lv_disp_drv_t *driver)
{
    s_render_start_us = esp_timer_get_time();
    s_pass_flush_us = 0;
    if (s_previous_render_start) s_previous_render_start(driver);
}

static void camera_flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *pixels)
{
    int64_t start = esp_timer_get_time();
    s_previous_flush(driver, area, pixels);
    s_pass_flush_us += esp_timer_get_time() - start;
}

static void camera_render_done(lv_disp_drv_t *driver, uint32_t time, uint32_t pixels)
{
    uint64_t elapsed = esp_timer_get_time() - s_render_start_us;
    uint32_t render_us = elapsed > s_pass_flush_us ? elapsed - s_pass_flush_us : 0;
    s_render_total_us += render_us;
    s_flush_total_us += s_pass_flush_us;
    if (render_us > s_render_max_us) s_render_max_us = render_us;
    if (s_pass_flush_us > s_flush_max_us) s_flush_max_us = s_pass_flush_us;
    ++s_render_count;
    if (s_previous_monitor) s_previous_monitor(driver, time, pixels);
}

static void camera_profile_start(void)
{
    s_profile_driver = lv_obj_get_disp(s_root)->driver;
    s_previous_render_start = s_profile_driver->render_start_cb;
    s_previous_monitor = s_profile_driver->monitor_cb;
    s_previous_flush = s_profile_driver->flush_cb;
    s_render_count = s_render_max_us = s_flush_max_us = 0;
    s_render_total_us = s_flush_total_us = 0;
    s_profile_driver->render_start_cb = camera_render_start;
    s_profile_driver->monitor_cb = camera_render_done;
    s_profile_driver->flush_cb = camera_flush;
}

static void camera_profile_stop(void)
{
    if (!s_profile_driver) return;
    s_profile_driver->render_start_cb = s_previous_render_start;
    s_profile_driver->monitor_cb = s_previous_monitor;
    s_profile_driver->flush_cb = s_previous_flush;
    s_profile_driver = NULL;
}

static void camera_set_status(const char *text, lv_color_t color)
{
    if (s_status && text) {
        lv_label_set_text(s_status, text);
        lv_obj_set_style_text_color(s_status, color, 0);
    }
}

static esp_err_t camera_ioctl(int request, void *arg)
{
    if (s_video_fd < 0) return ESP_ERR_INVALID_STATE;
    if (ioctl(s_video_fd, request, arg) != 0) {
        ESP_LOGE(TAG, "ioctl 0x%x failed: %s", request, strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t camera_deinit_video(void)
{
    if (!s_video_initialized) return ESP_OK;
    esp_err_t err = esp_video_deinit_with_flags(CAMERA_VIDEO_FLAGS);
    if (err == ESP_OK) {
        s_video_initialized = false;
    } else {
        // Keep ownership on failure so a later open retries cleanup rather
        // than attempting to register a second instance of the same device.
        ESP_LOGE(TAG, "camera video cleanup failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t camera_start_hardware(void)
{
    static const esp_video_init_csi_config_t csi_config = {
        .sccb_config = {
            .init_sccb = true,
            .i2c_config = {
                // Keep camera SCCB separate from touch (I2C0 on GPIO45/46).
                .port = I2C_NUM_1,
#if defined(CONFIG_CROWPANEL_P4_PANEL_RGB_800X480)
                // Elecrow 5-inch BSP: GPIO12/13 belong to the RGB display.
                .scl_pin = 34,
                .sda_pin = 33,
#else
                .scl_pin = 13,
                .sda_pin = 12,
#endif
            },
            .freq = 100000,
        },
        .reset_pin = -1,
        .pwdn_pin = -1,
    };
    const esp_video_init_config_t video_config = {
        .csi = &csi_config,
    };

    esp_err_t err = camera_deinit_video();
    if (err != ESP_OK) return err;
    err = esp_video_init_with_flags(&video_config, CAMERA_VIDEO_FLAGS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_video_initialized = true;

    s_video_fd = open(CAMERA_DEVICE, O_RDWR);
    if (s_video_fd < 0) {
        ESP_LOGE(TAG, "failed to open %s: %s", CAMERA_DEVICE, strerror(errno));
        err = ESP_FAIL;
        goto fail_video;
    }

    struct v4l2_format format = {0};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = CAMERA_WIDTH;
    format.fmt.pix.height = CAMERA_HEIGHT;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    format.fmt.pix.field = V4L2_FIELD_NONE;
    err = camera_ioctl(VIDIOC_S_FMT, &format);
    if (err != ESP_OK) goto fail_fd;
    if (format.fmt.pix.width != CAMERA_WIDTH || format.fmt.pix.height != CAMERA_HEIGHT ||
        format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565 ||
        format.fmt.pix.sizeimage > CAMERA_FRAME_BYTES ||
        (format.fmt.pix.bytesperline && format.fmt.pix.bytesperline != CAMERA_WIDTH * CAMERA_PIXEL_BYTES)) {
        ESP_LOGE(TAG, "unexpected camera format/stride");
        err = ESP_ERR_NOT_SUPPORTED;
        goto fail_fd;
    }

    // This esp_video version has its own dequeue timeout; O_NONBLOCK alone
    // does not bound the wait. A finite timeout also makes shutdown safe.
    struct timeval dequeue_timeout = {.tv_sec = 0, .tv_usec = 100000};
    err = camera_ioctl(VIDIOC_S_DQBUF_TIMEOUT, &dequeue_timeout);
    if (err != ESP_OK) goto fail_fd;

    struct v4l2_requestbuffers request = {0};
    request.count = CAMERA_BUFFER_COUNT;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_USERPTR;
    err = camera_ioctl(VIDIOC_REQBUFS, &request);
    if (err != ESP_OK || request.count != CAMERA_BUFFER_COUNT) {
        ESP_LOGE(TAG, "camera rejected user buffers (count=%u)", request.count);
        err = ESP_FAIL;
        goto fail_fd;
    }

    for (uint32_t i = 0; i < request.count && i < CAMERA_BUFFER_COUNT; ++i) {
        struct v4l2_buffer buffer = {0};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_USERPTR;
        buffer.index = i;
        buffer.m.userptr = (unsigned long)s_camera_buffers[i];
        buffer.length = CAMERA_FRAME_BYTES;
        err = camera_ioctl(VIDIOC_QBUF, &buffer);
        if (err != ESP_OK) goto fail_fd;
    }

    return ESP_OK;

fail_fd:
    close(s_video_fd);
    s_video_fd = -1;
fail_video:
    camera_deinit_video();
    return err;
}

static void camera_stream_task(void *arg)
{
    (void)arg;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (camera_ioctl(VIDIOC_STREAMON, &type) != ESP_OK) {
        s_stream_running = false;
        xSemaphoreGive(s_stream_done);
        vTaskDelete(NULL);
        return;
    }
    unsigned dequeue_misses = 0;
    while (s_stream_running) {
        struct v4l2_buffer buffer = {0};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_USERPTR;
        if (ioctl(s_video_fd, VIDIOC_DQBUF, &buffer) != 0) {
            // esp_video 2.3 maps an empty timed dequeue (ESP_FAIL) to EPERM.
            if (errno != EAGAIN && errno != EINTR && errno != ETIMEDOUT && errno != EPERM) {
                ESP_LOGE(TAG, "camera frame dequeue failed: %s", strerror(errno));
                break;
            }
            if (++dequeue_misses % 50 == 0) ESP_LOGW(TAG, "no camera frame for ~5 seconds");
            vTaskDelay(1);
            continue;
        }
        dequeue_misses = 0;

        if (buffer.index >= CAMERA_BUFFER_COUNT) {
            ESP_LOGE(TAG, "camera returned invalid buffer index %u", buffer.index);
            break;
        }

        int back = -1;
        if (xSemaphoreTake(s_frame_mutex, 0) == pdTRUE) {
            ++s_captured_frames;
            if (buffer.bytesused >= CAMERA_FRAME_BYTES && !(buffer.flags & V4L2_BUF_FLAG_ERROR)) {
                // USERPTR lets us return a different allocation for this CSI
                // slot. Exchange with a spare preview instead of copying
                // 1.2 MB through the CPU on every frame. Never give CSI the
                // displayed or pending image; the UI may adopt pending next.
                for (int i = 0; i < CAMERA_PREVIEW_BUFFER_COUNT; ++i) {
                    if (i != s_display_buffer && i != s_ready_buffer) {
                        back = i;
                        uint8_t *completed = s_camera_buffers[buffer.index];
                        s_camera_buffers[buffer.index] = s_preview_buffers[back];
                        s_preview_buffers[back] = completed;
                        break;
                    }
                }
            }
            xSemaphoreGive(s_frame_mutex);
        }
        // Return every CSI queue slot before publishing, without waiting for
        // the UI. The CSI driver invalidates DMA-written cache lines before
        // delivering frames. All exchanged allocations have its 64B alignment.
        buffer.m.userptr = (unsigned long)s_camera_buffers[buffer.index];
        buffer.length = CAMERA_FRAME_BYTES;
        if (camera_ioctl(VIDIOC_QBUF, &buffer) != ESP_OK) break;
        if (back >= 0 && xSemaphoreTake(s_frame_mutex, 0) == pdTRUE) {
            // Keep only the freshest pending frame if rendering falls behind.
            // Replaced pending allocations become spares on the next pass.
            s_ready_buffer = back;
            ++s_preview_frames;
            xSemaphoreGive(s_frame_mutex);
        }

        // Keep the capture loop from starving the idle task while frames
        // arrive continuously at the camera's configured rate.
        vTaskDelay(1);
    }

    if (s_video_fd >= 0) {
        ioctl(s_video_fd, VIDIOC_STREAMOFF, &type);
    }
    s_stream_running = false;
    xSemaphoreGive(s_stream_done);
    vTaskDelete(NULL);
}

static void camera_refresh_timer(lv_timer_t *timer)
{
    (void)timer;
    if (!s_image) return;
    if (s_snapshot_busy && xSemaphoreTake(s_snapshot_done, 0) == pdTRUE) {
        s_snapshot_busy = false;
        lv_obj_clear_state(s_shutter, LV_STATE_DISABLED);
        bool saved = s_snapshot_result == ESP_OK;
        toast_show(saved ? "PNG saved" : "Photo save failed", saved ? TOAST_SUCCESS : TOAST_ERROR);
        camera_set_status(saved ? "PNG saved to ghostesp/captures" : "Photo save failed - check SD card",
                          lv_color_hex(saved ? 0x80D080 : 0xFF8080));
    }
    if (!s_stream_running) {
        camera_set_status("Camera stopped - reopen to retry", lv_color_hex(0xFF8080));
        return;
    }
    // The full-screen settings panel obscures the preview. Keep capturing
    // fresh frames, but don't render video underneath an opaque panel.
    if (s_settings_panel) return;
    if (s_frame_mutex && xSemaphoreTake(s_frame_mutex, 0) == pdTRUE) {
        int next = s_ready_buffer;
        if (next >= 0) {
            s_ready_buffer = -1;
            s_display_buffer = next;
            ++s_displayed_frames;
        }
        uint32_t captured = s_captured_frames;
        uint32_t handed_off = s_preview_frames;
        xSemaphoreGive(s_frame_mutex);
        // LVGL timers and rendering run serially, and the P4 PPA operations
        // are blocking. The panel scans its own framebuffer, not these images.
        if (next >= 0) {
            lv_img_cache_invalidate_src(&s_preview_images[next]);
            // Descriptors belong to the UI, while buffer pointers rotate in
            // the worker. This displayed allocation is now protected by the
            // mutex handoff and remains immutable until the next UI adoption.
            s_preview_images[next].data = s_preview_buffers[next];
            lv_img_set_src(s_image, &s_preview_images[next]);
        }
        int64_t now_us = esp_timer_get_time();
        if (now_us - s_last_stats_us >= 5000000) {
            uint32_t fps10 = (uint64_t)(s_displayed_frames - s_last_stats_frames) * 10000000 /
                             (now_us - s_last_stats_us);
            ESP_LOGI(TAG, "frames captured=%lu handed_off=%lu submitted=%lu preview=%lu.%lu fps saving=%d",
                     (unsigned long)captured, (unsigned long)handed_off,
                     (unsigned long)s_displayed_frames,
                     (unsigned long)(fps10 / 10), (unsigned long)(fps10 % 10), s_snapshot_busy);
            ESP_LOGI(TAG, "timing us avg/max: copy=0 (buffer exchange) draw=%lu/%lu flush+vsync=%lu/%lu",
                     (unsigned long)(s_render_count ? s_render_total_us / s_render_count : 0),
                     (unsigned long)s_render_max_us,
                     (unsigned long)(s_render_count ? s_flush_total_us / s_render_count : 0),
                     (unsigned long)s_flush_max_us);
            s_last_stats_us = now_us;
            s_last_stats_frames = s_displayed_frames;
        }
    }
}

static void png_write_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint32_t png_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

static uint32_t png_adler32_update(uint32_t adler, const uint8_t *data, size_t length)
{
    uint32_t a = adler & 0xFFFFU;
    uint32_t b = adler >> 16;
    for (size_t i = 0; i < length; ++i) {
        a = (a + data[i]) % 65521U;
        b = (b + a) % 65521U;
    }
    return (b << 16) | a;
}

static bool png_write_chunk(FILE *file, const char type[4], const uint8_t *data, uint32_t length)
{
    uint8_t header[8];
    png_write_be32(header, length);
    memcpy(&header[4], type, 4);
    if (fwrite(header, 1, sizeof(header), file) != sizeof(header)) return false;
    if (length && fwrite(data, 1, length, file) != length) return false;

    uint32_t crc = png_crc32((const uint8_t *)type, 4);
    if (length) {
        // The chunk payload is small enough for the row-sized IDAT buffers,
        // but update CRC over it without requiring a second full-frame copy.
        uint32_t payload_crc = 0xFFFFFFFFU;
        for (int i = 0; i < 4; ++i) {
            payload_crc ^= (uint8_t)type[i];
            for (int bit = 0; bit < 8; ++bit) {
                payload_crc = (payload_crc >> 1) ^ (0xEDB88320U & (uint32_t)-(int32_t)(payload_crc & 1U));
            }
        }
        for (uint32_t i = 0; i < length; ++i) {
            payload_crc ^= data[i];
            for (int bit = 0; bit < 8; ++bit) {
                payload_crc = (payload_crc >> 1) ^ (0xEDB88320U & (uint32_t)-(int32_t)(payload_crc & 1U));
            }
        }
        crc = ~payload_crc;
    }
    uint8_t crc_bytes[4];
    png_write_be32(crc_bytes, crc);
    return fwrite(crc_bytes, 1, sizeof(crc_bytes), file) == sizeof(crc_bytes);
}

static esp_err_t camera_save_snapshot(void)
{
    bool display_suspended = false;
    if (!sd_card_jit_begin(&display_suspended, true)) return ESP_FAIL;
    esp_err_t err = ESP_OK;
    if (!sd_card_manager.is_initialized ||
        (mkdir(SD_DIR_CAPTURES, 0775) != 0 && errno != EEXIST)) {
        sd_card_jit_end(display_suspended);
        return ESP_FAIL;
    }

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char path[128];
    snprintf(path, sizeof(path), SD_DIR_CAPTURES "/camera_%04d%02d%02d_%02d%02d%02d_%06lu.png",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
             (unsigned long)(esp_timer_get_time() % 1000000));

    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    FILE *file = fd < 0 ? NULL : fdopen(fd, "wb");
    if (!file) {
        if (fd >= 0) {
            close(fd);
            unlink(path);
        }
        err = ESP_FAIL;
    } else {
        static const uint8_t png_signature[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        uint8_t ihdr[13] = {0};
        png_write_be32(&ihdr[0], CAMERA_WIDTH);
        png_write_be32(&ihdr[4], CAMERA_HEIGHT);
        ihdr[8] = 8;  // 8 bits per channel
        ihdr[9] = 2;  // truecolor RGB
        if (fwrite(png_signature, 1, sizeof(png_signature), file) != sizeof(png_signature) ||
            !png_write_chunk(file, "IHDR", ihdr, sizeof(ihdr))) {
            err = ESP_FAIL;
        }

        // Use PNG's legal stored-DEFLATE blocks. This avoids adding another
        // component while still producing a standard, lossless PNG.
        const size_t row_bytes = CAMERA_WIDTH * 3 + 1;
        uint8_t *row = heap_caps_malloc(row_bytes + 5, MALLOC_CAP_SPIRAM);
        uint32_t adler = 1;
        const uint8_t zlib_header[2] = {0x78, 0x01};
        if (!row) err = ESP_ERR_NO_MEM;
        if (err == ESP_OK && !png_write_chunk(file, "IDAT", zlib_header, sizeof(zlib_header))) err = ESP_FAIL;
        for (int y = 0; err == ESP_OK && y < CAMERA_HEIGHT; ++y) {
            const uint16_t *source = (const uint16_t *)(s_snapshot_buffer + y * CAMERA_WIDTH * 2);
            row[5] = 0; // filter type: None
            for (int x = 0; x < CAMERA_WIDTH; ++x) {
                uint16_t pixel = source[x];
                row[6 + x * 3 + 0] = (uint8_t)(((pixel >> 11) & 0x1F) * 255 / 31);
                row[6 + x * 3 + 1] = (uint8_t)(((pixel >> 5) & 0x3F) * 255 / 63);
                row[6 + x * 3 + 2] = (uint8_t)(((pixel >> 0) & 0x1F) * 255 / 31);
            }
            row[0] = (y == CAMERA_HEIGHT - 1) ? 1 : 0; // final block flag
            row[1] = (uint8_t)row_bytes;
            row[2] = (uint8_t)(row_bytes >> 8);
            row[3] = (uint8_t)~row[1];
            row[4] = (uint8_t)~row[2];
            adler = png_adler32_update(adler, &row[5], row_bytes);
            if (!png_write_chunk(file, "IDAT", row, (uint32_t)(row_bytes + 5))) err = ESP_FAIL;
            if ((y & 7) == 7) vTaskDelay(1);
        }
        uint8_t adler_bytes[4];
        png_write_be32(adler_bytes, adler);
        if (err == ESP_OK && !png_write_chunk(file, "IDAT", adler_bytes, sizeof(adler_bytes))) err = ESP_FAIL;
        if (err == ESP_OK && !png_write_chunk(file, "IEND", NULL, 0)) err = ESP_FAIL;
        if (row) heap_caps_free(row);
        if (fclose(file) != 0) err = ESP_FAIL;
        if (err != ESP_OK) unlink(path); // only the partial file created above
    }
    sd_card_jit_end(display_suspended);
    ESP_LOGI(TAG, "PNG save: %s (%s)", path, esp_err_to_name(err));
    return err;
}

static void camera_snapshot_task(void *arg)
{
    (void)arg;
    s_snapshot_result = camera_save_snapshot();
    // No LVGL calls here. The UI timer consumes the result; destroy waits
    // for the same semaphore before freeing the snapshot buffer.
    xSemaphoreGive(s_snapshot_done);
    vTaskDelete(NULL);
}

static void camera_snapshot_event(lv_event_t *event)
{
    (void)event;
    if (s_snapshot_busy) return;
    if (!s_stream_running || s_display_buffer < 0 || !s_snapshot_buffer) {
        camera_set_status("Waiting for a camera frame", lv_color_hex(0xFFFF80));
        return;
    }
    // The displayed preview cannot change inside this LVGL callback.
    memcpy(s_snapshot_buffer, s_preview_buffers[s_display_buffer], CAMERA_FRAME_BYTES);
    s_snapshot_busy = true;
    lv_obj_add_state(s_shutter, LV_STATE_DISABLED);
    camera_set_status("Saving photo...", lv_color_hex(0xFFFF80));
    if (xTaskCreate(camera_snapshot_task, "p4_camera_png", 6144, NULL, 3, NULL) != pdPASS) {
        s_snapshot_busy = false;
        lv_obj_clear_state(s_shutter, LV_STATE_DISABLED);
        camera_set_status("Unable to start photo save", lv_color_hex(0xFF8080));
    }
}

static bool camera_setting_read(camera_setting_t *setting)
{
    struct v4l2_ext_control control = {.id = setting->id};
    struct v4l2_ext_controls controls = {
        .ctrl_class = V4L2_CTRL_CLASS_USER, .count = 1, .controls = &control,
    };
    if (s_settings_fd < 0 || ioctl(s_settings_fd, VIDIOC_G_EXT_CTRLS, &controls) != 0) return false;
    setting->value = control.value;
    return true;
}

static bool camera_setting_write(camera_setting_t *setting, int64_t value)
{
    if (!setting->available || s_settings_fd < 0) return false;
    const struct v4l2_query_ext_ctrl *range = &setting->range;
    if (value < range->minimum) value = range->minimum;
    if (value > range->maximum) value = range->maximum;
    value = range->minimum + (value - range->minimum) / range->step * range->step;
    struct v4l2_ext_control control = {.id = setting->id, .value = (int32_t)value};
    struct v4l2_ext_controls controls = {
        .ctrl_class = V4L2_CTRL_CLASS_USER, .count = 1, .controls = &control,
    };
    if (ioctl(s_settings_fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(TAG, "could not set camera %s: %s", setting->name, strerror(errno));
        return false;
    }
    // Show hardware readback, never a guessed value after a failed command.
    if (!camera_setting_read(setting)) {
        setting->available = false;
        return false;
    }
    return true;
}

static void camera_setting_update_label(camera_setting_t *setting)
{
    if (setting->available) lv_label_set_text_fmt(setting->label, "%ld", (long)setting->value);
    else lv_label_set_text(setting->label, "Unavailable");
    lv_obj_clear_state(setting->minus, LV_STATE_DISABLED);
    lv_obj_clear_state(setting->plus, LV_STATE_DISABLED);
    if (!setting->available || setting->value <= setting->range.minimum)
        lv_obj_add_state(setting->minus, LV_STATE_DISABLED);
    if (!setting->available || setting->value >= setting->range.maximum)
        lv_obj_add_state(setting->plus, LV_STATE_DISABLED);
}

static void camera_settings_close(void)
{
    s_touch_button = NULL;
    if (s_settings_panel) lvgl_obj_del_safe(&s_settings_panel);
    memset(s_settings_buttons, 0, sizeof(s_settings_buttons));
    for (int i = 0; i < CAMERA_SETTING_COUNT; ++i) {
        s_camera_settings[i].label = NULL;
        s_camera_settings[i].minus = s_camera_settings[i].plus = NULL;
    }
    if (s_settings_fd >= 0) {
        close(s_settings_fd);
        s_settings_fd = -1;
    }
}

static void camera_settings_done_event(lv_event_t *event)
{
    (void)event;
    camera_settings_close();
}

static void camera_setting_change_event(lv_event_t *event)
{
    camera_setting_t *setting = lv_event_get_user_data(event);
    if (!setting || !setting->available) return;
    int64_t steps = (setting->range.maximum - setting->range.minimum) / setting->range.step / 20;
    int64_t delta = (steps > 0 ? steps : 1) * setting->range.step;
    if (lv_event_get_target(event) == setting->minus) delta = -delta;
    if (!camera_setting_write(setting, (int64_t)setting->value + delta))
        toast_show("Camera setting failed", TOAST_ERROR);
    camera_setting_update_label(setting);
}

static void camera_settings_reset_event(lv_event_t *event)
{
    (void)event;
    bool ok = true;
    for (int i = 0; i < CAMERA_SETTING_COUNT; ++i) {
        camera_setting_t *setting = &s_camera_settings[i];
        if (setting->available && !camera_setting_write(setting, setting->range.default_value)) ok = false;
        camera_setting_update_label(setting);
    }
    toast_show(ok ? "Camera defaults restored" : "Some settings could not be reset",
               ok ? TOAST_SUCCESS : TOAST_ERROR);
}

static lv_obj_t *camera_settings_add_button(const char *text, int x, int y, int width,
                                           lv_event_cb_t callback, void *data)
{
    lv_obj_t *button = lv_btn_create(s_settings_panel);
    lv_obj_set_size(button, width, 58);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x303B4C), 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, data);
    return button;
}

static void camera_settings_event(lv_event_t *event)
{
    (void)event;
    if (s_settings_panel) return;
    if (!s_stream_running) {
        toast_show("Start the camera before changing settings", TOAST_INFO);
        return;
    }
    s_settings_fd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
    if (s_settings_fd < 0) {
        toast_show("Camera settings unavailable", TOAST_ERROR);
        return;
    }
    s_touch_button = NULL;
    s_settings_panel = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_settings_panel);
    lv_obj_set_size(s_settings_panel, LV_HOR_RES, LV_VER_RES);
    lv_obj_clear_flag(s_settings_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_settings_panel, lv_color_hex(0x080B10), 0);
    lv_obj_set_style_bg_opa(s_settings_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_settings_panel, lv_color_white(), 0);
    lv_obj_t *title = lv_label_create(s_settings_panel);
    lv_label_set_text(title, "Camera settings");
    lv_obj_set_pos(title, 40, 26);
    lv_obj_t *hint = lv_label_create(s_settings_panel);
    lv_label_set_text(hint, "Applies to preview and photos. Settings last for this camera session.");
    lv_obj_set_pos(hint, 40, 66);
    lv_obj_set_style_text_font(hint, accessibility_get_font_small(), 0);
    bool any_available = false;
    for (int i = 0; i < CAMERA_SETTING_COUNT; ++i) {
        camera_setting_t *setting = &s_camera_settings[i];
        setting->range = (struct v4l2_query_ext_ctrl){.id = setting->id};
        setting->available = ioctl(s_settings_fd, VIDIOC_QUERY_EXT_CTRL, &setting->range) == 0 &&
            setting->range.type == V4L2_CTRL_TYPE_INTEGER &&
            !(setting->range.flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY)) &&
            setting->range.minimum >= INT32_MIN && setting->range.maximum <= INT32_MAX &&
            setting->range.maximum > setting->range.minimum &&
            setting->range.step > 0 && setting->range.step <= INT32_MAX && camera_setting_read(setting);
        any_available |= setting->available;
        // Keep the four controls compact on the 600px P4 panel.
        int y = 100 + i * 68;
        lv_obj_t *name = lv_label_create(s_settings_panel);
        lv_label_set_text(name, setting->name);
        lv_obj_set_pos(name, 40, y + 16);
        setting->label = lv_label_create(s_settings_panel);
        lv_obj_set_pos(setting->label, LV_HOR_RES - 330, y + 16);
        setting->minus = camera_settings_add_button(LV_SYMBOL_MINUS, LV_HOR_RES - 440, y, 72,
                                                    camera_setting_change_event, setting);
        setting->plus = camera_settings_add_button(LV_SYMBOL_PLUS, LV_HOR_RES - 112, y, 72,
                                                   camera_setting_change_event, setting);
        s_settings_buttons[i * 2] = setting->minus;
        s_settings_buttons[i * 2 + 1] = setting->plus;
        camera_setting_update_label(setting);
    }
    s_settings_buttons[CAMERA_SETTING_COUNT * 2] = camera_settings_add_button("Reset defaults", 40,
        LV_VER_RES - 90, 230, camera_settings_reset_event, NULL);
    if (!any_available) lv_obj_add_state(s_settings_buttons[CAMERA_SETTING_COUNT * 2], LV_STATE_DISABLED);
    s_settings_buttons[CAMERA_SETTING_COUNT * 2 + 1] = camera_settings_add_button("Done",
        LV_HOR_RES - 190, LV_VER_RES - 90, 150, camera_settings_done_event, NULL);
}

static void camera_back_event(lv_event_t *event)
{
    (void)event;
    if (s_settings_panel) {
        camera_settings_close();
        return;
    }
    if (s_snapshot_busy) {
        toast_show("Finishing photo save", TOAST_INFO);
        return;
    }
    display_manager_go_back();
}

static void camera_gallery_event(lv_event_t *event)
{
    (void)event;
    if (s_snapshot_busy) {
        toast_show("Finishing photo save", TOAST_INFO);
        return;
    }
    display_manager_switch_view(&sd_browser_view);
}

static void camera_stop(void)
{
    // Release our ISP fd before esp_video deinitializes the shared ISP device.
    camera_settings_close();
    s_stream_running = false;
    if (s_stream_task) {
        xSemaphoreTake(s_stream_done, portMAX_DELAY);
        s_stream_task = NULL;
    }
    if (s_snapshot_busy) {
        xSemaphoreTake(s_snapshot_done, portMAX_DELAY);
        s_snapshot_busy = false;
    }
    if (s_video_fd >= 0) {
        close(s_video_fd);
        s_video_fd = -1;
    }
    camera_deinit_video();
}

static void camera_create(void)
{
    lv_color_t bg = lv_color_hex(0x080B10);
    display_manager_fill_screen(bg);
    s_touch_button = NULL;
    s_root = gui_screen_create_root_no_bg(NULL, NULL, bg, LV_OPA_COVER);
    crowpanel_p4_camera_view.root = s_root;

    // Clip frame invalidation to the preview, keeping the control bars out of
    // the per-frame draw. The underlying image and saved PNG remain 1024x600;
    // only the rows behind the now-opaque bars are hidden.
    lv_obj_t *viewport = lv_obj_create(s_root);
    lv_obj_remove_style_all(viewport);
    lv_obj_clear_flag(viewport, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(viewport, LV_HOR_RES,
                    LV_VER_RES - CAMERA_TOP_BAR_HEIGHT - CAMERA_BOTTOM_BAR_HEIGHT);
    lv_obj_set_pos(viewport, 0, CAMERA_TOP_BAR_HEIGHT);
    s_image = lv_img_create(viewport);
    lv_obj_remove_style_all(s_image);
    lv_obj_clear_flag(s_image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_image, CAMERA_WIDTH, CAMERA_HEIGHT);
    lv_obj_set_pos(s_image, (LV_HOR_RES - CAMERA_WIDTH) / 2,
                   (LV_VER_RES - CAMERA_HEIGHT) / 2 - CAMERA_TOP_BAR_HEIGHT);

    lv_obj_t *top_bar = lv_obj_create(s_root);
    lv_obj_remove_style_all(top_bar);
    lv_obj_clear_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(top_bar, LV_PCT(100), CAMERA_TOP_BAR_HEIGHT);
    lv_obj_align(top_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(top_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(top_bar, LV_OPA_COVER, 0);

    lv_obj_t *back = lv_btn_create(top_bar);
    s_back = back;
    lv_obj_set_size(back, 56, 52);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back, camera_back_event, LV_EVENT_CLICKED, NULL);

    s_status = lv_label_create(s_root);
    lv_label_set_text(s_status, "Starting camera");
    lv_obj_set_style_text_font(s_status, accessibility_get_font_small(), 0);
    lv_obj_set_style_text_color(s_status, lv_color_white(), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 21);

    lv_obj_t *bottom_bar = lv_obj_create(s_root);
    lv_obj_remove_style_all(bottom_bar);
    lv_obj_clear_flag(bottom_bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(bottom_bar, LV_PCT(100), CAMERA_BOTTOM_BAR_HEIGHT);
    lv_obj_align(bottom_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bottom_bar, LV_OPA_COVER, 0);

    lv_obj_t *gallery = lv_btn_create(bottom_bar);
    s_gallery = gallery;
    lv_obj_set_size(gallery, 62, 62);
    lv_obj_align(gallery, LV_ALIGN_LEFT_MID, 40, -8);
    lv_obj_set_style_radius(gallery, 12, 0);
    lv_obj_set_style_bg_color(gallery, lv_color_hex(0x30343A), 0);
    lv_obj_set_style_bg_opa(gallery, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(gallery, 1, 0);
    lv_obj_set_style_border_color(gallery, lv_color_hex(0x70757C), 0);
    lv_obj_set_style_shadow_width(gallery, 0, 0);
    lv_obj_t *gallery_label = lv_label_create(gallery);
    lv_label_set_text(gallery_label, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_color(gallery_label, lv_color_white(), 0);
    lv_obj_center(gallery_label);
    lv_obj_add_event_cb(gallery, camera_gallery_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *snapshot = lv_btn_create(bottom_bar);
    s_shutter = snapshot;
    lv_obj_set_size(snapshot, 82, 82);
    // With no mode label below it, center the shutter inside the opaque bar.
    // Its shadow then stays clear of LVGL's preview invalidation margin.
    lv_obj_align(snapshot, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(snapshot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(snapshot, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(snapshot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(snapshot, 5, 0);
    lv_obj_set_style_border_color(snapshot, lv_color_hex(0xBFC5CA), 0);
    lv_obj_set_style_shadow_width(snapshot, 10, 0);
    lv_obj_set_style_shadow_opa(snapshot, LV_OPA_40, 0);
    lv_obj_add_event_cb(snapshot, camera_snapshot_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *settings = lv_btn_create(bottom_bar);
    s_settings = settings;
    lv_obj_set_size(settings, 62, 62);
    lv_obj_align(settings, LV_ALIGN_RIGHT_MID, -40, -8);
    lv_obj_set_style_bg_opa(settings, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(settings, 0, 0);
    lv_obj_set_style_shadow_width(settings, 0, 0);
    lv_obj_t *settings_label = lv_label_create(settings);
    lv_label_set_text(settings_label, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(settings_label, lv_color_white(), 0);
    lv_obj_center(settings_label);
    lv_obj_add_event_cb(settings, camera_settings_event, LV_EVENT_CLICKED, NULL);

    s_frame_mutex = xSemaphoreCreateMutex();
    s_stream_done = xSemaphoreCreateBinary();
    s_snapshot_done = xSemaphoreCreateBinary();
    if (!s_frame_mutex || !s_stream_done || !s_snapshot_done) {
        camera_set_status("Camera memory unavailable", lv_color_hex(0xFF8080));
        return;
    }
    for (int i = 0; i < CAMERA_BUFFER_COUNT; ++i) {
        s_camera_buffers[i] = heap_caps_aligned_alloc(64, CAMERA_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    }
    for (int i = 0; i < CAMERA_PREVIEW_BUFFER_COUNT; ++i) {
        s_preview_buffers[i] = heap_caps_aligned_alloc(64, CAMERA_FRAME_BYTES, MALLOC_CAP_SPIRAM);
        s_preview_images[i] = (lv_img_dsc_t) {
            .header.cf = LV_IMG_CF_TRUE_COLOR,
            .header.w = CAMERA_WIDTH,
            .header.h = CAMERA_HEIGHT,
            .data_size = CAMERA_FRAME_BYTES,
            .data = s_preview_buffers[i],
        };
    }
    s_snapshot_buffer = heap_caps_aligned_alloc(64, CAMERA_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    bool memory_ok = s_snapshot_buffer != NULL;
    for (int i = 0; i < CAMERA_BUFFER_COUNT; ++i) memory_ok = memory_ok && s_camera_buffers[i] != NULL;
    for (int i = 0; i < CAMERA_PREVIEW_BUFFER_COUNT; ++i) memory_ok = memory_ok && s_preview_buffers[i] != NULL;
    if (!memory_ok) {
        camera_set_status("Camera memory unavailable", lv_color_hex(0xFF8080));
        return;
    }

    esp_err_t err = camera_start_hardware();
    if (err != ESP_OK) {
        camera_set_status("Camera not detected", lv_color_hex(0xFF8080));
        return;
    }
    s_ready_buffer = -1;
    s_display_buffer = -1;
    s_captured_frames = s_preview_frames = s_displayed_frames = 0;
    s_last_stats_us = esp_timer_get_time();
    s_last_stats_frames = 0;
    s_stream_running = true;
    if (xTaskCreatePinnedToCore(camera_stream_task, "p4_camera", 4096, NULL, 10, &s_stream_task, 0) != pdPASS) {
        camera_stop();
        camera_set_status("Camera task failed", lv_color_hex(0xFF8080));
        return;
    }
    camera_profile_start();
    s_refresh_timer = lv_timer_create(camera_refresh_timer, CAMERA_PREVIEW_PERIOD_MS, NULL);
    camera_set_status("Live preview  •  SC2336", lv_color_hex(0x80D080));
}

static void camera_destroy(void)
{
    if (s_refresh_timer) lvgl_timer_del_safe(&s_refresh_timer);
    camera_profile_stop();
    s_touch_button = NULL;
    camera_stop();
    if (s_root) {
        lv_obj_clean(s_root);
        lvgl_obj_del_safe(&s_root);
        crowpanel_p4_camera_view.root = NULL;
    }
    for (int i = 0; i < CAMERA_BUFFER_COUNT; ++i) {
        if (s_camera_buffers[i]) heap_caps_free(s_camera_buffers[i]);
        s_camera_buffers[i] = NULL;
    }
    for (int i = 0; i < CAMERA_PREVIEW_BUFFER_COUNT; ++i) {
        lv_img_cache_invalidate_src(&s_preview_images[i]);
        heap_caps_free(s_preview_buffers[i]);
        s_preview_buffers[i] = NULL;
        s_preview_images[i].data = NULL;
    }
    if (s_snapshot_buffer) {
        heap_caps_free(s_snapshot_buffer);
        s_snapshot_buffer = NULL;
    }
    if (s_frame_mutex) {
        vSemaphoreDelete(s_frame_mutex);
        s_frame_mutex = NULL;
    }
    if (s_stream_done) vSemaphoreDelete(s_stream_done);
    if (s_snapshot_done) vSemaphoreDelete(s_snapshot_done);
    s_stream_done = s_snapshot_done = NULL;
    s_image = NULL;
    s_status = NULL;
    s_shutter = NULL;
    s_back = s_gallery = s_settings = NULL;
    s_ready_buffer = s_display_buffer = -1;
}

static lv_obj_t *camera_button_at(const lv_point_t *point)
{
    lv_obj_t *buttons[] = {s_back, s_gallery, s_shutter, s_settings};
    lv_obj_t **active = s_settings_panel ? s_settings_buttons : buttons;
    size_t count = s_settings_panel ? CAMERA_SETTINGS_BUTTON_COUNT : sizeof(buttons) / sizeof(buttons[0]);
    for (size_t i = 0; i < count; ++i) {
        lv_obj_t *button = active[i];
        if (!button || !lv_obj_is_valid(button) || lv_obj_has_state(button, LV_STATE_DISABLED)) continue;
        lv_area_t area;
        lv_obj_get_coords(button, &area);
        if (point->x >= area.x1 && point->x <= area.x2 &&
            point->y >= area.y1 && point->y <= area.y2) return button;
    }
    return NULL;
}

static void camera_input_handler(InputEvent *event)
{
    if (!event) return;
    // P4 has no native LVGL pointer indev. The manual input queue styles
    // pressed controls but each view must dispatch its own click on release.
    if (event->type == INPUT_TYPE_TOUCH) {
        const lv_indev_data_t *touch = &event->data.touch_data;
        if (touch->state == LV_INDEV_STATE_PR && !event->is_touch_move) {
            s_touch_start = touch->point;
            s_touch_button = camera_button_at(&touch->point);
            return;
        }
        if (abs(touch->point.x - s_touch_start.x) > CAMERA_TAP_SLOP ||
            abs(touch->point.y - s_touch_start.y) > CAMERA_TAP_SLOP) s_touch_button = NULL;
        if (touch->state == LV_INDEV_STATE_REL) {
            lv_obj_t *button = s_touch_button;
            s_touch_button = NULL;
            if (button && button == camera_button_at(&touch->point)) {
                // A click may destroy the view. Do not access its widgets afterward.
                lv_event_send(button, LV_EVENT_CLICKED, NULL);
            }
        }
    } else if (event->type == INPUT_TYPE_JOYSTICK || event->type == INPUT_TYPE_KEYBOARD ||
               event->type == INPUT_TYPE_EXIT_BUTTON) {
        camera_back_event(NULL);
    }
}

static void camera_get_hardware_callback(void **callback)
{
    if (callback) *callback = (void *)camera_input_handler;
}

View crowpanel_p4_camera_view = {
    .root = NULL,
    .create = camera_create,
    .destroy = camera_destroy,
    .input_callback = camera_input_handler,
    .name = "Camera",
    .get_hardwareinput_callback = camera_get_hardware_callback,
};

#endif
