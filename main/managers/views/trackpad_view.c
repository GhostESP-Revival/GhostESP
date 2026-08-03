#include "sdkconfig.h"

#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)

#include "managers/views/trackpad_view.h"

#include "managers/views/badusb_view.h"
#include "core/serial_manager.h"
#include "core/esp_comm_manager.h"
#include "gui/accessibility_fonts.h"
#include "gui/lvgl_safe.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "gui/design_tokens.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"
#include "esp_log.h"

#ifdef CONFIG_HAS_BADUSB
#include "managers/badusb_manager.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "trackpad_view";

static View *s_return_view = NULL;
static lv_obj_t *s_root = NULL;
static lv_obj_t *s_pad = NULL;
static lv_obj_t *s_hint_label = NULL;
static lv_obj_t *s_stop_btn = NULL;

#define TRACKPAD_STEP 12
#define CLICK_HOLD_MS 300

// --- Unified click state ---
// Any input (touch, joystick center, keyboard Enter/Space/D) can start a
// click candidate.  If released before CLICK_HOLD_MS → left click.
// If held >= CLICK_HOLD_MS → right click (fires immediately at threshold).
typedef enum {
    CK_IDLE,
    CK_WAITING,     // press recorded, waiting to see if it's a tap or hold
    CK_LONG_FIRED,  // long-press threshold reached, right-click already sent
    CK_DRAGGING,    // touch moved beyond threshold, no click
} click_state_t;

static click_state_t s_click = CK_IDLE;
static esp_timer_handle_t s_click_timer = NULL;
#define CLICK_HOLD_US  (300 * 1000)  // 300ms

// --- Touch drag ---
#ifdef CONFIG_USE_TOUCHSCREEN
static int s_tp_start_x = 0;
static int s_tp_start_y = 0;
static int s_tp_last_x = 0;
static int s_tp_last_y = 0;
#define TP_MOVE_THRESHOLD 8
#endif

// --- Direction key acceleration ---
// Track when a direction key was first pressed so we can ramp the step size
// as the global repeat fires.
static uint8_t s_dir_key_held = 0;     // current direction key being held
static int64_t s_dir_key_start_us = 0; // when it was first pressed
#define DIR_ACCEL_MIN_STEP   4
#define DIR_ACCEL_MAX_STEP  32
#define DIR_ACCEL_FULL_MS  800          // reach max speed after this many ms

// --- Joystick held-direction ---
static int s_joy_held_mask = 0;

void trackpad_view_set_return_view(View *view) {
    s_return_view = view;
}

static void trackpad_send_local_move(int dx, int dy) {
#ifdef CONFIG_HAS_BADUSB
    badusb_manager_trackpad_move(dx, dy);
#else
    (void)dx;
    (void)dy;
#endif
}

static void trackpad_send_remote_move(int dx, int dy) {
    if (!esp_comm_manager_is_connected()) return;
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "trackpad_move %d %d", dx, dy);
    esp_comm_manager_send_command("badusb", cmd);
}

static void trackpad_send_local_button(uint8_t mask) {
#ifdef CONFIG_HAS_BADUSB
    badusb_manager_trackpad_button(mask);
#else
    (void)mask;
#endif
}

static void trackpad_send_remote_button(uint8_t mask) {
    if (!esp_comm_manager_is_connected()) return;
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "trackpad_button %u", (unsigned)mask);
    esp_comm_manager_send_command("badusb", cmd);
}

static bool trackpad_is_remote(void) {
#ifdef CONFIG_HAS_BADUSB_REMOTE
    return true;
#else
    return false;
#endif
}

static void trackpad_apply_move(int dx, int dy) {
    if (dx == 0 && dy == 0) return;
    if (trackpad_is_remote()) {
        trackpad_send_remote_move(dx, dy);
    } else {
        trackpad_send_local_move(dx, dy);
    }
}

