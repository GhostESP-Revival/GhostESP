#include "managers/motion_detector_manager.h"
#include "core/glog.h"
#include "managers/sd_card_manager.h"
#include "esp_camera.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "img_converters.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#define MOTION_NVS_NAMESPACE "motion_cfg"
#define MOTION_NVS_WEBHOOK_KEY "webhook_url"

static bool sntp_synced = false;

static void load_webhook_from_nvs(void);
static void save_webhook_to_nvs(const char *url);

static TaskHandle_t motion_task_handle = NULL;
static uint8_t *prev_frame_buf = NULL;
static int frame_width = 0;
static int frame_height = 0;
static int prev_frame_len = 0;

#define MOTION_DISCORD_BOUNDARY "----GhostESPMotion"
#define JPEG_QUALITY 60
#define DISCORD_UPLOAD_QUALITY 40
#define MOTION_WARMUP_FRAMES 4
#define MOTION_CONFIRM_FRAMES 2
#define MOTION_CLEAR_FRAMES 2
#define MOTION_HYSTERESIS_PERCENT 3
#define MOTION_CAPTURES_DIR "/mnt/ghostesp/captures"

typedef struct {
    char url[256];
    char content_type[96];
    uint8_t *body;
    size_t body_len;
} WebhookJob;

MotionDetectorState g_motion_detector = {
    .is_running = false,
    .is_initialized = false,
    .threshold = CONFIG_MOTION_DETECT_THRESHOLD,
    .interval_ms = CONFIG_MOTION_DETECT_INTERVAL_MS,
    .trigger_percent = CONFIG_MOTION_DETECT_PERCENT,
    .sample_step = CONFIG_MOTION_DETECT_SAMPLE_STEP,
    .save_snapshots = false,
    .send_discord_image = true,
    .webhook_enabled = false,
    .webhook_cooldown_ms = CONFIG_MOTION_WEBHOOK_COOLDOWN_MS,
    .using_psram = false,
    .motion_count = 0,
    .webhook_url = {0},
};

static TickType_t last_webhook_tick = 0;

static bool init_camera(void)
{
    bool has_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0;
    camera_config_t config = {
        .pin_pwdn = CONFIG_CAM_PIN_PWDN,
        .pin_reset = CONFIG_CAM_PIN_RESET,
        .pin_xclk = CONFIG_CAM_PIN_XCLK,
        .pin_sccb_sda = CONFIG_CAM_PIN_SIOD,
        .pin_sccb_scl = CONFIG_CAM_PIN_SIOC,
        .pin_d7 = CONFIG_CAM_PIN_D7,
        .pin_d6 = CONFIG_CAM_PIN_D6,
        .pin_d5 = CONFIG_CAM_PIN_D5,
        .pin_d4 = CONFIG_CAM_PIN_D4,
        .pin_d3 = CONFIG_CAM_PIN_D3,
        .pin_d2 = CONFIG_CAM_PIN_D2,
        .pin_d1 = CONFIG_CAM_PIN_D1,
        .pin_d0 = CONFIG_CAM_PIN_D0,
        .pin_vsync = CONFIG_CAM_PIN_VSYNC,
        .pin_href = CONFIG_CAM_PIN_HREF,
        .pin_pclk = CONFIG_CAM_PIN_PCLK,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_GRAYSCALE,
        .frame_size = FRAMESIZE_QQVGA,
        .jpeg_quality = JPEG_QUALITY,
        .fb_count = has_psram ? 2 : 1,
        .fb_location = has_psram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        glog("[MOTION] Camera init failed: %s\n", esp_err_to_name(err));
        return false;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_brightness(s, 0);
        s->set_contrast(s, 0);
        s->set_saturation(s, 0);
    }

    frame_width = 160;
    frame_height = 120;
    prev_frame_len = frame_width * frame_height;
    g_motion_detector.using_psram = has_psram;
    glog("[MOTION] Camera: QQVGA grayscale (%s buffers)\n", has_psram ? "PSRAM" : "DRAM");
    return true;
}

