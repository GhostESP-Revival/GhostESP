#include "managers/views/menu_editor_screen.h"
#include "managers/settings_manager.h"
#include "gui/menu_catalog.h"
#include "gui/options_view.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "gui/design_tokens.h"
#include "gui/lvgl_safe.h"
#include "gui/toast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef enum { EDIT_LIST, EDIT_ADD, EDIT_ITEM, EDIT_RESET } editor_page_t;
static options_view_t *s_options;
static menu_catalog_item_t *s_items;
static int s_count;
static uint8_t s_menu = MENU_PLACE_MAIN;
static editor_page_t s_page;
static menu_catalog_item_t s_item;
static bool s_touch_started;
static touch_drag_t s_drag;

static void build_page(editor_page_t page, int selected);

static menu_catalog_item_t *collect_menu_items(int *count) {
    menu_catalog_item_t *items = menu_catalog_collect(s_menu, false, count);
    if (s_menu == MENU_PLACE_APPS && items) {
        int visible = 0;
        for (int i = 0; i < *count; ++i) {
            if (!menu_catalog_is_grouped_plugin(&items[i])) items[visible++] = items[i];
        }
        *count = visible;
    }
    return items;
}

static void add_row(const char *text) {
    /* Input is dispatched centrally, like the other options-based screens. */
    options_view_add_item(s_options, text, NULL, NULL);
}

static void editor_back(void) {
    if (s_page == EDIT_LIST) display_manager_go_back();
    else {
        char id[MENU_CONFIG_ID_LEN];
        snprintf(id, sizeof(id), "%s", s_item.id);
        build_page(EDIT_LIST, 0);
        for (int i = 0; i < s_count; ++i) {
            if (strcmp(id, s_items[i].id) == 0) options_view_set_selected(s_options, i);
        }
    }
}

static bool commit_placement(uint8_t placement) {
    if (strcmp(s_item.id, "apps") == 0 && placement != MENU_PLACE_MAIN) {
        toast_show("Apps stays on the main menu", TOAST_INFO);
        return false;
    }
    if (strcmp(s_item.id, "settings") == 0 && placement == MENU_PLACE_APPS) {
        toast_show("Settings must stay on the main menu", TOAST_INFO);
        return false;
    }
    if (!menu_config_set(&G_Settings.menu_config, s_item.id, placement)) {
        toast_show("Menu customization storage is full", TOAST_WARN);
        return false;
    }
    settings_persist_setting(SETTING_MENU_CONFIG);
    return true;
}

static void move_item(int delta) {
    int count = 0;
    menu_catalog_item_t *items = collect_menu_items(&count);
    if (!items) { toast_show("Not enough memory", TOAST_ERROR); return; }
    int index = -1;
    for (int i = 0; i < count; ++i) if (strcmp(items[i].id, s_item.id) == 0) index = i;
    if (index < 0 || index + delta < 0 || index + delta >= count) {
        free(items);
        toast_show(delta < 0 ? "Already first" : "Already last", TOAST_INFO);
        return;
    }
    menu_catalog_item_t swap = items[index];
    items[index] = items[index + delta];
    items[index + delta] = swap;
    /* Stage the whole reorder so a full preference table cannot half-apply it. */
    menu_config_t *draft = malloc(sizeof(*draft));
    if (!draft) { free(items); toast_show("Not enough memory", TOAST_ERROR); return; }
    *draft = G_Settings.menu_config;
    bool ok = count <= MENU_CONFIG_MAX;
    for (int i = 0; ok && i < count; ++i) {
        ok = menu_config_order(draft, items[i].id, items[i].default_placement, s_menu, i);
    }
    if (ok) {
        G_Settings.menu_config = *draft;
        settings_persist_setting(SETTING_MENU_CONFIG);
    }
    free(draft);
    free(items);
    if (ok) build_page(EDIT_LIST, index + delta);
    else toast_show("Menu customization storage is full", TOAST_WARN);
}

