#include "managers/views/number_pad_screen.h"
#include "core/serial_manager.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "managers/views/terminal_screen.h"
#include "gui/accessibility_fonts.h"
#include "managers/views/options_screen.h"
#include "managers/views/main_menu_screen.h"
#include "managers/settings_manager.h"
#include "gui/theme_palette_api.h"
#include "gui/design_tokens.h"
#include "gui/screen_layout.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"

static const char *TAG = "number_pad_screen";

#define NP_COLS 5
#define NP_ROWS 3
#define NP_BTN_COUNT 15
#define NP_TOUCH_TAP_THRESHOLD 12

uint32_t theme_palette_get_background(uint8_t theme);
uint32_t theme_palette_get_surface(uint8_t theme);
uint32_t theme_palette_get_text(uint8_t theme);
uint32_t theme_palette_get_accent(uint8_t theme);
bool theme_palette_is_bright(uint8_t theme);

static ENumberPadMode current_mode = NP_MODE_AP;
static lv_obj_t *root = NULL;
static lv_obj_t *content = NULL;
static lv_obj_t *number_display = NULL;
static lv_obj_t *numpad_cont = NULL;
static lv_obj_t *numpad_btns[NP_BTN_COUNT] = {0};
static int cursor_pos = 0;
static char input_buffer[32] = {0};
static int input_pos = 0;
static bool touch_started = false;
static int touch_start_x = 0;
static int touch_start_y = 0;
static int touch_pressed_idx = -1;
static int suppress_click_idx = -1;
static int64_t suppress_click_until_ms = 0;

static const char *const k_labels[NP_BTN_COUNT] = {
    "1", "2", "3", "4", "5",
    "6", "7", "8", "9", ",",
    LV_SYMBOL_BACKSPACE, "0", "OK", "BACK", NULL
};

static const char k_chars[NP_BTN_COUNT] = {
    '1', '2', '3', '4', '5',
    '6', '7', '8', '9', ',',
    '\b', '0', '\r', 'e', '\0'
};

static void number_pad_create(void);
static void number_pad_destroy(void);
static void handle_hardware_button_press_number_pad(InputEvent *event);
static void get_number_pad_callback(void **callback);

static void update_display(void) {
    if (!number_display || !lv_obj_is_valid(number_display)) return;
    lv_label_set_text(number_display, input_buffer);
}

static void refresh_focus(void) {
    if (!numpad_cont || !lv_obj_is_valid(numpad_cont)) return;
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
    lv_color_t text = lv_color_hex(theme_palette_get_text(theme));
    lv_color_t sel_fg = theme_palette_is_bright(theme) ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF);
    for (int i = 0; i < NP_BTN_COUNT; i++) {
        if (!numpad_btns[i] || !lv_obj_is_valid(numpad_btns[i])) continue;
        bool focused = (i == cursor_pos);
        lv_obj_set_style_border_width(numpad_btns[i], focused ? 3 : 1, 0);
        lv_obj_set_style_border_color(numpad_btns[i], focused ? accent : text, 0);
        lv_obj_set_style_bg_color(numpad_btns[i], focused ? accent : lv_color_hex(theme_palette_get_surface(theme)), 0);
        lv_obj_set_style_bg_opa(numpad_btns[i], focused ? LV_OPA_COVER : LV_OPA_20, 0);
        lv_obj_t *lbl = lv_obj_get_child(numpad_btns[i], 0);
        if (lbl) {
            lv_obj_set_style_text_color(lbl, focused ? sel_fg : text, 0);
        }
    }
}

static lv_color_t np_get_bg(void) {
    return lv_color_hex(theme_palette_get_background(settings_get_menu_theme(&G_Settings)));
}

static lv_color_t np_get_surface(void) {
    return lv_color_hex(theme_palette_get_surface(settings_get_menu_theme(&G_Settings)));
}

static lv_color_t np_get_text(void) {
    return lv_color_hex(theme_palette_get_text(settings_get_menu_theme(&G_Settings)));
}

static void numpad_activate(int idx);

