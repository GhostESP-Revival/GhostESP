#include "managers/views/ghostscript_browser_view.h"

#include "gui/lvgl_safe.h"
#include "gui/options_view.h"
#include "gui/screen_layout.h"
#include "gui/toast.h"
#include "managers/ghostscript_manager.h"
#include "managers/views/app_gallery_screen.h"
#include "managers/views/ghostscript_runner_view.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { ROW_ENTRY, ROW_UP, ROW_PREV, ROW_NEXT, ROW_REFRESH, ROW_BACK } row_type_t;

typedef struct { row_type_t type; int index; } row_t;

static lv_obj_t *s_root;
static options_view_t *s_opts;
static char *s_dir;
static ghostscript_browser_entry_t *s_entries;
static row_t *s_rows;
static int s_count;
static int s_row_count;
static int s_offset;
static bool s_has_more;
static bool s_touch_started;
static lv_point_t s_touch_start;

#define BROWSER_TAP_THRESHOLD 12
#define BROWSER_SCROLL_THRESHOLD 16

static void refresh(void);

static bool is_root(void) { return s_dir && strcmp(s_dir, GHOSTSCRIPT_ROOT_DIR) == 0; }

static void parent_dir(void) {
    if (is_root()) return;
    char *slash = strrchr(s_dir, '/');
    if (!slash || slash <= s_dir + strlen(GHOSTSCRIPT_ROOT_DIR)) snprintf(s_dir, GHOSTSCRIPT_PATH_MAX, "%s", GHOSTSCRIPT_ROOT_DIR);
    else *slash = '\0';
}

static void activate_row(int selected) {
    if (selected < 0 || selected >= s_row_count) return;
    row_t row = s_rows[selected];
    if (row.type == ROW_BACK) { display_manager_switch_view(&apps_menu_view); return; }
    if (row.type == ROW_UP) { parent_dir(); s_offset = 0; refresh(); return; }
    if (row.type == ROW_PREV) { s_offset -= GHOSTSCRIPT_BROWSER_PAGE_SIZE; if (s_offset < 0) s_offset = 0; refresh(); return; }
    if (row.type == ROW_NEXT) { s_offset += GHOSTSCRIPT_BROWSER_PAGE_SIZE; refresh(); return; }
    if (row.type == ROW_REFRESH) { refresh(); return; }
    if (row.type == ROW_ENTRY && row.index >= 0 && row.index < s_count) {
        ghostscript_browser_entry_t *entry = &s_entries[row.index];
        if (entry->is_dir && !entry->has_manifest) {
            snprintf(s_dir, GHOSTSCRIPT_PATH_MAX, "%s", entry->path);
            s_offset = 0;
            refresh();
        } else {
            ghostscript_runner_set_script(entry->path);
            display_manager_switch_view(&ghostscript_runner_view);
        }
    }
}

static void row_click(lv_event_t *e) {
    lv_obj_t *target = lv_event_get_target(e);
    int selected = options_view_get_selected(s_opts);
    for (int i = 0; i < s_row_count; ++i) {
        if (target == lv_obj_get_child(options_view_get_list(s_opts), i)) { selected = i; break; }
    }
    activate_row(selected);
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

static lv_obj_t *find_scrollable_at(const lv_point_t *point) {
    lv_obj_t *obj = find_clickable_at(s_root, point);
    while (obj && lv_obj_is_valid(obj)) {
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE)) return obj;
        obj = lv_obj_get_parent(obj);
    }
    return NULL;
}

static bool browser_handle_touch(InputEvent *event) {
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
    if (abs(dy) > BROWSER_SCROLL_THRESHOLD && abs(dy) >= abs(dx)) {
        lv_obj_t *target = find_scrollable_at(&s_touch_start);
        if (!target) target = find_scrollable_at(&data->point);
        if (target) {
            lv_obj_scroll_by_bounded(target, 0, dy, LV_ANIM_OFF);
            return true;
        }
        return false;
    }
    if (abs(dx) > BROWSER_TAP_THRESHOLD || abs(dy) > BROWSER_TAP_THRESHOLD) return false;

    lv_obj_t *target = find_clickable_at(s_root, &data->point);
    if (target) {
        lv_event_send(target, LV_EVENT_CLICKED, NULL);
        return true;
    }
    return false;
}

static void add_row(row_type_t type, int index, const char *label) {
    if (s_row_count >= GHOSTSCRIPT_BROWSER_PAGE_SIZE + 5) return;
    s_rows[s_row_count].type = type;
    s_rows[s_row_count].index = index;
    options_view_add_item(s_opts, label, row_click, NULL);
    s_row_count++;
}

