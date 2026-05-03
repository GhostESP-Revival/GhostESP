#include "managers/views/keyboard_screen.h"
#include "core/serial_manager.h"
#include "managers/views/options_screen.h"
#include "managers/views/terminal_screen.h"
#include "managers/views/main_menu_screen.h"
#include "gui/screen_layout.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "gui/lvgl_safe.h"
#include <string.h>
#include <ctype.h>

#define KEYBOARD_COLUMNS 10

static const char *TAG = "keyboard_screen";

static View *keyboard_return_view = NULL;

static lv_obj_t *root = NULL;
static lv_obj_t *input_label = NULL;
static char input_buffer[128] = {0};
static int input_len = 0;
static char pending_initial_text[128] = {0};
static bool has_pending_initial_text = false;
static bool start_with_caps = true;
static KeyboardSubmitCallback submit_callback = NULL;

static bool is_caps = true;
static bool is_symbols_mode = false;
static bool is_capslock = false;
#if defined(CONFIG_USE_ENCODER) && !defined(CONFIG_USE_JOYSTICK)
static lv_obj_t *encoder_cont = NULL;
static lv_obj_t *encoder_labels[50];
static const char *encoder_alpha_items[41] = {
    "Aa","A","B","C","D","E","F","G","H","I","J",
    "K","L","M","N","O","P","Q","R","S","T",
    "U","V","W","X","Y","Z","0","1","2","3",
    "4","5","6","7","8","9","SPA","SYM","<-","ENT"
};
static const int encoder_alpha_count = 41;
static const char *encoder_sym_items[40] = {
    "1","2","3","4","5","6","7","8","9","0",
    "!","@","#","$","%","^","&","*","(",")",
    "-","_","=","+","[","]","{","}","\\","|",
    ";",":","'","\"","<",">","?","/","ABC","ENT"
};
static const int encoder_sym_count = 40;
static const char **encoder_items = NULL;
static int encoder_item_count = 0;
static int encoder_sel_idx = 0;
static int encoder_item_spacing = 0;
static int encoder_screen_width = 0;
static int encoder_offset_x = 0;
static bool encoder_sym_mode = false;
static bool encoder_uppercase = true;
#endif

static char placeholder[64] = "Enter text...";

static bool styles_inited = false;
static lv_style_t style_key_btn;
static lv_style_t style_key_label;
// temporarily override rounded corners during initial build to avoid mask cost
static int saved_key_radius = 3;
static int saved_input_label_radius = 5;
static bool radius_override_active = false;
// btnmatrix-based keyboard to avoid per-key object creation
static lv_obj_t *key_matrix = NULL;
static const char *btn_map[64];
static uint8_t btn_widths[64];
static int btn_map_len = 0;
static int shift_btn_id = -1;
static int pressed_btn_id = -1;

static void key_matrix_event_cb(lv_event_t *e);
static void build_key_matrix(void);

// cache for key button objects to avoid invalid parent during label create
static lv_obj_t *key_btns[5][KEYBOARD_COLUMNS];

static void init_keyboard_styles(void) {
    if (styles_inited) return;
    lv_style_init(&style_key_btn);
    lv_style_set_bg_color(&style_key_btn, lv_color_hex(0xFFFFFF));
    lv_style_set_bg_opa(&style_key_btn, LV_OPA_COVER);
    lv_style_set_border_color(&style_key_btn, lv_color_hex(0x333333));
    lv_style_set_border_width(&style_key_btn, 1);
    lv_style_set_radius(&style_key_btn, 3);

    lv_style_init(&style_key_label);
    lv_style_set_text_color(&style_key_label, lv_color_hex(0x000000));
    lv_style_set_text_font(&style_key_label, &lv_font_montserrat_14);

    styles_inited = true;
}

static const char *keys[][10] = {
    {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"},
    {"A", "S", "D", "F", "G", "H", "J", "K", "L"},
    {"SHIFT", "Z", "X", "C", "V", "B", "N", "M"},
    {",", ".", "\"", " ", "DEL"},
    {"SYM", "Exit", "Done"}
};

static const char *symbols[][10] = {
    {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
    {"!", "@", "#", "$", "%", "^", "&", "*", "(", ")"},
    {"-", "_", "=", "+", "[", "]", "{", "}", "\\", "|"},
    {";", ":", "'", "\"", "<", ">", "?", "/"},
    {"ABC", "Exit", "Done"}
};

static const int row_lengths[] = {10, 9, 8, 5, 3};
static const int symbols_row_lengths[] = {10, 10, 10, 8, 3};
static const int max_row_lengths[] = {10, 10, 10, 8, 3};
static const int num_rows = 5;

static void submit_text();
static void add_char_to_buffer(char c);
static void remove_char_from_buffer();
static void update_input_label();
static void update_key_labels();
static void recreate_keyboard_buttons();
static void get_key_position(int row, int col, int *x, int *width, bool symbols_mode);
static void ensure_valid_cursor(void);
static lv_obj_t *get_key_button_at(int row, int col);
static void apply_selection_highlight(void);
static void activate_selected_key(void);
static void keyboard_build_step(lv_timer_t *t);
static void hide_all_key_buttons(void);
static void reveal_row(int row);
static void destroy_key_buttons(void);

static lv_obj_t *pressed_key_btn = NULL;
static lv_obj_t *selected_key_btn = NULL;
static int cursor_row = 0;
static int cursor_col = 0;
static lv_timer_t *keyboard_build_timer = NULL;
static int keyboard_build_row = 0;
static int keyboard_build_col = 0;
static int keyboard_build_phase = 0; // 0=create buttons, 1=create labels
// track which btnmatrix item is currently focused by joystick to manage CHECKED state
static int joy_focused_btn_id = -1;
// hold-to-invert state for joystick
#define KEYBOARD_HOLD_INVERT_MS 800
static bool joy_holding_letter = false;  // true after hold threshold crossed; label is inverted
static bool joy_pending_letter  = false; // true after press, before threshold or release
static bool joy_inverted_case   = false;
static lv_timer_t *joy_hold_timer = NULL;

static bool is_shift_key(const char *key) {
    return strcmp(key, "SHIFT") == 0;
}
static bool is_del_key(const char *key) {
    return strcmp(key, "DEL") == 0;
}
static bool is_space_key(const char *key) {
    return strcmp(key, " ") == 0;
}
static bool is_symbol_key(const char *key) {
    return strcmp(key, "SYM") == 0;
}
static bool is_alpha_key(const char *key) {
    return strlen(key) == 1 && isalpha((unsigned char)key[0]);
}

static const char* get_key_label(const char *key, bool caps, bool symbols_mode) {
    if (is_shift_key(key)) return LV_SYMBOL_UP;
    if (is_del_key(key)) return LV_SYMBOL_BACKSPACE;
    if (!symbols_mode && is_alpha_key(key)) {
        static char buf[2];
        buf[0] = caps ? toupper(key[0]) : tolower(key[0]);
        buf[1] = '\0';
        return buf;
    }
    return key;
}

static void style_shift_key(lv_obj_t *btn, lv_obj_t *label, bool capslock, bool caps) {
    if (capslock) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x00BFFF), 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    } else if (caps) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFD600), 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x000000), 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x000000), 0);
    }
}

static void clear_input_buffer(void) {
    memset(input_buffer, 0, sizeof(input_buffer));
    input_len = 0;
    update_input_label();
}
static void set_placeholder_text(const char *text) {
    if (text && strlen(text) <= sizeof(placeholder) - 1) {
        strncpy(placeholder, text, sizeof(placeholder) - 1);
        placeholder[sizeof(placeholder) - 1] = '\0';
    }
    clear_input_buffer();
}
static const char *(*get_current_keys(void))[10] {
    return is_symbols_mode ? symbols : keys;
}
static const int *get_current_row_lengths(void) {
    return is_symbols_mode ? symbols_row_lengths : row_lengths;
}

static void hide_all_key_buttons(void) {
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (!root) return;
    uint32_t child_count = lv_obj_get_child_cnt(root);
    for (uint32_t i = 1; i < child_count; i++) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        if (child) {
            lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
        }
    }
#endif
}

