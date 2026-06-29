#pragma once

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Marker used on the toggle's lv_obj user_data so other code can identify
// toggle children of a settings row. (1) = label, (2) = arrow, (3) = toggle.
#define IOS_TOGGLE_USER_DATA ((void *)3)

// Create an iOS-style toggle widget as a child of `parent`.
// The returned object is the "track" of the toggle. A "knob" is created
// automatically as its only child. Initial state is off.
//
// The widget sizes itself based on the parent's current height:
//   parent_h <= 40 -> track 40x24, knob 20x20
//   parent_h  > 40 -> track 52x30, knob 26x26
//
// The widget is not clickable on its own; clicks on it bubble to the
// parent (the options row button), which handles the actual toggle via
// option_event_cb -> change_setting_value.
lv_obj_t *ios_toggle_create(lv_obj_t *parent);

// Read the current state.
bool ios_toggle_get_value(lv_obj_t *obj);

// Set the state. If `animate` is true, the knob slides between positions
// (default ~180ms ease-out).
void ios_toggle_set_value(lv_obj_t *obj, bool value, bool animate);

// Re-apply theme-driven colors. Call after the menu theme changes so the
// "on" state matches the current theme accent.
void ios_toggle_refresh_style(lv_obj_t *obj);

#ifdef __cplusplus
}
#endif
