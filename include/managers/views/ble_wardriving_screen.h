#ifndef BLE_WARDRIVING_SCREEN_H
#define BLE_WARDRIVING_SCREEN_H

#include "lvgl/lvgl.h"
#include "managers/display_manager.h"

extern View ble_wardriving_view;

void ble_wardriving_screen_create(void);
void ble_wardriving_screen_destroy(void);
void ble_wardriving_screen_update_data(void);
void ble_wardriving_screen_input_cb(InputEvent *event);
void ble_wardriving_update_device_data(const char* mac, const char* name, int rssi, uint8_t type);

#endif // BLE_WARDRIVING_SCREEN_H