static void deinit_camera(void)
{
    esp_camera_deinit();
}

static bool save_snapshot_to_sd(camera_fb_t *fb, char *out_path, size_t out_path_len)
{
    if (!fb) return false;

    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    bool converted = false;

    if (fb->format == PIXFORMAT_JPEG) {
        jpg_buf = fb->buf;
        jpg_len = fb->len;
    } else {
        converted = fmt2jpg(fb->buf, fb->len, fb->width, fb->height,
                            fb->format, JPEG_QUALITY, &jpg_buf, &jpg_len);
        if (!converted || !jpg_buf) {
            glog("[MOTION] SD JPEG convert failed\n");
            return false;
        }
    }

    bool display_was_suspended = false;
    bool mounted_here = false;

    if (!sd_card_manager.is_initialized) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
        if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
            esp_err_t mount_ret = sd_card_mount_for_flush(&display_was_suspended);
            if (mount_ret != ESP_OK) {
                glog("[MOTION] SD mount failed: %s\n", esp_err_to_name(mount_ret));
                if (converted) free(jpg_buf);
                return false;
            }
            mounted_here = true;
        } else
#endif
        {
            if (converted) free(jpg_buf);
            return false;
        }
    }

    char path[64];
    snprintf(path, sizeof(path), MOTION_CAPTURES_DIR "/motion_%d.jpg", g_motion_detector.motion_count);

    if (!sd_card_exists(MOTION_CAPTURES_DIR)) {
        (void)sd_card_create_directory(MOTION_CAPTURES_DIR);
    }

    esp_err_t write_ret = sd_card_write_file(path, jpg_buf, jpg_len);
    if (mounted_here) {
        sd_card_unmount_after_flush(display_was_suspended);
    }
    if (write_ret != ESP_OK) {
        glog("[MOTION] SD write failed: %s\n", path);
        if (converted) free(jpg_buf);
        return false;
    }

    if (out_path && out_path_len > 0) {
        strlcpy(out_path, path, out_path_len);
    }

    glog("[MOTION] Saved: %s (%d B)\n", path, (int)jpg_len);
    if (converted) free(jpg_buf);
    return true;
}

static bool should_auto_save_webhook_snapshot(void)
{
    bool sd_available = sd_card_manager.is_initialized;

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (!sd_available && strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        sd_available = true;
    }
#endif

    return sd_available && g_motion_detector.webhook_enabled && g_motion_detector.send_discord_image;
}

static inline int compare_frames_fast(uint8_t *curr, uint8_t *prev, int len, int threshold, int step)
{
    int changed = 0;
    for (int i = 0; i < len; i += step) {
        int diff = (int)curr[i] - (int)prev[i];
        if (diff < 0) diff = -diff;
        if (diff > threshold) changed++;
    }
    return changed;
}

static void free_webhook_job(WebhookJob *job)
{
    if (!job) return;
    free(job->body);
    free(job);
}

static void ensure_sntp_synced(void)
{
    if (sntp_synced) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        if (tv.tv_sec > 1700000000) return;
        sntp_synced = false;
    }

    esp_netif_sntp_deinit();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t ret = esp_netif_sntp_init(&config);
    if (ret != ESP_OK) {
        glog("[MOTION] SNTP init failed: %s\n", esp_err_to_name(ret));
        return;
    }

    ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
    if (ret == ESP_OK) {
        time_t now;
        struct tm utc_timeinfo;
        char time_buf[32];

        time(&now);
        gmtime_r(&now, &utc_timeinfo);
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S UTC", &utc_timeinfo);

        sntp_synced = true;
        glog("[MOTION] SNTP synced: %s\n", time_buf);
    } else {
        glog("[MOTION] SNTP sync failed: %s\n", esp_err_to_name(ret));
    }

    esp_netif_sntp_deinit();
}

