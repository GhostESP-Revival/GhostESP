#include "managers/views/clock_screen.h"
#include "managers/views/main_menu_screen.h"
#include "managers/display_manager.h"
#include "lvgl.h"
#include <time.h>
#include "managers/settings_manager.h"
#include "esp_log.h"

static const char *TAG = "ClockScreens";


static lv_obj_t *clock_container;
static lv_obj_t *time_label;
static lv_obj_t *date_label;
lv_timer_t *clock_timer = NULL;

static void digital_clock_cb(lv_timer_t *timer) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
    lv_label_set_text(time_label, buf);
    char buf_date[16];
    strftime(buf_date, sizeof(buf_date), "%A, %b %d", &timeinfo);
    lv_label_set_text(date_label, buf_date);
}

static void clock_event_handler(InputEvent *event) {
    if (event->type == INPUT_TYPE_TOUCH && event->data.touch_data.state == LV_INDEV_STATE_REL) {
        ESP_LOGI(TAG, "Touch input type");
        display_manager_switch_view(&main_menu_view);
    } else if (event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_index == 2) {
        ESP_LOGI(TAG, "Joystick input type");
        display_manager_switch_view(&main_menu_view);
    } else if (event->type == INPUT_TYPE_KEYBOARD){
        ESP_LOGI(TAG, "keyboard input type");
        display_manager_switch_view(&main_menu_view);
#ifdef CONFIG_USE_ENCODER
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        ESP_LOGI(TAG, "IO6 exit button pressed, returning to main menu");
        display_manager_switch_view(&main_menu_view);
#endif

    }
}

void clock_create(void) {
    // Apply user's timezone for localtime
    const char *tz = settings_get_timezone_str(&G_Settings);
    if (tz) {
        setenv("TZ", tz, 1);
        tzset();
    }
    display_manager_fill_screen(lv_color_hex(0x121212));
    clock_container = lv_obj_create(lv_scr_act());
    clock_view.root = clock_container;
    lv_obj_set_size(clock_container, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_opa(clock_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(clock_container, 0, 0);
    lv_obj_set_style_border_color(clock_container, lv_color_hex(0x000000), 0);
    lv_obj_set_scrollbar_mode(clock_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(clock_container, LV_ALIGN_CENTER, 0, 0);

    time_label = lv_label_create(clock_container);
    lv_label_set_text(time_label, "00:00:00");
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(time_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(time_label, LV_ALIGN_CENTER, 0, -15);
    date_label = lv_label_create(clock_container);
    lv_label_set_text(date_label, "Wednesday, Jan 01");
    lv_obj_set_style_text_font(date_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(date_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(date_label, LV_ALIGN_CENTER, 0, 30);

    clock_timer = lv_timer_create(digital_clock_cb, 1000, NULL);
    digital_clock_cb(NULL);
    display_manager_add_status_bar("Clock");
}

void clock_destroy(void) {
    if (clock_timer) {
        lv_timer_del(clock_timer);
        clock_timer = NULL;
    }
    if (clock_container) {
        lv_obj_clean(clock_container);
        lv_obj_del(clock_container);
        clock_container = NULL;
        clock_view.root = NULL;
    }
}

void get_clock_callback(void **callback) {
    if (callback) *callback = (void *)clock_event_handler;
}

View clock_view = {
    .root = NULL,
    .create = clock_create,
    .destroy = clock_destroy,
    .input_callback = clock_event_handler,
    .name = "Clock",
    .get_hardwareinput_callback = get_clock_callback
}; 