static void trackpad_apply_button(uint8_t mask) {
    if (trackpad_is_remote()) {
        trackpad_send_remote_button(mask);
    } else {
        trackpad_send_local_button(mask);
    }
}

static void trackpad_click_pulse(uint8_t btn_mask) {
    trackpad_apply_button(btn_mask);
    trackpad_apply_button(0x00);
}

// --- Click state machine helpers ---

static void click_begin(void) {
    s_click = CK_WAITING;
    if (s_click_timer && !esp_timer_is_active(s_click_timer)) {
        esp_err_t err = esp_timer_start_once(s_click_timer, CLICK_HOLD_US);
        ESP_LOGI(TAG, "click_begin: timer start %s", err == ESP_OK ? "OK" : esp_err_to_name(err));
    }
}

static void click_end_tap(void) {
    // Short press → left click
    ESP_LOGI(TAG, "click_end_tap: left-click");
    trackpad_click_pulse(0x01);
    s_click = CK_IDLE;
    if (s_click_timer && esp_timer_is_active(s_click_timer)) {
        esp_timer_stop(s_click_timer);
    }
}

static void click_end_hold(void) {
    // Already fired right-click at threshold, just clean up
    s_click = CK_IDLE;
}

static void click_cancel(void) {
    // Moved or unrelated key: no click
    s_click = CK_IDLE;
    if (s_click_timer && esp_timer_is_active(s_click_timer)) {
        esp_timer_stop(s_click_timer);
    }
}

static void click_timer_cb(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "click_timer_cb: s_click=%d", (int)s_click);
    if (s_click != CK_WAITING) return;
    // Threshold reached: fire right-click immediately
    s_click = CK_LONG_FIRED;
    trackpad_click_pulse(0x02);
    ESP_LOGI(TAG, "click_timer_cb: right-click sent");
}

static bool click_is_active(void) {
    return s_click == CK_WAITING || s_click == CK_LONG_FIRED;
}

// --- View lifecycle ---

static void trackpad_stop_and_exit(void) {
    trackpad_apply_button(0);
    s_joy_held_mask = 0;
    s_dir_key_held = 0;
    s_click = CK_IDLE;
    if (trackpad_is_remote()) {
        esp_comm_manager_send_command("badusb", "trackpad_stop");
    } else {
#ifdef CONFIG_HAS_BADUSB
        badusb_manager_trackpad_stop();
#endif
    }
    View *back = s_return_view ? s_return_view : &badusb_view;
    display_manager_switch_view(back);
}

static int trackpad_accel_step(void) {
    if (!s_dir_key_held) return DIR_ACCEL_MIN_STEP;
    int64_t held_ms = (esp_timer_get_time() - s_dir_key_start_us) / 1000;
    if (held_ms <= 0) return DIR_ACCEL_MIN_STEP;
    if (held_ms >= DIR_ACCEL_FULL_MS) return DIR_ACCEL_MAX_STEP;
    int range = DIR_ACCEL_MAX_STEP - DIR_ACCEL_MIN_STEP;
    return DIR_ACCEL_MIN_STEP + (int)((int64_t)range * held_ms / DIR_ACCEL_FULL_MS);
}

static bool trackpad_key_to_delta(uint8_t k, int *dx, int *dy) {
    *dx = 0;
    *dy = 0;
    int step = trackpad_accel_step();
    if      (k == LV_KEY_UP    || k == 'k' || k == ';') { *dy = -step; }
    else if (k == LV_KEY_DOWN  || k == 'j' || k == '.') { *dy = +step; }
    else if (k == LV_KEY_LEFT  || k == 'h' || k == ',') { *dx = -step; }
    else if (k == LV_KEY_RIGHT || k == 'l' || k == '/') { *dx = +step; }
    else return false;
    return true;
}

static bool is_click_key(uint8_t k) {
    return k == LV_KEY_ENTER || k == '\n' || k == '\r' || k == ' ' || k == 'd';
}

