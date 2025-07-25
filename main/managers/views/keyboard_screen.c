#include "managers/views/keyboard_screen.h"
#include "core/serial_manager.h"
#include "managers/views/options_screen.h"
#include "managers/views/terminal_screen.h"
#include "managers/views/main_menu_screen.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>
#include <ctype.h>

static const char *TAG = "keyboard_screen";

static lv_obj_t *root = NULL;
static lv_obj_t *input_label = NULL;
static char input_buffer[128] = {0};
static int input_len = 0;
static KeyboardSubmitCallback submit_callback = NULL;

static bool is_caps = true;
static bool is_symbols_mode = false;
#ifdef CONFIG_USE_ENCODER
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

static void submit_text() {
    if (input_len > 0) {
        if (submit_callback) {
            submit_callback(input_buffer);
        } else {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);            vTaskDelay(pdMS_TO_TICKS(10));
            simulateCommand(input_buffer);
        }
        memset(input_buffer, 0, sizeof(input_buffer));
        input_len = 0;
        update_input_label();
    }
}

static void add_char_to_buffer(char c) {
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

static char placeholder[64] = "Enter text...";


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
#ifdef CONFIG_USE_TOUCHSCREEN
    if (!root) return;
    
    int key_index = 0;
    uint32_t child_count = lv_obj_get_child_cnt(root);
    const char *(*current_keys)[10] = is_symbols_mode ? symbols : keys;
    const int *current_row_lengths = is_symbols_mode ? symbols_row_lengths : row_lengths;
    
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
                            if (!is_symbols_mode && strlen(key_text) == 1) {
                                char new_text[2];
                                new_text[0] = is_caps ? toupper(key_text[0]) : tolower(key_text[0]);
                                new_text[1] = '\0';
                                lv_label_set_text(key_label, new_text);
                            } else {
                                lv_label_set_text(key_label, key_text);
                            }
                            lv_obj_clear_flag(key_btn, LV_OBJ_FLAG_HIDDEN);
                        } else {
                            lv_label_set_text(key_label, "");
                            lv_obj_add_flag(key_btn, LV_OBJ_FLAG_HIDDEN);
                        }
                        lv_obj_set_style_text_color(key_label, lv_color_hex(0xFFFFFF), 0);
                    }
                }
            }
            key_index++;
        }
    }
#endif
}