static void reveal_row(int row) {
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (!root || row < 0 || row >= num_rows) return;
    const int *row_lens = get_current_row_lengths();
    int offset = 0;
    for (int r = 0; r < row; r++) offset += max_row_lengths[r];
    for (int c = 0; c < max_row_lengths[row]; c++) {
        int child_idx = 1 + offset + c;
        uint32_t cnt = lv_obj_get_child_cnt(root);
        if ((uint32_t)child_idx >= cnt) continue;
        lv_obj_t *key_btn = lv_obj_get_child(root, child_idx);
        if (!key_btn) continue;
        if (c < row_lens[row]) {
            lv_obj_clear_flag(key_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(key_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
#endif
}

static lv_obj_t* create_key_button(lv_obj_t *parent, int x, int y, int w, int h, const char *label_text) {
    init_keyboard_styles();
    lv_obj_t *btn = lv_obj_create(parent);
    if (!btn) return NULL;
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_add_style(btn, &style_key_btn, 0);

    // try to reuse an existing label child if present to avoid repeated allocations
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (!label) {
        label = lv_label_create(btn);
        if (label) {
            lv_obj_add_style(label, &style_key_label, 0);
            lv_obj_center(label);
        }
    } else {
        // ensure style and centering are applied when reusing
        lv_obj_add_style(label, &style_key_label, 0);
        lv_obj_center(label);
    }

    if (label) {
        // keys array contains static string literals; use static set to avoid allocations
        if (label_text) lv_label_set_text_static(label, label_text);
        else lv_label_set_text_static(label, "");
    }
    return btn;
}

static void submit_text() {
    if (submit_callback) {
        submit_callback(input_buffer);
        memset(input_buffer, 0, sizeof(input_buffer));
        input_len = 0;
        update_input_label();
    } else if (input_len > 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        vTaskDelay(pdMS_TO_TICKS(10));
        simulateCommand(input_buffer);
        memset(input_buffer, 0, sizeof(input_buffer));
        input_len = 0;
        update_input_label();
    }
}

static void add_char_to_buffer(char c) {
    if (input_len < sizeof(input_buffer) - 1) {
        // Use capslock or SHIFT if active, otherwise lowercase
        bool use_caps = is_capslock || is_caps;
        if (isalpha((unsigned char)c)) {
            input_buffer[input_len++] = use_caps ? toupper((unsigned char)c) : tolower((unsigned char)c);
        } else {
            input_buffer[input_len++] = c; // Add non-alphabetic characters unchanged
        }
        input_buffer[input_len] = '\0';
        update_input_label();
    }
    if (is_caps && !is_capslock) {
        is_caps = false; // Reset to lowercase after any key press unless capslock is on
        update_key_labels(); // Update key labels to reflect the change
        #if defined(CONFIG_USE_ENCODER) && !defined(CONFIG_USE_JOYSTICK)
        encoder_uppercase = (is_capslock || is_caps);
        if (encoder_cont && !encoder_sym_mode) {
            for (int j = 0; j < encoder_item_count; j++) {
                const char *t = encoder_items[j];
                if (strlen(t) == 1 && isalpha((unsigned char)t[0])) {
                    char tmp2[2] = { encoder_uppercase ? toupper((unsigned char)t[0]) : tolower((unsigned char)t[0]), '\0' };
                    lv_label_set_text(encoder_labels[j], tmp2);
                }
            }
        }
        #endif
    }
}

static void add_char_to_buffer_raw(char c) {
    if (input_len < sizeof(input_buffer) - 1) {
        input_buffer[input_len++] = c;
        input_buffer[input_len] = '\0';
        update_input_label();
    }
}

static void remove_char_from_buffer() {
    if (input_len > 0) {
        input_buffer[--input_len] = '\0';
        update_input_label();
    }
}

static void update_input_label() {
    if (input_label) {
        if (input_len == 0) {
            lv_label_set_text(input_label, placeholder);
        } else {
            lv_label_set_text(input_label, input_buffer);
        }
    }
}

static void update_key_labels() {
#if defined(CONFIG_USE_TOUCHSCREEN)
    // touch build uses btnmatrix; rebuild its map/texts
    if (key_matrix) {
        build_key_matrix();
        return;
    }
#endif
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (!root) return;

    int key_index = 0;
    uint32_t child_count = lv_obj_get_child_cnt(root);
    const char *(*current_keys)[10] = get_current_keys();
    const int *current_row_lengths = get_current_row_lengths();

    for (int r = 0; r < num_rows; r++) {
        for (int c = 0; c < max_row_lengths[r]; c++) {
            int child_idx = 1 + key_index;
            if (child_idx < child_count) {
                lv_obj_t *key_btn = lv_obj_get_child(root, child_idx);
                if (key_btn) {
                    lv_obj_t *key_label = lv_obj_get_child(key_btn, 0);
                    if (key_label) {
                        if (c < current_row_lengths[r]) {
                            const char* key_text = current_keys[r][c];
                            lv_label_set_text(key_label, get_key_label(key_text, is_caps, is_symbols_mode));
                            // Style SHIFT key
                            if (is_shift_key(key_text)) {
                                style_shift_key(key_btn, key_label, is_capslock, is_caps);
                            }
                            lv_obj_clear_flag(key_btn, LV_OBJ_FLAG_HIDDEN);
                        } else {
                            lv_label_set_text(key_label, "");
                            lv_obj_add_flag(key_btn, LV_OBJ_FLAG_HIDDEN);
                        }
                        lv_obj_set_style_text_color(key_label, lv_color_hex(0x000000), 0);
                    }
                }
            }
            key_index++;
        }
    }
#endif
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (!keyboard_build_timer) apply_selection_highlight();
#endif
}

static void recreate_keyboard_buttons() {
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (!root) return;
    // For touch/joystick builds, just rebuild the btnmatrix map instead of recreating objects
    build_key_matrix();
    return;
#endif
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (!root) return;

    lvgl_timer_del_safe(&keyboard_build_timer);
    // hide the root during incremental rebuild to avoid heavy draw churn
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    destroy_key_buttons();
    keyboard_build_phase = 0;
    hide_all_key_buttons();
    keyboard_build_row = 0;
    keyboard_build_col = 0;
    keyboard_build_timer = lv_timer_create(keyboard_build_step, 10, NULL);
#else
    lvgl_timer_del_safe(&keyboard_build_timer);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    int screen_height = LV_VER_RES;
    int status_bar_height = GUI_STATUS_BAR_HEIGHT;
    int display_height = 40;
    int padding = 5;
    int keys_start_y = status_bar_height + display_height + padding * 2;
    int keys_area_height = screen_height - keys_start_y;
    int key_height = (keys_area_height / num_rows) - 4;
    int key_y = keys_start_y;

    int max_keys = 0;
    for (int r = 0; r < num_rows; r++) {
        int keys_in_row = row_lengths[r] > symbols_row_lengths[r] ? row_lengths[r] : symbols_row_lengths[r];
        if (keys_in_row > max_keys) max_keys = keys_in_row;
    }

    for (int r = 0; r < num_rows; r++) {
        int total_key_width = LV_HOR_RES - (padding * 2);
        int current_row_length = max_row_lengths[r];
        int key_width = total_key_width / current_row_length;
        
        const int *row_lens = is_symbols_mode ? symbols_row_lengths : row_lengths;
        int actual_len = row_lens[r];
        int special_count = 0;
        if (!is_symbols_mode) {
            for (int i = 0; i < actual_len; i++) {
                const char *txt = keys[r][i];
                if (strcmp(txt, "SHIFT") == 0 || strcmp(txt, "DEL") == 0 || strcmp(txt, " ") == 0) {
                    special_count++;
                }
            }
        }
        int extra_space = special_count * (key_width / 2);
        int total_keys_width = actual_len * key_width + extra_space;
        int blank_space = total_key_width - total_keys_width;
        int key_x = padding + blank_space / 2;
        
        for (int c = 0; c < current_row_length; c++) {
            int current_key_width = key_width;
            
            // adjust for wider SHIFT and DEL buttons
            if (!is_symbols_mode && c < row_lens[r]) {
                const char *txt = keys[r][c];
                if (strcmp(txt, "SHIFT") == 0 || strcmp(txt, "DEL") == 0 || strcmp(txt, " ") == 0) {
                    current_key_width += key_width / 2;
                }
            }
            
            int key_x, key_w;
            get_key_position(r, c, &key_x, &key_w, is_symbols_mode);
            lv_obj_t *key_btn = create_key_button(root, key_x, key_y, key_w - 2, key_height, "");
            if (key_btn) {
                lv_obj_t *key_label = lv_obj_get_child(key_btn, 0);
                const char *(*current_keys)[KEYBOARD_COLUMNS] = is_symbols_mode ? symbols : keys;
                if (c < row_lens[r]) {
                    const char* key_text = current_keys[r][c];
                    if (strcmp(key_text, "SHIFT") == 0) {
                        lv_label_set_text(key_label, LV_SYMBOL_UP);
                    } else if (!is_symbols_mode && strlen(key_text) == 1) {
                        char new_text[2];
                        if (isalpha((unsigned char)key_text[0])) {
                            new_text[0] = is_caps ? toupper(key_text[0]) : tolower(key_text[0]);
                        } else {
                            new_text[0] = key_text[0];
                        }
                        new_text[1] = '\0';
                        lv_label_set_text(key_label, new_text);
                    } else {
                        lv_label_set_text(key_label, key_text);
                    }
                } else {
                    lv_label_set_text(key_label, "");
                    lv_obj_add_flag(key_btn, LV_OBJ_FLAG_HIDDEN);
                }
            }
            key_x += current_key_width;
        }
        key_y += key_height + 2;
    }
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(root);
#endif
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (!keyboard_build_timer) {
        apply_selection_highlight();
    }
#endif
}

static void keyboard_build_step(lv_timer_t *t) {
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (!root || !lv_obj_is_valid(root)) { lv_timer_del(t); keyboard_build_timer = NULL; return; }

    int screen_height = LV_VER_RES;
    int status_bar_height = GUI_STATUS_BAR_HEIGHT;
    int display_height = 40;
    int padding = 5;
    int keys_start_y = status_bar_height + display_height + padding * 2;
    int keys_area_height = screen_height - keys_start_y;
    int key_height = (keys_area_height / num_rows) - 4;
    int built = 0;
    const int batch = 4;
    // timing for profiling heavy steps
    int64_t t0 = esp_timer_get_time();
    while (built < batch && keyboard_build_row < num_rows) {
        int r = keyboard_build_row;
        int key_y = keys_start_y + r * (key_height + 2);
        const char *(*current_keys)[10] = get_current_keys();
        const int *row_lens = get_current_row_lengths();
        int c = keyboard_build_col;
        int key_x, key_w;
        get_key_position(r, c, &key_x, &key_w, is_symbols_mode);
        lv_obj_t *key_btn = key_btns[r][c];
        if (key_btn && !lv_obj_is_valid(key_btn)) key_btn = NULL;
        if (key_btn && lv_obj_get_parent(key_btn) != root) key_btn = NULL;
        if (keyboard_build_phase == 0) {
            // phase 0: create buttons only (no labels yet)
            if (!key_btn) {
                key_btn = lv_obj_create(root);
                if (!key_btn) break;
                lv_obj_remove_style_all(key_btn);
                lv_obj_add_style(key_btn, &style_key_btn, 0);
                key_btns[r][c] = key_btn;
            }
            lv_obj_set_size(key_btn, key_w - 2, key_height);
            lv_obj_set_pos(key_btn, key_x, key_y);
            lv_obj_add_flag(key_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            // phase 1: attach/create labels and set text
            if (!key_btn) {
                // Should have been created in phase 0, but create defensively
                key_btn = lv_obj_create(root);
                if (!key_btn) break;
                lv_obj_remove_style_all(key_btn);
                lv_obj_add_style(key_btn, &style_key_btn, 0);
                lv_obj_set_size(key_btn, key_w - 2, key_height);
                lv_obj_set_pos(key_btn, key_x, key_y);
                lv_obj_add_flag(key_btn, LV_OBJ_FLAG_HIDDEN);
                key_btns[r][c] = key_btn;
            }
            if (!lv_obj_is_valid(key_btn) || lv_obj_get_parent(key_btn) != root) {
                // parent chain changed; skip this item safely
                built++;
                keyboard_build_col++;
                if (keyboard_build_col >= max_row_lengths[r]) { keyboard_build_col = 0; keyboard_build_row++; }
                continue;
            }
            lv_obj_t *key_label = lv_obj_get_child(key_btn, 0);
            if (!key_label) {
                key_label = lv_label_create(key_btn);
                if (key_label) {
                    lv_obj_add_style(key_label, &style_key_label, 0);
                    lv_obj_center(key_label);
                }
            }
            if (key_label) {
                if (c < row_lens[r]) {
                    const char* key_text = current_keys[r][c];
                    lv_label_set_text(key_label, get_key_label(key_text, is_caps, is_symbols_mode));
                    if (is_shift_key(key_text)) {
                        style_shift_key(key_btn, key_label, is_capslock, is_caps);
                    }
                } else {
                    lv_label_set_text(key_label, "");
                    lv_obj_add_flag(key_btn, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
        built++;
        keyboard_build_col++;
        if (keyboard_build_col >= max_row_lengths[r]) {
            keyboard_build_col = 0;
            // defer reveal until after build completes (root is hidden anyway)
            keyboard_build_row++;
        }
    }
    if (keyboard_build_row >= num_rows) {
        if (keyboard_build_phase == 0) {
            // move to phase 1: create labels
            keyboard_build_phase = 1;
            keyboard_build_row = 0;
            keyboard_build_col = 0;
        } else {
            // finished both phases
            lv_timer_del(t);
            keyboard_build_timer = NULL;
            cursor_row = 0;
            cursor_col = 0;
            apply_selection_highlight();

            // unhide root after build completes
            if (root) lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
            // restore radii after initial paint to re-enable rounded corners
            if (radius_override_active) {
                lv_style_set_radius(&style_key_btn, saved_key_radius);
                if (input_label) lv_obj_set_style_radius(input_label, saved_input_label_radius, 0);
                radius_override_active = false;
            }
            lv_obj_invalidate(root);
            int64_t dt = esp_timer_get_time() - t0;
            if (dt > 5000) {
                ESP_LOGW(TAG, "keyboard_build total took %lld us", dt);
            }
        }
    }
#else
    lv_timer_del(t);
    keyboard_build_timer = NULL;
#endif
}

static void destroy_key_buttons(void) {
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    for (int r = 0; r < num_rows; r++) {
        for (int c = 0; c < KEYBOARD_COLUMNS; c++) {
            lv_obj_t *btn = key_btns[r][c];
            lvgl_obj_del_safe(&btn);
            key_btns[r][c] = NULL;
        }
    }
    pressed_key_btn = NULL;
    selected_key_btn = NULL;
#endif
}

static void keyboard_create() {
    is_caps = start_with_caps;
    start_with_caps = true; /* reset so next open gets default (caps) */
    is_symbols_mode = false;
    input_len = 0;
    memset(input_buffer, 0, sizeof(input_buffer));
    if (has_pending_initial_text) {
        size_t n = strnlen(pending_initial_text, sizeof(pending_initial_text) - 1);
        if (n > 0) {
            memcpy(input_buffer, pending_initial_text, n + 1);
            input_len = (int)n;
        }
        has_pending_initial_text = false;
        memset(pending_initial_text, 0, sizeof(pending_initial_text));
    }

    int screen_height = LV_VER_RES;
    int status_bar_height = GUI_STATUS_BAR_HEIGHT;

    root = gui_screen_create_root(NULL, "Keyboard", lv_color_hex(0x121212), LV_OPA_COVER);
    keyboard_view.root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_HOR_RES, screen_height);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x121212), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    int padding = 5;
    int display_height = 40;
    input_label = lv_label_create(root);
    lv_obj_set_size(input_label, LV_HOR_RES - 2 * padding, display_height - 2 * padding);
    lv_obj_set_style_bg_color(input_label, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(input_label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(input_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_all(input_label, padding, 0);
    lv_obj_set_style_radius(input_label, 5, 0);
    lv_obj_set_pos(input_label, padding, status_bar_height + padding);
    lv_label_set_long_mode(input_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    update_input_label();

    // ensure styles are initialized so we can temporarily zero radius
    init_keyboard_styles();
    // hide root while building to avoid heavy invalidation churn
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    // zero out radii during build to bypass rounded-rect mask paths
    saved_key_radius = 3;
    saved_input_label_radius = 5;
    lv_style_set_radius(&style_key_btn, 0);
    lv_obj_set_style_radius(input_label, 0, 0);
    radius_override_active = true;

    // reset build state and key button cache
    for (int rr = 0; rr < num_rows; rr++) {
        for (int cc = 0; cc < KEYBOARD_COLUMNS; cc++) {
            key_btns[rr][cc] = NULL;
        }
    }
    keyboard_build_phase = 0;

#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    // use a single btnmatrix to render the keyboard for touch/joystick builds
    recreate_keyboard_buttons();
#elif defined(CONFIG_USE_ENCODER) && !defined(CONFIG_USE_JOYSTICK)
    encoder_cont = lv_obj_create(root);
    lv_obj_remove_style_all(encoder_cont);
    lv_obj_set_size(encoder_cont, LV_HOR_RES, display_height);
    lv_obj_set_pos(encoder_cont, 0, status_bar_height + display_height + padding);
    lv_obj_set_style_bg_opa(encoder_cont, LV_OPA_TRANSP, 0);
    // initialize encoder items and metrics
    encoder_items = encoder_alpha_items;
    encoder_item_count = encoder_alpha_count;
    encoder_sym_mode = false;
    encoder_sel_idx = 0;
    encoder_screen_width = LV_HOR_RES;
    encoder_item_spacing = display_height;
    encoder_offset_x = (encoder_screen_width / 2) - (encoder_item_spacing / 2);
    lv_obj_set_scroll_dir(encoder_cont, LV_DIR_LEFT | LV_DIR_RIGHT);
    lv_obj_set_scrollbar_mode(encoder_cont, LV_SCROLLBAR_MODE_OFF);
    // pad right to allow last items to center
    lv_obj_set_style_pad_right(encoder_cont, encoder_screen_width, 0);
    // create and position each item label (centered, avoid clipping)
    for(int i = 0; i < encoder_item_count; i++) {
        encoder_labels[i] = lv_label_create(encoder_cont);
        const char *txt = encoder_items[i];
        if(!encoder_sym_mode && strlen(txt)==1 && isalpha((unsigned char)txt[0])) {
            char tmp[2] = { encoder_uppercase ? toupper((unsigned char)txt[0]) : tolower((unsigned char)txt[0]), '\0' };
            lv_label_set_text(encoder_labels[i], tmp);
        } else {
            lv_label_set_text(encoder_labels[i], txt);
        }
        if(i == encoder_sel_idx) {
            lv_obj_set_style_text_color(encoder_labels[i], lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_font(encoder_labels[i], &lv_font_montserrat_24, 0);
        } else {
            lv_obj_set_style_text_color(encoder_labels[i], lv_color_hex(0x888888), 0);
            lv_obj_set_style_text_font(encoder_labels[i], &lv_font_montserrat_14, 0);
        }
        int lbl_w = lv_obj_get_width(encoder_labels[i]);
        int lbl_h = 24; // font height
        lv_obj_set_pos(encoder_labels[i],
            encoder_offset_x + i * encoder_item_spacing + (encoder_item_spacing - lbl_w) / 2,
            (display_height - lbl_h) / 2);
    }
    // ensure encoder builds unhide the root and restore radii
    if (radius_override_active) {
        lv_style_set_radius(&style_key_btn, saved_key_radius);
        if (input_label) lv_obj_set_style_radius(input_label, saved_input_label_radius, 0);
        radius_override_active = false;
    }
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
#else
    // non-touch, non-encoder devices (e.g., cardputer keyboard): unhide and restore radii
    if (radius_override_active) {
        lv_style_set_radius(&style_key_btn, saved_key_radius);
        if (input_label) lv_obj_set_style_radius(input_label, saved_input_label_radius, 0);
        radius_override_active = false;
    }
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
#endif
    
    display_manager_add_status_bar("Keyboard");

#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (!keyboard_build_timer) {
        update_key_labels();
        apply_selection_highlight();
    }
#else
    update_key_labels();
#endif
}

static void keyboard_destroy() {
    if (keyboard_view.root) {
        lvgl_timer_del_safe(&keyboard_build_timer);
        destroy_key_buttons();
        lvgl_obj_del_safe(&keyboard_view.root);
        root = NULL;
        key_matrix = NULL;
        shift_btn_id = -1;
        pressed_btn_id = -1;
        input_label = NULL;
        submit_callback = NULL;
        input_len = 0;
        input_buffer[0] = '\0';
        is_symbols_mode = false;
        is_caps = true;
        is_capslock = false;
#if defined(CONFIG_USE_ENCODER) && !defined(CONFIG_USE_JOYSTICK)
        encoder_cont = NULL;
        encoder_item_count = 0;
        encoder_screen_width = 0;
        encoder_item_spacing = 0;
        encoder_sym_mode = false;
#endif
        selected_key_btn = NULL;
        cursor_row = 0;
        cursor_col = 0;
        joy_focused_btn_id = -1;
        joy_holding_letter = false;
        joy_pending_letter  = false;
        joy_inverted_case   = false;
        if (joy_hold_timer) { lv_timer_del(joy_hold_timer); joy_hold_timer = NULL; }
    }
}

// Fires once after KEYBOARD_HOLD_INVERT_MS while select is still held on a letter.
// Flips the label to inverted case; release will then type the inverted character.
static void keyboard_hold_invert_cb(lv_timer_t *t) {
    (void)t;
    joy_hold_timer = NULL; // one-shot; LVGL auto-deletes after repeat_count reaches 0
    if (!joy_pending_letter || !key_matrix) return;
    joy_pending_letter = false;
    joy_holding_letter = true;
    build_key_matrix();
    // Re-apply focus highlight on the held key
    const int *lens = get_current_row_lengths();
    int id = 0;
    for (int r = 0; r < cursor_row; r++) id += lens[r];
    id += cursor_col;
    lv_btnmatrix_set_selected_btn(key_matrix, id);
    lv_btnmatrix_set_btn_ctrl(key_matrix, id, LV_BTNMATRIX_CTRL_CHECKED);
    joy_focused_btn_id = id;
}

static void handle_hardware_button_press_keyboard(InputEvent *event) {

#if defined(CONFIG_USE_ENCODER) && !defined(CONFIG_USE_JOYSTICK)
    if (event->type == INPUT_TYPE_ENCODER) {
        if (!encoder_cont) return;
        int dir = event->data.encoder.direction;
        int prev = encoder_sel_idx;
        encoder_sel_idx = (encoder_sel_idx + dir + encoder_item_count) % encoder_item_count;
        int scroll_x = encoder_sel_idx * encoder_item_spacing;
        lv_obj_scroll_to_x(encoder_cont, scroll_x, LV_ANIM_OFF);
        if (prev >= 0 && prev < encoder_item_count) {
            lv_obj_set_style_text_color(encoder_labels[prev], lv_color_hex(0x888888), 0);
            lv_obj_set_style_text_font(encoder_labels[prev], &lv_font_montserrat_14, 0);
        }
        lv_obj_set_style_text_color(encoder_labels[encoder_sel_idx], lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(encoder_labels[encoder_sel_idx], &lv_font_montserrat_24, 0);
        if (event->data.encoder.button) {
            const char *sel = encoder_items[encoder_sel_idx];
            if(strcmp(sel, "Aa") == 0) {
                // toggle case
                encoder_uppercase = !encoder_uppercase;
                // update labels
                for(int j = 0; j < encoder_item_count; j++) {
                    const char *t = encoder_items[j];
                    if(!encoder_sym_mode && strlen(t)==1 && isalpha((unsigned char)t[0])) {
                        char tmp2[2] = { encoder_uppercase ? toupper((unsigned char)t[0]) : tolower((unsigned char)t[0]), '\0' };
                        lv_label_set_text(encoder_labels[j], tmp2);
                    }
                }
                return;
            }
            if (!encoder_sym_mode && strcmp(sel, "SYM") == 0) {
                // switch to symbol mode
                for (int i = 0; i < encoder_item_count; i++) lv_obj_del(encoder_labels[i]);
                encoder_items = encoder_sym_items;
                encoder_item_count = encoder_sym_count;
                encoder_sym_mode = true;
                encoder_sel_idx = 0;
                // rebuild labels for symbol mode
                for (int i = 0; i < encoder_item_count; i++) {
                    encoder_labels[i] = lv_label_create(encoder_cont);
                    lv_label_set_text(encoder_labels[i], encoder_items[i]);
                    bool sel_i = (i == encoder_sel_idx);
                    lv_obj_set_style_text_color(encoder_labels[i], sel_i ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x888888), 0);
                    lv_obj_set_style_text_font(encoder_labels[i], sel_i ? &lv_font_montserrat_24 : &lv_font_montserrat_14, 0);
                    int lbl_w = lv_obj_get_width(encoder_labels[i]);
                    int enc_h = lv_obj_get_height(encoder_cont);
                    lv_obj_set_pos(encoder_labels[i], encoder_offset_x + i * encoder_item_spacing + (encoder_item_spacing - lbl_w) / 2, (enc_h - 24) / 2);
                }
            } else if (encoder_sym_mode && strcmp(sel, "ABC") == 0) {
                // switch back to alpha mode
                for (int i = 0; i < encoder_item_count; i++) lv_obj_del(encoder_labels[i]);
                encoder_items = encoder_alpha_items;
                encoder_item_count = encoder_alpha_count;
                encoder_sym_mode = false;
                encoder_sel_idx = 0;
                // rebuild labels for alpha mode
                for (int i = 0; i < encoder_item_count; i++) {
                    encoder_labels[i] = lv_label_create(encoder_cont);
                    lv_label_set_text(encoder_labels[i], encoder_items[i]);
                    bool sel_i = (i == encoder_sel_idx);
                    lv_obj_set_style_text_color(encoder_labels[i], sel_i ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x888888), 0);
                    lv_obj_set_style_text_font(encoder_labels[i], sel_i ? &lv_font_montserrat_24 : &lv_font_montserrat_14, 0);
                    int lbl_w = lv_obj_get_width(encoder_labels[i]);
                    int enc_h = lv_obj_get_height(encoder_cont);
                    lv_obj_set_pos(encoder_labels[i], encoder_offset_x + i * encoder_item_spacing + (encoder_item_spacing - lbl_w) / 2, (enc_h - 24) / 2);
                }
            } else if (strcmp(sel, "SPA") == 0) {
                add_char_to_buffer(' ');
            } else if (strcmp(sel, "<-") == 0) {
                remove_char_from_buffer();
            } else if (strcmp(sel, "ENT") == 0) {
                submit_text();
            } else {
                char c = sel[0];
                if (!encoder_sym_mode && isalpha((unsigned char)c)) {
                    c = encoder_uppercase ? (char)toupper((unsigned char)c) : (char)tolower((unsigned char)c);
                }
                // bypass global is_caps so encoder "Aa" accurately controls case
                add_char_to_buffer_raw(c);
            }
        }
        return;
    }
#endif
    if (event->type == INPUT_TYPE_JOYSTICK) {
        int button = event->data.joystick_index;
        bool pressed = event->data.joystick_pressed;
        const int *row_lens = get_current_row_lengths();

        // Navigation buttons (only on press, not release)
        if (pressed) {
            if (button == 0) { // left
                if (cursor_col > 0) cursor_col--; else cursor_col = row_lens[cursor_row] - 1;
            } else if (button == 3) { // right
                if (cursor_col < row_lens[cursor_row] - 1) cursor_col++; else cursor_col = 0;
            } else if (button == 2) { // up
                cursor_row = (cursor_row > 0) ? cursor_row - 1 : num_rows - 1;
                if (cursor_col >= row_lens[cursor_row]) cursor_col = row_lens[cursor_row] - 1;
            } else if (button == 4) { // down
                cursor_row = (cursor_row < num_rows - 1) ? cursor_row + 1 : 0;
                if (cursor_col >= row_lens[cursor_row]) cursor_col = row_lens[cursor_row] - 1;
            }
        }

        ensure_valid_cursor();

#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
        if (key_matrix) {
            // Map (cursor_row, cursor_col) to btnmatrix button index
            const int *lens = get_current_row_lengths();
            int id = 0;
            for (int r = 0; r < cursor_row; r++) {
                id += lens[r];
            }
            id += cursor_col;
            
            // Handle button 1 (select) with hold-to-invert for letters
            if (button == 1) {
                const char *txt = lv_btnmatrix_get_btn_text(key_matrix, id);
                bool is_letter = txt && strlen(txt) == 1 && isalpha((unsigned char)txt[0]);
                
                if (pressed) {
                    // On press: update focus highlight
                    lv_btnmatrix_set_selected_btn(key_matrix, id);
                    if (joy_focused_btn_id >= 0 && joy_focused_btn_id != id) {
                        bool shift_caps_active = (joy_focused_btn_id == shift_btn_id) && (is_caps || is_capslock);
                        if (!shift_caps_active) {
                            lv_btnmatrix_clear_btn_ctrl(key_matrix, joy_focused_btn_id, LV_BTNMATRIX_CTRL_CHECKED);
                        }
                    }
                    joy_focused_btn_id = id;
                    lv_btnmatrix_set_btn_ctrl(key_matrix, id, LV_BTNMATRIX_CTRL_CHECKED);

                    // Cancel any leftover hold timer from a previous press
                    if (joy_hold_timer) { lv_timer_del(joy_hold_timer); joy_hold_timer = NULL; }
                    joy_holding_letter = false;
                    joy_pending_letter = false;
                    joy_inverted_case  = false;

                    if (is_letter) {
                        // Start hold timer — inversion only activates after KEYBOARD_HOLD_INVERT_MS
                        joy_pending_letter = true;
                        joy_inverted_case  = !(is_capslock || is_caps);
                        joy_hold_timer = lv_timer_create(keyboard_hold_invert_cb, KEYBOARD_HOLD_INVERT_MS, NULL);
                        lv_timer_set_repeat_count(joy_hold_timer, 1);
                    } else {
                        // Non-letter key: trigger immediately on press
                        lv_event_send(key_matrix, LV_EVENT_VALUE_CHANGED, NULL);
                    }
                } else {
                    // On release: cancel timer if hold threshold wasn't reached yet
                    if (joy_hold_timer) { lv_timer_del(joy_hold_timer); joy_hold_timer = NULL; }

                    if (is_letter && (joy_holding_letter || joy_pending_letter)) {
                        // Capture the char before any rebuild that might invalidate txt
                        char typed = txt[0]; // btnmatrix label already reflects current/inverted case
                        bool was_holding = joy_holding_letter;
                        joy_holding_letter = false;
                        joy_pending_letter  = false;
                        joy_inverted_case   = false;
                        add_char_to_buffer_raw(typed);
                        bool needs_rebuild = was_holding; // always redraw after hold to restore labels
                        if (!was_holding && is_caps && !is_capslock) {
                            // Short press with one-shot shift: reset shift as normal
                            is_caps = false;
                            needs_rebuild = true;
                        }
                        if (needs_rebuild) build_key_matrix();
                    } else {
                        joy_holding_letter = false;
                        joy_pending_letter  = false;
                        joy_inverted_case   = false;
                    }
                }
                return;
            }
            
            lv_btnmatrix_set_selected_btn(key_matrix, id);
            // clear previous joystick highlight if any
            if (joy_focused_btn_id >= 0 && joy_focused_btn_id != id) {
                // don't clear SHIFT's CHECKED state when it's indicating caps/capslock
                bool shift_caps_active = (joy_focused_btn_id == shift_btn_id) && (is_caps || is_capslock);
                if (!shift_caps_active) {
                    lv_btnmatrix_clear_btn_ctrl(key_matrix, joy_focused_btn_id, LV_BTNMATRIX_CTRL_CHECKED);
                }
            }
            joy_focused_btn_id = id;
            // mark current key as CHECKED so it uses inverted colors
            lv_btnmatrix_set_btn_ctrl(key_matrix, id, LV_BTNMATRIX_CTRL_CHECKED);
            return;
        }
#endif

        // Fallback for non-btnmatrix (legacy per-key buttons)
        if (button == 1) {
            if (pressed) {
                // Cancel any leftover hold timer
                if (joy_hold_timer) { lv_timer_del(joy_hold_timer); joy_hold_timer = NULL; }
                joy_holding_letter = false;
                joy_pending_letter  = false;
                joy_inverted_case   = false;
                const char *(*current_keys)[10] = get_current_keys();
                if (cursor_row >= 0 && cursor_row < num_rows && cursor_col >= 0 && cursor_col < row_lens[cursor_row]) {
                    const char *key = current_keys[cursor_row][cursor_col];
                    if (strlen(key) == 1 && isalpha((unsigned char)key[0])) {
                        // Letter key: start hold timer
                        joy_pending_letter = true;
                        joy_inverted_case  = !(is_capslock || is_caps);
                        joy_hold_timer = lv_timer_create(keyboard_hold_invert_cb, KEYBOARD_HOLD_INVERT_MS, NULL);
                        lv_timer_set_repeat_count(joy_hold_timer, 1);
                        apply_selection_highlight();
                    } else {
                        // Non-letter: activate immediately
                        activate_selected_key();
                    }
                }
            } else {
                // Release: cancel timer if not yet fired
                if (joy_hold_timer) { lv_timer_del(joy_hold_timer); joy_hold_timer = NULL; }
                if (joy_holding_letter || joy_pending_letter) {
                    const char *(*current_keys)[10] = get_current_keys();
                    const char *key = current_keys[cursor_row][cursor_col];
                    char c = key[0];
                    bool was_holding = joy_holding_letter;
                    if (was_holding) {
                        c = joy_inverted_case ? (char)toupper((unsigned char)c) : (char)tolower((unsigned char)c);
                    } else {
                        c = is_caps ? (char)toupper((unsigned char)c) : (char)tolower((unsigned char)c);
                    }
                    joy_holding_letter = false;
                    joy_pending_letter  = false;
                    joy_inverted_case   = false;
                    add_char_to_buffer_raw(c);
                    bool needs_rebuild = was_holding; // restore inverted label after hold
                    if (!was_holding && is_caps && !is_capslock) {
                        is_caps = false;
                        needs_rebuild = true;
                    }
                    if (needs_rebuild) update_key_labels();
                } else {
                    joy_holding_letter = false;
                    joy_pending_letter  = false;
                    joy_inverted_case   = false;
                }
                apply_selection_highlight();
            }
            return;
        }

    } else if (event->type == INPUT_TYPE_TOUCH && event->data.touch_data.state == LV_INDEV_STATE_PR) {
        int touch_x = event->data.touch_data.point.x;
        int touch_y = event->data.touch_data.point.y;
        ESP_LOGD(TAG, "touch PR x=%d y=%d", touch_x, touch_y);
        
        int screen_width = LV_HOR_RES;
        int screen_height = LV_VER_RES;
        int status_bar_height = 20;
        int display_height = 40;
        int padding = 5;
        int keys_start_y = status_bar_height + display_height + padding * 2;

        int row = -1;
        int km_x = 0, km_y = 0, km_w = 0, km_h = 0;
        if (key_matrix) {
            km_x = lv_obj_get_x(key_matrix);
            km_y = lv_obj_get_y(key_matrix);
            km_w = lv_obj_get_width(key_matrix);
            km_h = lv_obj_get_height(key_matrix);
            if (touch_y < km_y || touch_y >= km_y + km_h || touch_x < km_x || touch_x >= km_x + km_w) return;
            int row_h = km_h / num_rows;
            if (row_h <= 0) row_h = 1;
            row = (touch_y - km_y) / row_h;
            if (row < 0) row = 0;
            if (row >= num_rows) row = num_rows - 1;
        } else {
            if (touch_y < keys_start_y) return;
            int keys_area_height = screen_height - keys_start_y - padding;
            int key_height = keys_area_height / num_rows; if (key_height <= 0) key_height = 1;
            row = (touch_y - keys_start_y) / key_height;
        }

        if (row >= 0 && row < num_rows) {
            const char *(*current_keys)[10] = is_symbols_mode ? symbols : keys;
            const int *current_row_lengths = is_symbols_mode ? symbols_row_lengths : row_lengths;
            int create_row_length = max_row_lengths[row];
            int base_key_width = screen_width / create_row_length;
            
            // calculate which column was touched considering variable widths
            int col = -1;
            if (key_matrix) {
                // match btnmatrix width logic: width units per button (SHIFT/DEL/space -> 2 units)
                int total_key_width = km_w;
                int units_sum = 0;
                for (int c = 0; c < current_row_lengths[row]; c++) {
                    const char *src = current_keys[row][c];
                    int w = 1;
                    if (!is_symbols_mode && (strcmp(src, "SHIFT") == 0 || strcmp(src, "DEL") == 0 || strcmp(src, " ") == 0)) w = 2;
                    units_sum += w;
                }
                if (units_sum < 1) units_sum = 1;
                int unit_w = total_key_width / units_sum;
                int x0 = km_x;
                int x = x0;
                for (int c = 0; c < current_row_lengths[row]; c++) {
                    const char *src = current_keys[row][c];
                    int w = 1;
                    if (!is_symbols_mode && (strcmp(src, "SHIFT") == 0 || strcmp(src, "DEL") == 0 || strcmp(src, " ") == 0)) w = 2;
                    int key_w = w * unit_w;
                    int key_x = x;
                    if (touch_x >= key_x && touch_x < key_x + key_w) { col = c; break; }
                    x += key_w;
                }
            } else {
                for (int c = 0; c < current_row_lengths[row]; c++) {
                    int key_x, key_w;
                    get_key_position(row, c, &key_x, &key_w, is_symbols_mode);
                    if (touch_x >= key_x && touch_x < key_x + key_w) {
                        col = c;
                        break;
                    }
                }
            }

            ESP_LOGD(TAG, "touch row=%d col=%d (sym=%d caps=%d capslock=%d)", row, col, (int)is_symbols_mode, (int)is_caps, (int)is_capslock);
            if (col >= 0) {
                if (!key_matrix) {
                    // Find the key button object (legacy per-key objects)
                    int key_index = 0;
                    for (int rr = 0; rr < num_rows; rr++) {
                        for (int cc = 0; cc < max_row_lengths[rr]; cc++) {
                            if (rr == row && cc == col) {
                                int child_idx = 1 + key_index;
                                pressed_key_btn = lv_obj_get_child(root, child_idx);
                                if (pressed_key_btn) {
                                    lv_obj_set_style_bg_color(pressed_key_btn, lv_color_hex(0xFF9800), 0); // Orange highlight
                                }
                            }
                            key_index++;
                        }
                    }
                }
                const char* key = current_keys[row][col];
                if (strcmp(key, "SHIFT") == 0) {
                    if (is_caps) {
                        // If SHIFT is already active, toggle capslock
                        is_capslock = !is_capslock;
                        is_caps = is_capslock; // Keep caps active if capslock is on
                    } else {
                        is_caps = true;
                    }
                    update_key_labels();
                    ESP_LOGD(TAG, "shift toggled: caps=%d capslock=%d", (int)is_caps, (int)is_capslock);
                } else if (strcmp(key, "SYM") == 0) {
                    /* Switching to symbols mode rebuilds the keyboard and deletes existing key buttons.
                     * Clear any stored pointer to the previously pressed key to avoid accessing
                     * a freed object in the subsequent LV_INDEV_STATE_REL event. */
                    pressed_key_btn = NULL;
                    is_symbols_mode = true;
                    recreate_keyboard_buttons();
                } else if (strcmp(key, "ABC") == 0) {
                    /* Same safety measure when switching back to alphabet mode. */
                    pressed_key_btn = NULL;
                    is_symbols_mode = false;
                    recreate_keyboard_buttons();
                } else if (strcmp(key, "Exit") == 0) {
                    if (keyboard_return_view) {
                        display_manager_switch_view(keyboard_return_view);
                    } else {
                        display_manager_switch_view(&options_menu_view); // fallback
                    }
                } else if (strcmp(key, "Done") == 0) {
                    submit_text();
                } else if (strcmp(key, "DEL") == 0) {
                    remove_char_from_buffer();
                } else if (strcmp(key, " ") == 0) {
                    add_char_to_buffer(' ');
                } else if (strlen(key) == 1) {
                    char adjusted_char = key[0];
                    if (!is_symbols_mode && strlen(key) == 1 && isalpha(adjusted_char)) {
                        adjusted_char = is_caps ? toupper(adjusted_char) : tolower(adjusted_char);
                    }
                    add_char_to_buffer(adjusted_char);
                    ESP_LOGD(TAG, "char added: %c", adjusted_char);
                }
            }
        }
    } else if (event->type == INPUT_TYPE_TOUCH && event->data.touch_data.state == LV_INDEV_STATE_REL) {
        if (pressed_key_btn) {
            // Only restore style if not SHIFT key, otherwise let update_key_labels() handle it
            lv_obj_t *key_label = lv_obj_get_child(pressed_key_btn, 0);
            const char *label_text = lv_label_get_text(key_label);
            if (strcmp(label_text, LV_SYMBOL_UP) != 0) {
                lv_obj_set_style_bg_color(pressed_key_btn, lv_color_hex(0xFFFFFF), 0);
            }
            pressed_key_btn = NULL;
            // Always update key labels to refresh SHIFT key highlight
            update_key_labels();
            apply_selection_highlight();
        }
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        char c = (char)event->data.key_value;
        if (c == '`') {
            display_manager_switch_view(&options_menu_view);
        } else if (c == '\n' || c == '\r' || c == '=') {
            submit_text();
        } else if (c == '\b') {
            remove_char_from_buffer();
        } else if (c >= ' ' && c <= '~') {
            add_char_to_buffer_raw(c);
        }
#ifdef CONFIG_USE_ENCODER
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        ESP_LOGI(TAG, "IO6 exit button pressed, returning to main menu");
        display_manager_switch_view(&main_menu_view);
#endif
    }
}

static void get_keyboard_callback(void **callback) {
    *callback = keyboard_view.input_callback;
}

void keyboard_view_set_submit_callback(KeyboardSubmitCallback cb){
    submit_callback = cb;
}

void keyboard_view_set_placeholder(const char *text){
    if (text && strlen(text) < sizeof(placeholder)) {
        strncpy(placeholder, text, sizeof(placeholder) - 1);
        placeholder[sizeof(placeholder) - 1] = '\0';
    }
    memset(input_buffer, 0, sizeof(input_buffer));
    input_len = 0;
    update_input_label();
}

void keyboard_view_set_start_caps(bool start_caps) {
    start_with_caps = start_caps;
}

void keyboard_view_set_initial_text(const char *text) {
    memset(pending_initial_text, 0, sizeof(pending_initial_text));
    has_pending_initial_text = true;
    if (text && text[0] != '\0') {
        size_t n = strlen(text);
        if (n >= sizeof(pending_initial_text)) n = sizeof(pending_initial_text) - 1;
        memcpy(pending_initial_text, text, n + 1);
    }
    /* If view already exists, apply now so it shows without waiting for next create */
    if (root != NULL) {
        memset(input_buffer, 0, sizeof(input_buffer));
        input_len = 0;
        if (text && text[0] != '\0') {
            size_t n = strlen(text);
            if (n >= sizeof(input_buffer)) n = sizeof(input_buffer) - 1;
            memcpy(input_buffer, text, n + 1);
            input_buffer[n] = '\0';
            input_len = (int)n;
        }
        has_pending_initial_text = false;
        update_input_label();
    }
}

void keyboard_view_set_return_view(View *view) {
    keyboard_return_view = view;
}

View keyboard_view = {
    .root = NULL,
    .create = keyboard_create,
    .destroy = keyboard_destroy,
    .input_callback = handle_hardware_button_press_keyboard,
    .name = "Keyboard Screen",
    .get_hardwareinput_callback = get_keyboard_callback
};

static void key_matrix_event_cb(lv_event_t *e) {
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    lv_obj_t *m = lv_event_get_target(e);
    if (!m) return;
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    int id = lv_btnmatrix_get_selected_btn(m);
    if (id < 0) return;
    const char *txt = lv_btnmatrix_get_btn_text(m, id);
    if (!txt) return;
    if (strcmp(txt, LV_SYMBOL_UP) == 0 || strcmp(txt, "SHIFT") == 0) {
        if (is_caps) {
            is_capslock = !is_capslock;
            is_caps = is_capslock;
        } else {
            is_caps = true;
        }
        build_key_matrix();
    } else if (strcmp(txt, "SYM") == 0) {
        is_symbols_mode = true;
        build_key_matrix();
    } else if (strcmp(txt, "ABC") == 0) {
        is_symbols_mode = false;
        build_key_matrix();
    } else if (strcmp(txt, "Exit") == 0) {
        if (keyboard_return_view) display_manager_switch_view(keyboard_return_view);
        else display_manager_switch_view(&options_menu_view);
    } else if (strcmp(txt, "Done") == 0) {
        submit_text();
    } else if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0 || strcmp(txt, "DEL") == 0) {
        remove_char_from_buffer();
    } else if (strcmp(txt, " ") == 0) {
        add_char_to_buffer(' ');
    } else if (strlen(txt) == 1) {
        char c = txt[0];
        if (!is_symbols_mode && isalpha((unsigned char)c)) {
            c = (is_capslock || is_caps) ? (char)toupper((unsigned char)c) : (char)tolower((unsigned char)c);
        }
        add_char_to_buffer_raw(c);
        // Only reset temporary SHIFT/caps and rebuild when in alpha mode; symbols shouldn't rebuild
        if (!is_symbols_mode && is_caps && !is_capslock) {
            is_caps = false;
            build_key_matrix();
        }
    }
#else
    (void)e;
#endif
}

static void build_key_matrix(void) {
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (!root) return;
    int screen_width = LV_HOR_RES;
    int screen_height = LV_VER_RES;
    int status_bar_height = 20;
    int padding = 5;
    int display_height = 40;
    int keys_start_y = status_bar_height + display_height + padding * 2;
    int keys_area_height = screen_height - keys_start_y - padding;

    const char *(*current_keys)[10] = get_current_keys();
    const int *row_lens = get_current_row_lengths();

    static char map_text_storage[64][8];
    int map_idx = 0;
    int btn_idx = 0;
    shift_btn_id = -1;
    // Calculate focused button index from cursor position
    int focused_btn_idx = 0;
    if (joy_holding_letter) {
        for (int r = 0; r < cursor_row && r < num_rows; r++) {
            focused_btn_idx += row_lens[r];
        }
        focused_btn_idx += cursor_col;
    } else {
        focused_btn_idx = -1; // No focus when not holding
    }
    for (int r = 0; r < num_rows; r++) {
        for (int c = 0; c < row_lens[r]; c++) {
            const char *src = current_keys[r][c];
            const char *label = src;
            if (is_shift_key(src)) label = LV_SYMBOL_UP;
            else if (is_del_key(src)) label = LV_SYMBOL_BACKSPACE;
            else if (!is_symbols_mode && is_alpha_key(src)) {
                char ch;
                // Use inverted case for the focused button when holding
                if (joy_holding_letter && btn_idx == focused_btn_idx) {
                    ch = joy_inverted_case ? (char)toupper((unsigned char)src[0]) : (char)tolower((unsigned char)src[0]);
                } else {
                    ch = is_caps ? (char)toupper((unsigned char)src[0]) : (char)tolower((unsigned char)src[0]);
                }
                map_text_storage[btn_idx][0] = ch;
                map_text_storage[btn_idx][1] = '\0';
                label = map_text_storage[btn_idx];
            }
            if (label == src) {
                size_t n = strlen(label);
                if (n > sizeof(map_text_storage[0]) - 1) n = sizeof(map_text_storage[0]) - 1;
                memcpy(map_text_storage[btn_idx], label, n);
                map_text_storage[btn_idx][n] = '\0';
                label = map_text_storage[btn_idx];
            }
            btn_map[map_idx++] = label;
            if (is_shift_key(src)) shift_btn_id = btn_idx;
            btn_idx++;
        }
        if (r < num_rows - 1) {
            btn_map[map_idx++] = "\n";
        }
    }
    btn_map[map_idx] = "";
    btn_map_len = map_idx;

    if (!key_matrix) {
        key_matrix = lv_btnmatrix_create(root);
        lv_obj_remove_style_all(key_matrix);
        lv_obj_set_pos(key_matrix, padding, keys_start_y);
        lv_obj_set_size(key_matrix, screen_width - 2 * padding, keys_area_height);
        lv_obj_set_style_bg_opa(key_matrix, LV_OPA_TRANSP, 0);
        lv_obj_add_style(key_matrix, &style_key_btn, LV_PART_ITEMS);
        lv_obj_add_style(key_matrix, &style_key_label, LV_PART_ITEMS);
        lv_obj_add_event_cb(key_matrix, key_matrix_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    } else {
        lv_obj_set_pos(key_matrix, padding, keys_start_y);
        lv_obj_set_size(key_matrix, screen_width - 2 * padding, keys_area_height);
    }

    lv_btnmatrix_set_map(key_matrix, btn_map);

    int id = 0;
    for (int r = 0; r < num_rows; r++) {
        for (int c = 0; c < row_lens[r]; c++) {
            const char *src = current_keys[r][c];
            int w = 1;
            if (!is_symbols_mode) {
                if (strcmp(src, "SHIFT") == 0 || strcmp(src, "DEL") == 0 || strcmp(src, " ") == 0) w = 2;
            }
            lv_btnmatrix_set_btn_width(key_matrix, id, w);
            // clear any stale CHECKED state from previous layouts before marking as checkable
            lv_btnmatrix_clear_btn_ctrl(key_matrix, id, LV_BTNMATRIX_CTRL_CHECKED);
            lv_btnmatrix_set_btn_ctrl(key_matrix, id, LV_BTNMATRIX_CTRL_CHECKABLE);
            id++;
        }
    }

    if (shift_btn_id >= 0) {
        // SHIFT is click-triggered and also reflects caps/capslock state via CHECKED (inverted colors)
        lv_btnmatrix_set_btn_ctrl(key_matrix, shift_btn_id,
                                  LV_BTNMATRIX_CTRL_CHECKABLE | LV_BTNMATRIX_CTRL_CLICK_TRIG);
        if (is_caps || is_capslock) {
            lv_btnmatrix_set_btn_ctrl(key_matrix, shift_btn_id, LV_BTNMATRIX_CTRL_CHECKED);
        }
    }

    lv_obj_set_style_bg_color(key_matrix, lv_color_hex(0xFFFFFF), LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(key_matrix, lv_color_hex(0x000000), LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(key_matrix, lv_color_hex(0x000000), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(key_matrix, lv_color_hex(0xFFFFFF), LV_PART_ITEMS | LV_STATE_CHECKED);

    // any rebuild invalidates previous focus id; it will be re-established on next joystick move
    joy_focused_btn_id = -1;

    if (radius_override_active) {
        lv_style_set_radius(&style_key_btn, saved_key_radius);
        if (input_label) lv_obj_set_style_radius(input_label, saved_input_label_radius, 0);
        radius_override_active = false;
    }
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
#endif
}

static void get_key_position(int row, int col, int *x, int *width, bool symbols_mode) {
    int screen_width = LV_HOR_RES;
    int padding = 5;
    const int *row_lens = symbols_mode ? symbols_row_lengths : row_lengths;
    int actual_len = row_lens[row];
    int special_count = 0;
    if (!symbols_mode) {
        for (int i = 0; i < actual_len; i++) {
            const char *txt = keys[row][i];
            if (strcmp(txt, "SHIFT") == 0 || strcmp(txt, "DEL") == 0 || strcmp(txt, " ") == 0) {
                special_count++;
            }
        }
    }
    int total_key_width = screen_width - (padding * 2);
    int current_row_length = max_row_lengths[row];
    int key_width = total_key_width / current_row_length;
    int extra_space = special_count * (key_width / 2);
    int total_keys_width = actual_len * key_width + extra_space;
    int blank_space = total_key_width - total_keys_width;
    int key_x = padding + blank_space / 2;

    // Calculate position for each key
    for (int c = 0; c < col; c++) {
        int current_key_width = key_width;
        if (!symbols_mode && c < row_lens[row]) {
            const char *txt = keys[row][c];
            if (strcmp(txt, "SHIFT") == 0 || strcmp(txt, "DEL") == 0 || strcmp(txt, " ") == 0) {
                current_key_width += key_width / 2;
            }
        }
        key_x += current_key_width;
    }
    int current_key_width = key_width;
    if (!symbols_mode && col < row_lens[row]) {
        const char *txt = keys[row][col];
        if (strcmp(txt, "SHIFT") == 0 || strcmp(txt, "DEL") == 0 || strcmp(txt, " ") == 0) {
            current_key_width += key_width / 2;
        }
    }
    *x = key_x;
    *width = current_key_width;
}

static void ensure_valid_cursor(void) {
    const int *row_lens = get_current_row_lengths();
    if (cursor_row < 0) cursor_row = 0;
    if (cursor_row >= num_rows) cursor_row = num_rows - 1;
    int max_col = row_lens[cursor_row] - 1;
    if (max_col < 0) max_col = 0;
    if (cursor_col < 0) cursor_col = 0;
    if (cursor_col > max_col) cursor_col = max_col;
}

static lv_obj_t *get_key_button_at(int row, int col) {
    if (!root) return NULL;
    int key_index = 0;
    for (int rr = 0; rr < row; rr++) key_index += max_row_lengths[rr];
    key_index += col;
    uint32_t child_count = lv_obj_get_child_cnt(root);
    int child_idx = 1 + key_index; // skip input_label at 0
    if (child_idx >= 0 && (uint32_t)child_idx < child_count) {
        return lv_obj_get_child(root, child_idx);
    }
    return NULL;
}

static void apply_selection_highlight(void) {
    if (!root) return;
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (key_matrix) return; // btnmatrix handles its own visuals on touch/joystick builds
#endif
    ensure_valid_cursor();
    // reset all key borders to default
    int key_index = 0;
    uint32_t child_count = lv_obj_get_child_cnt(root);
    for (int r = 0; r < num_rows; r++) {
        for (int c = 0; c < max_row_lengths[r]; c++) {
            int child_idx = 1 + key_index;
            if ((uint32_t)child_idx < child_count) {
                lv_obj_t *btn = lv_obj_get_child(root, child_idx);
                if (btn) {
                    lv_obj_set_style_border_color(btn, lv_color_hex(0x666666), 0);
                    lv_obj_set_style_border_width(btn, 1, 0);
                }
            }
            key_index++;
        }
    }
    // highlight current cursor key
    lv_obj_t *btn = get_key_button_at(cursor_row, cursor_col);
    if (btn) {
        lv_obj_set_style_border_color(btn, lv_color_hex(0x00BFFF), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        selected_key_btn = btn;
    } else {
        selected_key_btn = NULL;
    }
}

static void activate_selected_key(void) {
    const char *(*current_keys)[10] = get_current_keys();
    const int *current_row_lengths = get_current_row_lengths();
    if (cursor_row < 0 || cursor_row >= num_rows) return;
    if (cursor_col < 0 || cursor_col >= current_row_lengths[cursor_row]) return;
    const char *key = current_keys[cursor_row][cursor_col];
    if (strcmp(key, "SHIFT") == 0) {
        if (is_caps) {
            is_capslock = !is_capslock;
            is_caps = is_capslock;
        } else {
            is_caps = true;
        }
        update_key_labels();
        apply_selection_highlight();
    } else if (strcmp(key, "SYM") == 0) {
        pressed_key_btn = NULL;
        is_symbols_mode = true;
        recreate_keyboard_buttons();
        ensure_valid_cursor();
        apply_selection_highlight();
    } else if (strcmp(key, "ABC") == 0) {
        pressed_key_btn = NULL;
        is_symbols_mode = false;
        recreate_keyboard_buttons();
        ensure_valid_cursor();
        apply_selection_highlight();
    } else if (strcmp(key, "Exit") == 0) {
        if (keyboard_return_view) {
            display_manager_switch_view(keyboard_return_view);
        } else {
            display_manager_switch_view(&options_menu_view);
        }
    } else if (strcmp(key, "Done") == 0) {
        submit_text();
    } else if (strcmp(key, "DEL") == 0) {
        remove_char_from_buffer();
        apply_selection_highlight();
    } else if (strcmp(key, " ") == 0) {
        add_char_to_buffer(' ');
        apply_selection_highlight();
    } else if (strlen(key) == 1) {
        char adjusted_char = key[0];
        if (!is_symbols_mode && isalpha((unsigned char)adjusted_char)) {
            adjusted_char = is_caps ? (char)toupper((unsigned char)adjusted_char) : (char)tolower((unsigned char)adjusted_char);
        }
        add_char_to_buffer(adjusted_char);
        apply_selection_highlight();
    }
}