static void webhook_task(void *arg)
{
    WebhookJob *job = (WebhookJob *)arg;

    ensure_sntp_synced();

    esp_http_client_config_t config = {
        .url = job->url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        glog("[MOTION] HTTP client init failed\n");
        free_webhook_job(job);
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_set_header(client, "Content-Type", job->content_type);
    esp_http_client_set_post_field(client, (const char *)job->body, job->body_len);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        glog("[MOTION] Discord sent (HTTP %d)\n", status);
    } else {
        glog("[MOTION] Discord failed: %s\n", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free_webhook_job(job);
    vTaskDelete(NULL);
}

static bool build_json_webhook_body(WebhookJob *job, float pct, int changed, int sampled, const char *snap)
{
    static const char *embed_fmt =
        "{\"username\":\"GhostESP Motion\",\"embeds\":[{"
        "\"title\":\"Motion #%d\","
        "\"color\":16753920,"
        "\"fields\":["
        "{\"name\":\"Change\",\"value\":\"%.1f%%\",\"inline\":true},"
        "{\"name\":\"Pixels\",\"value\":\"%d/%d\",\"inline\":true},"
        "{\"name\":\"Snapshot\",\"value\":\"%s\",\"inline\":false}"
        "],\"footer\":{\"text\":\"GhostESP\"}}]}";

    job->body_len = 512;
    job->body = heap_caps_malloc(job->body_len, MALLOC_CAP_SPIRAM);
    if (!job->body) job->body = heap_caps_malloc(job->body_len, MALLOC_CAP_8BIT);
    if (!job->body) return false;

    int n = snprintf((char *)job->body, job->body_len, embed_fmt,
                     g_motion_detector.motion_count, pct, changed, sampled,
                     snap ? snap : "N/A");
    if (n <= 0 || (size_t)n >= job->body_len) return false;
    job->body_len = n;
    strlcpy(job->content_type, "application/json", sizeof(job->content_type));
    return true;
}

static bool build_multipart_webhook_body(WebhookJob *job, camera_fb_t *fb, float pct, int changed, int sampled, const char *snap)
{
    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    bool converted = false;

    if (fb->format == PIXFORMAT_JPEG) {
        jpg_buf = fb->buf;
        jpg_len = fb->len;
    } else {
        converted = fmt2jpg(fb->buf, fb->len, fb->width, fb->height,
                            fb->format, DISCORD_UPLOAD_QUALITY, &jpg_buf, &jpg_len);
        if (!converted || !jpg_buf) {
            glog("[MOTION] JPEG convert failed\n");
            return false;
        }
    }

    static const char *embed_fmt =
        "{\"username\":\"GhostESP Motion\",\"embeds\":[{"
        "\"title\":\"Motion #%d\","
        "\"color\":16753920,"
        "\"image\":{\"url\":\"attachment://motion.jpg\"},"
        "\"fields\":["
        "{\"name\":\"Change\",\"value\":\"%.1f%%\",\"inline\":true},"
        "{\"name\":\"Pixels\",\"value\":\"%d/%d\",\"inline\":true},"
        "{\"name\":\"Snapshot\",\"value\":\"%s\",\"inline\":false}"
        "],\"footer\":{\"text\":\"GhostESP\"}}]}";

    char payload[512];
    int pn = snprintf(payload, sizeof(payload), embed_fmt,
                      g_motion_detector.motion_count, pct, changed, sampled,
                      snap ? snap : "Attached");
    if (pn <= 0 || pn >= (int)sizeof(payload)) {
        if (converted) free(jpg_buf);
        return false;
    }

    static const char *boundary = MOTION_DISCORD_BOUNDARY;
    char header[192];
    int hn = snprintf(header, sizeof(header),
                      "--%s\r\nContent-Disposition: form-data; name=\"payload_json\"\r\n"
                      "Content-Type: application/json\r\n\r\n", boundary);
    int mn = snprintf(NULL, 0,
                      "\r\n--%s\r\nContent-Disposition: form-data; name=\"files[0]\"; "
                      "filename=\"motion.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n", boundary);
    char mid[140];
    snprintf(mid, sizeof(mid),
             "\r\n--%s\r\nContent-Disposition: form-data; name=\"files[0]\"; "
             "filename=\"motion.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n", boundary);

    char tail[48];
    int tn = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", boundary);

    if (hn <= 0 || mn <= 0 || tn <= 0) {
        if (converted) free(jpg_buf);
        return false;
    }

    size_t total = (size_t)hn + (size_t)pn + 2 + (size_t)mn + jpg_len + (size_t)tn;
    job->body = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!job->body) job->body = heap_caps_malloc(total, MALLOC_CAP_8BIT);
    if (!job->body) {
        if (converted) free(jpg_buf);
        return false;
    }

    uint8_t *c = job->body;
    memcpy(c, header, hn); c += hn;
    memcpy(c, payload, pn); c += pn;
    memcpy(c, "\r\n", 2); c += 2;
    memcpy(c, mid, mn); c += mn;
    memcpy(c, jpg_buf, jpg_len); c += jpg_len;
    memcpy(c, tail, tn);
    job->body_len = total;

    snprintf(job->content_type, sizeof(job->content_type),
             "multipart/form-data; boundary=%s", boundary);

    if (converted) free(jpg_buf);
    return true;
}

static void send_motion_webhook(camera_fb_t *fb, float pct, int changed, int sampled, const char *snap)
{
    if (!g_motion_detector.webhook_enabled || g_motion_detector.webhook_url[0] == '\0') return;

    TickType_t now = xTaskGetTickCount();
    TickType_t cooldown = pdMS_TO_TICKS(g_motion_detector.webhook_cooldown_ms);
    if (last_webhook_tick != 0 && (now - last_webhook_tick) < cooldown) return;

    WebhookJob *job = heap_caps_calloc(1, sizeof(WebhookJob), MALLOC_CAP_SPIRAM);
    if (!job) job = heap_caps_calloc(1, sizeof(WebhookJob), MALLOC_CAP_8BIT);
    if (!job) return;

    strlcpy(job->url, g_motion_detector.webhook_url, sizeof(job->url));

    bool built = false;
    if (g_motion_detector.send_discord_image && fb) {
        built = build_multipart_webhook_body(job, fb, pct, changed, sampled, snap);
    }
    if (!built) {
        built = build_json_webhook_body(job, pct, changed, sampled, snap);
    }
    if (!built) {
        free(job);
        return;
    }

    BaseType_t ret = xTaskCreate(webhook_task, "disc_webhook", 12288, job, 4, NULL);
    if (ret != pdPASS) {
        free_webhook_job(job);
        return;
    }
    last_webhook_tick = now;
}

static void motion_detection_task(void *arg)
{
    int total = prev_frame_len;
    int step = g_motion_detector.sample_step;
    int sampled = (total + step - 1) / step;
    int warmup_frames = MOTION_WARMUP_FRAMES;
    int above_count = 0;
    int below_count = 0;
    bool in_motion = false;

    glog("[MOTION] Running: QQVGA thresh=%d interval=%dms trigger=%d%% step=%d psram=%s\n",
         g_motion_detector.threshold,
         g_motion_detector.interval_ms,
         g_motion_detector.trigger_percent,
         step,
         g_motion_detector.using_psram ? "yes" : "no");

    bool first = true;

    while (g_motion_detector.is_running) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(g_motion_detector.interval_ms));
            continue;
        }

        if (fb->format != PIXFORMAT_GRAYSCALE || (int)fb->len < prev_frame_len) {
            esp_camera_fb_return(fb);
            vTaskDelay(pdMS_TO_TICKS(g_motion_detector.interval_ms));
            continue;
        }

        if (first) {
            memcpy(prev_frame_buf, fb->buf, prev_frame_len);
            first = false;
            esp_camera_fb_return(fb);
            vTaskDelay(pdMS_TO_TICKS(g_motion_detector.interval_ms));
            continue;
        }

        int changed = compare_frames_fast(fb->buf, prev_frame_buf, prev_frame_len,
                                          g_motion_detector.threshold, step);
        float pct = ((float)changed / sampled) * 100.0f;
        int exit_percent = g_motion_detector.trigger_percent - MOTION_HYSTERESIS_PERCENT;
        if (exit_percent < 1) exit_percent = 1;

        if (warmup_frames > 0) {
            warmup_frames--;
            memcpy(prev_frame_buf, fb->buf, prev_frame_len);
            esp_camera_fb_return(fb);
            vTaskDelay(pdMS_TO_TICKS(g_motion_detector.interval_ms));
            continue;
        }

        bool trigger_event = false;

        if (!in_motion) {
            if (pct >= g_motion_detector.trigger_percent) {
                above_count++;
                if (above_count >= MOTION_CONFIRM_FRAMES) {
                    in_motion = true;
                    above_count = 0;
                    below_count = 0;
                    trigger_event = true;
                }
            } else {
                above_count = 0;
            }
        } else {
            if (pct < exit_percent) {
                below_count++;
                if (below_count >= MOTION_CLEAR_FRAMES) {
                    in_motion = false;
                    below_count = 0;
                }
            } else {
                below_count = 0;
            }
        }

        if (trigger_event) {
            g_motion_detector.motion_count++;
            glog("[MOTION] #%d %.1f%% changed\n", g_motion_detector.motion_count, pct);

            const char *snap = NULL;
            char path[64] = {0};
            bool should_save_snapshot = g_motion_detector.save_snapshots || should_auto_save_webhook_snapshot();
            if (should_save_snapshot) {
                if (save_snapshot_to_sd(fb, path, sizeof(path))) {
                    snap = path;
                }
            }
            send_motion_webhook(fb, pct, changed, sampled, snap);
        }

        memcpy(prev_frame_buf, fb->buf, prev_frame_len);
        esp_camera_fb_return(fb);

        vTaskDelay(pdMS_TO_TICKS(g_motion_detector.interval_ms));
    }

    deinit_camera();
    glog("[MOTION] Stopped (%d events)\n", g_motion_detector.motion_count);
    vTaskDelete(NULL);
}

