// flock_detector_manager.h
//
// Flock Safety camera detector for GhostESP
// Based on bennjordan/flock-you (https://github.com/bennjordan/flock-you)
//
#ifndef FLOCK_DETECTOR_MANAGER_H
#define FLOCK_DETECTOR_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef CONFIG_SPIRAM
#define FLOCK_MAX_DETECTIONS 64
#else
#define FLOCK_MAX_DETECTIONS 32
#endif
#define FLOCK_MAC_STR_LEN 18
#define FLOCK_METHOD_STR_LEN 16
#define FLOCK_SSID_STR_LEN 33

typedef enum {
    FLOCK_DETECT_OUI_ADDR2 = 0,
    FLOCK_DETECT_OUI_ADDR1 = 1,
    FLOCK_DETECT_OUI_ADDR3 = 2,
    FLOCK_DETECT_SSID      = 3,
    FLOCK_DETECT_WILDCARD_PROBE = 4,
} FlockDetectMethod;

typedef enum {
    FLOCK_CONF_LOW  = 0,   // OUI-only match (chip vendor OUI, high false-positive rate)
    FLOCK_CONF_HIGH = 1,   // Wildcard probe or SSID keyword (Flock-specific signature)
} FlockConfidence;

typedef struct {
    char mac[FLOCK_MAC_STR_LEN];
    char method[FLOCK_METHOD_STR_LEN];
    int8_t rssi;
    uint8_t channel;
    uint32_t first_seen_ms;
    uint32_t last_seen_ms;
    uint16_t count;
    char ssid[FLOCK_SSID_STR_LEN];
    FlockConfidence confidence;
} FlockDetection;

typedef void (*FlockDetectorCallback)(const FlockDetection *det, void *user_data);

void flock_detector_init(void);
void flock_detector_deinit(void);

esp_err_t flock_detector_start(void);
esp_err_t flock_detector_stop(void);
bool flock_detector_is_running(void);

int flock_detector_get_count(void);
const FlockDetection* flock_detector_get_detection(int index);
void flock_detector_clear_detections(void);

void flock_detector_set_callback(FlockDetectorCallback cb, void *user_data);

const char* flock_detector_method_str(FlockDetectMethod method);

#endif
