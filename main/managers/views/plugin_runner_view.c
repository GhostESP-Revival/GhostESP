#include "managers/views/plugin_runner_view.h"

#include "gui/lvgl_safe.h"
#include "gui/screen_layout.h"
#include "managers/plugin_api.h"
#include "managers/plugin_loader.h"
#include "managers/sd_card_manager.h"
#include "managers/views/app_gallery_screen.h"
#include "managers/views/error_popup.h"
#include "gui/toast.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "PluginRunner";

static char s_pending_app_id[PLUGIN_APP_ID_MAX];
static lv_obj_t *s_root = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_output = NULL;
static lv_timer_t *s_tick_timer = NULL;
static lv_timer_t *s_launch_timer = NULL;
/* Heap-allocated so no-PSRAM boards (e.g. CYDMicroUSB) don't put 2 KB of
 * scrollback into .dram0.bss and overflow the region. Allocated on view
 * create, freed on destroy. */
#define PLUGIN_RUNNER_OUTPUT_BUF_SIZE 2048
static char *s_output_buf = NULL;
static bool s_touch_started = false;
static lv_point_t s_touch_start = {0};

#define PLUGIN_RUNNER_TICK_MS 100
#define PLUGIN_RUNNER_TAP_THRESHOLD 12
#define PLUGIN_RUNNER_SCROLL_THRESHOLD 16

typedef enum {
    RUNNER_UI_SET_TITLE,
    RUNNER_UI_PRINT,
    RUNNER_UI_CLEAR,
    RUNNER_UI_TOAST,
} runner_ui_action_type_t;

typedef struct {
    runner_ui_action_type_t type;
    char text[256];
} runner_ui_action_t;

static void runner_set_title_now(const char *title) {
    if (s_title && lv_obj_is_valid(s_title)) {
        lv_label_set_text(s_title, title ? title : "SD App");
    }
}

static void runner_print_now(const char *text) {
    if (!s_output || !lv_obj_is_valid(s_output) || !text) return;
    if (!s_output_buf) return;
    size_t cur = strlen(s_output_buf);
    size_t add = strlen(text);
    if (cur + add + 2 >= PLUGIN_RUNNER_OUTPUT_BUF_SIZE) {
        memmove(s_output_buf, s_output_buf + PLUGIN_RUNNER_OUTPUT_BUF_SIZE / 2, strlen(s_output_buf + PLUGIN_RUNNER_OUTPUT_BUF_SIZE / 2) + 1);
        cur = strlen(s_output_buf);
    }
    snprintf(s_output_buf + cur, PLUGIN_RUNNER_OUTPUT_BUF_SIZE - cur, "%s", text);
    lv_label_set_text(s_output, s_output_buf);
}

static void runner_clear_now(void) {
    if (s_output_buf) s_output_buf[0] = '\0';
    if (s_output && lv_obj_is_valid(s_output)) lv_label_set_text(s_output, "");
}

static void runner_ui_apply(void *arg) {
    runner_ui_action_t *action = (runner_ui_action_t *)arg;
    if (!action) return;
    switch (action->type) {
        case RUNNER_UI_SET_TITLE:
            runner_set_title_now(action->text);
            break;
        case RUNNER_UI_PRINT:
            runner_print_now(action->text);
            break;
        case RUNNER_UI_CLEAR:
            runner_clear_now();
            break;
        case RUNNER_UI_TOAST:
            runner_print_now(action->text);
            runner_print_now("\n");
            break;
    }
    free(action);
}

static void runner_post_ui(runner_ui_action_type_t type, const char *text) {
    runner_ui_action_t *action = calloc(1, sizeof(*action));
    if (!action) return;
    action->type = type;
    if (text) {
        strncpy(action->text, text, sizeof(action->text) - 1);
    }
    display_manager_run_on_lvgl(runner_ui_apply, action);
}

static void runner_api_set_title(const char *title) {
    runner_post_ui(RUNNER_UI_SET_TITLE, title);
}

static void runner_api_print(const char *text) {
    runner_post_ui(RUNNER_UI_PRINT, text);
}

static void runner_api_clear(void) {
    runner_post_ui(RUNNER_UI_CLEAR, NULL);
}

static void runner_api_toast(const char *message) {
    runner_post_ui(RUNNER_UI_TOAST, message);
}

