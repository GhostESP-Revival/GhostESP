#include "managers/views/number_pad_screen.h"
#include "core/serial_manager.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "managers/views/terminal_screen.h"
#include "managers/views/options_screen.h"
#include "managers/views/main_menu_screen.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "number_pad_screen";


static ENumberPadMode current_mode = NP_MODE_AP;
static lv_obj_t *root = NULL;
static lv_obj_t *number_display = NULL;
static char input_buffer[32] = {0};
static int input_pos = 0;
static int cursor_pos = 0; // For hardware navigation

static void number_pad_create();
static void number_pad_destroy();
static void handle_hardware_button_press_number_pad(InputEvent *event);
static void get_number_pad_callback(void **callback);

static void update_display() {
    lv_label_set_text(number_display, input_buffer);
    lv_obj_set_style_text_color(number_display, lv_color_hex(0xFFFFFF), 0);
}

static void add_digit(char digit) {
    if (input_pos < sizeof(input_buffer) - 1) {
        input_buffer[input_pos++] = digit;
        input_buffer[input_pos] = '\0';
        update_display();
    }
}

static void remove_digit() {
    if (input_pos > 0) {
        input_buffer[--input_pos] = '\0';
        update_display();
    }
}

static void submit_number() {
    if (input_pos > 0) {
    terminal_set_return_view(&options_menu_view);
display_manager_switch_view(&terminal_view);
        vTaskDelay(pdMS_TO_TICKS(10));
        
        char command[64];
        switch (current_mode) {
            case NP_MODE_AP:
                snprintf(command, sizeof(command), "select -a %s", input_buffer);
                break;
            case NP_MODE_STA:
                snprintf(command, sizeof(command), "select -s %s", input_buffer);
                break;
            case NP_MODE_AIRTAG:
                snprintf(command, sizeof(command), "selectairtag %s", input_buffer);
                break;
            case NP_MODE_LAN:
                snprintf(command, sizeof(command), "select -a %s", input_buffer);
                break;
            case NP_MODE_FLIPPER:
                snprintf(command, sizeof(command), "selectflipper %s", input_buffer);
                break;
            default:
                snprintf(command, sizeof(command), "select -a %s", input_buffer);
        }
        simulateCommand(command);
        
        input_buffer[0] = '\0';
        input_pos = 0;
    }
}