static void trackpad_stop_btn_cb(lv_event_t *e) {
    (void)e;
    trackpad_stop_and_exit();
}

void trackpad_view_create(void) {
    if (s_root) return;

    display_manager_fill_screen(lv_color_hex(GUI_DEFAULT_BG_COLOR));
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);

    s_root = gui_screen_create_root_default(NULL, NULL);
    trackpad_view.root = s_root;

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t bg = lv_color_hex(theme_palette_get_background(theme));
    lv_color_t surface = lv_color_hex(theme_palette_get_surface(theme));
    lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
    lv_color_t text = lv_color_hex(theme_palette_get_text(theme));
    bool bright = theme_palette_is_bright(theme);
    lv_color_t muted = lv_color_hex(theme_palette_get_surface_alt(theme));
    lv_color_t pad_grid = bright ? lv_color_hex(0xCCCCCC) : lv_color_hex(0x404040);

    // Trackpad surface
    int bar_h = 28;
    s_pad = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_pad);
    lv_obj_set_size(s_pad, LV_HOR_RES, LV_VER_RES - GUI_STATUS_BAR_H - bar_h);
    lv_obj_align(s_pad, LV_ALIGN_TOP_MID, 0, GUI_STATUS_BAR_H);
    lv_obj_set_style_bg_color(s_pad, bg, 0);
    lv_obj_set_style_bg_opa(s_pad, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_pad, pad_grid, 0);
    lv_obj_set_style_border_width(s_pad, 1, 0);
    lv_obj_set_style_radius(s_pad, 0, 0);
    lv_obj_clear_flag(s_pad, LV_OBJ_FLAG_SCROLLABLE);
#ifdef CONFIG_USE_TOUCHSCREEN
    lv_obj_add_flag(s_pad, LV_OBJ_FLAG_CLICKABLE);
#endif

    s_hint_label = lv_label_create(s_pad);
#ifdef CONFIG_USE_HW_KB
    lv_label_set_text(s_hint_label,
                      "Drag=move Tap=L Hold=R  |  Keys: Enter=L F=R");
#else
    lv_label_set_text(s_hint_label, "Drag to move  Tap=L  Hold=R");
#endif
    lv_obj_set_style_text_color(s_hint_label, muted, 0);
    lv_obj_set_style_text_font(s_hint_label, accessibility_get_font_small(), 0);
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_hint_label, LV_ALIGN_CENTER, 0, 0);

    // Bottom bar — minimal
    lv_obj_t *bar = lv_obj_create(s_root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_HOR_RES, bar_h);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, surface, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_stop_btn = lv_btn_create(bar);
    gui_apply_pressed_style(s_stop_btn);
    lv_obj_set_size(s_stop_btn, 50, 22);
    lv_obj_set_style_bg_color(s_stop_btn, accent, LV_PART_MAIN);
    lv_obj_set_style_radius(s_stop_btn, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_stop_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_stop_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_stop_btn, 0, LV_PART_MAIN);
    lv_obj_t *lbl_s = lv_label_create(s_stop_btn);
    lv_label_set_text(lbl_s, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(lbl_s, bright ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_s, accessibility_get_font_small(), 0);
    lv_obj_center(lbl_s);
    lv_obj_add_event_cb(s_stop_btn, trackpad_stop_btn_cb, LV_EVENT_CLICKED, NULL);

    // Timer for tap/hold click detection
    if (!s_click_timer) {
        esp_timer_create_args_t args = {
            .callback = click_timer_cb,
            .name = "tp_click",
        };
        esp_timer_create(&args, &s_click_timer);
    }

    display_manager_add_status_bar("Trackpad");
}

