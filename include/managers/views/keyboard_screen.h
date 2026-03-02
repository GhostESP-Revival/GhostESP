#ifndef KEYBOARD_SCREEN_H
#define KEYBOARD_SCREEN_H

#include "lvgl/lvgl.h"
#include "managers/display_manager.h"

extern View keyboard_view;

typedef void (*KeyboardSubmitCallback)(const char *text);
void keyboard_view_set_submit_callback(KeyboardSubmitCallback cb);
void keyboard_view_set_placeholder(const char *text);
/** Prefill the keyboard input with text (e.g. for editing). Max 127 chars. */
void keyboard_view_set_initial_text(const char *text);
/** When true (default), keyboard starts with first letter capitalized. Set false for command input. */
void keyboard_view_set_start_caps(bool start_caps);
void keyboard_view_set_return_view(View *view);

#endif 