static const char *runner_friendly_load_error(esp_err_t err, const char *detail) {
    if (err == ESP_ERR_NO_MEM) return "Not enough memory for app";
    if (err == ESP_ERR_NOT_SUPPORTED) {
        if (detail && strstr(detail, "PSRAM")) return "App requires PSRAM";
        return "App not supported here";
    }
    if (err == ESP_ERR_INVALID_VERSION || err == ESP_ERR_INVALID_SIZE) return "App SDK mismatch";
    if (err == ESP_ERR_NOT_FOUND) return "App not found on SD";
    if (err == ESP_ERR_INVALID_STATE) return "SD storage not mounted";
    return "App launch failed";
}

static void runner_show_load_error_toast(esp_err_t err) {
    const char *detail = plugin_loader_last_error();
    const char *summary = runner_friendly_load_error(err, detail);
    char msg[TOAST_MAX_TEXT_LEN + 1];

    if (detail && detail[0]) {
        snprintf(msg, sizeof(msg), "%s: %.36s", summary, detail);
    } else {
        snprintf(msg, sizeof(msg), "%s", summary);
    }
    toast_show_duration(msg, TOAST_ERROR, 2800);
}

static bool s_sd_eject_detected = false;

static void plugin_runner_launch_pending(void);

static void plugin_runner_tick_cb(lv_timer_t *timer) {
    (void)timer;
    if (s_sd_eject_detected) return;
    plugin_loaded_app_t *loaded = plugin_loader_current();
    if (!loaded || !loaded->running || loaded->state != PLUGIN_APP_STATE_RUNNING) return;
    if (!sd_card_manager.is_initialized) {
        ESP_LOGW(TAG, "SD card removed while app running, stopping");
        s_sd_eject_detected = true;
        if (loaded->app && loaded->app->on_stop) loaded->app->on_stop();
        loaded->running = false;
        loaded->state = PLUGIN_APP_STATE_LOADED;
        display_manager_switch_view(&apps_menu_view);
        return;
    }
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t elapsed_ms = loaded->last_tick_ms ? now_ms - loaded->last_tick_ms : PLUGIN_RUNNER_TICK_MS;
    loaded->last_tick_ms = now_ms;
    plugin_loader_tick(loaded, elapsed_ms);
}

void plugin_runner_set_app(const char *app_id) {
    s_pending_app_id[0] = '\0';
    if (app_id) {
        strncpy(s_pending_app_id, app_id, sizeof(s_pending_app_id) - 1);
    }
}

static ghostesp_input_event_t convert_input(const InputEvent *event) {
    ghostesp_input_event_t out = { .type = GHOSTESP_INPUT_NONE };
    if (!event) return out;
    switch (event->type) {
        case INPUT_TYPE_JOYSTICK:
            out.pressed = event->data.joystick_pressed;
            switch (event->data.joystick_index) {
                case 0: out.type = GHOSTESP_INPUT_LEFT; break;
                case 1: out.type = GHOSTESP_INPUT_SELECT; break;
                case 2: out.type = GHOSTESP_INPUT_UP; break;
                case 3: out.type = GHOSTESP_INPUT_RIGHT; break;
                case 4: out.type = GHOSTESP_INPUT_DOWN; break;
                default: out.type = GHOSTESP_INPUT_NONE; break;
            }
            break;
        case INPUT_TYPE_KEYBOARD:
            out.type = GHOSTESP_INPUT_KEY;
            out.value = event->data.key_value;
            if (event->data.key_value == LV_KEY_ESC || event->data.key_value == '`') out.type = GHOSTESP_INPUT_BACK;
            break;
        case INPUT_TYPE_TOUCH:
            out.type = GHOSTESP_INPUT_TOUCH;
            out.x = event->data.touch_data.point.x;
            out.y = event->data.touch_data.point.y;
            out.pressed = event->data.touch_data.state == LV_INDEV_STATE_PR;
            break;
        case INPUT_TYPE_ENCODER:
            out.type = event->data.encoder.button ? GHOSTESP_INPUT_SELECT : (event->data.encoder.direction > 0 ? GHOSTESP_INPUT_RIGHT : GHOSTESP_INPUT_LEFT);
            out.value = event->data.encoder.direction;
            break;
        case INPUT_TYPE_EXIT_BUTTON:
            out.type = GHOSTESP_INPUT_BACK;
            out.pressed = event->data.exit_pressed;
            break;
    }
    return out;
}

static bool point_in_obj(lv_obj_t *obj, const lv_point_t *point) {
    if (!obj || !point || !lv_obj_is_valid(obj)) return false;
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return point->x >= area.x1 && point->x <= area.x2 && point->y >= area.y1 && point->y <= area.y2;
}

