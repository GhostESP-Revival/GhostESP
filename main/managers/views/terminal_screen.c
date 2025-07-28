#include "managers/views/terminal_screen.h"
#include "core/serial_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "managers/views/main_menu_screen.h"
#include "managers/wifi_manager.h"
#include "managers/display_manager.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>

extern View keyboard_view;

#include "lvgl.h"
#include "managers/settings_manager.h"

static const char *TAG = "Terminal";
static lv_obj_t *terminal_page = NULL;
static SemaphoreHandle_t terminal_mutex = NULL;
static bool retry_cleanup_flag = false;
static lv_timer_t *terminal_cleanup_retry_timer = NULL;
static bool terminal_active = false;
static bool is_stopping = false;
#define MAX_TEXT_LENGTH 4096
#define CLEANUP_THRESHOLD (MAX_TEXT_LENGTH * 3 / 4)
#define CLEANUP_AMOUNT (MAX_TEXT_LENGTH / 2)
#define MAX_QUEUE_SIZE 10
#define MAX_MESSAGE_SIZE 256
#define MIN_SCREEN_SIZE 239
#define BUTTON_SIZE 40
#define BUTTON_PADDING 5

static lv_obj_t *back_btn = NULL;
static lv_obj_t *input_label = NULL;
static size_t current_text_length = 0; // track total characters to manage memory
lv_timer_t *terminal_update_timer = NULL;
static unsigned long createdTimeInMs = 0;
#define ENCODER_DEBOUNCE_TIME_MS 500

static char input_buffer[128] = {0}; // keyboard input buffer
static int input_len = 0; // input length counter

static void scroll_terminal_up(void);
static void scroll_terminal_down(void);
static void stop_all_operations(void);

// keyboard function predefs
static void submit_text();
static void add_char_to_buffer(char c);
static void remove_char_from_buffer();
static void update_input_label();

typedef struct {
  char messages[MAX_QUEUE_SIZE][MAX_MESSAGE_SIZE];
  int head;
  int tail;
  int count;
} MessageQueue;

static MessageQueue message_queue = {.head = 0, .tail = 0, .count = 0};

