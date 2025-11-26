#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include "lvgl.h"
#include "managers/joystick_manager.h"
#include <stdbool.h>
#include <stdint.h>

typedef void *QueueHandle_tt;
typedef void *SemaphoreHandle_tt; // Because Circular Includes are fun :)

static lv_timer_t *rainbow_timer = NULL;
static uint16_t rainbow_hue = 0;

typedef enum {
    INPUT_TYPE_TOUCH,
    INPUT_TYPE_JOYSTICK,
    INPUT_TYPE_KEYBOARD,
    INPUT_TYPE_ENCODER,         // --- new
    INPUT_TYPE_EXIT_BUTTON      // --- new for IO6 exit button
} InputType;

typedef struct {
  InputType type;
  union {
    int joystick_index;         // Used for joystick inputs
    lv_indev_data_t touch_data; // Used for touchscreen inputs
    uint8_t key_value;          // Used for keyboard inputs
    struct { int8_t direction; bool button; } encoder; // Added for encoder input
    bool exit_pressed;          // Used for IO6 exit button
  } data;
} InputEvent;

#define INPUT_QUEUE_LENGTH 10
#define INPUT_ITEM_SIZE sizeof(int)
QueueHandle_tt input_queue;

#define MUTEX_TIMEOUT_MS 100

#define HARDWARE_INPUT_TASK_PRIORITY (14)
#define RENDERING_TASK_PRIORITY (15)

typedef struct {
  lv_obj_t *root;
  void (*create)(void);
  void (*destroy)(void);
  const char *name;
  void (*get_hardwareinput_callback)(void **callback);
  void (*input_callback)(InputEvent *);
} View;

typedef struct {
  View *current_view;
  View *previous_view;
  SemaphoreHandle_tt mutex;
} DisplayManager;

extern View options_menu_view;
extern View terminal_view;
extern View number_pad_view;
extern View keyboard_view;
extern View *display_manager_previous_view;

/* Function prototypes */

/**
 * @brief Initialize the Display Manager.
 */
void display_manager_init(void);

/**
 * @brief Register a new view.
 */
bool display_manager_register_view(View *view);

/**
 * @brief Switch to a new view.
 */
void display_manager_switch_view(View *view);

void apply_power_management_config(bool power_save_enabled);

void display_manager_update_status_bar_color(void);

void rainbow_effect_cb(lv_timer_t *timer);

/**
 * @brief Destroy the current view.
 */
void display_manager_destroy_current_view(void);

/**
 * @brief Get the current active view.
 */
View *display_manager_get_current_view(void);

bool display_manager_is_available(void);

void lvgl_tick_task(void *arg);

void hardware_input_task(void *pvParameters);

void display_manager_fill_screen(lv_color_t color);

/**
 * @brief Notify the display manager that a user input occurred (external driver/task).
 * If the display was dimmed/off this will restore backlight and return true to indicate
 * the input was consumed for wake purposes and should not be forwarded as a UI event.
 * Returns true if the input woke the display (and should be swallowed), false otherwise.
 */
bool display_manager_notify_user_input(void);

lv_color_t hex_to_lv_color(const char *hex_str);

// Status Bar Functions

void update_status_bar(bool wifi_enabled, bool bt_enabled, bool sd_card_mounted, int batteryPercentage, bool power_save_enabled, bool is_ap_active);

void display_manager_add_status_bar(const char *CurrentMenuName);

// Reduce I2C activity (e.g., pause battery polling/logging) while other subsystems
// such as PN532 scanning/bruteforcing are active to avoid bus contention.
void display_manager_set_low_i2c_mode(bool on);

void display_manager_suspend_lvgl_task(void);
void display_manager_resume_lvgl_task(void);

LV_IMG_DECLARE(Ghost_ESP);
LV_IMG_DECLARE(Map);
LV_IMG_DECLARE(bluetooth);
LV_IMG_DECLARE(wifi);
LV_IMG_DECLARE(rave);
LV_IMG_DECLARE(GESPFlappyghost);
LV_IMG_DECLARE(ghost);
LV_IMG_DECLARE(GESPAppGallery);
LV_IMG_DECLARE(clock_icon);
LV_IMG_DECLARE(settings_icon);
LV_IMG_DECLARE(infrared);
LV_IMG_DECLARE(terminal_icon);
LV_IMG_DECLARE(nfc_icon);

joystick_t joysticks[5];
#ifdef CONFIG_USE_ENCODER
#endif

void set_backlight_brightness(uint8_t value);

#endif /* DISPLAY_MANAGER_H */