static lv_obj_t *find_clickable_at(lv_obj_t *obj, const lv_point_t *point) {
    if (!obj || !point || !lv_obj_is_valid(obj)) return NULL;
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) || !point_in_obj(obj, point)) return NULL;

    uint32_t child_count = lv_obj_get_child_cnt(obj);
    for (int32_t i = (int32_t)child_count - 1; i >= 0; --i) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        lv_obj_t *hit = find_clickable_at(child, point);
        if (hit) return hit;
    }

    return lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) ? obj : NULL;
}

static lv_obj_t *find_deepest_at(lv_obj_t *obj, const lv_point_t *point) {
    if (!obj || !point || !lv_obj_is_valid(obj)) return NULL;
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) || !point_in_obj(obj, point)) return NULL;

    uint32_t child_count = lv_obj_get_child_cnt(obj);
    for (int32_t i = (int32_t)child_count - 1; i >= 0; --i) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        lv_obj_t *hit = find_deepest_at(child, point);
        if (hit) return hit;
    }

    return obj;
}

static lv_obj_t *find_scrollable_at(const lv_point_t *point) {
    lv_obj_t *obj = find_deepest_at(s_root, point);
    while (obj && lv_obj_is_valid(obj)) {
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE)) return obj;
        obj = lv_obj_get_parent(obj);
    }
    return NULL;
}

static lv_obj_t *find_scrollable_descendant(lv_obj_t *obj) {
    if (!obj || !lv_obj_is_valid(obj) || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE)) {
        if (lv_obj_get_scroll_top(obj) > 0 || lv_obj_get_scroll_bottom(obj) > 0) return obj;
    }
    uint32_t cnt = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *found = find_scrollable_descendant(lv_obj_get_child(obj, i));
        if (found) return found;
    }
    return NULL;
}

static bool plugin_runner_handle_touch(const InputEvent *event) {
    if (!event || event->type != INPUT_TYPE_TOUCH) return false;

    const lv_indev_data_t *data = &event->data.touch_data;
    if (data->state == LV_INDEV_STATE_PR) {
        s_touch_started = true;
        s_touch_start = data->point;
        return false;
    }

    if (data->state != LV_INDEV_STATE_REL || !s_touch_started) return false;
    s_touch_started = false;

    int dx = data->point.x - s_touch_start.x;
    int dy = data->point.y - s_touch_start.y;
    if (abs(dy) > PLUGIN_RUNNER_SCROLL_THRESHOLD && abs(dy) >= abs(dx)) {
        lv_obj_t *target = find_scrollable_at(&s_touch_start);
        if (!target) target = find_scrollable_at(&data->point);
        if (target) {
            lv_obj_scroll_by_bounded(target, 0, dy, LV_ANIM_OFF);
            return true;
        }
        return false;
    }
    if (abs(dx) > PLUGIN_RUNNER_TAP_THRESHOLD || abs(dy) > PLUGIN_RUNNER_TAP_THRESHOLD) return false;

    lv_obj_t *target = find_clickable_at(s_root, &data->point);
    if (!target) return false;

    lv_event_send(target, LV_EVENT_CLICKED, NULL);
    return true;
}

static void plugin_runner_event_handler(InputEvent *event) {
    if (!event) return;
    ghostesp_input_event_t app_event = convert_input(event);
    if (app_event.type == GHOSTESP_INPUT_BACK) {
        display_manager_switch_view(&apps_menu_view);
        return;
    }
    if (plugin_runner_handle_touch(event)) return;
    if (event->type == INPUT_TYPE_ENCODER && !event->data.encoder.button && s_root && lv_obj_is_valid(s_root)) {
        lv_obj_t *scrollable = find_scrollable_descendant(s_root);
        if (scrollable) {
            int dy = event->data.encoder.direction > 0 ? -40 : 40;
            lv_obj_scroll_by_bounded(scrollable, 0, dy, LV_ANIM_ON);
        }
    }
    plugin_loaded_app_t *loaded = plugin_loader_current();
    if (loaded && loaded->running && loaded->app && loaded->app->on_input) {
        loaded->app->on_input(&app_event);
    }
}

static void plugin_runner_launch_cb(lv_timer_t *timer) {
    (void)timer;
    s_launch_timer = NULL;
    plugin_runner_launch_pending();
}

