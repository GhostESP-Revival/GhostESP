#ifndef BADUSB_MANAGER_H
#define BADUSB_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t badusb_manager_init(void);
esp_err_t badusb_manager_start(void);
esp_err_t badusb_manager_stop(void);
esp_err_t badusb_manager_execute_file(const char *path);
esp_err_t badusb_manager_execute_buffer(char *buf, size_t len);
bool badusb_manager_is_active(void);
int badusb_manager_list_scripts(char scripts[][64], int max_scripts);

// Apply current settings (VID/PID/strings/layout) to USB descriptors
void badusb_manager_apply_settings(void);

// VBUS sense support: returns true if VSENSE pin is configured (>= 0)
bool badusb_has_vsense(void);

// Returns true if VBUS is detected (pin HIGH), or always true if no VSENSE pin
bool badusb_vsense_connected(void);

// Stream receive for remote script execution (C5 -> S3)
esp_err_t badusb_manager_prepare_receive(size_t size);
void badusb_manager_register_stream_handler(void);

// Send a mouse HID report (dx, dy are relative movement, buttons is bitmask)
bool badusb_hid_mouse_send(int8_t dx, int8_t dy, uint8_t buttons);

// Mouse jiggler: moves mouse periodically to keep PC awake
esp_err_t badusb_manager_mouse_jiggle_start(void);
esp_err_t badusb_manager_mouse_jiggle_stop(void);
bool badusb_manager_is_jiggling(void);

// Keyboard mode: real-time key forwarding via COMM_STREAM_CHANNEL_KEYBOARD
esp_err_t badusb_manager_keyboard_mode_start(void);
esp_err_t badusb_manager_keyboard_mode_stop(void);
bool badusb_manager_is_keyboard_mode(void);

// Send a single keypress (requires active HID device - keyboard mode or script mode)
bool badusb_manager_send_keypress(uint8_t modifier, uint8_t keycode);

// Type text through the active HID keyboard device
bool badusb_manager_send_text(const char *text);

#endif