static void recreate_keyboard_buttons() {
#ifdef CONFIG_USE_TOUCHSCREEN
    if (!root) return;
    
    // remove all existing key buttons (skip input_label at index 0)
    uint32_t child_count = lv_obj_get_child_cnt(root);
    for (int i = child_count - 1; i >= 1; i--) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        if (child && child != input_label) {
            lv_obj_del(child);
        }
    }
    
    // recreate buttons with current mode sizing
    int screen_width = LV_HOR_RES;
    int screen_height = LV_VER_RES;
    int status_bar_height = 20;
    int display_height = 40;
    int padding = 5;
    int keys_start_y = status_bar_height + display_height + padding * 2;
    int keys_area_height = screen_height - keys_start_y;
    int key_height = (keys_area_height / num_rows) - 4;
    int key_y = keys_start_y;

    for (int r = 0; r < num_rows; r++) {
        int total_key_width = screen_width - (padding * 2);
        int current_row_length = max_row_lengths[r];
        int key_width = total_key_width / current_row_length;
        const char *(*current_keys)[10] = is_symbols_mode ? symbols : keys;
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
            
            lv_obj_t *key_btn = lv_btn_create(root);
            lv_obj_remove_style_all(key_btn);
            lv_obj_set_size(key_btn, current_key_width - 2, key_height);
            lv_obj_set_pos(key_btn, key_x, key_y);
            
            lv_obj_set_style_bg_color(key_btn, lv_color_hex(0x7B1FA2), 0);
            lv_obj_set_style_bg_opa(key_btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(key_btn, lv_color_hex(0x444444), 0);
            lv_obj_set_style_border_width(key_btn, 1, 0);
            lv_obj_set_style_radius(key_btn, 3, 0);

            lv_obj_t *key_label = lv_label_create(key_btn);
            if (c < row_lens[r]) {
                const char* key_text = current_keys[r][c];
                if (!is_symbols_mode && strlen(key_text) == 1) {
                    char new_text[2];
                    new_text[0] = is_caps ? toupper(key_text[0]) : tolower(key_text[0]);
                    new_text[1] = '\0';
                    lv_label_set_text(key_label, new_text);
                } else {
                    lv_label_set_text(key_label, key_text);
                }
            } else {
                lv_label_set_text(key_label, "");
                lv_obj_add_flag(key_btn, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_set_style_text_color(key_label, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_font(key_label, &lv_font_montserrat_14, 0);
            lv_obj_center(key_label);
            
            key_x += current_key_width;
        }
        key_y += key_height + 2;
    }
#endif
}

static void keyboard_create() {
    is_caps = true;
    is_symbols_mode = false;
    input_len = 0;
    memset(input_buffer, 0, sizeof(input_buffer));

    int screen_width = LV_HOR_RES;
    int screen_height = LV_VER_RES;
    int status_bar_height = 20;

    root = lv_obj_create(lv_scr_act());
    keyboard_view.root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, screen_width, screen_height);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x121212), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    int padding = 5;
    int display_height = 40;
    input_label = lv_label_create(root);
    lv_obj_set_size(input_label, screen_width - 2 * padding, display_height - 2 * padding);
    lv_obj_set_style_bg_color(input_label, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(input_label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(input_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_all(input_label, padding, 0);
    lv_obj_set_style_radius(input_label, 5, 0);
    lv_obj_set_pos(input_label, padding, status_bar_height + padding);
    lv_label_set_long_mode(input_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    update_input_label();

#ifdef CONFIG_USE_TOUCHSCREEN
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
        int total_key_width = screen_width - (padding * 2);
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
            
            lv_obj_t *key_btn = lv_btn_create(root);
            lv_obj_remove_style_all(key_btn);
            lv_obj_set_size(key_btn, current_key_width - 2, key_height);
            lv_obj_set_pos(key_btn, key_x, key_y);
            
            lv_obj_set_style_bg_color(key_btn, lv_color_hex(0x7B1FA2), 0);
            lv_obj_set_style_bg_opa(key_btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(key_btn, lv_color_hex(0x444444), 0);
            lv_obj_set_style_border_width(key_btn, 1, 0);
            lv_obj_set_style_radius(key_btn, 3, 0);

            lv_obj_t *key_label = lv_label_create(key_btn);
            if (c < row_lens[r]) {
                lv_label_set_text(key_label, keys[r][c]);
            } else {
                lv_label_set_text(key_label, "");
                lv_obj_add_flag(key_btn, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_set_style_text_color(key_label, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_font(key_label, &lv_font_montserrat_14, 0);
            lv_obj_center(key_label);
            
            key_x += current_key_width;
        }
        key_y += key_height + 2;
    }
#elif defined(CONFIG_USE_ENCODER)
    encoder_cont = lv_obj_create(root);
    lv_obj_remove_style_all(encoder_cont);
    lv_obj_set_size(encoder_cont, screen_width, display_height);
    lv_obj_set_pos(encoder_cont, 0, status_bar_height + display_height + padding);
    lv_obj_set_style_bg_opa(encoder_cont, LV_OPA_TRANSP, 0);
    // initialize encoder items and metrics
    encoder_items = encoder_alpha_items;
    encoder_item_count = encoder_alpha_count;
    encoder_sym_mode = false;
    encoder_sel_idx = 0;
    encoder_screen_width = screen_width;
    encoder_item_spacing = display_height;
    encoder_offset_x = (screen_width / 2) - (encoder_item_spacing / 2);
    lv_obj_set_scroll_dir(encoder_cont, LV_DIR_LEFT | LV_DIR_RIGHT);
    lv_obj_set_scrollbar_mode(encoder_cont, LV_SCROLLBAR_MODE_OFF);
    // pad right to allow last items to center
    lv_obj_set_style_pad_right(encoder_cont, screen_width, 0);
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
#endif
    
    display_manager_add_status_bar("Keyboard");
}

static void keyboard_destroy() {
    if (keyboard_view.root) {
        lv_obj_del(keyboard_view.root);
        keyboard_view.root = NULL;
        root = NULL;
        input_label = NULL;
        submit_callback = NULL;
        input_len = 0;
        input_buffer[0] = '\0';
        is_symbols_mode = false;
        is_caps = true;
#ifdef CONFIG_USE_ENCODER
        encoder_cont = NULL;
        encoder_item_count = 0;
        encoder_screen_width = 0;
        encoder_item_spacing = 0;
        encoder_sym_mode = false;
#endif
    }
}

static void handle_hardware_button_press_keyboard(InputEvent *event) {
#ifdef CONFIG_USE_ENCODER
    if (event->type == INPUT_TYPE_ENCODER) {
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
                if (!encoder_sym_mode && isalpha((unsigned char)c) && !encoder_uppercase) {
                    c = (char)tolower((unsigned char)c);
                }
                add_char_to_buffer(c);
            }
        }
        return;
    }
#endif
    if (event->type == INPUT_TYPE_TOUCH && event->data.touch_data.state == LV_INDEV_STATE_PR) {
        int touch_x = event->data.touch_data.point.x;
        int touch_y = event->data.touch_data.point.y;
        
        int screen_width = LV_HOR_RES;
        int screen_height = LV_VER_RES;
        int status_bar_height = 20;
        
        int display_height = 40;
        int padding = 5;
        int keys_area_y = status_bar_height + display_height + padding;

        if (touch_y < keys_area_y) return;

        int key_height = (screen_height - keys_area_y - 15) / num_rows;
        
        int row = (touch_y - keys_area_y) / key_height;

        if (row >= 0 && row < num_rows) {
            const char *(*current_keys)[10] = is_symbols_mode ? symbols : keys;
            const int *current_row_lengths = is_symbols_mode ? symbols_row_lengths : row_lengths;
            int create_row_length = max_row_lengths[row];
            int base_key_width = screen_width / create_row_length;
            
            // calculate which column was touched considering variable widths
            int col = -1;
            int current_x = 0;
            for (int c = 0; c < current_row_lengths[row]; c++) {
                int current_key_width = base_key_width;
                
                // adjust for wider SHIFT and DEL buttons
                if (!is_symbols_mode && c < row_lengths[row]) {
                    if (strcmp(keys[row][c], "SHIFT") == 0 || strcmp(keys[row][c], "DEL") == 0 || strcmp(keys[row][c], " ") == 0) {
                        current_key_width = base_key_width * 2;
                    }
                }
                
                if (touch_x >= current_x && touch_x < current_x + current_key_width) {
                    col = c;
                    break;
                }
                current_x += current_key_width;
            }

            if (col >= 0) {
                const char* key = current_keys[row][col];
                if (strcmp(key, "SHIFT") == 0) {
                    is_caps = !is_caps;
                    update_key_labels();
                } else if (strcmp(key, "SYM") == 0) {
                    is_symbols_mode = true;
                    recreate_keyboard_buttons();
                } else if (strcmp(key, "ABC") == 0) {
                    is_symbols_mode = false;
                    recreate_keyboard_buttons();
                } else if (strcmp(key, "Exit") == 0) {
                    display_manager_switch_view(&options_menu_view);
                } else if (strcmp(key, "Done") == 0) {
                    submit_text();
                } else if (strcmp(key, "DEL") == 0) {
                    remove_char_from_buffer();
                } else if (strcmp(key, " ") == 0) {
                    add_char_to_buffer(' ');
                } else if (strlen(key) == 1) {
                    if (is_symbols_mode) {
                        add_char_to_buffer(key[0]);
                    } else {
                        add_char_to_buffer(is_caps ? toupper(key[0]) : tolower(key[0]));
                    }
                }
            }
        }
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        char c = (char)event->data.key_value;
        if (c == '`') {
            display_manager_switch_view(&options_menu_view);
        } else if (c == '\n' || c == '\r' || c == '=') {
            submit_text();
        } else if (c == '\b' || c == '*') {
            remove_char_from_buffer();
        } else if (c >= ' ' && c <= '~') {
            add_char_to_buffer(c);
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

View keyboard_view = {
    .root = NULL,
    .create = keyboard_create,
    .destroy = keyboard_destroy,
    .input_callback = handle_hardware_button_press_keyboard,
    .name = "Keyboard Screen",
    .get_hardwareinput_callback = get_keyboard_callback
};