static void refresh(void) {
    if (!s_opts) return;
    options_view_clear(s_opts);
    s_row_count = 0;
    char title[96];
    snprintf(title, sizeof(title), "GhostScript: %s", is_root() ? "Scripts" : strrchr(s_dir, '/') + 1);
    options_view_set_title(s_opts, title);
    s_count = ghostscript_manager_list(s_dir, s_offset, s_entries, GHOSTSCRIPT_BROWSER_PAGE_SIZE, &s_has_more);
    if (!is_root()) add_row(ROW_UP, -1, LV_SYMBOL_UP " ..");
    if (s_count == 0 && ghostscript_manager_last_error()[0]) {
        add_row(ROW_REFRESH, -1, ghostscript_manager_last_error());
    }
    for (int i = 0; i < s_count; ++i) {
        char label[160];
        const char *prefix = s_entries[i].is_dir ? (s_entries[i].has_manifest ? LV_SYMBOL_PLAY " " : LV_SYMBOL_DIRECTORY " ") : LV_SYMBOL_FILE " ";
        snprintf(label, sizeof(label), "%s%s", prefix, s_entries[i].name);
        add_row(ROW_ENTRY, i, label);
    }
    if (s_offset > 0) add_row(ROW_PREV, -1, LV_SYMBOL_LEFT " Previous Page");
    if (s_has_more) add_row(ROW_NEXT, -1, "Next Page " LV_SYMBOL_RIGHT);
    add_row(ROW_REFRESH, -1, LV_SYMBOL_REFRESH " Refresh");
    add_row(ROW_BACK, -1, LV_SYMBOL_LEFT " Back");
    options_view_set_selected(s_opts, 0);
    options_view_trigger_wipe(s_opts);
}

static bool is_back(InputEvent *event) {
    if (event->type == INPUT_TYPE_EXIT_BUTTON) return true;
    if (event->type == INPUT_TYPE_KEYBOARD && (event->data.key_value == LV_KEY_ESC || event->data.key_value == '`')) return true;
    if (event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_pressed && event->data.joystick_index == 0) return true;
    if (event->type == INPUT_TYPE_ENCODER && event->data.encoder.button && event->data.encoder.direction == 0) return true;
    return false;
}

static void event_handler(InputEvent *event) {
    if (!event || !s_opts) return;
    if (is_back(event)) {
        display_manager_switch_view(&apps_menu_view);
        return;
    }
    if (browser_handle_touch(event)) return;
    if (event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_pressed) {
        if (event->data.joystick_index == 2) options_view_move_selection(s_opts, -1);
        else if (event->data.joystick_index == 4) options_view_move_selection(s_opts, 1);
        else if (event->data.joystick_index == 1) activate_row(options_view_get_selected(s_opts));
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) activate_row(options_view_get_selected(s_opts));
        else options_view_move_selection(s_opts, event->data.encoder.direction > 0 ? 1 : -1);
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        if (event->data.key_value == LV_KEY_UP) options_view_move_selection(s_opts, -1);
        else if (event->data.key_value == LV_KEY_DOWN) options_view_move_selection(s_opts, 1);
        else if (event->data.key_value == LV_KEY_ENTER) activate_row(options_view_get_selected(s_opts));
    }
}

void ghostscript_browser_view_create(void) {
    ghostscript_manager_init();
    s_dir = malloc(GHOSTSCRIPT_PATH_MAX);
    s_entries = calloc(GHOSTSCRIPT_BROWSER_PAGE_SIZE, sizeof(*s_entries));
    s_rows = calloc(GHOSTSCRIPT_BROWSER_PAGE_SIZE + 5, sizeof(*s_rows));
    if (!s_dir || !s_entries || !s_rows) return;
    snprintf(s_dir, GHOSTSCRIPT_PATH_MAX, "%s", GHOSTSCRIPT_ROOT_DIR);
    s_root = gui_screen_create_root(NULL, "GhostScript", lv_color_black(), LV_OPA_COVER);
    ghostscript_browser_view.root = s_root;
    s_opts = options_view_create(s_root, "GhostScript");
    s_offset = 0;
    s_touch_started = false;
    refresh();
}

void ghostscript_browser_view_destroy(void) {
    if (s_opts) { options_view_destroy(s_opts); s_opts = NULL; }
    lvgl_obj_del_safe(&s_root);
    free(s_dir); s_dir = NULL;
    free(s_entries); s_entries = NULL;
    free(s_rows); s_rows = NULL;
    ghostscript_browser_view.root = NULL;
}

static void get_cb(void **callback) { *callback = event_handler; }

View ghostscript_browser_view = {
    .root = NULL,
    .create = ghostscript_browser_view_create,
    .destroy = ghostscript_browser_view_destroy,
    .input_callback = event_handler,
    .name = "GhostScript",
    .get_hardwareinput_callback = get_cb,
};
