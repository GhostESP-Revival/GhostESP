#include "managers/views/lockscreen.h"
#include "managers/settings_manager.h"
#include "managers/views/main_menu_screen.h"
#include "gui/screen_layout.h"
#include "gui/lvgl_safe.h"
#include "gui/theme_palette_api.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

extern const lv_img_dsc_t tired_50x50;
extern const lv_img_dsc_t what2_50x50;
extern const lv_img_dsc_t angry_50x50;
extern const lv_img_dsc_t happy_50x50;
extern const lv_img_dsc_t love_50x50;

#define MAX_INPUT_LEN 31
#define STORED_PIN_MARKER 0x80
#define STORED_PIN_LEN_MASK 0x7F
#define NUMPAD_COLS 3
#define NUMPAD_ROWS 4
#define NUMPAD_BTNS 12

static const char * const k_numpad_labels[NUMPAD_BTNS] = {
    "1", "2", "3",
    "4", "5", "6",
    "7", "8", "9",
    LV_SYMBOL_BACKSPACE, "0", "OK"
};

static const char k_numpad_chars[NUMPAD_BTNS] = {
    '1', '2', '3',
    '4', '5', '6',
    '7', '8', '9',
    '\b', '0', '\r'
};

typedef enum {
    GHOST_SLEEPING,
    GHOST_TYPING,
    GHOST_ERROR,
    GHOST_UNLOCKED
} GhostState;

static lv_obj_t *s_root;
static lv_obj_t *s_content;
static lv_obj_t *s_ghost;
static lv_obj_t *s_prompt;
static lv_obj_t *s_dots;
static lv_obj_t *s_numpad_cont;
static lv_obj_t *s_numpad_btns[NUMPAD_BTNS];
static lv_timer_t *s_idle_timer;
static lv_timer_t *s_bob_timer;
static lv_timer_t *s_unlock_timer;

static int s_focus_idx;
static bool s_touch_started;
static int s_touch_pressed_idx;
static int s_suppress_click_idx;
static int64_t s_suppress_click_until_ms;
static char s_input[MAX_INPUT_LEN + 1];
static uint8_t s_input_len;
static bool s_setup_mode;
static bool s_setup_confirm;
static char s_setup_first[MAX_INPUT_LEN + 1];
static GhostState s_ghost_state;
static int s_ghost_base_y;

static void lockscreen_clear_input(void);
static void lockscreen_add_char(char c);
static void lockscreen_delete_last(void);
static void lockscreen_submit(void);
static void lockscreen_on_wrong(void);
static void lockscreen_on_correct(void);
static void lockscreen_update_dots(void);
static void lockscreen_update_ghost(bool immediate);
static void lockscreen_apply_ghost_bob(void);
static void lockscreen_set_prompt(const char *text);
static void lockscreen_build_numpad(void);
static void lockscreen_idle_cb(lv_timer_t *timer);
static void lockscreen_bob_cb(lv_timer_t *timer);
static void lockscreen_unlock_cb(lv_timer_t *timer);
static void lockscreen_numpad_cb(lv_event_t *e);
static void lockscreen_normalize_input(InputEvent *event, bool *up, bool *down, bool *left, bool *right, bool *select, bool *back);
static void lockscreen_input_handler(InputEvent *event);
static void lockscreen_focus_btn(int idx);
static void lockscreen_move_focus(int dx, int dy);

static void derive_key(uint8_t *out, size_t len) {
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        for (size_t i = 0; i < len; i++) {
            out[i] = mac[i % 6] ^ (uint8_t)(0xA5 + i);
        }
    } else {
        for (size_t i = 0; i < len; i++) {
            out[i] = (uint8_t)(0x42 + i);
        }
    }
}

static bool verify_input(const char *input) {
    uint8_t key[32];
    derive_key(key, sizeof(key));
    const uint8_t *stored = (const uint8_t *)G_Settings.lockscreen_obfuscated;
    if ((stored[0] & STORED_PIN_MARKER) == 0) return false;
    size_t stored_len = stored[0] & STORED_PIN_LEN_MASK;
    size_t in_len = strlen(input);
    if (in_len != stored_len) return false;
    for (size_t i = 0; i < in_len; i++) {
        char decrypted = (char)(stored[i + 1] ^ key[i % 32]);
        if (decrypted != input[i]) return false;
    }
    return true;
}

