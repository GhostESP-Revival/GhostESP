#include "managers/camera_stream_manager.h"
#include "managers/motion_detector_manager.h"
#include "core/glog.h"
#include "core/serial_manager.h"
#include "sdkconfig.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "img_converters.h"
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "camera_stream";

static volatile bool stream_running = false;
static volatile bool stream_active = false;

static CameraStreamState stream_state = {
    .is_running = false,
    .quality = 80,
    .frame_size = FRAMESIZE_SVGA,
    .fps_target = 15,
    .using_psram = false,
    .client_count = 0,
    .frames_sent = 0,
};

typedef struct {
    const char *name;
    int size;
} framesize_entry_t;

static const framesize_entry_t framesize_table[] = {
    {"QQVGA", FRAMESIZE_QQVGA},
    {"QVGA",  FRAMESIZE_QVGA},
    {"VGA",   FRAMESIZE_VGA},
    {"SVGA",  FRAMESIZE_SVGA},
    {"XGA",   FRAMESIZE_XGA},
    {"SXGA",  FRAMESIZE_SXGA},
    {"UXGA",  FRAMESIZE_UXGA},
};
#define FRAMESIZE_TABLE_LEN (sizeof(framesize_table) / sizeof(framesize_table[0]))

static const char *framesize_to_name(int size) {
    for (int i = 0; i < (int)FRAMESIZE_TABLE_LEN; i++) {
        if (framesize_table[i].size == size) return framesize_table[i].name;
    }
    return "UNKNOWN";
}

void camera_stream_init(void) {
    stream_state.is_running = false;
    stream_state.frames_sent = 0;
    stream_state.client_count = 0;
    glog("[CAM_STREAM] Initialized (default: SVGA, quality 80, 15 fps)\n");
}

static bool init_camera_stream(void) {
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
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = stream_state.frame_size,
        .jpeg_quality = stream_state.quality,
        .fb_count = has_psram ? 2 : 1,
        .fb_location = has_psram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        glog("[CAM_STREAM] Camera init failed: %s\n", esp_err_to_name(err));
        return false;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_brightness(s, 0);
        s->set_contrast(s, 1);
        s->set_saturation(s, 0);
        s->set_sharpness(s, 1);
    }

    stream_state.using_psram = has_psram;

    glog("[CAM_STREAM] Camera: %s JPEG quality=%d (%s buffers)\n",
         framesize_to_name(stream_state.frame_size),
         stream_state.quality,
         has_psram ? "PSRAM" : "DRAM");
    return true;
}

