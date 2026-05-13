#ifndef CAMERA_STREAM_MANAGER_H
#define CAMERA_STREAM_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include <esp_http_server.h>

typedef struct {
    bool is_running;
    int quality;
    int frame_size;
    int fps_target;
    bool using_psram;
    int client_count;
    int frames_sent;
} CameraStreamState;

void camera_stream_init(void);
esp_err_t camera_stream_start(void);
void camera_stream_stop(void);
void camera_stream_set_quality(int quality);
void camera_stream_set_framesize(const char *name);
void camera_stream_set_fps(int fps);
CameraStreamState camera_stream_get_state(void);
esp_err_t camera_stream_page_handler(httpd_req_t *req);
esp_err_t camera_stream_http_handler(httpd_req_t *req);
esp_err_t camera_stream_api_handler(httpd_req_t *req);

#endif
