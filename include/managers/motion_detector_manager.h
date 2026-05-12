#ifndef MOTION_DETECTOR_MANAGER_H
#define MOTION_DETECTOR_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool is_running;
    bool is_initialized;
    int threshold;
    int interval_ms;
    int trigger_percent;
    int sample_step;
    bool save_snapshots;
    bool send_discord_image;
    bool webhook_enabled;
    int webhook_cooldown_ms;
    bool using_psram;
    int motion_count;
    char webhook_url[256];
} MotionDetectorState;

void motion_detector_init(void);
esp_err_t motion_detector_start(void);
void motion_detector_stop(void);
void motion_detector_set_threshold(int threshold);
void motion_detector_set_interval(int interval_ms);
void motion_detector_set_trigger_percent(int percent);
void motion_detector_set_sample_step(int sample_step);
void motion_detector_set_save_snapshots(bool save);
void motion_detector_set_discord_image(bool enabled);
void motion_detector_set_webhook(const char *url);
void motion_detector_clear_webhook(void);
void motion_detector_set_webhook_cooldown(int cooldown_ms);
MotionDetectorState motion_detector_get_state(void);

extern MotionDetectorState g_motion_detector;

#endif
