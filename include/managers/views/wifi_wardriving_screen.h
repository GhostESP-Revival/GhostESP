#ifndef WIFI_WARDRIVING_SCREEN_H
#define WIFI_WARDRIVING_SCREEN_H

#include "lvgl/lvgl.h"
#include "managers/display_manager.h"

extern View wifi_wardriving_view;

void wifi_wardriving_screen_create(void);
void wifi_wardriving_screen_destroy(void);
void wifi_wardriving_screen_update_data(void);
void wifi_wardriving_screen_input_cb(InputEvent *event);
void wifi_wardriving_update_network_data(const char* ssid, const char* bssid, int rssi, int channel, const char* encryption);

#endif // WIFI_WARDRIVING_SCREEN_H
