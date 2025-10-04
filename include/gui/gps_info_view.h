#pragma once

#include "lvgl.h"
#include "managers/display_manager.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Create the GPS info view
void gps_info_view_create(void);

// Destroy the GPS info view
void gps_info_view_destroy(void);

// Handle input events on the GPS info view
void gps_info_view_input_cb(InputEvent *event);

// Get hardware input callback
void gps_info_view_get_hardwareinput_callback(void **callback);

// Update GPS data display
void gps_info_view_update_data(void);

// GPS info view object
extern View gps_info_view;

#ifdef __cplusplus
}
#endif