esp_err_t camera_stream_start(void) {
    if (stream_running) {
        glog("[CAM_STREAM] Already running\n");
        return ESP_OK;
    }

    if (g_motion_detector.is_running) {
        glog("[CAM_STREAM] Stopping motion detector to free camera...\n");
        motion_detector_stop();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (!init_camera_stream()) {
        glog("[CAM_STREAM] Failed to start\n");
        return ESP_FAIL;
    }

    stream_running = true;
    stream_state.is_running = true;
    stream_state.frames_sent = 0;
    glog("[CAM_STREAM] Started - %s @ %d fps, quality %d\n",
         framesize_to_name(stream_state.frame_size),
         stream_state.fps_target, stream_state.quality);
    return ESP_OK;
}

void camera_stream_stop(void) {
    if (!stream_running) {
        glog("[CAM_STREAM] Not running\n");
        return;
    }

    stream_running = false;

    int timeout = 60;
    while (stream_active && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (stream_active) {
        glog("[CAM_STREAM] Warning: stream still active during stop\n");
    }

    esp_camera_deinit();
    stream_state.is_running = false;
    glog("[CAM_STREAM] Stopped (frames sent: %d)\n", stream_state.frames_sent);
}

void camera_stream_set_quality(int quality) {
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    stream_state.quality = quality;

    if (stream_running) {
        sensor_t *s = esp_camera_sensor_get();
        if (s) {
            s->set_quality(s, quality);
        }
    }
    glog("[CAM_STREAM] Quality set to %d\n", quality);
}

void camera_stream_set_framesize(const char *name) {
    if (!name) return;

    for (int i = 0; i < (int)FRAMESIZE_TABLE_LEN; i++) {
        if (strcasecmp(framesize_table[i].name, name) == 0) {
            int new_size = framesize_table[i].size;

            if (stream_running) {
                sensor_t *s = esp_camera_sensor_get();
                if (s) {
                    if (s->set_framesize(s, new_size) != 0) {
                        glog("[CAM_STREAM] Failed to set resolution to %s\n", name);
                        return;
                    }
                }
            }

            stream_state.frame_size = new_size;
            glog("[CAM_STREAM] Resolution set to %s\n", name);
            return;
        }
    }
    glog("[CAM_STREAM] Unknown resolution: %s\n", name);
    glog("[CAM_STREAM] Available: QQVGA QVGA VGA SVGA XGA SXGA UXGA\n");
}

void camera_stream_set_fps(int fps) {
    if (fps < 1) fps = 1;
    if (fps > 30) fps = 30;
    stream_state.fps_target = fps;
    glog("[CAM_STREAM] Target FPS set to %d\n", fps);
}

CameraStreamState camera_stream_get_state(void) {
    stream_state.client_count = stream_active ? 1 : 0;
    return stream_state;
}

esp_err_t camera_stream_api_handler(httpd_req_t *req) {
    char content[256];
    int command_len = req->content_len;
    if (command_len <= 0 || command_len >= (int)sizeof(content)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Bad request");
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, content, command_len);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    content[command_len] = '\0';

    cJSON *json = cJSON_Parse(content);
    if (!json) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *cmd = cJSON_GetObjectItem(json, "command");
    if (!cmd || !cJSON_IsString(cmd)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Missing command");
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    const char *command = cmd->valuestring;

    if (strncmp(command, "camerastream ", 13) == 0 || strcmp(command, "camerastream") == 0) {
        simulateCommand(command);
        httpd_resp_sendstr(req, "OK");
    } else {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "Only camerastream commands allowed");
    }

    cJSON_Delete(json);
    return ESP_OK;
}

esp_err_t camera_stream_page_handler(httpd_req_t *req) {
    const char *html =
        "<!DOCTYPE html><html><head><title>GhostESP Camera</title>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>*{margin:0;padding:0;box-sizing:border-box}"
        "body{background:#111;color:#fff;font-family:'Courier New',monospace;"
        "display:flex;flex-direction:column;align-items:center;height:100vh;overflow:hidden}"
        ".hdr{padding:10px 0;text-align:center;width:100%;background:#1a1a1a;"
        "border-bottom:1px solid #333;flex-shrink:0}"
        ".hdr h2{font-size:15px;font-weight:400;letter-spacing:2px;color:#fff}"
        ".hdr .sub{color:#888;font-size:11px;margin-top:3px}"
        ".wrap{flex:1;display:flex;align-items:center;justify-content:center;"
        "padding:6px;width:100%;min-height:0}"
        ".wrap img{max-width:100%;max-height:72vh;border:1px solid #333;border-radius:2px}"
        ".ctrls{width:100%;padding:8px 12px;background:#1a1a1a;border-top:1px solid #333;"
        "display:flex;align-items:center;justify-content:center;gap:8px;flex-wrap:wrap;flex-shrink:0}"
        ".ctrls label{font-size:11px;color:#aaa}"
        ".ctrls select,.ctrls input[type=range]{background:#222;color:#fff;border:1px solid #444;"
        "padding:3px 6px;border-radius:3px;font-size:11px;font-family:inherit}"
        ".ctrls input[type=range]{width:80px;vertical-align:middle}"
        ".btn{background:#222;color:#fff;border:1px solid #555;padding:5px 14px;"
        "border-radius:3px;cursor:pointer;font-size:11px;font-family:inherit}"
        ".btn:hover{background:#333}.btn.r{border-color:#a33}.btn.r:hover{background:#411}"
        ".btn.g{border-color:#3a3}.btn.g:hover{background:#131}"
        ".val{color:#fff;font-size:11px;min-width:24px;display:inline-block;text-align:center}"
        ".err{color:#f44;text-align:center;padding:40px;display:none}"
        ".err h3{font-size:14px}.err p{color:#888;margin-top:8px;font-size:12px}"
        "</style></head>"
        "<body>"
        "<div class='hdr'><h2>GHOSTESP CAMERA</h2>"
        "<div class='sub' id='st'>Starting...</div></div>"
        "<div class='wrap'><img id='vid' style='display:none'"
        " onerror='onErr()' onload='onOk()'></div>"
        "<div class='err' id='err'><h3>Stream Unavailable</h3>"
        "<p>Auto-starting...</p></div>"
        "<div class='ctrls'>"
        "<label>Quality<input type='range' id='qslider' min='10' max='100' value='80'"
        " oninput='setQ(this.value)'></label><span class='val' id='qval'>80</span>"
        "<label>Res <select id='ressel' onchange='setRes(this.value)'>"
        "<option value='QQVGA'>160x120</option>"
        "<option value='QVGA'>320x240</option>"
        "<option value='VGA'>640x480</option>"
        "<option value='SVGA' selected>800x600</option>"
        "<option value='XGA'>1024x768</option>"
        "<option value='SXGA'>1280x1024</option>"
        "<option value='UXGA'>1600x1200</option>"
        "</select></label>"
        "<label>FPS<select id='fpssel' onchange='setFps(this.value)'>"
        "<option value='5'>5</option><option value='10'>10</option>"
        "<option value='15' selected>15</option><option value='20'>20</option>"
        "<option value='25'>25</option><option value='30'>30</option>"
        "</select></label>"
        "<button class='btn r' id='stopbtn' onclick='doStop()'>Stop</button>"
        "<button class='btn g' id='startbtn' onclick='doStart()' style='display:none'>Start</button>"
        "</div>"
        "<script>"
        "var v=document.getElementById('vid'),"
        "st=document.getElementById('st'),"
        "er=document.getElementById('err'),"
        "stopBtn=document.getElementById('stopbtn'),"
        "startBtn=document.getElementById('startbtn');"
        "function cmd(c){var x=new XMLHttpRequest();"
        "x.open('POST','/camera/api');x.setRequestHeader('Content-Type','application/json');"
        "x.send(JSON.stringify({command:c}));}"
        "function doStart(){cmd('camerastream start');"
        "st.textContent='Starting...';st.style.color='#888';er.style.display='none';"
        "stopBtn.style.display='';startBtn.style.display='none';"
        "setTimeout(function(){v.src='/camera/stream';v.style.display='';},2000);}"
        "function doStop(){cmd('camerastream stop');"
        "v.style.display='none';v.src='';"
        "st.textContent='Stopped';st.style.color='#888';"
        "stopBtn.style.display='none';startBtn.style.display='';}"
        "function setQ(v){document.getElementById('qval').textContent=v;"
        "cmd('camerastream quality '+v);}"
        "function setRes(v){cmd('camerastream resolution '+v);"
        "v2=document.getElementById('vid');v2.src='/camera/stream?t='+Date.now();}"
        "function setFps(v){cmd('camerastream fps '+v);}"
        "function onErr(){v.style.display='none';er.style.display='block';"
        "st.textContent='Disconnected';st.style.color='#f44';"
        "stopBtn.style.display='none';startBtn.style.display='';"
        "setTimeout(function(){v.src='/camera/stream?t='+Date.now();"
        "v.style.display='';er.style.display='none';"
        "stopBtn.style.display='';startBtn.style.display='none';},3000);}"
        "function onOk(){er.style.display='none';st.textContent='Streaming';"
        "st.style.color='#fff';stopBtn.style.display='';startBtn.style.display='none';}"
        "doStart();"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, html);
}

esp_err_t camera_stream_http_handler(httpd_req_t *req) {
    if (!stream_running) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Camera stream not active. Start with: camerastream start");
        return ESP_OK;
    }

    if (stream_active) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Stream busy - another client is connected");
        return ESP_OK;
    }

    stream_active = true;

    esp_err_t res = httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
    if (res != ESP_OK) {
        stream_active = false;
        return res;
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Framerate", "15");

    TickType_t frame_delay = pdMS_TO_TICKS(1000 / stream_state.fps_target);
    int frames = 0;

    while (stream_running) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            glog("[CAM_STREAM] Frame capture failed\n");
            break;
        }

        char part_hdr[96];
        int hl = snprintf(part_hdr, sizeof(part_hdr),
                          "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                          (unsigned)fb->len);

        res = httpd_resp_send_chunk(req, part_hdr, hl);
        if (res != ESP_OK) {
            esp_camera_fb_return(fb);
            break;
        }

        res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        if (res != ESP_OK) {
            esp_camera_fb_return(fb);
            break;
        }

        res = httpd_resp_send_chunk(req, "\r\n", 2);
        esp_camera_fb_return(fb);
        if (res != ESP_OK) {
            break;
        }

        frames++;
        stream_state.frames_sent = frames;
        vTaskDelay(frame_delay);
    }

    stream_active = false;
    glog("[CAM_STREAM] Client disconnected after %d frames\n", frames);
    return ESP_OK;
}