static void plugin_runner_launch_pending(void) {
    if (s_pending_app_id[0] == '\0') {
        runner_set_title_now("No app selected");
        runner_print_now("Return to Apps and select an SD app.\n");
        toast_show_duration("No SD app selected", TOAST_WARN, 1800);
        return;
    }

    plugin_loaded_app_t *loaded = NULL;
    esp_err_t err = plugin_loader_load(s_pending_app_id, &loaded);
    if (err != ESP_OK) {
        runner_set_title_now("Launch Failed");
        runner_print_now(plugin_loader_last_error());
        runner_print_now("\n");
        runner_show_load_error_toast(err);
        return;
    }

    runner_set_title_now(loaded->manifest && loaded->manifest->valid ? loaded->manifest->name : "SD App");
    ESP_LOGI(TAG, "Starting app %s", s_pending_app_id);
    toast_show_duration("Starting app...", TOAST_INFO, 1000);
    err = plugin_loader_start(loaded);
    if (err != ESP_OK) {
        runner_set_title_now("Start Failed");
        runner_print_now(plugin_loader_last_error());
        runner_print_now("\n");
        runner_show_load_error_toast(err);
        return;
    }
    if (loaded->app && loaded->app->on_tick && !s_tick_timer) {
        s_tick_timer = lv_timer_create(plugin_runner_tick_cb, PLUGIN_RUNNER_TICK_MS, NULL);
    }
}

void plugin_runner_view_create(void) {
    if (s_tick_timer) {
        lv_timer_del(s_tick_timer);
        s_tick_timer = NULL;
    }
    if (s_launch_timer) {
        lv_timer_del(s_launch_timer);
        s_launch_timer = NULL;
    }
    if (!s_output_buf) {
        s_output_buf = malloc(PLUGIN_RUNNER_OUTPUT_BUF_SIZE);
        if (!s_output_buf) return;
        s_output_buf[0] = '\0';
    }
    s_sd_eject_detected = false;
    s_output_buf[0] = '\0';
    s_touch_started = false;
    s_root = gui_screen_create_root(NULL, "SD App", lv_color_hex(0x121212), LV_OPA_COVER);
    plugin_runner_view.root = s_root;
    display_manager_add_status_bar("SD App");

    lv_obj_t *content = gui_screen_create_content(s_root, GUI_STATUS_BAR_HEIGHT);
    lv_obj_set_style_pad_all(content, 8, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_title = lv_label_create(content);
    lv_label_set_text(s_title, "Loading SD app...");
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_16, 0);

    s_output = lv_label_create(content);
    lv_label_set_text(s_output, "");
    lv_label_set_long_mode(s_output, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_output, LV_PCT(100));
    lv_obj_set_style_text_align(s_output, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_output, lv_color_hex(0xD0D0D0), 0);
    lv_obj_set_style_text_font(s_output, &lv_font_montserrat_12, 0);

    plugin_api_set_ui_hooks(runner_api_set_title, runner_api_print, runner_api_clear, runner_api_toast);

    if (s_pending_app_id[0] == '\0') {
        runner_set_title_now("No app selected");
        runner_print_now("Return to Apps and select an SD app.\n");
        toast_show_duration("No SD app selected", TOAST_WARN, 1800);
        return;
    }

    toast_show_duration("Loading SD app...", TOAST_INFO, 1200);
    s_launch_timer = lv_timer_create(plugin_runner_launch_cb, 50, NULL);
    if (s_launch_timer) {
        lv_timer_set_repeat_count(s_launch_timer, 1);
    } else {
        plugin_runner_launch_pending();
    }
}

void plugin_runner_stop_tick(void) {
    if (s_tick_timer) {
        lv_timer_del(s_tick_timer);
        s_tick_timer = NULL;
    }
}

void plugin_runner_view_destroy(void) {
    if (s_launch_timer) {
        lv_timer_del(s_launch_timer);
        s_launch_timer = NULL;
    }
    plugin_runner_stop_tick();
    plugin_loader_unload(plugin_loader_current());
    plugin_api_set_ui_hooks(NULL, NULL, NULL, NULL);
    lvgl_obj_del_safe(&s_root);
    s_title = NULL;
    s_output = NULL;
    s_touch_started = false;
    free(s_output_buf);
    s_output_buf = NULL;
    plugin_runner_view.root = NULL;
}

void plugin_runner_get_callback(void **callback) {
    *callback = plugin_runner_event_handler;
}

View plugin_runner_view = {
    .root = NULL,
    .create = plugin_runner_view_create,
    .destroy = plugin_runner_view_destroy,
    .input_callback = plugin_runner_event_handler,
    .name = "SD App",
    .get_hardwareinput_callback = plugin_runner_get_callback,
};