static int hit_test_numpad_button(int x, int y) {
    for (int i = 0; i < NP_BTN_COUNT; i++) {
        if (!numpad_btns[i] || !lv_obj_is_valid(numpad_btns[i])) continue;
        lv_area_t area;
        lv_obj_get_coords(numpad_btns[i], &area);
        if (x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2) {
            return i;
        }
    }
    return -1;
}

static void numpad_btn_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (idx == suppress_click_idx && now_ms < suppress_click_until_ms) {
        suppress_click_idx = -1;
        return;
    }
    cursor_pos = idx;
    refresh_focus();
    numpad_activate(idx);
}

static void numpad_activate(int idx) {
    if (idx < 0 || idx >= NP_BTN_COUNT || k_chars[idx] == '\0') return;
    char c = k_chars[idx];
    if (c == '\b') {
        if (input_pos > 0) {
            input_buffer[--input_pos] = '\0';
            update_display();
        }
    } else if (c == '\r') {
        if (input_pos > 0) {
            terminal_set_return_view(&options_menu_view);
            if (current_mode == NP_MODE_AP_REMOTE || current_mode == NP_MODE_STA_REMOTE ||
                current_mode == NP_MODE_AIRTAG_REMOTE || current_mode == NP_MODE_LAN_REMOTE ||
                current_mode == NP_MODE_FLIPPER_REMOTE) {
                terminal_set_dualcomm_filter(true);
            }
            display_manager_switch_view(&terminal_view);
            vTaskDelay(pdMS_TO_TICKS(10));
            char command[64];
            switch (current_mode) {
                case NP_MODE_AP:        snprintf(command, sizeof(command), "select -a %s", input_buffer); break;
                case NP_MODE_STA:       snprintf(command, sizeof(command), "select -s %s", input_buffer); break;
                case NP_MODE_AIRTAG:    snprintf(command, sizeof(command), "selectairtag %s", input_buffer); break;
                case NP_MODE_LAN:       snprintf(command, sizeof(command), "select -a %s", input_buffer); break;
                case NP_MODE_FLIPPER:   snprintf(command, sizeof(command), "selectflipper %s", input_buffer); break;
                case NP_MODE_GATT:      snprintf(command, sizeof(command), "selectgatt %s", input_buffer); break;
                case NP_MODE_AP_REMOTE:      snprintf(command, sizeof(command), "commsend select -a %s", input_buffer); break;
                case NP_MODE_STA_REMOTE:     snprintf(command, sizeof(command), "commsend select -s %s", input_buffer); break;
                case NP_MODE_AIRTAG_REMOTE:  snprintf(command, sizeof(command), "commsend selectairtag %s", input_buffer); break;
                case NP_MODE_LAN_REMOTE:     snprintf(command, sizeof(command), "commsend select -a %s", input_buffer); break;
                case NP_MODE_FLIPPER_REMOTE: snprintf(command, sizeof(command), "commsend selectflipper %s", input_buffer); break;
                default: snprintf(command, sizeof(command), "select -a %s", input_buffer); break;
            }
            simulateCommand(command);
            input_buffer[0] = '\0';
            input_pos = 0;
        }
    } else if (c == 'e') {
        display_manager_switch_view(&options_menu_view);
    } else if (c != '\0') {
        if (input_pos < (int)sizeof(input_buffer) - 1) {
            input_buffer[input_pos++] = c;
            input_buffer[input_pos] = '\0';
            update_display();
        }
    }
}