static void number_pad_create() {
    int screen_width = LV_HOR_RES;
    int screen_height = LV_VER_RES;
    
    display_manager_fill_screen(lv_color_hex(0x121212));
    
    // Root object
    root = lv_obj_create(lv_scr_act());
    number_pad_view.root = root;
    lv_obj_set_size(root, screen_width, screen_height);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x121212), 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_align(root, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(root, 0, 0);      // Remove border
    lv_obj_set_style_outline_width(root, 0, 0);     // Remove outline
    

    const lv_font_t *font = (screen_height >= 240) ? &lv_font_montserrat_16 : &lv_font_montserrat_12;
    int padding = 10;
    int display_height = (screen_height >= 240) ? 40 : 30;

    // Number display area
    number_display = lv_label_create(root);
    lv_obj_set_width(number_display, screen_width - 2 * padding);
    lv_obj_set_style_bg_color(number_display, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_text_color(number_display, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(number_display, font, 0);
    lv_obj_set_style_pad_all(number_display, padding, 0);
    lv_obj_align(number_display, LV_ALIGN_TOP_MID, 0, padding);
    lv_obj_set_scrollbar_mode(number_display, LV_SCROLLBAR_MODE_OFF); // Disable scrollbars
    lv_obj_set_style_border_width(number_display, 0, 0);             // Remove border
    lv_obj_set_style_outline_width(number_display, 0, 0);            // Remove outline
    update_display();
    
    // Options container
    lv_obj_t *options_container = lv_obj_create(root);
    lv_obj_set_size(options_container, screen_width, screen_height - display_height - padding);
    lv_obj_align(options_container, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(options_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(options_container, padding, 0);
    lv_obj_set_scrollbar_mode(options_container, LV_SCROLLBAR_MODE_OFF); // Disable scrollbars
    lv_obj_set_style_border_width(options_container, 0, 0);             // Remove border
    lv_obj_set_style_outline_width(options_container, 0, 0);            // Remove outline
    
    // Options: 0-9, DEL, OK, BACK
    const char *options[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ",", "DEL", "OK", "BACK"};
    int option_count = sizeof(options) / sizeof(options[0]);
    int cols = 5;
    int rows = 3;
    int btn_width = (screen_width - 2 * padding) / cols;
    int btn_height = (screen_height - display_height - 2 * padding) / rows;

    for (int i = 0; i < option_count; i++) {
        lv_obj_t *label = lv_label_create(options_container);
        lv_label_set_text(label, options[i]);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, (i == cursor_pos) ? lv_color_hex(0x00FF00) : lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_size(label, btn_width, btn_height);
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, (i % cols) * btn_width, (i / cols) * btn_height);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_scrollbar_mode(label, LV_SCROLLBAR_MODE_OFF); // Disable scrollbars
        lv_obj_set_style_border_width(label, 0, 0);             // Remove border
        lv_obj_set_style_outline_width(label, 0, 0);            // Remove outline
        lv_obj_set_style_bg_color(label, lv_color_hex(0x121212), 0); // Match root background
    }
    
    const char *title;
    if (current_mode == NP_MODE_AP) {
        title = "Select AP";
    } else if (current_mode == NP_MODE_STA) {
        title = "Select Station";
    } else if (current_mode == NP_MODE_AIRTAG) {
        title = "Select AirTag";
    } else if (current_mode == NP_MODE_FLIPPER) {
        title = "Select Flipper";
    } else {
        title = "Select LAN";
    }
    display_manager_add_status_bar(title);
}

static void number_pad_destroy() {
    if (number_pad_view.root) {
        lv_obj_clean(number_pad_view.root);
        lv_obj_del(number_pad_view.root);
        number_pad_view.root = NULL;
        root = NULL;
        number_display = NULL;
        input_buffer[0] = '\0';
        input_pos = 0;
        cursor_pos = 0;
    }
}

static void handle_hardware_button_press_number_pad(InputEvent *event) {
    const char *options[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ",", "DEL", "OK", "BACK"};
    int option_count = sizeof(options) / sizeof(options[0]);
    
    if (event->type == INPUT_TYPE_KEYBOARD){
        int key_value = event->data.key_value;
        bool is_numeric = false;
        int option_index;
        char key_str[2];
        ESP_LOGI(TAG, "Keyboard event: %c pressed", key_value);
        sprintf(key_str, "%c", key_value); // convert key_value to str for comparisons
        int prev_cursor_pos = cursor_pos;

        for (int i = 0; i < option_count; i++){
            ESP_LOGD(TAG, "Checking key pressed value: %s against %s", key_str, options[i]);
            if (strcmp(key_str, options[i]) == 0){
                ESP_LOGI(TAG, "Number key %c pressed", key_value);
                is_numeric = true;
                option_index=i;
                break;
            }
        }

        if (is_numeric){
            ESP_LOGI(TAG, "adding number to screen");
            add_digit(options[option_index][0]);
            update_display();
        }
        else if(key_value == 8){
            ESP_LOGI(TAG, "Removing last number");
            remove_digit();
            update_display();
        }
        else if(strcmp(key_str, "`") == 0){ // escape key
            ESP_LOGI(TAG, "Escaping to previous screen");
            display_manager_switch_view(&options_menu_view);
            return;
        }
        else if(strcmp(key_str, "=") == 0){ //set = as a submit key for this menu since okay is tied up at a higher level
            ESP_LOGI(TAG, "Submitting Number");
            submit_number();
            return;
        }
        // --- Vim keybinds ---
        else if (key_value == 'h') { // Vim left
            ESP_LOGI(TAG, "Vim 'h' pressed (left)");
            cursor_pos = (cursor_pos > 0) ? cursor_pos - 1 : option_count - 1;
        }
        else if (key_value == 'l') { // Vim right
            ESP_LOGI(TAG, "Vim 'l' pressed (right)");
            cursor_pos = (cursor_pos < option_count - 1) ? cursor_pos + 1 : 0;
        }
        else if (key_value == 'k') { // Vim up
            ESP_LOGI(TAG, "Vim 'k' pressed (up)");
            if (cursor_pos >= 5) {
                cursor_pos -= 5;
            } else if (cursor_pos >= 0 && cursor_pos <= 4) {
                cursor_pos = (cursor_pos == 0) ? 10 : (cursor_pos == 1) ? 11 : (cursor_pos == 2) ? 12 : cursor_pos + 5;
            }
        }
        else if (key_value == 'j') { // Vim down
            ESP_LOGI(TAG, "Vim 'j' pressed (down)");
            if (cursor_pos <= 7) {
                cursor_pos += 5;
            } else if (cursor_pos >= 10) {
                cursor_pos = cursor_pos - 10;
            }
        }
        // --- End Vim keybinds ---
        else if (key_value == 44 || key_value == ',') { // Left
            ESP_LOGI(TAG, "Left button pressed\n");
            cursor_pos = (cursor_pos > 0) ? cursor_pos - 1 : option_count - 1;
        } 
        else if ((key_value == 47 || key_value == '/') ) { // Right
            ESP_LOGI(TAG, "Right button pressed\n");
            cursor_pos = (cursor_pos < option_count - 1) ? cursor_pos + 1 : 0;
        }
        else if (key_value == 59 || key_value == ';'){ //up
            ESP_LOGI(TAG, "Up button pressed");
            if (cursor_pos >= 5) {
                cursor_pos -= 5;
            } else if (cursor_pos >= 0 && cursor_pos <= 4) {
                cursor_pos = (cursor_pos == 0) ? 10 : (cursor_pos == 1) ? 11 : (cursor_pos == 2) ? 12 : cursor_pos + 5;
            }
        } else if (key_value == 46 || key_value == '.'){ //down
            ESP_LOGI(TAG, "Down button pressed");
            if (cursor_pos <= 7) {
                cursor_pos += 5;
            } else if (cursor_pos >= 10) {
                cursor_pos = cursor_pos - 10;
            }
        } else if (key_value == 13){ //enter key
            if (strcmp(options[cursor_pos], "DEL") == 0) {
                remove_digit();
                update_display();
            } else if (strcmp(options[cursor_pos], "OK") == 0) {
                submit_number();
                return;
            } else if (strcmp(options[cursor_pos], "BACK") == 0) {
                display_manager_switch_view(&options_menu_view);
                return;
            } else {
                add_digit(options[cursor_pos][0]);
                update_display();
            }
        }
        if (prev_cursor_pos != cursor_pos) {
            lv_obj_t *options_container = lv_obj_get_child(root, 1);
            for (int i = 0; i < option_count; i++) {
                lv_obj_t *label = lv_obj_get_child(options_container, i);
                lv_obj_set_style_text_color(label, (i == cursor_pos) ? lv_color_hex(0x00FF00) : lv_color_hex(0xFFFFFF), 0);
            }
        }
    }
    else if (event->type == INPUT_TYPE_JOYSTICK) {
        int button = event->data.joystick_index;
        int prev_cursor_pos = cursor_pos;
        

        if (button == 0) { // Left
            cursor_pos = (cursor_pos > 0) ? cursor_pos - 1 : option_count - 1;
        } else if (button == 3) { // Right
            cursor_pos = (cursor_pos < option_count - 1) ? cursor_pos + 1 : 0;
        } else if (button == 2) { // Up
            if (cursor_pos >= 5) {
                cursor_pos -= 5;
            } else if (cursor_pos >= 0 && cursor_pos <= 4) {
                cursor_pos = (cursor_pos == 0) ? 10 : (cursor_pos == 1) ? 11 : (cursor_pos == 2) ? 12 : cursor_pos + 5;
            }
        } else if (button == 4) { // Down
            if (cursor_pos <= 7) {
                cursor_pos += 5;
            } else if (cursor_pos >= 10) {
                cursor_pos = cursor_pos - 10;
            }
        } else if (button == 1) {
            if (strcmp(options[cursor_pos], "DEL") == 0) {
                remove_digit();
                update_display();
            } else if (strcmp(options[cursor_pos], "OK") == 0) {
                submit_number();
                return;
            } else if (strcmp(options[cursor_pos], "BACK") == 0) {
                display_manager_switch_view(&options_menu_view);
                return;
            } else {
                add_digit(options[cursor_pos][0]);
                update_display();
            }
        }


        if (prev_cursor_pos != cursor_pos) {
            lv_obj_t *options_container = lv_obj_get_child(root, 1);
            for (int i = 0; i < option_count; i++) {
                lv_obj_t *label = lv_obj_get_child(options_container, i);
                lv_obj_set_style_text_color(label, (i == cursor_pos) ? lv_color_hex(0x00FF00) : lv_color_hex(0xFFFFFF), 0);
            }
        }
    } 
    else if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *data = &event->data.touch_data;
        if (data->state != LV_INDEV_STATE_PR) return;
        int screen_width = LV_HOR_RES;
        int screen_height = LV_VER_RES;
        
        int grid_width = screen_width / 5;
        int grid_height = (screen_height - 30) / 3;
        
        int x = data->point.x / grid_width;
        int y = (data->point.y - 30) / grid_height;
        
        if (x >= 0 && x < 5 && y >= 0 && y < 3) {
            int index = y * 5 + x;
            if (index < option_count) {
                if (strcmp(options[index], "DEL") == 0) {
                    remove_digit();
                } else if (strcmp(options[index], "OK") == 0) {
                    submit_number();
                } else if (strcmp(options[index], "BACK") == 0) {
                    display_manager_switch_view(&options_menu_view);
                } else {
                    add_digit(options[index][0]);
                }
                update_display();
            }
        }
    }
    else if (event->type == INPUT_TYPE_ENCODER) {
        int prev_cursor_pos = cursor_pos;
        if (event->data.encoder.button) {
            if (strcmp(options[cursor_pos], "DEL") == 0) {
                remove_digit();
                update_display();
            } else if (strcmp(options[cursor_pos], "OK") == 0) {
                submit_number();
                return;
            } else if (strcmp(options[cursor_pos], "BACK") == 0) {
                display_manager_switch_view(&options_menu_view);
                return;
            } else {
                add_digit(options[cursor_pos][0]);
                update_display();
            }
        } else {
            if (event->data.encoder.direction > 0) {
                cursor_pos = (cursor_pos < option_count - 1) ? cursor_pos + 1 : 0;
            } else {
                cursor_pos = (cursor_pos > 0) ? cursor_pos - 1 : option_count - 1;
            }
        }
        if (prev_cursor_pos != cursor_pos) {
            lv_obj_t *options_container = lv_obj_get_child(root, 1);
            for (int i = 0; i < option_count; i++) {
                lv_obj_t *label = lv_obj_get_child(options_container, i);
                lv_obj_set_style_text_color(label, (i == cursor_pos) ? lv_color_hex(0x00FF00) : lv_color_hex(0xFFFFFF), 0);
            }
        }
    }
#ifdef CONFIG_USE_ENCODER
    else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        ESP_LOGI(TAG, "IO6 exit button pressed, returning to main menu");
        display_manager_switch_view(&main_menu_view);
    }
#endif
}

void get_number_pad_callback(void **callback) {
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