static void activate(int row) {
    if (row < 0 || row >= options_view_get_item_count(s_options)) return;
    if (s_page == EDIT_LIST) {
        if (row < s_count) { s_item = s_items[row]; build_page(EDIT_ITEM, 0); }
        else if (row == s_count) build_page(EDIT_ADD, 0);
        else if (row == s_count + 1) build_page(EDIT_RESET, 1);
        else editor_back();
    } else if (s_page == EDIT_ADD) {
        if (row >= s_count) { editor_back(); return; }
        s_item = s_items[row];
        uint8_t placement = menu_config_placement(&G_Settings.menu_config, s_item.id, s_item.default_placement);
        if (commit_placement(placement | s_menu)) build_page(EDIT_ITEM, 0);
    } else if (s_page == EDIT_RESET) {
        if (row == 0) {
            menu_config_reset(&G_Settings.menu_config);
            settings_persist_setting(SETTING_MENU_CONFIG);
        }
        build_page(EDIT_LIST, 0);
    } else {
        if (row == 0) move_item(-1);
        else if (row == 1) move_item(1);
        else if (row >= 2 && row <= 4) {
            if (commit_placement(row - 1)) editor_back();
        } else editor_back();
    }
}

static void build_page(editor_page_t page, int selected) {
    s_page = page;
    s_touch_started = false;
    touch_drag_reset(&s_drag);
    options_view_clear(s_options);
    free(s_items);
    s_items = NULL;
    s_count = 0;
    if (page == EDIT_LIST || page == EDIT_ADD) {
        s_items = page == EDIT_LIST ? collect_menu_items(&s_count) : menu_catalog_collect(0, false, &s_count);
        if (page == EDIT_ADD) {
            int count = 0;
            for (int i = 0; i < s_count; ++i) {
                uint8_t placement = menu_config_placement(&G_Settings.menu_config, s_items[i].id, s_items[i].default_placement);
                bool grouped = s_menu == MENU_PLACE_APPS && menu_catalog_is_grouped_plugin(&s_items[i]);
                if ((placement & s_menu) && !grouped) continue;
                if (strcmp(s_items[i].id, "apps") == 0) continue;
                s_items[count++] = s_items[i];
            }
            s_count = count;
        }
        options_view_set_title(s_options, page == EDIT_ADD ? "Add to Menu" :
                               s_menu == MENU_PLACE_MAIN ? "Main Menu Items" : "Apps Gallery Items");
        for (int i = 0; i < s_count; ++i) {
            char label[100];
            uint8_t placement = menu_config_placement(&G_Settings.menu_config, s_items[i].id, s_items[i].default_placement);
            snprintf(label, sizeof(label), "%s%s", s_items[i].name, placement == MENU_PLACE_BOTH ? " (both)" : "");
            add_row(label);
        }
        if (!s_items) toast_show("Not enough memory to list items", TOAST_ERROR);
        else if (page == EDIT_ADD && !s_count) toast_show("All items are already in this menu", TOAST_INFO);
        if (page == EDIT_LIST) {
            add_row("+ Add item");
            add_row("Reset both menus...");
        }
        add_row(LV_SYMBOL_LEFT " Back");
    } else if (page == EDIT_RESET) {
        options_view_set_title(s_options, "Reset menu arrangement?");
        add_row("Reset both menus");
        add_row("Cancel");
    } else {
        options_view_set_title(s_options, s_item.name);
        add_row(LV_SYMBOL_UP " Move up");
        add_row(LV_SYMBOL_DOWN " Move down");
        uint8_t placement = menu_config_placement(&G_Settings.menu_config, s_item.id, s_item.default_placement);
        add_row(placement == MENU_PLACE_MAIN ? LV_SYMBOL_OK " Main Menu only" : "Main Menu only");
        add_row(placement == MENU_PLACE_APPS ? LV_SYMBOL_OK " Apps Gallery only" : "Apps Gallery only");
        add_row(placement == MENU_PLACE_BOTH ? LV_SYMBOL_OK " Both menus" : "Both menus");
        add_row(LV_SYMBOL_LEFT " Back");
    }
    options_view_set_selected(s_options, selected);
}