static void number_pad_create(void) {
    int screen_width = LV_HOR_RES;
    int screen_height = LV_VER_RES;
    int status_bar_h = GUI_STATUS_BAR_H;
    int content_h = screen_height - status_bar_h;
    if (content_h < 60) content_h = 60;
    bool small = (screen_width <= 240 || screen_height <= 240);
    bool landscape = (screen_width > screen_height && screen_height <= 160);

    lv_color_t bg = np_get_bg();
    lv_color_t surface = np_get_surface();
    lv_color_t text = np_get_text();
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
    bool bright = theme_palette_is_bright(theme);
    lv_color_t sel_fg = bright ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF);
    bool rounded = settings_get_menu_rounded(&G_Settings);
    lv_coord_t btn_radius = rounded ? GUI_RADIUS_SM / 2 : 0;

    display_manager_fill_screen(bg);

    root = lv_obj_create(lv_scr_act());
    number_pad_view.root = root;
    lv_obj_set_size(root, screen_width, screen_height);
    lv_obj_set_style_bg_color(root, bg, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_align(root, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);

    content = lv_obj_create(root);
    lv_obj_set_size(content, screen_width, content_h);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, status_bar_h);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    const lv_font_t *display_font = small ? &lv_font_montserrat_12 : &lv_font_montserrat_16;
    int pad_h = landscape ? 4 : GUI_SAFEAREA_VER;
    int pad_v = landscape ? 2 : GUI_SAFEAREA_VER;
    int display_h = small ? 30 : 40;

    number_display = lv_label_create(content);
    lv_label_set_text(number_display, "");
    lv_obj_set_width(number_display, screen_width - 2 * pad_h);
    lv_obj_set_style_bg_color(number_display, surface, 0);
    lv_obj_set_style_bg_opa(number_display, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(number_display, text, 0);
    lv_obj_set_style_text_font(number_display, display_font, 0);
    lv_obj_set_style_pad_all(number_display, pad_h, 0);
    lv_obj_set_style_border_width(number_display, 1, 0);
    lv_obj_set_style_border_color(number_display, text, 0);
    lv_obj_set_style_border_opa(number_display, LV_OPA_30, 0);
    lv_obj_set_style_radius(number_display, btn_radius, 0);
    lv_obj_align(number_display, LV_ALIGN_TOP_MID, 0, pad_v);

    int gap = landscape ? 2 : GUI_GRID;
    int numpad_top = display_h + 2 * pad_v + pad_v;
    numpad_cont = lv_obj_create(content);
    lv_obj_set_style_bg_opa(numpad_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(numpad_cont, 0, 0);
    lv_obj_set_style_pad_all(numpad_cont, 0, 0);
    lv_obj_set_style_pad_row(numpad_cont, gap, 0);
    lv_obj_set_style_pad_column(numpad_cont, gap, 0);
    lv_obj_clear_flag(numpad_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(numpad_cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(numpad_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    int avail_h = content_h - numpad_top - pad_v;
    if (avail_h < 36) avail_h = 36;
    int btn_h = (avail_h - (NP_ROWS - 1) * gap) / NP_ROWS;
    if (btn_h > 38) btn_h = 38;
    if (btn_h < 14) btn_h = 14;
    int btn_w = btn_h;
    int grid_w = NP_COLS * btn_w + (NP_COLS - 1) * gap;
    if (grid_w > screen_width - 2 * pad_h) {
        grid_w = screen_width - 2 * pad_h;
        btn_w = (grid_w - (NP_COLS - 1) * gap) / NP_COLS;
        if (btn_w < 14) btn_w = 14;
    }
    lv_obj_set_size(numpad_cont, grid_w, NP_ROWS * btn_h + (NP_ROWS - 1) * gap);
    lv_obj_align(numpad_cont, LV_ALIGN_TOP_MID, 0, numpad_top);

    const lv_font_t *btn_font = (btn_h < 20) ? &lv_font_montserrat_10 : &lv_font_montserrat_12;

    for (int i = 0; i < NP_BTN_COUNT; i++) {
        if (!k_labels[i]) continue;
        lv_obj_t *btn = lv_btn_create(numpad_cont);
        numpad_btns[i] = btn;
        lv_obj_set_size(btn, btn_w, btn_h);
        lv_obj_set_style_bg_color(btn, surface, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_20, 0);
        lv_obj_set_style_radius(btn, btn_radius, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, text, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, numpad_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, k_labels[i]);
        lv_obj_set_style_text_font(lbl, btn_font, 0);
        lv_obj_set_style_text_color(lbl, text, 0);
        lv_obj_center(lbl);

        if (strcmp(k_labels[i], "OK") == 0 || strcmp(k_labels[i], LV_SYMBOL_BACKSPACE) == 0) {
            lv_obj_set_style_text_font(lbl, btn_font, 0);
        }
    }

    cursor_pos = 0;
    refresh_focus();

    const char *title;
    if (current_mode == NP_MODE_AP || current_mode == NP_MODE_AP_REMOTE) title = "Select AP";
    else if (current_mode == NP_MODE_STA || current_mode == NP_MODE_STA_REMOTE) title = "Select Station";
    else if (current_mode == NP_MODE_AIRTAG || current_mode == NP_MODE_AIRTAG_REMOTE) title = "Select AirTag";
    else if (current_mode == NP_MODE_FLIPPER || current_mode == NP_MODE_FLIPPER_REMOTE) title = "Select Flipper";
    else if (current_mode == NP_MODE_GATT) title = "Select GATT";
    else title = "Select LAN";
    display_manager_add_status_bar(title);
}

static void number_pad_destroy(void) {
    if (number_pad_view.root) {
        lv_obj_clean(number_pad_view.root);
        lv_obj_del(number_pad_view.root);
        number_pad_view.root = NULL;
        root = NULL;
        content = NULL;
        number_display = NULL;
        numpad_cont = NULL;
        for (int i = 0; i < NP_BTN_COUNT; i++) numpad_btns[i] = NULL;
        input_buffer[0] = '\0';
        input_pos = 0;
        cursor_pos = 0;
        touch_started = false;
        touch_pressed_idx = -1;
        suppress_click_idx = -1;
        suppress_click_until_ms = 0;
    }
}

static void handle_hardware_button_press_number_pad(InputEvent *event) {
    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *data = &event->data.touch_data;
        if (data->state == LV_INDEV_STATE_PR) {
            int idx = hit_test_numpad_button(data->point.x, data->point.y);
            touch_started = (idx >= 0);
            touch_start_x = data->point.x;
            touch_start_y = data->point.y;
            touch_pressed_idx = idx;
            if (idx >= 0) {
                cursor_pos = idx;
                refresh_focus();
            }
        } else if (data->state == LV_INDEV_STATE_REL && touch_started) {
            int idx = hit_test_numpad_button(data->point.x, data->point.y);
            int dx = data->point.x - touch_start_x;
            int dy = data->point.y - touch_start_y;
            bool is_tap = abs(dx) <= NP_TOUCH_TAP_THRESHOLD && abs(dy) <= NP_TOUCH_TAP_THRESHOLD;
            if (idx >= 0 && idx == touch_pressed_idx && is_tap) {
                cursor_pos = idx;
                refresh_focus();
                suppress_click_idx = idx;
                suppress_click_until_ms = (esp_timer_get_time() / 1000) + 250;
                numpad_activate(idx);
            }
            touch_started = false;
            touch_pressed_idx = -1;
        }
        return;
    }

    if (event->type == INPUT_TYPE_KEYBOARD) {
        int key_value = event->data.key_value;
        int prev_cursor = cursor_pos;

        if (key_value >= '0' && key_value <= '9') {
            if (input_pos < (int)sizeof(input_buffer) - 1) {
                input_buffer[input_pos++] = (char)key_value;
                input_buffer[input_pos] = '\0';
                update_display();
            }
            return;
        }
        if (key_value == 8) {
            if (input_pos > 0) {
                input_buffer[--input_pos] = '\0';
                update_display();
            }
            return;
        }
        if (key_value == ',' || key_value == 44) {
            if (input_pos < (int)sizeof(input_buffer) - 1) {
                input_buffer[input_pos++] = ',';
                input_buffer[input_pos] = '\0';
                update_display();
            }
            return;
        }
        if (key_value == 13 || key_value == '\n' || key_value == '\r') {
            numpad_activate(12);
            return;
        }
        if (key_value == '`' || key_value == 27 || key_value == 29) {
            display_manager_switch_view(&options_menu_view);
            return;
        }
        if (key_value == 'h' || key_value == ',') { cursor_pos = (cursor_pos > 0) ? cursor_pos - 1 : NP_BTN_COUNT - 1; }
        else if (key_value == 'l' || key_value == '/') { cursor_pos = (cursor_pos < NP_BTN_COUNT - 1) ? cursor_pos + 1 : 0; }
        else if (key_value == 'k' || key_value == ';') {
            int col = cursor_pos % NP_COLS;
            int row = cursor_pos / NP_COLS;
            row = (row > 0) ? row - 1 : NP_ROWS - 1;
            cursor_pos = row * NP_COLS + col;
            if (cursor_pos >= NP_BTN_COUNT) cursor_pos = NP_BTN_COUNT - 1;
        }
        else if (key_value == 'j' || key_value == '.') {
            int col = cursor_pos % NP_COLS;
            int row = cursor_pos / NP_COLS;
            row = (row < NP_ROWS - 1) ? row + 1 : 0;
            cursor_pos = row * NP_COLS + col;
            if (cursor_pos >= NP_BTN_COUNT) cursor_pos = NP_BTN_COUNT - 1;
        }
        else if (key_value == '=') {
            numpad_activate(cursor_pos);
            return;
        }

        if (prev_cursor != cursor_pos) refresh_focus();
    }
    else if (event->type == INPUT_TYPE_JOYSTICK) {
        int button = event->data.joystick_index;
        int prev_cursor = cursor_pos;

        if (button == 0) { cursor_pos = (cursor_pos > 0) ? cursor_pos - 1 : NP_BTN_COUNT - 1; }
        else if (button == 3) { cursor_pos = (cursor_pos < NP_BTN_COUNT - 1) ? cursor_pos + 1 : 0; }
        else if (button == 2) {
            int col = cursor_pos % NP_COLS;
            int row = cursor_pos / NP_COLS;
            row = (row > 0) ? row - 1 : NP_ROWS - 1;
            cursor_pos = row * NP_COLS + col;
            if (cursor_pos >= NP_BTN_COUNT) cursor_pos = NP_BTN_COUNT - 1;
        }
        else if (button == 4) {
            int col = cursor_pos % NP_COLS;
            int row = cursor_pos / NP_COLS;
            row = (row < NP_ROWS - 1) ? row + 1 : 0;
            cursor_pos = row * NP_COLS + col;
            if (cursor_pos >= NP_BTN_COUNT) cursor_pos = NP_BTN_COUNT - 1;
        }
        else if (button == 1) {
            numpad_activate(cursor_pos);
            return;
        }

        if (prev_cursor != cursor_pos) refresh_focus();
    }
    else if (event->type == INPUT_TYPE_ENCODER) {
        int prev_cursor = cursor_pos;
        if (event->data.encoder.button) {
            numpad_activate(cursor_pos);
        } else {
            if (event->data.encoder.direction > 0) {
                cursor_pos = (cursor_pos < NP_BTN_COUNT - 1) ? cursor_pos + 1 : 0;
            } else {
                cursor_pos = (cursor_pos > 0) ? cursor_pos - 1 : NP_BTN_COUNT - 1;
            }
        }
        if (prev_cursor != cursor_pos) refresh_focus();
    }
#ifdef CONFIG_USE_ENCODER
    else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        ESP_LOGI(TAG, "Exit button pressed, returning to main menu");
        display_manager_switch_view(&main_menu_view);
    }
#endif
}

void get_number_pad_callback(void **callback) {
    if (!callback) return;
    *callback = number_pad_view.input_callback;
}

View number_pad_view = {
    .root = NULL,
    .create = number_pad_create,
    .destroy = number_pad_destroy,
    .input_callback = handle_hardware_button_press_number_pad,
    .name = "Number Pad Screen",
    .get_hardwareinput_callback = get_number_pad_callback
};

void set_number_pad_mode(ENumberPadMode mode) {
    current_mode = mode;
}
