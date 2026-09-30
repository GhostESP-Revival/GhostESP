// hop_profile_screen.c
// Simple screen to pick the channel hopping profile used by all WiFi
// hopping features: Auto (feature default), All (country list), 1,6,11,
// or a Custom typed list.

#include "managers/views/hop_profile_screen.h"
#include "gui/options_view.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"
#include "managers/views/error_popup.h"
#include "managers/views/keyboard_screen.h"
#include "scans/wifi/hop_profile.h"
#include "scans/wifi/wifi_channels.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Row layout: [Auto] [All] [Basic] [Custom] [Back]
enum {
    HOP_ROW_AUTO = 0,
    HOP_ROW_ALL,
    HOP_ROW_BASIC,
    HOP_ROW_CUSTOM,
    HOP_ROW_BACK,
    HOP_ROW_COUNT
};

static lv_obj_t *s_root = NULL;
static options_view_t *s_ov = NULL;
static lv_obj_t *s_custom_row = NULL;

/* Touch is dispatched as raw InputEvents by display_manager's polling task; no
 * LVGL indev is registered, so LVGL never raises LV_EVENT_CLICKED on the list
 * rows on its own. The press/release taps have to be hit-tested and forwarded
 * here, the way favorites_manager/ethernet do it. */
static touch_drag_t s_touch_drag;
static bool s_touch_started = false;

static const char *hop_mode_row_label(hop_mode_t mode) {
    switch (mode) {
        case HOP_MODE_DEFAULT: return "Auto (country/AP list)";
        case HOP_MODE_ALL: return "All channels";
        case HOP_MODE_BASIC: return "1, 6, 11";
        case HOP_MODE_CUSTOM: return "Custom: (type channels)";
        default: return "Auto (country/AP list)";
    }
}

static void hop_profile_save_and_back(void) {
    settings_save(&G_Settings);
    display_manager_go_back();
}

static void hop_profile_custom_submit(const char *text) {
    if (!text) {
        display_manager_switch_view(&hop_profile_view);
        return;
    }
    if (!hop_profile_set_custom_from_string(text)) {
        error_popup_create("Enter channels like 1,6,11");
        return;
    }
    hop_profile_set_mode(HOP_MODE_CUSTOM);
    hop_profile_save_and_back();
}

static void hop_profile_select_row(int row) {
    switch (row) {
        case HOP_ROW_AUTO:
            hop_profile_set_mode(HOP_MODE_DEFAULT);
            hop_profile_save_and_back();
            break;
        case HOP_ROW_ALL:
            hop_profile_set_mode(HOP_MODE_ALL);
            hop_profile_save_and_back();
            break;
        case HOP_ROW_BASIC:
            hop_profile_set_mode(HOP_MODE_BASIC);
            hop_profile_save_and_back();
            break;
        case HOP_ROW_CUSTOM: {
            keyboard_view_set_return_view(&hop_profile_view);
            keyboard_view_set_submit_callback(hop_profile_custom_submit);
            keyboard_view_set_placeholder("Channels, e.g. 1,6,11");
            keyboard_view_set_initial_text(hop_profile_get_custom_str());
            keyboard_view_set_start_caps(false);
            display_manager_switch_view(&keyboard_view);
            break;
        }
        case HOP_ROW_BACK:
        default:
            display_manager_go_back();
            break;
    }
}

static void hop_profile_row_click(lv_event_t *event) {
    hop_profile_select_row((int)(intptr_t)lv_event_get_user_data(event));
}

static void hop_profile_select_row_cb(void) {
    int selected = options_view_get_selected(s_ov);
    hop_profile_select_row(selected);
}

/* Returns true when the point is inside `obj`'s current (post-scroll) bounds. */
static bool hop_point_in_obj(lv_obj_t *obj, const lv_point_t *point) {
    if (!obj || !point || !lv_obj_is_valid(obj)) return false;
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return point->x >= area.x1 && point->x <= area.x2 &&
           point->y >= area.y1 && point->y <= area.y2;
}

static void hop_touch_reset(void) {
    s_touch_started = false;
    touch_drag_reset(&s_touch_drag);
}

static bool hop_handle_touch(InputEvent *event) {
#ifdef CONFIG_USE_TOUCHSCREEN
    if (!event || event->type != INPUT_TYPE_TOUCH || !s_ov) return false;

    lv_indev_data_t *data = &event->data.touch_data;
    lv_obj_t *list = options_view_get_list(s_ov);
    if (!list || !lv_obj_is_valid(list)) return false;

    if (event->is_touch_move) {
        if (!s_touch_drag.started) return true;
        touch_drag_update(&s_touch_drag, data, list);
        return true;
    }

    if (data->state == LV_INDEV_STATE_PR) {
        s_touch_started = true;
        /* Only start drag tracking when the press lands inside the list, so a
         * stray press on the background neither scrolls nor taps a row. */
        if (hop_point_in_obj(list, &data->point)) {
            touch_drag_begin(&s_touch_drag, data);
        } else {
            touch_drag_reset(&s_touch_drag);
        }
        return true;
    }

    if (data->state != LV_INDEV_STATE_REL) return true;
    if (!s_touch_started) return true;
    s_touch_started = false;

    if (touch_drag_release(&s_touch_drag, data)) {
        /* A drag happened (or a release-on-release scroll applied): the tap
         * that ended the drag must not also activate a row. */
        display_manager_flush_pending_scroll();
        return true;
    }

    if (!hop_point_in_obj(list, &data->point)) return true;

    int idx = options_view_item_at(s_ov, data->point.x, data->point.y);
    if (idx < 0) return true;

    options_view_set_selected(s_ov, idx);
    hop_profile_select_row(idx);
    return true;
#else
    (void)event;
    return false;
#endif
}