static void save_obfuscated(const char *input) {
    uint8_t key[32];
    derive_key(key, sizeof(key));
    size_t len = strlen(input);
    if (len > sizeof(G_Settings.lockscreen_obfuscated) - 1) {
        len = sizeof(G_Settings.lockscreen_obfuscated) - 1;
    }
    memset(G_Settings.lockscreen_obfuscated, 0, sizeof(G_Settings.lockscreen_obfuscated));
    G_Settings.lockscreen_obfuscated[0] = (char)(STORED_PIN_MARKER | len);
    uint8_t *stored = (uint8_t *)G_Settings.lockscreen_obfuscated;
    for (size_t i = 0; i < len; i++) {
        stored[i + 1] = (uint8_t)input[i] ^ key[i % 32];
    }
    settings_persist_setting(SETTING_LOCKSCREEN_CHANGE_PIN);
}

static int lockscreen_ghost_bob_offset(void) {
    uint32_t phase = (uint32_t)((esp_timer_get_time() / 70000ULL) % 24ULL);
    int step = (phase < 12U) ? (int)phase : (23 - (int)phase);
    return (step / 2) - 3;
}

bool lockscreen_is_configured(void) {
    uint8_t stored_len = (uint8_t)G_Settings.lockscreen_obfuscated[0];
    return (stored_len & STORED_PIN_MARKER) != 0 &&
           (stored_len & STORED_PIN_LEN_MASK) > 0 &&
           (stored_len & STORED_PIN_LEN_MASK) <= MAX_INPUT_LEN;
}

void lockscreen_reset_input(void) {
    memset(s_input, 0, sizeof(s_input));
    s_input_len = 0;
    s_setup_mode = false;
    s_setup_confirm = false;
    s_setup_first[0] = '\0';
    s_ghost_state = GHOST_SLEEPING;
    s_focus_idx = 0;
    s_touch_started = false;
    s_touch_pressed_idx = -1;
    s_suppress_click_idx = -1;
    s_suppress_click_until_ms = 0;
}

void lockscreen_enter_setup(void) {
    lockscreen_reset_input();
    s_setup_mode = true;
}

static void lockscreen_clear_input(void) {
    memset(s_input, 0, sizeof(s_input));
    s_input_len = 0;
    lockscreen_update_dots();
}

static void lockscreen_add_char(char c) {
    if (s_input_len >= MAX_INPUT_LEN) return;
    if (c < '0' || c > '9') return;
    s_input[s_input_len++] = c;
    s_input[s_input_len] = '\0';
    s_ghost_state = GHOST_TYPING;
    lockscreen_update_ghost(true);
    lockscreen_update_dots();
    if (s_idle_timer) lv_timer_reset(s_idle_timer);
}

static void lockscreen_delete_last(void) {
    if (s_input_len > 0) {
        s_input[--s_input_len] = '\0';
        s_ghost_state = GHOST_TYPING;
        lockscreen_update_ghost(true);
        lockscreen_update_dots();
        if (s_idle_timer) lv_timer_reset(s_idle_timer);
    }
}

static void lockscreen_set_prompt(const char *text) {
    if (s_prompt && lv_obj_is_valid(s_prompt)) {
        lv_label_set_text(s_prompt, text);
    }
}

static void lockscreen_update_dots(void) {
    if (!s_dots || !lv_obj_is_valid(s_dots)) return;
    char dots[(MAX_INPUT_LEN * 4) + 1];
    int offset = 0;
    for (int i = 0; i < (int)s_input_len && i < MAX_INPUT_LEN; i++) {
        strcpy(dots + offset, LV_SYMBOL_BULLET);
        offset += strlen(LV_SYMBOL_BULLET);
    }
    dots[offset] = '\0';
    if (s_input_len == 0) {
        lv_label_set_text(s_dots, "");
    } else {
        lv_label_set_text(s_dots, dots);
    }
}

static void lockscreen_update_ghost(bool immediate) {
    if (!s_ghost || !lv_obj_is_valid(s_ghost)) return;
    const lv_img_dsc_t *src = &tired_50x50;
    switch (s_ghost_state) {
        case GHOST_SLEEPING:  src = &tired_50x50; break;
        case GHOST_TYPING:    src = &what2_50x50; break;
        case GHOST_ERROR:     src = &angry_50x50; break;
        case GHOST_UNLOCKED:  src = &happy_50x50; break;
    }
    lv_img_set_src(s_ghost, src);
    int content_h = LV_VER_RES - GUI_STATUS_BAR_H;
    if (LV_HOR_RES > LV_VER_RES && content_h <= 146) {
        int ghost_sz = 60;
        lv_img_set_zoom(s_ghost, (ghost_sz * 256) / 50);
    }
    lockscreen_apply_ghost_bob();
    (void)immediate;
}