void trackpad_view_destroy(void) {
    trackpad_apply_button(0);
    s_joy_held_mask = 0;
    s_dir_key_held = 0;
    s_click = CK_IDLE;
    if (s_click_timer) {
        if (esp_timer_is_active(s_click_timer)) esp_timer_stop(s_click_timer);
        esp_timer_delete(s_click_timer);
        s_click_timer = NULL;
    }
    lvgl_obj_del_safe(&s_root);
    s_root = NULL;
    s_pad = NULL;
    s_hint_label = NULL;
    s_stop_btn = NULL;
    trackpad_view.root = NULL;
}

static void trackpad_input_cb(InputEvent *event) {
    if (!event) return;

    switch (event->type) {
    case INPUT_TYPE_TOUCH: {
        lv_indev_data_t *data = &event->data.touch_data;
#ifdef CONFIG_USE_TOUCHSCREEN

        if (data->state == LV_INDEV_STATE_PR && !event->is_touch_move) {
            if (data->point.y >= LV_VER_RES - 28) {
                trackpad_stop_and_exit();
                return;
            }
        }

        if (data->state == LV_INDEV_STATE_PR) {
            if (s_click == CK_IDLE && !event->is_touch_move) {
                // New touch down
                s_tp_start_x = data->point.x;
                s_tp_start_y = data->point.y;
                s_tp_last_x = data->point.x;
                s_tp_last_y = data->point.y;
                click_begin();
            } else if (event->is_touch_move && (s_click == CK_WAITING || s_click == CK_DRAGGING)) {
                int dx_total = data->point.x - s_tp_start_x;
                int dy_total = data->point.y - s_tp_start_y;
                if (s_click == CK_WAITING &&
                    (abs(dx_total) > TP_MOVE_THRESHOLD || abs(dy_total) > TP_MOVE_THRESHOLD)) {
                    // Moved too far: cancel click, start dragging
                    click_cancel();
                    s_click = CK_DRAGGING;
                    s_tp_last_x = s_tp_start_x + (dx_total > 0 ? TP_MOVE_THRESHOLD : -TP_MOVE_THRESHOLD);
                    s_tp_last_y = s_tp_start_y + (dy_total > 0 ? TP_MOVE_THRESHOLD : -TP_MOVE_THRESHOLD);
                    trackpad_apply_move(data->point.x - s_tp_last_x, data->point.y - s_tp_last_y);
                    s_tp_last_x = data->point.x;
                    s_tp_last_y = data->point.y;
                } else if (s_click == CK_DRAGGING) {
                    int dx = data->point.x - s_tp_last_x;
                    int dy = data->point.y - s_tp_last_y;
                    s_tp_last_x = data->point.x;
                    s_tp_last_y = data->point.y;
                    trackpad_apply_move(dx, dy);
                }
            }
            return;
        }

        if (data->state == LV_INDEV_STATE_REL) {
            if (s_click == CK_WAITING) {
                click_end_tap();   // short press → left click
            } else if (s_click == CK_LONG_FIRED) {
                click_end_hold();  // right-click already sent
            }
            // CK_DRAGGING: no click, just stop
            s_click = CK_IDLE;
            if (s_click_timer && esp_timer_is_active(s_click_timer)) esp_timer_stop(s_click_timer);
            return;
        }
#else
        (void)data;
        return;
#endif
    }

    case INPUT_TYPE_JOYSTICK: {
        int button = event->data.joystick_index;

        if (!event->data.joystick_pressed) {
            // Release
            if (button == 1 && click_is_active()) {
                if (s_click == CK_WAITING) {
                    click_end_tap();   // short tap → left click
                } else if (s_click == CK_LONG_FIRED) {
                    click_end_hold();
                }
                s_click = CK_IDLE;
                if (s_click_timer) lv_timer_pause(s_click_timer);
            }
            switch (button) {
            case 0: s_joy_held_mask &= ~(1 << 0); break;
            case 3: s_joy_held_mask &= ~(1 << 1); break;
            case 2: s_joy_held_mask &= ~(1 << 2); break;
            case 4: s_joy_held_mask &= ~(1 << 3); break;
            default: break;
            }
            return;
        }

        // Press — track when direction first held for acceleration
        int jstep;
        switch (button) {
        case 0:
            if (!(s_joy_held_mask & (1 << 0))) { s_dir_key_start_us = esp_timer_get_time(); s_dir_key_held = 0; }
            s_joy_held_mask |= (1 << 0);
            jstep = trackpad_accel_step();
            trackpad_apply_move(-jstep, 0);
            break;
        case 3:
            if (!(s_joy_held_mask & (1 << 1))) { s_dir_key_start_us = esp_timer_get_time(); s_dir_key_held = 0; }
            s_joy_held_mask |= (1 << 1);
            jstep = trackpad_accel_step();
            trackpad_apply_move(+jstep, 0);
            break;
        case 2:
            if (!(s_joy_held_mask & (1 << 2))) { s_dir_key_start_us = esp_timer_get_time(); s_dir_key_held = 0; }
            s_joy_held_mask |= (1 << 2);
            jstep = trackpad_accel_step();
            trackpad_apply_move(0, -jstep);
            break;
        case 4:
            if (!(s_joy_held_mask & (1 << 3))) { s_dir_key_start_us = esp_timer_get_time(); s_dir_key_held = 0; }
            s_joy_held_mask |= (1 << 3);
            jstep = trackpad_accel_step();
            trackpad_apply_move(0, +jstep);
            break;
        case 1:
            click_begin();  // start tap/hold timer
            break;
        default: break;
        }
        return;
    }

    case INPUT_TYPE_KEYBOARD: {
        uint8_t k = event->data.key_value;
        bool is_dir;
        int dx, dy;

        // Release event: finalize tap/hold click
        if (event->is_touch_move) {
            ESP_LOGI(TAG, "key release: 0x%02x, s_click=%d", k, (int)s_click);
            if (is_click_key(k) && s_click == CK_WAITING) {
                click_end_tap();   // released before hold threshold → left click
            }
            s_dir_key_held = 0;
            return;
        }

        // Direction keys — track held state for acceleration
        is_dir = trackpad_key_to_delta(k, &dx, &dy);
        if (is_dir) {
            if (!event->is_repeat) {
                s_dir_key_held = k;
                s_dir_key_start_us = esp_timer_get_time();
            }
            trackpad_apply_move(dx, dy);
            return;
        }
        s_dir_key_held = 0;

        // Left/Right click: Enter, Space, or 'd'
        // Fresh press starts tap/hold. Hold → right click. Release before hold → left click.
        if (is_click_key(k)) {
            ESP_LOGI(TAG, "click key press: 0x%02x repeat=%d s_click=%d", k, event->is_repeat, (int)s_click);
            if (!event->is_repeat) {
                click_begin();
            }
            return;
        }

        // Right click: 'f' (instant)
        if (k == 'f') {
            trackpad_click_pulse(0x02);
            return;
        }

        // Exit
        if (k == LV_KEY_ESC || k == 27 || k == 29 || k == '`' || k == 'q' || k == 'Q') {
            trackpad_stop_and_exit();
            return;
        }

        return;
    }

    case INPUT_TYPE_ENCODER:
        if (event->data.encoder.button) {
            trackpad_stop_and_exit();
        }
        return;

    case INPUT_TYPE_EXIT_BUTTON:
        trackpad_stop_and_exit();
        return;
    }
}

static void trackpad_get_hardwareinput_callback(void **callback) {
    *callback = (void *)trackpad_input_cb;
}

View trackpad_view = {
    .root = NULL,
    .create = trackpad_view_create,
    .destroy = trackpad_view_destroy,
    .input_callback = trackpad_input_cb,
    .name = "Trackpad",
    .get_hardwareinput_callback = trackpad_get_hardwareinput_callback,
};

#endif // CONFIG_HAS_BADUSB || CONFIG_HAS_BADUSB_REMOTE