static void editor_input(InputEvent *event) {
    if (!s_options || !event) return;
    if (event->type == INPUT_TYPE_EXIT_BUTTON) { editor_back(); return; }
    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *data = &event->data.touch_data;
        lv_obj_t *list = options_view_get_list(s_options);
        lv_area_t area;
        lv_obj_get_coords(list, &area);
        bool inside = data->point.x >= area.x1 && data->point.x <= area.x2 &&
                      data->point.y >= area.y1 && data->point.y <= area.y2;
        if (event->is_touch_move) {
            if (s_touch_started) touch_drag_update(&s_drag, data, list);
        } else if (data->state == LV_INDEV_STATE_PR) {
            s_touch_started = inside;
            if (inside) touch_drag_begin(&s_drag, data);
        } else if (data->state == LV_INDEV_STATE_REL && s_touch_started) {
            s_touch_started = false;
            /* Some drivers coalesce all motion into the final release. */
            touch_drag_update(&s_drag, data, list);
            bool dragged = touch_drag_release(&s_drag, data);
            display_manager_flush_pending_scroll();
            if (dragged || !inside) return;
            for (int i = 0; i < options_view_get_item_count(s_options); ++i) {
                lv_obj_t *btn = lv_obj_get_child(list, i);
                lv_area_t hit;
                lv_obj_get_coords(btn, &hit);
                if (data->point.x >= hit.x1 && data->point.x <= hit.x2 &&
                    data->point.y >= hit.y1 && data->point.y <= hit.y2) { activate(i); break; }
            }
        }
        return;
    }
    int delta = 0;
    bool select = false, back = false;
    if (event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_pressed) {
        delta = event->data.joystick_index == 2 ? -1 : event->data.joystick_index == 4 ? 1 : 0;
        select = event->data.joystick_index == 1 || event->data.joystick_index == 3;
        back = event->data.joystick_index == 0;
    } else if (event->type == INPUT_TYPE_ENCODER) {
        select = event->data.encoder.button;
        if (!select) delta = event->data.encoder.direction > 0 ? 1 : event->data.encoder.direction < 0 ? -1 : 0;
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        int key = event->data.key_value;
        if (key == LV_KEY_UP || key == 'k' || key == ';') delta = -1;
        if (key == LV_KEY_DOWN || key == 'j' || key == '.') delta = 1;
        select = key == LV_KEY_ENTER || key == '\r' || key == '\n';
        back = key == LV_KEY_ESC || key == 29 || key == '`' || key == '\b';
    }
    if (back) editor_back();
    else if (select) activate(options_view_get_selected(s_options));
    else if (delta) options_view_move_selection(s_options, delta);
}

static void editor_create(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    menu_editor_view.root = gui_screen_create_root(NULL, NULL, lv_color_hex(theme_palette_get_background(theme)), LV_OPA_TRANSP);
    s_options = options_view_create(menu_editor_view.root, "Menu Items");
    if (!s_options) { toast_show("Not enough memory", TOAST_ERROR); return; }
    build_page(EDIT_LIST, 0);
}

static void editor_destroy(void) {
    free(s_items);
    s_items = NULL;
    s_count = 0;
    options_view_destroy(s_options);
    s_options = NULL;
    lvgl_obj_del_safe(&menu_editor_view.root);
    s_touch_started = false;
    touch_drag_reset(&s_drag);
}

static void get_callback(void **callback) { *callback = editor_input; }

void menu_editor_open(uint8_t menu) {
    s_menu = menu == MENU_PLACE_APPS ? MENU_PLACE_APPS : MENU_PLACE_MAIN;
    display_manager_switch_view(&menu_editor_view);
}

View menu_editor_view = {
    .create = editor_create,
    .destroy = editor_destroy,
    .input_callback = editor_input,
    .get_hardwareinput_callback = get_callback,
    .name = "Menu Editor",
};