static void submit_text() {
    if (input_len > 0) {
      char prompt_buf[sizeof(input_buffer) + 4]; // +4 for "> " and null terminator
      snprintf(prompt_buf, sizeof(prompt_buf), "> %s", input_buffer); // format the prompt
      terminal_view_add_text(prompt_buf); // add prompt before the command when printing to screen
      simulateCommand(input_buffer); // execute the command
      memset(input_buffer, 0, sizeof(input_buffer)); // clear the input buffer
      input_len = 0; // reset input length
      update_input_label(); // update the input label to show empty state
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
static void update_input_label() {
    if (input_label) {
        lv_label_set_text(input_label, input_buffer);
    }
}

static void queue_message(const char *text) {
  if (message_queue.count >= MAX_QUEUE_SIZE) {
    message_queue.head = (message_queue.head + 1) % MAX_QUEUE_SIZE;
    message_queue.count--;
  }
  strncpy(message_queue.messages[message_queue.tail], text, MAX_MESSAGE_SIZE - 1);
  message_queue.messages[message_queue.tail][MAX_MESSAGE_SIZE - 1] = '\0';
  message_queue.tail = (message_queue.tail + 1) % MAX_QUEUE_SIZE;
  message_queue.count++;
}

static void clear_message_queue(void) {
  message_queue.head = 0;
  message_queue.tail = 0;
  message_queue.count = 0;
}

static void process_queued_messages(void) {
  if (!terminal_active || !terminal_page || is_stopping || message_queue.count == 0) {
    return;
  }

  if (xSemaphoreTake(terminal_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    ESP_LOGW(TAG, "Failed to acquire terminal mutex in process_queued_messages");
    return; // Try again later
  }

  lv_obj_t *last_item = NULL;
  while (message_queue.count > 0) {
    const char *msg = message_queue.messages[message_queue.head];
    
    lv_obj_t *item = lv_list_add_text(terminal_page, msg);
    lv_label_set_long_mode(item, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(item, lv_color_hex(settings_get_terminal_text_color(&G_Settings)), 0);
    lv_obj_set_style_text_font(item, &lv_font_montserrat_10, 0);
    last_item = item;

    current_text_length += strlen(msg);

    if (current_text_length > CLEANUP_THRESHOLD) {
        // aim to free at least CLEANUP_AMOUNT characters
        size_t target_len = (current_text_length > CLEANUP_AMOUNT) ? current_text_length - CLEANUP_AMOUNT : 0;
        while (current_text_length > target_len && lv_obj_get_child_cnt(terminal_page) > 0) {
            lv_obj_t *oldest = lv_obj_get_child(terminal_page, 0); // first child is oldest
            const char *old_text = lv_label_get_text(oldest);
            if (old_text) {
                size_t old_len = strlen(old_text);
                if (current_text_length > old_len) {
                    current_text_length -= old_len;
                } else {
                    current_text_length = 0;
                }
            }
            lv_obj_del(oldest);
        }
    }

    // dequeue
    message_queue.head = (message_queue.head + 1) % MAX_QUEUE_SIZE;
    message_queue.count--;
  }

  // Scroll to the last item added in this batch
  if (last_item) {
    lv_obj_scroll_to_view(last_item, LV_ANIM_OFF);
  }

  xSemaphoreGive(terminal_mutex);
}

static void process_queued_messages_callback(lv_timer_t * timer) {
    process_queued_messages();
}


static int (*default_log_vprintf)(const char *, va_list) = NULL;

static void scroll_terminal_up(void) {
  if (!terminal_page) return;
  lv_coord_t scroll_pixels = lv_obj_get_height(terminal_page) / 2;
  lv_obj_scroll_by(terminal_page, 0, scroll_pixels, LV_ANIM_OFF);
  lv_obj_invalidate(terminal_page);
  ESP_LOGI(TAG, "Scroll up triggered");
}

static void scroll_terminal_down(void) {
  if (!terminal_page) return;
  lv_coord_t scroll_pixels = lv_obj_get_height(terminal_page) / 2;
  lv_obj_scroll_by(terminal_page, 0, -scroll_pixels, LV_ANIM_OFF);
  lv_obj_invalidate(terminal_page);
  ESP_LOGI(TAG, "Scroll down triggered");
}

static void stop_all_operations(void) {
  terminal_active = false;
  is_stopping = true;

  // Send all stop commands
  simulateCommand("stop");

  vTaskDelay(pdMS_TO_TICKS(20));

  // Now, switch the view
  display_manager_switch_view(display_manager_previous_view);
  ESP_LOGI(TAG, "Stop all operations triggered");
}
#if defined(CONFIG_USE_HW_KB) || defined(CONFIG_USE_TOUCHSCREEN)
void text_box_click_cb(lv_event_t *e){
  ESP_LOGI(TAG, "Text box clicked");
  printf("Text box clicked\n");

  display_manager_switch_view(&keyboard_view);

  // If using a hardware keyboard, we can ignore this click
}
#endif
void terminal_view_create(void) {
    is_stopping = false;
    if (terminal_view.root != NULL) {
        return;
    }

    if (!terminal_mutex) {
        terminal_mutex = xSemaphoreCreateMutex();
        if (!terminal_mutex) {
            ESP_LOGE(TAG, "Failed to create terminal mutex");
            return;
        }
    }

    terminal_active = true;

    terminal_view.root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(terminal_view.root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(terminal_view.root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(terminal_view.root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(terminal_view.root, 0, 0); // Remove border
    lv_obj_set_scrollbar_mode(terminal_view.root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(terminal_view.root, 0, 0);

    const int STATUS_BAR_HEIGHT = 20;
    const int padding = 5;
    const int textbox_height = 40;

    int back_button_height = 0;
    bool show_back_btn = false;
    bool show_input_bar = false;

#ifdef CONFIG_USE_TOUCHSCREEN
    if (LV_HOR_RES > MIN_SCREEN_SIZE && LV_VER_RES > MIN_SCREEN_SIZE) {
        show_back_btn = true;
        back_button_height = BUTTON_SIZE + BUTTON_PADDING * 2;
    }
#endif

#if defined(CONFIG_USE_HW_KB) || defined(CONFIG_USE_TOUCHSCREEN)
    show_input_bar = true;
#endif

    // Calculate the height for the input area (input box + padding)
    int input_area_height = 0;
    if (show_back_btn && show_input_bar) {
        input_area_height = (back_button_height > (textbox_height + padding) ? back_button_height : (textbox_height + padding));
    } else if (show_back_btn) {
        input_area_height = back_button_height;
    } else if (show_input_bar) {
        input_area_height = textbox_height + padding;
    } else {
        input_area_height = 0;
    }

    // Calculate the height for the terminal readout area
    int textarea_height = LV_VER_RES - STATUS_BAR_HEIGHT - input_area_height;

    // Create the terminal_page to fill all space above the input box and back button
    terminal_page = lv_list_create(terminal_view.root);
    lv_obj_set_pos(terminal_page, 0, STATUS_BAR_HEIGHT);
    lv_obj_set_size(terminal_page, LV_HOR_RES, textarea_height);
    lv_obj_set_style_bg_color(terminal_page, lv_color_black(), 0);
    lv_obj_set_style_pad_all(terminal_page, 0, 0);
    lv_obj_set_scrollbar_mode(terminal_page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(terminal_page, 0, 0);
    lv_obj_set_style_clip_corner(terminal_page, false, 0);
    lv_obj_set_scroll_dir(terminal_page, LV_DIR_VER);

#ifdef CONFIG_USE_TOUCHSCREEN
    if (show_back_btn) {
        back_btn = lv_btn_create(terminal_view.root);
        lv_obj_set_size(back_btn, BUTTON_SIZE, BUTTON_SIZE);
        lv_obj_align(back_btn, LV_ALIGN_BOTTOM_LEFT, BUTTON_PADDING, -BUTTON_PADDING);
        lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x333333), LV_PART_MAIN);
        lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_border_width(back_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
        lv_obj_t *back_label = lv_label_create(back_btn);
        lv_label_set_text(back_label, LV_SYMBOL_LEFT);
        lv_obj_center(back_label);

        lv_obj_update_layout(terminal_view.root);
        ESP_LOGW(TAG, "Back pos: x=%d, y=%d, w=%d, h=%d",
                 lv_obj_get_x(back_btn), lv_obj_get_y(back_btn),
                 lv_obj_get_width(back_btn), lv_obj_get_height(back_btn));
    }
#endif

#if defined(CONFIG_USE_HW_KB) || defined(CONFIG_USE_TOUCHSCREEN)
    if (show_input_bar) {
        int textbox_width = LV_HOR_RES - 2 * padding;
    #ifdef CONFIG_USE_TOUCHSCREEN
        if (show_back_btn) {
            textbox_width -= BUTTON_SIZE + 2 * BUTTON_PADDING;
        }
    #endif
        if (textbox_width < 40) textbox_width = 40;

        input_label = lv_label_create(terminal_view.root);
        lv_obj_set_size(input_label, textbox_width, textbox_height);
        lv_obj_set_style_bg_color(input_label, lv_color_hex(0x333333), 0);
        lv_obj_set_style_bg_opa(input_label, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(input_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_pad_all(input_label, padding, 0);
        lv_obj_set_style_radius(input_label, 6, 0);
        lv_obj_set_style_border_width(input_label, 0, 0);
        lv_obj_set_style_shadow_width(input_label, 0, 0);
        lv_obj_align(input_label, LV_ALIGN_BOTTOM_RIGHT, -padding, -padding);
        lv_obj_add_event_cb(input_label, text_box_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_flag(input_label, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_long_mode(input_label, LV_LABEL_LONG_CLIP);
        lv_label_set_text(input_label, "Type Command...");
        // Center vertically by adjusting vertical padding
        const lv_font_t *current_font = lv_obj_get_style_text_font(input_label, 0);
        int font_height = lv_font_get_line_height(current_font);
        int vertical_pad = (textbox_height - font_height) / 2;
        if (vertical_pad < 0) vertical_pad = 0; // Prevent negative padding
        lv_obj_set_style_pad_top(input_label, vertical_pad, 0);
        lv_obj_set_style_pad_bottom(input_label, vertical_pad, 0);
    }
#endif

    display_manager_add_status_bar("Terminal");

    if (!terminal_update_timer) {
        terminal_update_timer = lv_timer_create(process_queued_messages_callback, 50, NULL);
        if (!terminal_update_timer) {
            ESP_LOGE(TAG, "Failed to create terminal update timer");
        }
    }
    createdTimeInMs = (unsigned long)(esp_timer_get_time() / 1000ULL);
}
static void terminal_retry_cleanup_cb(lv_timer_t *timer) {
    if (!retry_cleanup_flag) {
        lv_timer_del(timer);
        terminal_cleanup_retry_timer = NULL;
        return;
    }
    ESP_LOGI(TAG, "Retrying terminal cleanup...");
    // Try to destroy again
    retry_cleanup_flag = false;
    terminal_view_destroy();
    // If cleanup succeeds, the flag will stay false and timer will be deleted
    // If not, the flag will be set again and timer will keep running
}

void terminal_view_destroy(void) {
    // Signal all callbacks/timers to stop
    terminal_active = false;
    is_stopping = true;

    // Clear message queue and reset state
    clear_message_queue();
    current_text_length = 0;
    input_len = 0;
    input_buffer[0] = '\0';

    // Delete timer first to prevent callbacks after objects are freed
    if (terminal_update_timer) {
        lv_timer_del(terminal_update_timer);
        terminal_update_timer = NULL;
    }

    // Safely delete LVGL objects
    if (terminal_mutex) {
        if (xSemaphoreTake(terminal_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            // Delete LVGL objects if they exist
            if (terminal_view.root) {
                lv_obj_del(terminal_view.root);
                terminal_view.root = NULL;
            }
            // Set all pointers to NULL to avoid dangling references
            terminal_page = NULL;
            back_btn = NULL;
            input_label = NULL;

            vSemaphoreDelete(terminal_mutex); // Delete mutex directly after acquiring
            terminal_mutex = NULL;
        } else {
            ESP_LOGE(TAG, "Failed to acquire terminal mutex during destroy. A leak may occur.");
            retry_cleanup_flag = true; // Set flag to retry cleanup later
            if (!terminal_cleanup_retry_timer) {
                terminal_cleanup_retry_timer = lv_timer_create(terminal_retry_cleanup_cb, 250, NULL);
            }
        }
    } else {
        // If mutex is already NULL, still clear pointers
        terminal_view.root = NULL;
        terminal_page = NULL;
        back_btn = NULL;
        input_label = NULL;
    }

    // Final state reset
    is_stopping = false;
    if (terminal_cleanup_retry_timer) {
        lv_timer_del(terminal_cleanup_retry_timer);
        terminal_cleanup_retry_timer = NULL;
    }
}

void terminal_view_add_text(const char *text) {
  if (!text || is_stopping || text[0] == '\0') {
      return;
  }

  if (!terminal_mutex) {
      ESP_LOGW(TAG, "Attempted to add text while terminal is destroying. Ignoring.");
      return;
  }

  if (xSemaphoreTake(terminal_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      queue_message(text);
      xSemaphoreGive(terminal_mutex);
  } else {
      ESP_LOGW(TAG, "Failed to acquire terminal mutex in add_text");
  }
}

void terminal_view_hardwareinput_callback(InputEvent *event) {
  if (event->type == INPUT_TYPE_TOUCH) {
    ESP_LOGW(TAG, "Touch event");
    if (event->data.touch_data.state != LV_INDEV_STATE_PR) {
      return;
    }
    int touch_x = event->data.touch_data.point.x;
    int touch_y = event->data.touch_data.point.y;
    ESP_LOGW(TAG, "Touch detected at x=%d, y=%d (screen: %dx%d)", touch_x, touch_y, LV_HOR_RES, LV_VER_RES);

    if (input_label){
      ESP_LOGI(TAG, "Input label exists, checking for click");
      // Check if the touch is within the input label area
      lv_obj_t *input_area = lv_obj_get_parent(input_label);
      int input_x_min = lv_obj_get_x(input_label);
      int input_x_max = input_x_min + lv_obj_get_width(input_label);
      int input_y_min = lv_obj_get_y(input_label);
      int input_y_max = input_y_min + lv_obj_get_height(input_label);

      if (touch_x >= input_x_min && touch_x <= input_x_max &&
          touch_y >= input_y_min && touch_y <= input_y_max) {
        ESP_LOGI(TAG, "Input label clicked at x=%d, y=%d", touch_x, touch_y);
        lv_event_send(input_label, LV_EVENT_CLICKED, NULL);
        return;
      }
    }

    if (LV_HOR_RES > MIN_SCREEN_SIZE && LV_VER_RES > MIN_SCREEN_SIZE) {
      int button_y_min = LV_VER_RES - (BUTTON_SIZE + BUTTON_PADDING * 2);
      int button_y_max = LV_VER_RES - BUTTON_PADDING;
      

      if (touch_y >= button_y_min && touch_y <= button_y_max) {
        int back_x_min = BUTTON_PADDING;
        int back_x_max = BUTTON_PADDING + BUTTON_SIZE + 25;
        if (touch_x >= back_x_min && touch_x <= back_x_max) {
          ESP_LOGW(TAG, "Back button triggered");
          stop_all_operations();
          return;
        }
      }
      

      int screen_half = LV_VER_RES / 2;
      if (touch_y < screen_half) {
        ESP_LOGW(TAG, "Top half tap - Scroll up");
        scroll_terminal_up();
      } else if (touch_y < button_y_min) {
        ESP_LOGW(TAG, "Bottom half tap - Scroll down");
        scroll_terminal_down();
      }
    } else {
      int screen_half = LV_VER_RES / 2;
      if (touch_y < screen_half) {
        ESP_LOGW(TAG, "Top half tap - Scroll up (small screen)");
        scroll_terminal_up();
      } else {
        ESP_LOGW(TAG, "Bottom half tap - Scroll down (small screen)");
        scroll_terminal_down();
      }
    }
  } else if (event->type == INPUT_TYPE_JOYSTICK) {
    ESP_LOGI(TAG, "Joystick event");
    int button = event->data.joystick_index;
    if (button == 1) {
      ESP_LOGW(TAG, "Joystick button 1: Stop all operations");
      stop_all_operations();
    } else if (button == 2) {
      ESP_LOGW(TAG, "Joystick button 2: Scroll up");
      scroll_terminal_up();
    } else if (button == 4) {
      ESP_LOGW(TAG, "Joystick button 4: Scroll down");
      scroll_terminal_down();
    }
  } else if (event->type == INPUT_TYPE_KEYBOARD) {
    ESP_LOGI(TAG, "keyboard event");
    uint8_t key = event->data.key_value;
    if (key == 29 || key == '`') {
      stop_all_operations();
    } else if (key == 59 || key == ';') {// up arrow
      scroll_terminal_up();
    } else if (key == 46 || key == '.') {      //down arrow
      scroll_terminal_down();
    } else if (key == 13){
      ESP_LOGW(TAG, "Enter key pressed, submitting text");
      submit_text();
    } else if (key == 8 || key == 127) { // backspace
      ESP_LOGW(TAG, "Backspace key pressed, removing last character");
      remove_char_from_buffer();
    } else if (key == 32) { // space
      ESP_LOGW(TAG, "Space key pressed, adding space to input buffer");
      add_char_to_buffer(' ');
    } else if (key >= 32 && key <= 126) { // printable ASCII characters
      ESP_LOGW(TAG, "Adding character '%c' to input buffer", (char)key);
      add_char_to_buffer((char)key);
    } else if (key == 0) {
      ESP_LOGW(TAG, "Null character received, ignoring"); 
    }
    else {
      ESP_LOGW(TAG, "Unhandled keyboard input: %d", key);
      // Optionally handle other keys or log them
      char key_str[2];
      key_str[0] = (char)key;
      key_str[1] = '\0';
      terminal_view_add_text(key_str); // Add unhandled keys to terminal
    }
  } else if (event->type == INPUT_TYPE_ENCODER) {
    unsigned long now_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
    if (event->data.encoder.button) {
      if (now_ms - createdTimeInMs <= ENCODER_DEBOUNCE_TIME_MS) {
        ESP_LOGD(TAG, "Encoder button press debounced");
        return;
      }
      ESP_LOGW(TAG, "Encoder button pressed, stopping all operations");
      stop_all_operations();
      createdTimeInMs = now_ms; // Update last press time
    } else {
      if (event->data.encoder.direction > 0) {
        ESP_LOGW(TAG, "Encoder CW, scrolling down");
        scroll_terminal_down();
      } else {
        ESP_LOGW(TAG, "Encoder CCW, scrolling up");
        scroll_terminal_up();
      }
    }
#ifdef CONFIG_USE_ENCODER
  } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
    ESP_LOGI(TAG, "IO6 exit button pressed, returning to main menu");
    stop_all_operations();
    display_manager_switch_view(&main_menu_view);
#endif
  }
}



void terminal_view_get_hardwareinput_callback(void **callback) {
  if (callback != NULL) {
    *callback = (void *)terminal_view_hardwareinput_callback;
  }
}


static View *terminal_return_view = NULL;

View terminal_view = {
  .root = NULL,
  .create = terminal_view_create,
  .destroy = terminal_view_destroy,
  .input_callback = terminal_view_hardwareinput_callback,
  .name = "TerminalView",
  .get_hardwareinput_callback = terminal_view_get_hardwareinput_callback
};

void terminal_set_return_view(View *view) {
    terminal_return_view = view;
}
