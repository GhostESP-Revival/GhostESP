#ifndef KEYBOARD_SCREEN_H
#define KEYBOARD_SCREEN_H

#include "lvgl/lvgl.h"
#include "managers/display_manager.h"

extern View keyboard_view;

typedef void (*KeyboardSubmitCallback)(const char *text);
void keyboard_view_set_submit_callback(KeyboardSubmitCallback cb);
void keyboard_view_set_placeholder(const char *text);
void keyboard_view_set_return_view(View *view);

#endif 