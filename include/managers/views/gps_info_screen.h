#ifndef GPS_INFO_SCREEN_H
#define GPS_INFO_SCREEN_H

#include "lvgl/lvgl.h"
#include "managers/display_manager.h"

extern View gps_info_view;

void gps_info_screen_create(void);
void gps_info_screen_destroy(void);
void gps_info_screen_update_data(void);
void gps_info_screen_input_cb(InputEvent *event);

#endif // GPS_INFO_SCREEN_H