static void lockscreen_apply_ghost_bob(void) {
    if (!s_ghost || !lv_obj_is_valid(s_ghost)) return;
    lv_obj_set_y(s_ghost, s_ghost_base_y + lockscreen_ghost_bob_offset());
}

static void lockscreen_idle_cb(lv_timer_t *timer) {
    (void)timer;
    if (s_ghost_state == GHOST_TYPING || s_ghost_state == GHOST_ERROR) {
        s_ghost_state = GHOST_SLEEPING;
        lockscreen_update_ghost(true);
    }
}

static void lockscreen_bob_cb(lv_timer_t *timer) {
    (void)timer;
    lockscreen_apply_ghost_bob();
}

static void lockscreen_unlock_cb(lv_timer_t *timer) {
    s_unlock_timer = NULL;
    lv_timer_del(timer);
    View *return_view = display_manager_get_lockscreen_return_view();
    if (return_view == NULL || return_view == &lockscreen_view) {
        return_view = &main_menu_view;
    }
    display_manager_clear_lockscreen_return_view();
    display_manager_switch_view(return_view);
}

static void lockscreen_on_wrong(void) {
    s_ghost_state = GHOST_ERROR;
    lockscreen_update_ghost(true);
    lockscreen_set_prompt("Wrong PIN");
    lockscreen_clear_input();
}

static void lockscreen_on_correct(void) {
    s_ghost_state = GHOST_UNLOCKED;
    lockscreen_update_ghost(true);
    lockscreen_set_prompt("Unlocked!");
    if (s_unlock_timer) {
        lv_timer_del(s_unlock_timer);
        s_unlock_timer = NULL;
    }
    s_unlock_timer = lv_timer_create(lockscreen_unlock_cb, 600, NULL);
}

static void lockscreen_submit(void) {
    if (s_input_len == 0) return;

    if (s_setup_mode) {
        if (!s_setup_confirm) {
            // First entry
            strncpy(s_setup_first, s_input, sizeof(s_setup_first) - 1);
            s_setup_first[sizeof(s_setup_first) - 1] = '\0';
            s_setup_confirm = true;
            lockscreen_clear_input();
            lockscreen_set_prompt("Confirm PIN");
            return;
        } else {
            // Confirmation
            if (strcmp(s_input, s_setup_first) == 0) {
                save_obfuscated(s_input);
                s_setup_mode = false;
                lockscreen_set_prompt("Saved!");
                lockscreen_on_correct();
                return;
            } else {
                s_setup_confirm = false;
                s_setup_first[0] = '\0';
                lockscreen_clear_input();
                lockscreen_set_prompt("Mismatch! Set again");
                s_ghost_state = GHOST_ERROR;
                lockscreen_update_ghost(true);
                if (s_idle_timer) lv_timer_reset(s_idle_timer);
                return;
            }
        }
    }

    if (verify_input(s_input)) {
        lockscreen_on_correct();
    } else {
        lockscreen_on_wrong();
    }
}

static int lockscreen_hit_test(int x, int y) {
    for (int i = 0; i < NUMPAD_BTNS; i++) {
        if (!s_numpad_btns[i] || !lv_obj_is_valid(s_numpad_btns[i])) continue;
        lv_area_t area;
        lv_obj_get_coords(s_numpad_btns[i], &area);
        if (x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2) {
            return i;
        }
    }
    return -1;
}

