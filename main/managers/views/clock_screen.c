#include "managers/views/clock_screen.h"
#include "managers/views/main_menu_screen.h"
#include "managers/display_manager.h"
#include "gui/screen_layout.h"
#include "lvgl.h"
#include <time.h>
#include "managers/settings_manager.h"
#include "gui/lvgl_safe.h"
#include "esp_log.h"

static const char *TAG = "ClockScreens";

static lv_obj_t *clock_container;
static lv_obj_t *time_label;
static lv_obj_t *date_label;
static lv_obj_t *year_label;
lv_timer_t *clock_timer = NULL;

static void digital_clock_cb(lv_timer_t *timer) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
    lv_label_set_text(time_label, buf);
    char buf_date[32];
    strftime(buf_date, sizeof(buf_date), "%A, %B %d", &timeinfo);
    lv_label_set_text(date_label, buf_date);
    char buf_year[8];
    strftime(buf_year, sizeof(buf_year), "%Y", &timeinfo);
    lv_label_set_text(year_label, buf_year);
}

static void clock_event_handler(InputEvent *event) {
    if (event->type == INPUT_TYPE_TOUCH && event->data.touch_data.state == LV_INDEV_STATE_REL) {
        display_manager_switch_view(&main_menu_view);
    } else if (event->type == INPUT_TYPE_JOYSTICK) {
        display_manager_switch_view(&main_menu_view);
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        display_manager_switch_view(&main_menu_view);
#ifdef CONFIG_USE_ENCODER
    } else if (event->type == INPUT_TYPE_ENCODER && event->data.encoder.button) {
        display_manager_switch_view(&main_menu_view);
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
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
    clock_container = gui_screen_create_root(NULL, "Clock", lv_color_hex(0x121212), LV_OPA_COVER);
    clock_view.root = clock_container;
    lv_obj_t *content = gui_screen_create_content(clock_container, GUI_STATUS_BAR_HEIGHT);

    time_label = lv_label_create(content);
    lv_label_set_text(time_label, "00:00:00");
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(time_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(time_label, LV_ALIGN_CENTER, 0, -15);
    date_label = lv_label_create(content);
    lv_label_set_text(date_label, "Wednesday, January 01");
    lv_obj_set_style_text_font(date_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(date_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(date_label, LV_ALIGN_CENTER, 0, 30);
    year_label = lv_label_create(content);
    lv_label_set_text(year_label, "2025");
    lv_obj_set_style_text_font(year_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(year_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(year_label, LV_ALIGN_CENTER, 0, 50);

    clock_timer = lv_timer_create(digital_clock_cb, 1000, NULL);
    digital_clock_cb(NULL);
}

void clock_destroy(void) {
    if (clock_timer) {
        lvgl_timer_del_safe(&clock_timer);
    }
    if (clock_container) {
        lv_obj_clean(clock_container);
        lvgl_obj_del_safe(&clock_container);
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