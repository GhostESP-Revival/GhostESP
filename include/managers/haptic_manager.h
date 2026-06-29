#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HAPTIC_EFFECT_CLICK = 0,
    HAPTIC_EFFECT_SELECTION,
    HAPTIC_EFFECT_SUCCESS,
    HAPTIC_EFFECT_WARNING,
    HAPTIC_EFFECT_ERROR,
    HAPTIC_EFFECT_NOTIFICATION,
} haptic_effect_t;

#ifdef CONFIG_HAS_DRV2605_HAPTICS
esp_err_t haptic_manager_init(void);
bool haptic_manager_is_ready(void);
void haptic_manager_play(haptic_effect_t effect);
#else
static inline esp_err_t haptic_manager_init(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline bool haptic_manager_is_ready(void) { return false; }
static inline void haptic_manager_play(haptic_effect_t effect) { (void)effect; }
#endif

#ifdef __cplusplus
}
#endif