void motion_detector_init(void)
{
    g_motion_detector.is_initialized = true;
    g_motion_detector.is_running = false;
    g_motion_detector.motion_count = 0;
    g_motion_detector.threshold = CONFIG_MOTION_DETECT_THRESHOLD;
    g_motion_detector.interval_ms = CONFIG_MOTION_DETECT_INTERVAL_MS;
    g_motion_detector.trigger_percent = CONFIG_MOTION_DETECT_PERCENT;
    g_motion_detector.sample_step = CONFIG_MOTION_DETECT_SAMPLE_STEP;
    g_motion_detector.save_snapshots = false;
    g_motion_detector.send_discord_image = true;
    g_motion_detector.webhook_enabled = false;
    g_motion_detector.webhook_cooldown_ms = CONFIG_MOTION_WEBHOOK_COOLDOWN_MS;
    g_motion_detector.using_psram = false;
    g_motion_detector.webhook_url[0] = '\0';
    last_webhook_tick = 0;
    load_webhook_from_nvs();
    glog("[MOTION] Manager initialized\n");
}

esp_err_t motion_detector_start(void)
{
    if (g_motion_detector.is_running) {
        glog("[MOTION] Already running\n");
        return ESP_OK;
    }

    if (!init_camera()) return ESP_FAIL;

    bool psram = g_motion_detector.using_psram;
    int caps = psram ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : MALLOC_CAP_8BIT;

    prev_frame_buf = heap_caps_malloc(prev_frame_len, caps);
    if (!prev_frame_buf && psram) prev_frame_buf = malloc(prev_frame_len);
    if (!prev_frame_buf) {
        glog("[MOTION] Failed to alloc prev frame buffer\n");
        deinit_camera();
        return ESP_FAIL;
    }

    g_motion_detector.is_running = true;
    g_motion_detector.motion_count = 0;

    BaseType_t ret = xTaskCreate(
        motion_detection_task, "motion_det", 8192, NULL, 5, &motion_task_handle);

    if (ret != pdPASS) {
        glog("[MOTION] Task create failed\n");
        g_motion_detector.is_running = false;
        free(prev_frame_buf);
        prev_frame_buf = NULL;
        deinit_camera();
        return ESP_FAIL;
    }

    return ESP_OK;
}