static void lockscreen_focus_btn(int idx) {
    if (idx < 0 || idx >= NUMPAD_BTNS) return;
    if (!s_numpad_cont || !lv_obj_is_valid(s_numpad_cont)) return;
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
    for (int i = 0; i < NUMPAD_BTNS; i++) {
        if (!s_numpad_btns[i] || !lv_obj_is_valid(s_numpad_btns[i])) continue;
        bool focused = (i == idx);
        lv_obj_set_style_border_width(s_numpad_btns[i], focused ? 3 : 1, 0);
        lv_obj_set_style_border_color(s_numpad_btns[i], focused ? accent : lv_color_hex(0x444444), 0);
        lv_obj_set_style_bg_opa(s_numpad_btns[i], focused ? LV_OPA_40 : LV_OPA_20, 0);
        lv_obj_t *lbl = lv_obj_get_child(s_numpad_btns[i], 0);
        if (lbl) {
            lv_obj_set_style_text_color(lbl, focused ? lv_color_hex(theme_palette_get_accent(theme)) : lv_color_hex(0xFFFFFF), 0);
        }
    }
}

static void lockscreen_move_focus(int dx, int dy) {
    int col = s_focus_idx % NUMPAD_COLS;
    int row = s_focus_idx / NUMPAD_COLS;
    col += dx;
    row += dy;
    if (col < 0) col = NUMPAD_COLS - 1;
    if (col >= NUMPAD_COLS) col = 0;
    if (row < 0) row = NUMPAD_ROWS - 1;
    if (row >= NUMPAD_ROWS) row = 0;
    s_focus_idx = row * NUMPAD_COLS + col;
    lockscreen_focus_btn(s_focus_idx);
}

static void lockscreen_numpad_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (idx == s_suppress_click_idx && now_ms < s_suppress_click_until_ms) {
        s_suppress_click_idx = -1;
        return;
    }
    if (idx < 0 || idx >= NUMPAD_BTNS) return;
    s_focus_idx = idx;
    lockscreen_focus_btn(s_focus_idx);
    char c = k_numpad_chars[idx];
    if (c == '\b') {
        lockscreen_delete_last();
    } else if (c == '\r') {
        lockscreen_submit();
    } else {
        lockscreen_add_char(c);
    }
}