static void hop_profile_input_handler(InputEvent *event) {
    if (!event || !s_ov) return;

    switch (event->type) {
        case INPUT_TYPE_TOUCH: {
            hop_handle_touch(event);
            break;
        }
        case INPUT_TYPE_JOYSTICK: {
            int ji = event->data.joystick_index;
            if (ji == 2) {
                options_view_move_selection(s_ov, -1);
            } else if (ji == 4) {
                options_view_move_selection(s_ov, 1);
            } else if (ji == 1) {
                hop_profile_select_row_cb();
            } else if (ji == 0) {
                display_manager_go_back();
            }
            break;
        }
        case INPUT_TYPE_ENCODER: {
            if (event->data.encoder.button) {
                hop_profile_select_row_cb();
            } else if (event->data.encoder.direction > 0) {
                options_view_move_selection(s_ov, -1);
            } else if (event->data.encoder.direction < 0) {
                options_view_move_selection(s_ov, 1);
            }
            break;
        }
        case INPUT_TYPE_KEYBOARD: {
            int kv = event->data.key_value;
            if (kv == LV_KEY_UP || kv == 'k' || kv == ';') {
                options_view_move_selection(s_ov, -1);
            } else if (kv == LV_KEY_DOWN || kv == 'j' || kv == '.') {
                options_view_move_selection(s_ov, 1);
            } else if (kv == 13 || kv == 10 || kv == LV_KEY_RIGHT || kv == '/') {
                hop_profile_select_row_cb();
            } else if (kv == LV_KEY_ESC || kv == 27 || kv == LV_KEY_LEFT ||
                       kv == ',' || kv == '`') {
                display_manager_go_back();
            }
            break;
        }
        case INPUT_TYPE_EXIT_BUTTON:
        case INPUT_TYPE_HOME_BUTTON:
            display_manager_go_back();
            break;
        default:
            break;
    }
}

static void hop_profile_create(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t bg = lv_color_hex(theme_palette_get_background(theme));

    hop_touch_reset();

    s_root = gui_screen_create_root(NULL, NULL, bg, LV_OPA_TRANSP);
    if (!s_root) return;
    hop_profile_view.root = s_root;

    s_ov = options_view_create(s_root, "Hop Channels");
    if (!s_ov) return;

    lv_obj_t *list = options_view_get_list(s_ov);
#ifdef CONFIG_USE_TOUCHSCREEN
    const int status_h = GUI_STATUS_BAR_HEIGHT;
    int list_h = LV_VER_RES - status_h;
    lv_obj_set_size(list, GUI_OPTIONS_LIST_WIDTH, list_h);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, status_h);
#else
    (void)list;
#endif

    // Determine current mode so the list starts on the active row.
    hop_mode_t mode = HOP_MODE_DEFAULT;
    hop_profile_get_mode(&mode);
    int initial_row = (int)mode;
    if (initial_row < HOP_ROW_AUTO || initial_row > HOP_ROW_BASIC) {
        initial_row = HOP_ROW_AUTO;
    }
    if (mode == HOP_MODE_CUSTOM) {
        initial_row = HOP_ROW_CUSTOM;
    }

    options_view_add_item(s_ov, hop_mode_row_label(HOP_MODE_DEFAULT),
                          hop_profile_row_click, (void *)(intptr_t)HOP_ROW_AUTO);
    options_view_add_item(s_ov, hop_mode_row_label(HOP_MODE_ALL),
                          hop_profile_row_click, (void *)(intptr_t)HOP_ROW_ALL);
    options_view_add_item(s_ov, hop_mode_row_label(HOP_MODE_BASIC),
                          hop_profile_row_click, (void *)(intptr_t)HOP_ROW_BASIC);

    char custom_label[64];
    const char *custom = hop_profile_get_custom_str();
    if (custom && *custom) {
        snprintf(custom_label, sizeof(custom_label), "Custom: %s", custom);
    } else {
        snprintf(custom_label, sizeof(custom_label), "Custom...");
    }
    s_custom_row = options_view_add_item(s_ov, custom_label,
                                         hop_profile_row_click,
                                         (void *)(intptr_t)HOP_ROW_CUSTOM);

    options_view_add_back_row(s_ov, hop_profile_row_click,
                              (void *)(intptr_t)HOP_ROW_BACK);

    options_view_set_selected(s_ov, initial_row);
}

static void hop_profile_destroy(void) {
    hop_touch_reset();
    if (s_ov) {
        options_view_destroy(s_ov);
        s_ov = NULL;
    }
    if (s_root && lv_obj_is_valid(s_root)) {
        lv_obj_del(s_root);
    }
    s_root = NULL;
    s_custom_row = NULL;
    hop_profile_view.root = NULL;
}

static void get_hop_profile_callback(void **callback) {
    if (callback) *callback = hop_profile_view.input_callback;
}

View hop_profile_view = {
    .root = NULL,
    .create = hop_profile_create,
    .destroy = hop_profile_destroy,
    .name = "Hop Channels",
    .get_hardwareinput_callback = get_hop_profile_callback,
    .input_callback = hop_profile_input_handler,
};