void motion_detector_stop(void)
{
    if (!g_motion_detector.is_running) return;
    g_motion_detector.is_running = false;
    vTaskDelay(pdMS_TO_TICKS(g_motion_detector.interval_ms + 200));
    if (prev_frame_buf) {
        free(prev_frame_buf);
        prev_frame_buf = NULL;
    }
    motion_task_handle = NULL;
}

void motion_detector_set_threshold(int threshold)
{
    if (threshold < 1) threshold = 1;
    if (threshold > 255) threshold = 255;
    g_motion_detector.threshold = threshold;
    glog("[MOTION] Threshold=%d\n", threshold);
}

void motion_detector_set_interval(int interval_ms)
{
    if (interval_ms < 100) interval_ms = 100;
    if (interval_ms > 10000) interval_ms = 10000;
    g_motion_detector.interval_ms = interval_ms;
    glog("[MOTION] Interval=%dms\n", interval_ms);
}

void motion_detector_set_trigger_percent(int percent)
{
    if (percent < 1) percent = 1;
    if (percent > 100) percent = 100;
    g_motion_detector.trigger_percent = percent;
    glog("[MOTION] Trigger=%d%%\n", percent);
}

void motion_detector_set_sample_step(int sample_step)
{
    if (sample_step < 1) sample_step = 1;
    if (sample_step > 32) sample_step = 32;
    g_motion_detector.sample_step = sample_step;
    glog("[MOTION] Sample=%d\n", sample_step);
}