static void lockscreen_build_numpad(void) {
    if (!s_content || !lv_obj_is_valid(s_content)) return;
    if (s_numpad_cont && lv_obj_is_valid(s_numpad_cont)) {
        lv_obj_del(s_numpad_cont);
    }
    s_numpad_cont = lv_obj_create(s_content);
    lv_obj_set_style_bg_opa(s_numpad_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_numpad_cont, 0, 0);
    lv_obj_set_style_pad_all(s_numpad_cont, 0, 0);
    lv_obj_clear_flag(s_numpad_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_numpad_cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_numpad_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_numpad_cont, 2, 0);
    lv_obj_set_style_pad_column(s_numpad_cont, 2, 0);

    int content_h = LV_VER_RES - GUI_STATUS_BAR_H;
    int content_w = lv_obj_get_width(s_content);
    if (content_w <= 0) content_w = LV_HOR_RES;
    bool landscape = (content_w > content_h && content_h <= 146);

    int gap = 2;
    int btn_h, btn_w, numpad_x, numpad_y;

    if (landscape) {
        int left_col_w = content_w / 2;
        int numpad_area_x = left_col_w;
        int numpad_area_w = content_w - numpad_area_x;
        numpad_y = 0;
        int numpad_h = content_h;
        btn_h = (numpad_h - (NUMPAD_ROWS + 1) * gap) / NUMPAD_ROWS;
        if (btn_h > 30) btn_h = 30;
        if (btn_h < 14) btn_h = 14;
        btn_w = btn_h;
        int grid_w = NUMPAD_COLS * btn_w + (NUMPAD_COLS - 1) * gap;
        numpad_x = numpad_area_x + (numpad_area_w - grid_w) / 2;
        if (numpad_x < numpad_area_x) numpad_x = numpad_area_x;
    } else {
        int min_numpad_y = 94;
        int bottom_margin = 10;
        int numpad_h = content_h - min_numpad_y - bottom_margin;
        if (numpad_h < 36) {
            numpad_h = 36;
        }
        btn_h = (numpad_h - (NUMPAD_ROWS - 1) * gap) / NUMPAD_ROWS;
        if (btn_h > 30) btn_h = 30;
        if (btn_h < 14) btn_h = 14;
        btn_w = btn_h;
        int grid_h = NUMPAD_ROWS * btn_h + (NUMPAD_ROWS - 1) * gap;
        numpad_y = content_h - grid_h - bottom_margin;
        if (numpad_y < min_numpad_y) numpad_y = min_numpad_y;
        int grid_w = NUMPAD_COLS * btn_w + (NUMPAD_COLS - 1) * gap;
        numpad_x = (content_w - grid_w) / 2;
        if (numpad_x < 0) numpad_x = 0;
    }

    int grid_w = NUMPAD_COLS * btn_w + (NUMPAD_COLS - 1) * gap;
    int grid_h = NUMPAD_ROWS * btn_h + (NUMPAD_ROWS - 1) * gap;
    int grid_y_offset = landscape ? ((content_h - grid_h) / 2) : 0;

    lv_obj_set_size(s_numpad_cont, grid_w, grid_h);
    lv_obj_set_pos(s_numpad_cont, numpad_x, numpad_y + grid_y_offset);

    for (int i = 0; i < NUMPAD_BTNS; i++) {
        s_numpad_btns[i] = lv_btn_create(s_numpad_cont);
        lv_obj_set_size(s_numpad_btns[i], btn_w, btn_h);
        lv_obj_add_event_cb(s_numpad_btns[i], lockscreen_numpad_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_set_style_bg_color(s_numpad_btns[i], lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_numpad_btns[i], LV_OPA_20, 0);
        lv_obj_set_style_radius(s_numpad_btns[i], GUI_RADIUS_SM / 2, 0);
        lv_obj_set_style_border_width(s_numpad_btns[i], 1, 0);
        lv_obj_set_style_border_color(s_numpad_btns[i], lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_shadow_width(s_numpad_btns[i], 0, 0);
        lv_obj_set_style_shadow_color(s_numpad_btns[i], lv_color_hex(0x000000), 0);
        lv_obj_set_style_shadow_opa(s_numpad_btns[i], LV_OPA_TRANSP, 0);
        lv_obj_t *lbl = lv_label_create(s_numpad_btns[i]);
        lv_label_set_text(lbl, k_numpad_labels[i]);
        const lv_font_t *f = (btn_h < 20) ? &lv_font_montserrat_10 : &lv_font_montserrat_12;
        lv_obj_set_style_text_font(lbl, f, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(lbl);
    }
    s_focus_idx = 0;
    lockscreen_focus_btn(0);
}

static void lockscreen_destroy_numpad(void) {
    if (s_numpad_cont && lv_obj_is_valid(s_numpad_cont)) {
        lv_obj_del(s_numpad_cont);
        s_numpad_cont = NULL;
    }
    for (int i = 0; i < NUMPAD_BTNS; i++) s_numpad_btns[i] = NULL;
}

static void lockscreen_normalize_input(InputEvent *event, bool *up, bool *down, bool *left, bool *right, bool *select, bool *back) {
    *up = *down = *left = *right = *select = *back = false;
    if (!event) return;
    switch (event->type) {
        case INPUT_TYPE_JOYSTICK:
            if (!event->data.joystick_pressed) return;
            if (event->data.joystick_index == 2) *up = true;
            else if (event->data.joystick_index == 4) *down = true;
            else if (event->data.joystick_index == 0) *left = true;
            else if (event->data.joystick_index == 3) *right = true;
            else if (event->data.joystick_index == 1) *select = true;
            break;
        case INPUT_TYPE_KEYBOARD:
            if (event->data.key_value == LV_KEY_UP || event->data.key_value == ';' || event->data.key_value == 'k') *up = true;
            else if (event->data.key_value == LV_KEY_DOWN || event->data.key_value == '.' || event->data.key_value == 'j') *down = true;
            else if (event->data.key_value == LV_KEY_LEFT || event->data.key_value == ',' || event->data.key_value == 'h') *left = true;
            else if (event->data.key_value == LV_KEY_RIGHT || event->data.key_value == '/' || event->data.key_value == 'l') *right = true;
            else if (event->data.key_value == LV_KEY_ENTER || event->data.key_value == '\n' || event->data.key_value == '\r' || event->data.key_value == 13) *select = true;
            else if (event->data.key_value == LV_KEY_ESC || event->data.key_value == 27 || event->data.key_value == 29 || event->data.key_value == '`') *back = true;
            else if (event->data.key_value == '\b') *back = true;
            break;
        case INPUT_TYPE_ENCODER:
            if (event->data.encoder.button) *select = true;
            else if (event->data.encoder.direction < 0) *up = true;
            else if (event->data.encoder.direction > 0) *down = true;
            break;
        case INPUT_TYPE_EXIT_BUTTON:
            *back = true;
            break;
        default:
            break;
    }
}

static void lockscreen_input_handler(InputEvent *event) {
    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *data = &event->data.touch_data;
        if (data->state == LV_INDEV_STATE_PR) {
            int idx = lockscreen_hit_test(data->point.x, data->point.y);
            s_touch_started = (idx >= 0);
            s_touch_pressed_idx = idx;
            if (idx >= 0) {
                s_focus_idx = idx;
                lockscreen_focus_btn(s_focus_idx);
            }
        } else if (data->state == LV_INDEV_STATE_REL && s_touch_started) {
            int idx = lockscreen_hit_test(data->point.x, data->point.y);
            if (idx >= 0 && idx == s_touch_pressed_idx) {
                s_suppress_click_idx = idx;
                s_suppress_click_until_ms = (esp_timer_get_time() / 1000) + 250;
                char c = k_numpad_chars[idx];
                if (c == '\b') lockscreen_delete_last();
                else if (c == '\r') lockscreen_submit();
                else lockscreen_add_char(c);
            }
            s_touch_started = false;
            s_touch_pressed_idx = -1;
        }
        return;
    }

    bool up, down, left, right, select, back;
    lockscreen_normalize_input(event, &up, &down, &left, &right, &select, &back);

    // Keyboard direct typing for PIN entry
    if (event->type == INPUT_TYPE_KEYBOARD) {
        uint8_t kv = event->data.key_value;
        if (kv >= '0' && kv <= '9') {
            lockscreen_add_char((char)kv);
            return;
        }
        if (kv == '\b') {
            lockscreen_delete_last();
            return;
        }
        if (kv == '\r' || kv == '\n' || kv == 13) {
            lockscreen_submit();
            return;
        }
    }

    if (s_numpad_cont && lv_obj_is_valid(s_numpad_cont)) {
        // Encoder gets linear navigation (CW = next, CCW = prev)
        if (event->type == INPUT_TYPE_ENCODER) {
            if (select) {
                char c = k_numpad_chars[s_focus_idx];
                if (c == '\b') lockscreen_delete_last();
                else if (c == '\r') lockscreen_submit();
                else lockscreen_add_char(c);
            } else if (event->data.encoder.direction > 0) {
                s_focus_idx++;
                if (s_focus_idx >= NUMPAD_BTNS) s_focus_idx = 0;
                lockscreen_focus_btn(s_focus_idx);
            } else if (event->data.encoder.direction < 0) {
                s_focus_idx--;
                if (s_focus_idx < 0) s_focus_idx = NUMPAD_BTNS - 1;
                lockscreen_focus_btn(s_focus_idx);
            }
            return;
        }
        if (up) lockscreen_move_focus(0, -1);
        if (down) lockscreen_move_focus(0, 1);
        if (left) lockscreen_move_focus(-1, 0);
        if (right) lockscreen_move_focus(1, 0);
        if (select) {
            char c = k_numpad_chars[s_focus_idx];
            if (c == '\b') lockscreen_delete_last();
            else if (c == '\r') lockscreen_submit();
            else lockscreen_add_char(c);
        }
        if (back) lockscreen_delete_last();
    }
}

void lockscreen_create(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t bg_color = lv_color_hex(theme_palette_get_background(theme));
    display_manager_fill_screen(bg_color);

    s_root = gui_screen_create_root(NULL, "Locked", bg_color, LV_OPA_COVER);
    lockscreen_view.root = s_root;
    s_content = gui_screen_create_content(s_root, GUI_STATUS_BAR_H);

    int content_h = LV_VER_RES - GUI_STATUS_BAR_H;
    int content_w = lv_obj_get_width(s_content);
    if (content_w <= 0) content_w = LV_HOR_RES;
    bool landscape = (content_w > content_h && content_h <= 146);

    if (landscape) {
        int left_col_w = content_w / 2;
        int ghost_sz = 60;
        int ghost_x = (left_col_w - ghost_sz) / 2;
        int group_h = ghost_sz + 8 + 13 + 4 + 17;
        int group_y = (content_h - group_h) / 2;
        if (group_y < 0) group_y = 0;
        s_ghost_base_y = group_y;

        s_ghost = lv_img_create(s_content);
        lv_img_set_src(s_ghost, &tired_50x50);
        lv_obj_set_pos(s_ghost, ghost_x, s_ghost_base_y + lockscreen_ghost_bob_offset());
        lv_img_set_zoom(s_ghost, (ghost_sz * 256) / 50);

        s_prompt = lv_label_create(s_content);
        lv_obj_set_style_text_font(s_prompt, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(s_prompt, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_align(s_prompt, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(s_prompt, left_col_w);
        lv_obj_align(s_prompt, LV_ALIGN_TOP_LEFT, 0, group_y + ghost_sz + 8);

        s_dots = lv_label_create(s_content);
        lv_obj_set_style_text_font(s_dots, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_dots, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_align(s_dots, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(s_dots, left_col_w);
        lv_obj_align(s_dots, LV_ALIGN_TOP_LEFT, 0, group_y + ghost_sz + 25);
    } else {
        int min_numpad_y = 94;
        int bottom_margin = 10;
        int numpad_h = content_h - min_numpad_y - bottom_margin;
        if (numpad_h < 36) numpad_h = 36;
        int btn_h = (numpad_h - (NUMPAD_ROWS - 1) * 2) / NUMPAD_ROWS;
        if (btn_h > 30) btn_h = 30;
        if (btn_h < 14) btn_h = 14;
        int grid_h = NUMPAD_ROWS * btn_h + (NUMPAD_ROWS - 1) * 2;
        int numpad_y = content_h - grid_h - bottom_margin;
        if (numpad_y < min_numpad_y) numpad_y = min_numpad_y;

        int icon_y = (numpad_y - 88) / 2;
        if (icon_y < 2) icon_y = 2;
        int prompt_y_offset = icon_y + 56;
        int dots_y_offset = icon_y + 72;
        s_ghost_base_y = icon_y;

        s_ghost = lv_img_create(s_content);
        lv_img_set_src(s_ghost, &tired_50x50);
        lv_obj_set_pos(s_ghost, (content_w - 50) / 2, s_ghost_base_y + lockscreen_ghost_bob_offset());

        s_prompt = lv_label_create(s_content);
        lv_obj_set_style_text_font(s_prompt, gui_font_caption(), 0);
        lv_obj_set_style_text_color(s_prompt, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(s_prompt, LV_ALIGN_TOP_MID, 0, prompt_y_offset);

        s_dots = lv_label_create(s_content);
        lv_obj_set_style_text_font(s_dots, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(s_dots, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(s_dots, LV_ALIGN_TOP_MID, 0, dots_y_offset);
    }

    // Check if we need setup mode
    if (!lockscreen_is_configured() || s_setup_mode) {
        s_setup_mode = true;
        s_setup_confirm = false;
        s_setup_first[0] = '\0';
        lockscreen_set_prompt("Set PIN");
    } else {
        lockscreen_set_prompt("Enter PIN");
    }

    lockscreen_build_numpad();

    lockscreen_update_dots();
    s_ghost_state = GHOST_SLEEPING;
    lockscreen_update_ghost(true);

    if (s_idle_timer) {
        lv_timer_del(s_idle_timer);
        s_idle_timer = NULL;
    }
    s_idle_timer = lv_timer_create(lockscreen_idle_cb, 3000, NULL);
    if (s_bob_timer) {
        lv_timer_del(s_bob_timer);
        s_bob_timer = NULL;
    }
    s_bob_timer = lv_timer_create(lockscreen_bob_cb, 250, NULL);
}

void lockscreen_destroy(void) {
    if (s_idle_timer) {
        lv_timer_del(s_idle_timer);
        s_idle_timer = NULL;
    }
    if (s_bob_timer) {
        lv_timer_del(s_bob_timer);
        s_bob_timer = NULL;
    }
    if (s_unlock_timer) {
        lv_timer_del(s_unlock_timer);
        s_unlock_timer = NULL;
    }
    lockscreen_destroy_numpad();
    s_touch_started = false;
    s_touch_pressed_idx = -1;
    s_suppress_click_idx = -1;
    s_suppress_click_until_ms = 0;
    lvgl_obj_del_safe(&s_root);
    lockscreen_view.root = NULL;
    s_content = NULL;
    s_ghost = NULL;
    s_prompt = NULL;
    s_dots = NULL;
}

View lockscreen_view = {
    .root = NULL,
    .create = lockscreen_create,
    .destroy = lockscreen_destroy,
    .input_callback = lockscreen_input_handler,
    .name = "Lockscreen",
};