void motion_detector_set_save_snapshots(bool save)
{
    g_motion_detector.save_snapshots = save;
    glog("[MOTION] Snapshots=%s\n", save ? "ON" : "OFF");
}

void motion_detector_set_discord_image(bool enabled)
{
    g_motion_detector.send_discord_image = enabled;
    glog("[MOTION] Discord image=%s\n", enabled ? "ON" : "OFF");
}

static void save_webhook_to_nvs(const char *url)
{
    nvs_handle_t h;
    if (nvs_open(MOTION_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    if (url && url[0]) {
        nvs_set_str(h, MOTION_NVS_WEBHOOK_KEY, url);
    } else {
        nvs_erase_key(h, MOTION_NVS_WEBHOOK_KEY);
    }
    nvs_commit(h);
    nvs_close(h);
}

static void load_webhook_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(MOTION_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(g_motion_detector.webhook_url);
    if (nvs_get_str(h, MOTION_NVS_WEBHOOK_KEY, g_motion_detector.webhook_url, &len) == ESP_OK) {
        g_motion_detector.webhook_enabled = true;
        glog("[MOTION] Webhook restored from NVS\n");
    }
    nvs_close(h);
}

void motion_detector_set_webhook(const char *url)
{
    if (!url || url[0] == '\0') {
        motion_detector_clear_webhook();
        return;
    }
    strlcpy(g_motion_detector.webhook_url, url, sizeof(g_motion_detector.webhook_url));
    g_motion_detector.webhook_enabled = true;
    last_webhook_tick = 0;
    save_webhook_to_nvs(url);
    glog("[MOTION] Discord webhook enabled\n");
}

void motion_detector_clear_webhook(void)
{
    g_motion_detector.webhook_enabled = false;
    g_motion_detector.webhook_url[0] = '\0';
    last_webhook_tick = 0;
    save_webhook_to_nvs(NULL);
    glog("[MOTION] Webhook disabled\n");
}

void motion_detector_set_webhook_cooldown(int cooldown_ms)
{
    if (cooldown_ms < 0) cooldown_ms = 0;
    if (cooldown_ms > 3600000) cooldown_ms = 3600000;
    g_motion_detector.webhook_cooldown_ms = cooldown_ms;
    glog("[MOTION] Cooldown=%dms\n", cooldown_ms);
}

MotionDetectorState motion_detector_get_state(void)
{
    return g_motion_detector;
}
