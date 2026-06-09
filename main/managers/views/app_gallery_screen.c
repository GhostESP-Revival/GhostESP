#include "managers/views/app_gallery_screen.h"
#include "managers/views/ghostchi_screen.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/music_visualizer.h"
#include "managers/views/offline_map_screen.h"
#include "managers/views/plugin_runner_view.h"
#include "managers/views/sd_browser_screen.h"
#include "managers/views/terminal_screen.h"
#ifdef CONFIG_HAS_COMPASS
#include "managers/views/compass_screen.h"
#endif
#ifdef CONFIG_HAS_ENVIII
#include "managers/views/enviii_screen.h"
#endif
#ifdef CONFIG_HAS_ACCELEROMETER
#include "managers/views/accelerometer_screen.h"
#endif
#include "managers/views/clock_screen.h"
#ifdef CONFIG_HAS_AUDIO_PLAYER
#include "managers/views/audio_player_screen.h"
#endif

LV_IMG_DECLARE(speaker_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48);
LV_IMG_DECLARE(enviii);
LV_IMG_DECLARE(accelerometer_icon);

#include "managers/plugin_manager.h"
#include "managers/settings_manager.h"
#include "gui/accessibility_fonts.h"
#include "gui/asset_pack.h"
#include "gui/main_menu_layout.h"
#include "gui/theme_palette_api.h"
#include "gui/lvgl_safe.h"
#include "gui/screen_layout.h"
#include "gui/design_tokens.h"
#include "gui/toast.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "core/memory_debug.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

uint32_t theme_palette_get_background(uint8_t theme);
uint32_t theme_palette_get_surface(uint8_t theme);
uint32_t theme_palette_get_text(uint8_t theme);

static const char *TAG = "AppGalleryScreen";

static inline int get_app_anim_duration(void) {
    return settings_get_reduced_motion(&G_Settings) ? 0 : 60;
}

#define ANIM_DURATION get_app_anim_duration()

static void select_app_item(int index, bool slide_left);
static void apps_plugin_reload_done(void *arg);

lv_obj_t *apps_container;
static lv_obj_t *apps_root = NULL;
static lv_obj_t *current_app_obj = NULL;
static int selected_app_index = 0;

typedef struct {
    const char *name;
    const char *asset_key;
    const char *symbol_icon;
    const lv_img_dsc_t *icon;
    int palette_index;
    lv_color_t border_color;
    View *view;
    char plugin_id[PLUGIN_APP_ID_MAX];
    char accent_color[PLUGIN_APP_ACCENT_COLOR_MAX];
    bool disabled;
} app_item_t;

static const app_item_t builtin_app_items[] = {
    {
        .name = "Visualizer",
        .asset_key = "rave",
        .icon = &rave,
        .palette_index = 4,
        .view = &music_visualizer_view,
    },
#ifdef CONFIG_HAS_AUDIO_PLAYER
    {
        .name = "Audio",
        .asset_key = "speaker_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48",
        .icon = &speaker_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48,
        .palette_index = 3,
        .view = &audio_player_view,
    },
#endif
    {
        .name = "Terminal",
        .asset_key = "terminal_icon",
        .icon = &terminal_icon,
        .palette_index = 5,
        .view = &terminal_view,
    },
    {
        .name = "SD Browser",
        .asset_key = NULL,
        .symbol_icon = LV_SYMBOL_DIRECTORY,
        .palette_index = 1,
        .view = &sd_browser_view,
    },
    {
        .name = "Ghostchi",
        .asset_key = "ghost",
        .icon = &ghost,
        .palette_index = 2,
        .view = &ghostchi_view,
    },
    {
        .name = "Maps",
        .asset_key = "Map",
        .icon = &Map,
        .palette_index = 2,
        .view = &offline_map_view,
    },
    {
        .name = "Clock",
        .asset_key = "clock_icon",
        .icon = &clock_icon,
        .palette_index = 4,
        .view = &clock_view,
    },
#ifdef CONFIG_HAS_COMPASS
    {
        .name = "Compass",
        .asset_key = "compass",
        .icon = &compass,
        .palette_index = 2,
        .view = &compass_view,
    },
#endif
#ifdef CONFIG_HAS_ENVIII
    {
        .name = "ENV-III",
        .asset_key = "enviii",
        .icon = &enviii,
        .palette_index = 2,
        .view = &enviii_view,
    },
#endif
#ifdef CONFIG_HAS_ACCELEROMETER
    {
        .name = "Accelerometer",
        .asset_key = "accelerometer_icon",
        .icon = &accelerometer_icon,
        .palette_index = 4,
        .view = &accelerometer_view,
    },
#endif
};

#define MAX_APP_GALLERY_ITEMS (PLUGIN_APP_MAX_COUNT + 9)
static app_item_t *app_items = NULL;
static int num_apps = 0;
lv_obj_t *back_button = NULL;

// Add navigation button objects
static lv_obj_t *left_nav_btn = NULL;
static lv_obj_t *right_nav_btn = NULL;
static int touch_start_x;
static int touch_start_y;
static int touch_last_x;
static int touch_last_y;
static bool touch_started = false;
static bool touch_dragged = false;
static int touch_drag_axis = 0;
static const int SWIPE_THRESHOLD = 50;
static const int TAP_THRESHOLD = 14;
static const int DRAG_AXIS_THRESHOLD = 14;
static const int DRAG_AXIS_BIAS = 4;
static const int DRAG_DELTA_DEADZONE = 1;
static const int DRAG_MAX_STEP = 36;

static bool menu_item_selected = false;

typedef enum {
    APPS_LAYOUT_CAROUSEL = 0,
    APPS_LAYOUT_GRID_CARDS = 1,
    APPS_LAYOUT_LIST = 2
} AppsLayoutType;

static AppsLayoutType apps_layout = APPS_LAYOUT_CAROUSEL;
static lv_obj_t **apps_grid_cards = NULL;
static lv_obj_t **apps_list_buttons = NULL;
static lv_obj_t *grid_cards_container = NULL;
static int apps_grid_cols = 1;
static lv_color_t apps_bg_color;
static lv_color_t apps_surface_color;
static lv_color_t apps_text_color;
static volatile bool apps_plugin_reload_in_progress = false;
static bool apps_allow_plugin_icon_load = false;
static StackType_t *apps_plugin_reload_stack = NULL;
static StaticTask_t *apps_plugin_reload_tcb = NULL;

#define PLUGIN_RELOAD_STACK_BYTES 32768

static bool ensure_app_items(void) {
    if (app_items) return true;
    app_items = spiram_calloc(MAX_APP_GALLERY_ITEMS, sizeof(*app_items));
    if (!app_items) {
        ESP_LOGE(TAG, "Failed to allocate app gallery items");
        return false;
    }
    return true;
}

static void select_app_item(int index, bool slide_left);

static const lv_img_dsc_t *app_item_icon(int index) {
    if (!app_items || index < 0 || index >= num_apps) return NULL;
    const lv_img_dsc_t *fallback = app_items[index].icon;
    if (apps_allow_plugin_icon_load && app_items[index].plugin_id[0] != '\0') {
        const plugin_app_manifest_t *app = plugin_manager_find(app_items[index].plugin_id);
        const lv_img_dsc_t *plugin_icon = plugin_manager_get_icon(app);
        if (plugin_icon) fallback = plugin_icon;
    }
    const lv_img_dsc_t *icon = asset_pack_get_icon(app_items[index].asset_key, fallback);
    if (icon == fallback && fallback) {
        icon = asset_pack_get_app_icon(fallback);
    }
    return icon;
}

static int clamp_drag_delta(int delta) {
    if (abs(delta) <= DRAG_DELTA_DEADZONE) return 0;
    if (delta > DRAG_MAX_STEP) return DRAG_MAX_STEP;
    if (delta < -DRAG_MAX_STEP) return -DRAG_MAX_STEP;
    return delta;
}

static int resolve_drag_axis(int total_dx, int total_dy) {
    int abs_dx = abs(total_dx);
    int abs_dy = abs(total_dy);
    if (abs_dx < DRAG_AXIS_THRESHOLD && abs_dy < DRAG_AXIS_THRESHOLD) return 0;
    if (abs_dy >= abs_dx + DRAG_AXIS_BIAS) return 1;
    if (abs_dx >= abs_dy + DRAG_AXIS_BIAS) return 2;
    return 0;
}

static bool app_item_icon_should_recolor(int index, const lv_img_dsc_t *icon) {
    if (!app_items || index < 0 || index >= num_apps || !icon) return false;
    if (app_items[index].plugin_id[0] != '\0' && apps_allow_plugin_icon_load) {
        const plugin_app_manifest_t *app = plugin_manager_find(app_items[index].plugin_id);
        const lv_img_dsc_t *plugin_icon = plugin_manager_get_icon(app);
        if (plugin_icon && icon == plugin_icon) {
            const lv_img_dsc_t *pack_icon = asset_pack_get_icon(app_items[index].asset_key, plugin_icon);
            return icon == pack_icon;
        }
    }
    return icon == app_items[index].icon;
}

static const char *app_item_symbol_icon(int index) {
    if (!app_items || index < 0 || index >= num_apps) return NULL;
    return app_items[index].symbol_icon;
}

static lv_obj_t *create_app_symbol_icon(lv_obj_t *parent, const char *symbol, lv_color_t color, const lv_font_t *font) {
    if (!parent || !symbol) return NULL;
    lv_obj_t *icon = lv_label_create(parent);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_color(icon, color, 0);
    lv_obj_set_style_text_font(icon, font ? font : &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return icon;
}

static void refresh_apps_surface_colors(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    apps_bg_color = lv_color_hex(theme_palette_get_background(theme));
    apps_surface_color = lv_color_hex(theme_palette_get_surface(theme));
    apps_text_color = lv_color_hex(theme_palette_get_text(theme));
}

static bool parse_accent_color(const char *text, lv_color_t *out) {
    if (!text || !out) return false;
    if (text[0] == '#') text++;
    if (strlen(text) != 6) return false;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 16);
    if (!end || *end != '\0' || value > 0xFFFFFFUL) return false;
    *out = lv_color_hex((uint32_t)value);
    return true;
}

static void add_back_app_item(void) {
    if (!app_items || num_apps >= MAX_APP_GALLERY_ITEMS) return;
    app_items[num_apps].name = "Back";
    app_items[num_apps].icon = NULL;
    app_items[num_apps].palette_index = 0;
    app_items[num_apps].view = NULL;
    num_apps++;
}

static void add_loaded_plugin_app_items(void) {
    bool has_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0;
    int plugin_count = plugin_manager_count();
    for (int i = 0; i < plugin_count && num_apps < MAX_APP_GALLERY_ITEMS - 1; ++i) {
        const plugin_app_manifest_t *app = plugin_manager_get(i);
        if (!app) continue;
        if (app->requires_psram && !has_psram) continue;
        app_items[num_apps].name = app->name;
        app_items[num_apps].asset_key = NULL;
        app_items[num_apps].icon = &GESPAppGallery;
        app_items[num_apps].palette_index = 3;
        app_items[num_apps].view = &plugin_runner_view;
        app_items[num_apps].disabled = false;
        strncpy(app_items[num_apps].plugin_id, app->id, sizeof(app_items[num_apps].plugin_id) - 1);
        strncpy(app_items[num_apps].accent_color, app->accent_color, sizeof(app_items[num_apps].accent_color) - 1);
        num_apps++;
    }
}

static void rebuild_app_items(bool include_loaded_plugins) {
    num_apps = 0;
    if (!ensure_app_items()) return;
    memset(app_items, 0, sizeof(*app_items) * MAX_APP_GALLERY_ITEMS);

    for (int i = 0; i < (int)(sizeof(builtin_app_items) / sizeof(builtin_app_items[0])) && num_apps < MAX_APP_GALLERY_ITEMS - 1; ++i) {
        app_items[num_apps++] = builtin_app_items[i];
    }

    if (include_loaded_plugins) add_loaded_plugin_app_items();
    add_back_app_item();
}

static void plugin_reload_task_fn(void *arg) {
    (void)arg;
    plugin_manager_reload();
    display_manager_run_on_lvgl(apps_plugin_reload_done, NULL);
    vTaskDelete(NULL);
}

static void start_plugin_reload_async(void) {
    if (apps_plugin_reload_in_progress) {
        toast_show_duration("Still scanning SD apps...", TOAST_INFO, 900);
        return;
    }
    apps_plugin_reload_in_progress = true;
    toast_show_duration("Scanning SD apps...", TOAST_INFO, 1200);
    if (!apps_plugin_reload_stack) {
        apps_plugin_reload_stack = spiram_malloc(PLUGIN_RELOAD_STACK_BYTES);
    }
    if (!apps_plugin_reload_stack) {
        apps_plugin_reload_in_progress = false;
        ESP_LOGE(TAG, "Failed to allocate plugin reload task stack");
        toast_show_duration("App scan failed: no task memory", TOAST_ERROR, 2200);
        return;
    }
    if (!apps_plugin_reload_tcb) {
        apps_plugin_reload_tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!apps_plugin_reload_tcb) {
        apps_plugin_reload_in_progress = false;
        ESP_LOGE(TAG, "Failed to allocate plugin reload TCB");
        toast_show_duration("App scan failed: no task memory", TOAST_ERROR, 2200);
        return;
    }
    if (xTaskCreateStatic(plugin_reload_task_fn, "plugin_reload", PLUGIN_RELOAD_STACK_BYTES, NULL, 5,
                          apps_plugin_reload_stack, apps_plugin_reload_tcb) == NULL) {
        apps_plugin_reload_in_progress = false;
        ESP_LOGE(TAG, "Failed to create plugin reload task");
        toast_show_duration("App scan failed: task start", TOAST_ERROR, 2200);
    }
}

// Use the same theme palettes as the main menu to color app borders
static void init_app_colors(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    refresh_apps_surface_colors();
    for (int i = 0; i < num_apps; ++i) {
        int slot = i % THEME_PALETTE_SLOT_COUNT;
        if (!parse_accent_color(app_items[i].accent_color, &app_items[i].border_color)) {
            app_items[i].border_color = lv_color_hex(theme_palette_get(theme, slot));
        }
    }
}

// Animation callback wrapper
static void anim_set_x(void *obj, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)obj;
    lv_coord_t curr = lv_obj_get_x(o);
    if (curr == (lv_coord_t)v) return;
    lv_obj_set_x(o, (lv_coord_t)v);
}

static bool colors_equal(lv_color_t a, lv_color_t b) {
    return a.full == b.full;
}

static void anim_set_opa(void *obj, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)obj;
    lv_opa_t curr = lv_obj_get_style_opa(o, 0);
    if (curr == (lv_opa_t)v) return;
    lv_obj_set_style_opa(o, v, 0);
}

static bool apps_carousel_next_slide_left = false;
static bool apps_is_animating = false;

typedef struct {
    lv_obj_t *card;
    lv_obj_t *icon;
    lv_obj_t *label;
    const lv_img_dsc_t *icon_src;
    const char *symbol_src;
    const char *label_text;
    lv_color_t border_color;
    bool icon_recolor_enabled;
    bool icon_is_symbol;
    int item_index;
} apps_carousel_cache_t;

static apps_carousel_cache_t apps_carousel_cache = {0};

static void apps_carousel_fade_in_ready_cb(lv_anim_t *a);

static void apps_cancel_carousel_anims(void) {
    if (current_app_obj) lv_anim_del(current_app_obj, NULL);
    if (apps_carousel_cache.card && apps_carousel_cache.card != current_app_obj) {
        lv_anim_del(apps_carousel_cache.card, NULL);
    }
}

static void apps_carousel_fade_out_ready_cb(lv_anim_t *a) {
    lv_obj_t *obj = (lv_obj_t *)a->var;
    if (!obj || !lv_obj_is_valid(obj) || !app_items || num_apps <= 0) {
        apps_is_animating = false;
        return;
    }
    if (selected_app_index < 0 || selected_app_index >= num_apps) {
        apps_is_animating = false;
        return;
    }
    int start_x = apps_carousel_next_slide_left ? LV_HOR_RES : -LV_HOR_RES;

    apps_carousel_cache.card = obj;
    int app_idx = selected_app_index;

    lv_color_t new_border = app_items[app_idx].border_color;
    bool border_changed = !colors_equal(apps_carousel_cache.border_color, new_border);
    if (border_changed) {
        lv_obj_set_style_border_color(obj, new_border, LV_PART_MAIN);
        apps_carousel_cache.border_color = new_border;
    }

    const char *new_symbol = app_item_symbol_icon(app_idx);
    const lv_img_dsc_t *new_icon = new_symbol ? NULL : app_item_icon(app_idx);
    lv_obj_t *icon = apps_carousel_cache.icon;
    if (new_symbol) {
        if (!icon || !lv_obj_is_valid(icon) || !apps_carousel_cache.icon_is_symbol) {
            if (icon && lv_obj_is_valid(icon)) lv_obj_del(icon);
            icon = create_app_symbol_icon(obj, new_symbol, new_border, &lv_font_montserrat_24);
            apps_carousel_cache.icon = icon;
        }
        if (icon) {
            lv_label_set_text(icon, new_symbol);
            lv_obj_set_style_text_color(icon, new_border, 0);
            lv_obj_align(icon, LV_ALIGN_CENTER, 0, -10);
            lv_obj_move_to_index(icon, 0);
        }
        apps_carousel_cache.icon_src = NULL;
        apps_carousel_cache.symbol_src = new_symbol;
        apps_carousel_cache.icon_recolor_enabled = false;
        apps_carousel_cache.icon_is_symbol = true;
    } else {
        if (!icon || !lv_obj_is_valid(icon) || apps_carousel_cache.icon_is_symbol || !lv_obj_check_type(icon, &lv_img_class)) {
            if (icon && lv_obj_is_valid(icon)) lv_obj_del(icon);
            icon = lv_img_create(obj);
            lv_img_set_antialias(icon, false);
            apps_carousel_cache.icon = icon;
        }
        if (icon) {
            if (apps_carousel_cache.icon_src != new_icon) {
                if (new_icon) {
                    lv_img_set_src(icon, new_icon);
                    lv_obj_clear_flag(icon, LV_OBJ_FLAG_HIDDEN);

                    int btn_size = lv_obj_get_width(obj);
                    const int icon_size = 50;
                    lv_coord_t img_w = new_icon->header.w;
                    lv_coord_t img_h = new_icon->header.h;
                    if (img_w > 0 && img_h > 0) {
                        int zoom_w = (icon_size * 256) / img_w;
                        int zoom_h = (icon_size * 256) / img_h;
                        int zoom = LV_MIN(zoom_w, zoom_h);
                        if (zoom > 512) zoom = 512;
                        lv_img_set_zoom(icon, zoom);
                    }
                    int icon_x_offset = -3;
                    int icon_y_offset = -5;
                    if (app_items[app_idx].view == &ghostchi_view) icon_x_offset = 9;
                    int x_pos = (btn_size - icon_size) / 2 + icon_x_offset;
                    int y_pos = (btn_size - icon_size) / 2 + icon_y_offset;
                    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, x_pos, y_pos);
                    lv_obj_move_to_index(icon, 0);
                } else {
                    lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
                }
            }
            apps_carousel_cache.icon_src = new_icon;

            if (new_icon) {
                bool wants_recolor = app_item_icon_should_recolor(app_idx, new_icon);
                if (wants_recolor) {
                    if (!apps_carousel_cache.icon_recolor_enabled || border_changed) {
                        lv_obj_set_style_img_recolor(icon, new_border, 0);
                        lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
                    }
                } else if (apps_carousel_cache.icon_recolor_enabled) {
                    lv_obj_set_style_img_recolor_opa(icon, LV_OPA_TRANSP, 0);
                }
                apps_carousel_cache.icon_recolor_enabled = wants_recolor;
            }
        }
        apps_carousel_cache.symbol_src = NULL;
        apps_carousel_cache.icon_is_symbol = false;
    }

    lv_obj_t *label = apps_carousel_cache.label;
    if (!label || !lv_obj_is_valid(label) || !lv_obj_check_type(label, &lv_label_class)) {
        label = (lv_obj_is_valid(obj) && lv_obj_get_child_cnt(obj) > 1) ? lv_obj_get_child(obj, 1) : NULL;
        if (label && (!lv_obj_is_valid(label) || !lv_obj_check_type(label, &lv_label_class))) label = NULL;
        apps_carousel_cache.label = label;
    }
    const char *new_label = app_items[app_idx].name;
    if (app_items[app_idx].view == NULL) new_label = "< Back";
    if (label && apps_carousel_cache.label_text != new_label) {
        lv_label_set_text(label, new_label);
    }
    apps_carousel_cache.label_text = new_label;
    apps_carousel_cache.item_index = selected_app_index;

    lv_obj_set_x(obj, start_x);
    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);

    lv_anim_t anim_in;
    lv_anim_init(&anim_in);
    lv_anim_set_var(&anim_in, obj);
    lv_anim_set_values(&anim_in, start_x, 0);
    lv_anim_set_time(&anim_in, ANIM_DURATION);
    lv_anim_set_path_cb(&anim_in, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&anim_in, anim_set_x);
    lv_anim_start(&anim_in);

    lv_anim_t fade_in;
    lv_anim_init(&fade_in);
    lv_anim_set_var(&fade_in, obj);
    lv_anim_set_values(&fade_in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&fade_in, ANIM_DURATION);
    lv_anim_set_exec_cb(&fade_in, anim_set_opa);
    lv_anim_set_ready_cb(&fade_in, apps_carousel_fade_in_ready_cb);
    lv_anim_start(&fade_in);
}

static void apps_carousel_fade_in_ready_cb(lv_anim_t *a) {
    (void)a;
    apps_is_animating = false;
}

static void apps_cleanup_layout(void) {
    if (apps_grid_cards) {
        free(apps_grid_cards);
        apps_grid_cards = NULL;
    }
    if (apps_list_buttons) {
        free(apps_list_buttons);
        apps_list_buttons = NULL;
    }
    grid_cards_container = NULL;
}

static void update_app_item(bool slide_left) {
    if (!app_items || num_apps <= 0) return;
    apps_carousel_next_slide_left = slide_left;
    apps_is_animating = true;

    if (current_app_obj) {
        lv_anim_t anim_out;
        lv_anim_init(&anim_out);
        lv_anim_set_var(&anim_out, current_app_obj);
        int end_x = slide_left ? -LV_HOR_RES : LV_HOR_RES;
        lv_anim_set_values(&anim_out, 0, end_x);
        lv_anim_set_time(&anim_out, ANIM_DURATION);
        lv_anim_set_path_cb(&anim_out, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&anim_out, anim_set_x);
        lv_anim_set_ready_cb(&anim_out, apps_carousel_fade_out_ready_cb);
        lv_anim_start(&anim_out);

        lv_anim_t fade_out;
        lv_anim_init(&fade_out);
        lv_anim_set_var(&fade_out, current_app_obj);
        lv_anim_set_values(&fade_out, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_time(&fade_out, ANIM_DURATION);
        lv_anim_set_exec_cb(&fade_out, anim_set_opa);
        lv_anim_start(&fade_out);
        return;
    }

    int app_idx = selected_app_index;
    current_app_obj = lv_btn_create(apps_container);
    apps_carousel_cache.card = current_app_obj;

    lv_obj_set_style_bg_color(current_app_obj, apps_surface_color, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(current_app_obj, 3, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(current_app_obj, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(current_app_obj, settings_get_menu_item_borders(&G_Settings) ? 2 : 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(current_app_obj, app_items[app_idx].border_color, LV_PART_MAIN);
    lv_obj_set_style_radius(current_app_obj, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(current_app_obj, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(current_app_obj, false, 0);
    apps_carousel_cache.border_color = app_items[app_idx].border_color;
    apps_carousel_cache.item_index = selected_app_index;

    int btn_size = LV_MIN(LV_HOR_RES, LV_VER_RES) * 0.6;
    if (LV_HOR_RES <= 128 && LV_VER_RES <= 128) {
        btn_size = 80;
    }
    lv_obj_set_size(current_app_obj, btn_size, btn_size);
    lv_obj_align(current_app_obj, LV_ALIGN_CENTER, 0, 0);

    const char *item_symbol = app_item_symbol_icon(app_idx);
    const lv_img_dsc_t *item_icon = item_symbol ? NULL : app_item_icon(app_idx);
    if (item_symbol) {
        lv_obj_t *icon = create_app_symbol_icon(current_app_obj, item_symbol, app_items[app_idx].border_color, &lv_font_montserrat_24);
        if (icon) {
            lv_obj_align(icon, LV_ALIGN_CENTER, 0, -10);
            apps_carousel_cache.icon = icon;
            apps_carousel_cache.icon_src = NULL;
            apps_carousel_cache.symbol_src = item_symbol;
            apps_carousel_cache.icon_recolor_enabled = false;
            apps_carousel_cache.icon_is_symbol = true;
        }
    } else if (item_icon) {
        lv_obj_t *icon = lv_img_create(current_app_obj);
        lv_img_set_src(icon, item_icon);
        const int icon_size = 50;
        lv_img_set_antialias(icon, false);
        bool recolor_enabled = app_item_icon_should_recolor(app_idx, item_icon);
        if (recolor_enabled) {
            lv_obj_set_style_img_recolor(icon, app_items[app_idx].border_color, 0);
            lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_img_recolor_opa(icon, LV_OPA_TRANSP, 0);
        }
        lv_obj_set_style_clip_corner(icon, false, 0);

        lv_coord_t img_width = item_icon->header.w;
        lv_coord_t img_height = item_icon->header.h;
        if (img_width > 0 && img_height > 0) {
            int zoom_w = (icon_size * 256) / img_width;
            int zoom_h = (icon_size * 256) / img_height;
            int zoom = LV_MIN(zoom_w, zoom_h);
            if (zoom > 512) zoom = 512;
            lv_img_set_zoom(icon, zoom);
        }
        int icon_x_offset = -3;
        int icon_y_offset = -5;
        if (app_items[app_idx].view == &ghostchi_view) {
            icon_x_offset = 9;
        }
        int x_pos = (btn_size - icon_size) / 2 + icon_x_offset;
        int y_pos = (btn_size - icon_size) / 2 + icon_y_offset;
        lv_obj_align(icon, LV_ALIGN_TOP_LEFT, x_pos, y_pos);
        apps_carousel_cache.icon = icon;
        apps_carousel_cache.icon_src = item_icon;
        apps_carousel_cache.symbol_src = NULL;
        apps_carousel_cache.icon_recolor_enabled = recolor_enabled;
        apps_carousel_cache.icon_is_symbol = false;
    }

    if (LV_HOR_RES > 150) {
        lv_obj_t *label = lv_label_create(current_app_obj);
        const char *label_text = app_items[app_idx].name;
        if (app_items[app_idx].view == NULL) label_text = "< Back";
        lv_label_set_text(label, label_text);
        lv_obj_set_style_text_font(label, accessibility_get_font_body(), 0);
        lv_obj_set_style_text_color(label, apps_text_color, 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -5);
        apps_carousel_cache.label = label;
        apps_carousel_cache.label_text = label_text;
    }
}

static void create_apps_grid_menu(void) {
    main_menu_layout_metrics_t layout;
    main_menu_layout_get_metrics(MAIN_MENU_LAYOUT_CARD_GRID, num_apps, &layout);

    int screen_width = layout.screen_width;
    int cols = layout.columns;
    int margin = layout.margin;
    int avail_height = layout.content_height;
    int card_width = layout.card_width;
    int card_height = layout.card_height;
    apps_grid_cols = cols > 0 ? cols : 1;

    apps_cleanup_layout();

    grid_cards_container = lv_obj_create(apps_container);
    lv_obj_set_size(grid_cards_container, screen_width, avail_height);
    lv_obj_set_style_bg_opa(grid_cards_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid_cards_container, 0, 0);
    lv_obj_set_style_pad_all(grid_cards_container, margin, 0);
    lv_obj_set_style_pad_row(grid_cards_container, margin, 0);
    lv_obj_set_flex_flow(grid_cards_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(grid_cards_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_align(grid_cards_container, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_scrollbar_mode(grid_cards_container, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(grid_cards_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(grid_cards_container, LV_DIR_VER);
    lv_obj_clear_flag(grid_cards_container, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(grid_cards_container, LV_OBJ_FLAG_SCROLL_ELASTIC);

    apps_grid_cards = calloc(num_apps, sizeof(lv_obj_t *));
    if (!apps_grid_cards) {
        ESP_LOGE(TAG, "failed to alloc apps grid cards");
        return;
    }

    bool show_borders = settings_get_menu_item_borders(&G_Settings);
    lv_obj_t *current_row = NULL;

    for (int i = 0; i < num_apps; ++i) {
        if (i % cols == 0) {
            current_row = lv_obj_create(grid_cards_container);
            lv_obj_set_width(current_row, LV_PCT(100));
            lv_obj_set_height(current_row, card_height);
            lv_obj_set_flex_flow(current_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(current_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(current_row, margin, 0);
            lv_obj_set_style_pad_all(current_row, 0, 0);
            lv_obj_set_style_bg_opa(current_row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(current_row, 0, 0);
            lv_obj_set_style_radius(current_row, 0, 0);
        }

        lv_obj_t *card = lv_btn_create(current_row);
        apps_grid_cards[i] = card;
        lv_obj_set_width(card, card_width);
        lv_obj_set_height(card, LV_PCT(100));

        lv_obj_set_style_bg_color(card, apps_surface_color, LV_PART_MAIN);
        int shadow_w = (card_height <= 50 ? 4 : 8);
        lv_obj_set_style_shadow_width(card, shadow_w, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(card, LV_OPA_50, LV_PART_MAIN);
        lv_obj_set_style_border_width(card, show_borders ? 2 : 0, LV_PART_MAIN);
        lv_obj_set_style_border_color(card, app_items[i].border_color, LV_PART_MAIN);
        lv_obj_set_style_radius(card, GUI_RADIUS_MD, LV_PART_MAIN);
        lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);

        int reserved_for_label = (card_height <= 50 ? 14 : 20);
        int avail_w = (int)(card_width * 0.78f);
        int avail_h = (int)((card_height - reserved_for_label) * 0.78f);
        if (avail_h < 10) avail_h = card_height - reserved_for_label;

        const char *item_symbol = app_item_symbol_icon(i);
        const lv_img_dsc_t *item_icon = item_symbol ? NULL : app_item_icon(i);
        if (item_symbol) {
            lv_obj_t *icon = create_app_symbol_icon(card, item_symbol, app_items[i].border_color, &lv_font_montserrat_24);
            if (icon) {
                lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 4);
            }
        } else if (item_icon) {
            lv_obj_t *icon = lv_img_create(card);
            lv_img_set_src(icon, item_icon);
            lv_img_set_antialias(icon, false);
            if (strcmp(app_items[i].name, "Flap") && app_item_icon_should_recolor(i, item_icon)) {
                lv_obj_set_style_img_recolor(icon, app_items[i].border_color, 0);
                lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
            } else {
                lv_obj_set_style_img_recolor_opa(icon, LV_OPA_TRANSP, 0);
            }
            lv_coord_t img_w = item_icon->header.w;
            lv_coord_t img_h = item_icon->header.h;
            int zoom_w = img_w > 0 ? (avail_w * 256) / img_w : 256;
            int zoom_h = img_h > 0 ? (avail_h * 256) / img_h : 256;
            int zoom = LV_MIN(zoom_w, zoom_h);
            if (zoom > 256) zoom = 256;
            if (zoom < 64) zoom = 64;
            lv_img_set_zoom(icon, zoom);

            int icon_draw_h = (img_h * zoom) / 256;
            int icon_area_h = card_height - reserved_for_label;
            int top_offset = (icon_area_h - icon_draw_h) / 2 - (card_height <= 50 ? 15 : 18);
            if (top_offset < 0) top_offset = 0;
            lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, top_offset);
        }

        lv_obj_t *label = lv_label_create(card);
        const char *label_text = app_items[i].name;
        if (app_items[i].view == NULL) {
            label_text = "< Back";
        }
        lv_label_set_text(label, label_text);
        const lv_font_t *lbl_font = accessibility_get_font_small();
        lv_obj_set_style_text_font(label, lbl_font, 0);
        lv_obj_set_style_text_color(label, apps_text_color, 0);

        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, card_width - 8);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -2);
    }

    if (selected_app_index >= 0 && selected_app_index < num_apps && apps_grid_cards[selected_app_index]) {
        uint8_t theme = settings_get_menu_theme(&G_Settings);
        lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
        lv_obj_set_style_border_color(apps_grid_cards[selected_app_index], accent, LV_PART_MAIN);
        lv_obj_set_style_border_width(apps_grid_cards[selected_app_index], 3, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(apps_grid_cards[selected_app_index], 12, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(apps_grid_cards[selected_app_index], accent, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(apps_grid_cards[selected_app_index], LV_OPA_30, LV_PART_MAIN);
        lv_obj_scroll_to_view(apps_grid_cards[selected_app_index], LV_ANIM_OFF);
    }
}

static void create_apps_list_menu(void) {
    int button_height = (LV_VER_RES <= 160 || LV_HOR_RES <= 160) ? 32 : 44;
    int icon_target = button_height <= 38 ? 20 : 26;

    lv_obj_set_flex_flow(apps_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(apps_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(apps_container, LV_HOR_RES > 200 ? 16 : 10, 0);
    lv_obj_set_style_pad_row(apps_container, 6, 0);
    lv_obj_add_flag(apps_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(apps_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(apps_container, LV_SCROLLBAR_MODE_AUTO);

    apps_cleanup_layout();
    apps_list_buttons = calloc(num_apps, sizeof(lv_obj_t *));
    if (!apps_list_buttons) {
        ESP_LOGE(TAG, "failed to alloc apps list buttons");
        return;
    }

    for (int i = 0; i < num_apps; ++i) {
        lv_obj_t *btn = lv_btn_create(apps_container);
        apps_list_buttons[i] = btn;
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, button_height);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(btn, apps_surface_color, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, settings_get_menu_item_borders(&G_Settings) ? 2 : 0, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, app_items[i].border_color, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_column(btn, 12, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(btn, 6, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(btn, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(btn, LV_OPA_40, LV_PART_MAIN);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

        const char *item_symbol = app_item_symbol_icon(i);
        const lv_img_dsc_t *item_icon = item_symbol ? NULL : app_item_icon(i);
        if (item_symbol) {
            lv_obj_t *icon = create_app_symbol_icon(btn, item_symbol, app_items[i].border_color, &lv_font_montserrat_24);
            if (icon) {
                lv_obj_set_width(icon, icon_target);
            }
        } else if (item_icon) {
            lv_obj_t *icon = lv_img_create(btn);
            lv_img_set_src(icon, item_icon);
            lv_img_set_antialias(icon, false);
            if (strcmp(app_items[i].name, "Flap") && app_item_icon_should_recolor(i, item_icon)) {
                lv_obj_set_style_img_recolor(icon, app_items[i].border_color, 0);
                lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
            } else {
                lv_obj_set_style_img_recolor_opa(icon, LV_OPA_TRANSP, 0);
            }
            lv_coord_t img_w = item_icon->header.w;
            lv_coord_t img_h = item_icon->header.h;
            int zoom_w = img_w > 0 ? (icon_target * 256) / img_w : 256;
            int zoom_h = img_h > 0 ? (icon_target * 256) / img_h : 256;
            int zoom = LV_MIN(zoom_w, zoom_h);
            if (zoom > 256) zoom = 256;
            if (zoom < 64) zoom = 64;
            lv_img_set_zoom(icon, zoom);
            lv_img_set_size_mode(icon, LV_IMG_SIZE_MODE_REAL);
            lv_obj_refresh_self_size(icon);
        }

        lv_obj_t *label = lv_label_create(btn);
        const char *label_text = app_items[i].name;
        if (app_items[i].view == NULL) {
            label_text = "< Back";
        }
        lv_label_set_text(label, label_text);
        lv_obj_set_style_text_color(label, apps_text_color, 0);
        const lv_font_t *lbl_font = accessibility_get_font_body();
        lv_obj_set_style_text_font(label, lbl_font, 0);

        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(label, 1);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    }
}

static void move_app_nav_buttons_foreground(void) {
    if (left_nav_btn) lv_obj_move_foreground(left_nav_btn);
    if (right_nav_btn) lv_obj_move_foreground(right_nav_btn);
}

static void render_app_items(void) {
    if (!apps_container || !lv_obj_is_valid(apps_container)) return;

    apps_cancel_carousel_anims();
    lv_obj_clean(apps_container);
    apps_cleanup_layout();
    current_app_obj = NULL;
    apps_carousel_cache = (apps_carousel_cache_t){0};
    apps_is_animating = false;

    if (selected_app_index < 0) selected_app_index = 0;
    if (selected_app_index >= num_apps) selected_app_index = num_apps > 0 ? num_apps - 1 : 0;

    if (apps_layout == APPS_LAYOUT_GRID_CARDS) {
        create_apps_grid_menu();
        select_app_item(selected_app_index, false);
    } else if (apps_layout == APPS_LAYOUT_LIST) {
        create_apps_list_menu();
        select_app_item(selected_app_index, false);
    } else {
        update_app_item(false);
    }

    move_app_nav_buttons_foreground();
}

static void apps_plugin_reload_done(void *arg) {
    (void)arg;
    apps_plugin_reload_in_progress = false;

    if (display_manager_get_current_view() != &apps_menu_view) return;
    if (!apps_container || !lv_obj_is_valid(apps_container)) return;

    bool selected_back = false;
    if (app_items && selected_app_index >= 0 && selected_app_index < num_apps) {
        selected_back = app_items[selected_app_index].view == NULL;
    }

    rebuild_app_items(true);
    init_app_colors();
    apps_allow_plugin_icon_load = true;
    if (selected_back) selected_app_index = num_apps > 0 ? num_apps - 1 : 0;
    render_app_items();
    gui_screen_apply_background(apps_menu_view.root);

    int plugin_count = plugin_manager_count();
    if (plugin_count > 0) {
        char msg[48];
        snprintf(msg, sizeof(msg), "%d SD app%s ready", plugin_count, plugin_count == 1 ? "" : "s");
        toast_show_duration(msg, TOAST_SUCCESS, 1000);
    } else if (plugin_manager_last_error()[0] != '\0') {
        char msg[64];
        snprintf(msg, sizeof(msg), "App scan failed: %.45s", plugin_manager_last_error());
        toast_show_duration(msg, TOAST_WARN, 2200);
    }
}

/**
 * @brief Creates the apps menu screen view
 */
 void apps_menu_create(void) {
    plugin_manager_init();
    int boot_count = plugin_manager_count();
    apps_allow_plugin_icon_load = (boot_count > 0);
    rebuild_app_items(boot_count > 0);
    if (boot_count > 0) {
        char msg[48];
        snprintf(msg, sizeof(msg), "%d SD app%s ready", boot_count, boot_count == 1 ? "" : "s");
        toast_show_duration(msg, TOAST_SUCCESS, 1000);
    }
    refresh_apps_surface_colors();
    display_manager_fill_screen(apps_bg_color);

#if CONFIG_ENABLE_NATIVE_SD_APPS
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) == 0) {
        toast_show_duration("Native SD apps require PSRAM", TOAST_WARN, 3500);
    }
#endif

    const char *title = (LV_VER_RES > 320 ? "Apps Menu" : "Apps");

    apps_root = gui_screen_create_root(NULL, title, apps_bg_color, LV_OPA_TRANSP);
    apps_menu_view.root = apps_root;

    apps_container = lv_obj_create(apps_root);
    lv_obj_set_size(apps_container, LV_HOR_RES, LV_VER_RES);
    lv_obj_clear_flag(apps_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(apps_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(apps_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(apps_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(apps_container, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(apps_container, 0, LV_PART_MAIN);

    init_app_colors();

    uint8_t layout_setting = settings_get_menu_layout(&G_Settings);
    switch (layout_setting) {
        case 1:
            apps_layout = APPS_LAYOUT_GRID_CARDS;
            break;
        case 2:
            apps_layout = APPS_LAYOUT_LIST;
            break;
        default:
            apps_layout = APPS_LAYOUT_CAROUSEL;
            break;
    }

    int status_bar_height = GUI_STATUS_BAR_H;
    if (apps_container) {

        if (apps_layout == APPS_LAYOUT_GRID_CARDS) {
            lv_obj_align(apps_container, LV_ALIGN_TOP_MID, 0, status_bar_height);
            lv_obj_set_size(apps_container, LV_HOR_RES, LV_VER_RES - status_bar_height);
        } else {
            lv_obj_align(apps_container, LV_ALIGN_CENTER, 0, status_bar_height / 2);
        }
    }

    bool should_show_nav_buttons = settings_get_nav_buttons_enabled(&G_Settings);

    if (should_show_nav_buttons) {
#ifdef CONFIG_LVGL_TOUCH
        should_show_nav_buttons = true;
#else
        int screen_width = lv_disp_get_hor_res(lv_disp_get_default());
        should_show_nav_buttons = (screen_width > 200);
#endif
    }

    if (should_show_nav_buttons && (apps_layout == APPS_LAYOUT_GRID_CARDS || apps_layout == APPS_LAYOUT_LIST)) {
        should_show_nav_buttons = false;
    }

    if (should_show_nav_buttons) {
        left_nav_btn = lv_btn_create(lv_scr_act());

        int btn_size = 52;
        int btn_margin = 15;
        int screen_width = lv_disp_get_hor_res(lv_disp_get_default());
        if (screen_width <= 128) {
            btn_size = 40;
            btn_margin = 10;
        } else if (screen_width >= 320) {
            btn_size = 60;
            btn_margin = 20;
        }

        lv_obj_set_size(left_nav_btn, btn_size, btn_size);
        lv_obj_set_style_bg_opa(left_nav_btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(left_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(left_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(left_nav_btn, 0, LV_PART_MAIN);

        lv_obj_align(left_nav_btn, LV_ALIGN_LEFT_MID, btn_margin, 0);

        lv_obj_t *left_label = lv_label_create(left_nav_btn);
        lv_label_set_text(left_label, "<");
        lv_obj_set_style_text_font(left_label, accessibility_get_font_display(), 0);
        if (btn_size < 40) {
            lv_obj_set_style_text_font(left_label, accessibility_get_font_title(), 0);
        }
        lv_obj_set_style_text_color(left_label, apps_text_color, 0);
        lv_obj_align(left_label, LV_ALIGN_CENTER, 0, 0);

        right_nav_btn = lv_btn_create(lv_scr_act());
        lv_obj_set_size(right_nav_btn, btn_size, btn_size);
        lv_obj_set_style_bg_opa(right_nav_btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(right_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(right_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(right_nav_btn, 0, LV_PART_MAIN);

        lv_obj_align(right_nav_btn, LV_ALIGN_RIGHT_MID, -btn_margin, 0);

        lv_obj_t *right_label = lv_label_create(right_nav_btn);
        lv_label_set_text(right_label, ">");
        lv_obj_set_style_text_font(right_label, accessibility_get_font_display(), 0);
        if (btn_size < 40) {
            lv_obj_set_style_text_font(right_label, accessibility_get_font_title(), 0);
        }
        lv_obj_set_style_text_color(right_label, apps_text_color, 0);
        lv_obj_align(right_label, LV_ALIGN_CENTER, 0, 0);

        ESP_LOGI(TAG, "Navigation buttons created for apps menu");

        lv_obj_move_foreground(left_nav_btn);
        lv_obj_move_foreground(right_nav_btn);
    }

    selected_app_index = 0;
    render_app_items();
    gui_screen_apply_background(apps_menu_view.root);

    if (left_nav_btn) {
        lv_coord_t old_y = lv_obj_get_y(left_nav_btn);
        lv_obj_set_y(left_nav_btn, old_y + status_bar_height / 2);
        lv_obj_move_foreground(left_nav_btn);
    }
    if (right_nav_btn) {
        lv_coord_t old_y = lv_obj_get_y(right_nav_btn);
        lv_obj_set_y(right_nav_btn, old_y + status_bar_height / 2);
        lv_obj_move_foreground(right_nav_btn);
    }

    // No re-scan on open. The boot-time scan (run from the splash deferred
    // task) is the canonical app discovery — re-running it here blanks the
    // UI while plugin_manager_reload wipes s_apps[]. Users can refresh via
    // reboot or the `apps reload` CLI command.
}

/**
 * @brief Destroys the apps menu screen view
 */
void apps_menu_destroy(void) {
    apps_cancel_carousel_anims();
    if (apps_root) {
        lvgl_obj_del_safe(&apps_root);
        apps_container = NULL;
        apps_menu_view.root = NULL;
        current_app_obj = NULL;
        back_button = NULL;
    }
    apps_cleanup_layout();
    apps_carousel_cache = (apps_carousel_cache_t){0};
    apps_is_animating = false;
    lvgl_obj_del_safe(&left_nav_btn);
    lvgl_obj_del_safe(&right_nav_btn);

    if (!apps_plugin_reload_in_progress) {
        free(apps_plugin_reload_stack);
        apps_plugin_reload_stack = NULL;
        free(apps_plugin_reload_tcb);
        apps_plugin_reload_tcb = NULL;
    }

    selected_app_index = 0;
    touch_started = false;
    touch_dragged = false;
    touch_drag_axis = 0;
    touch_start_x = 0;
    touch_start_y = 0;
    touch_last_x = 0;
    touch_last_y = 0;
}

/**
 * @brief Selects an app item and updates the display
 */
static void select_app_item(int index, bool slide_left) {
    if (!app_items || num_apps <= 0) return;
    if (index < 0) index = num_apps - 1;
    if (index >= num_apps) index = 0;

    if (apps_layout == APPS_LAYOUT_GRID_CARDS && apps_grid_cards) {
        bool show_borders_sel = settings_get_menu_item_borders(&G_Settings);
        if (selected_app_index >= 0 && selected_app_index < num_apps && apps_grid_cards[selected_app_index]) {
            lv_obj_t *old = apps_grid_cards[selected_app_index];
            lv_obj_set_style_border_width(old, show_borders_sel ? 2 : 0, LV_PART_MAIN);
            lv_obj_set_style_border_color(old, app_items[selected_app_index].border_color, LV_PART_MAIN);
            lv_obj_set_style_shadow_width(old, 8, LV_PART_MAIN);
            lv_obj_set_style_shadow_color(old, lv_color_hex(0x000000), LV_PART_MAIN);
            lv_obj_set_style_shadow_opa(old, LV_OPA_50, LV_PART_MAIN);
        }
        selected_app_index = index;
        if (apps_grid_cards[selected_app_index]) {
            lv_obj_t *card = apps_grid_cards[selected_app_index];
            uint8_t theme = settings_get_menu_theme(&G_Settings);
            lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
            lv_obj_set_style_border_width(card, 3, LV_PART_MAIN);
            lv_obj_set_style_border_color(card, accent, LV_PART_MAIN);
            lv_obj_set_style_shadow_width(card, 12, LV_PART_MAIN);
            lv_obj_set_style_shadow_color(card, accent, LV_PART_MAIN);
            lv_obj_set_style_shadow_opa(card, LV_OPA_30, LV_PART_MAIN);
            lv_obj_scroll_to_view(card, LV_ANIM_OFF);
        }
        return;
    }

    if (apps_layout == APPS_LAYOUT_LIST && apps_list_buttons) {
        if (selected_app_index >= 0 && selected_app_index < num_apps && apps_list_buttons[selected_app_index]) {
            lv_obj_t *old = apps_list_buttons[selected_app_index];
            lv_obj_set_style_border_width(old, settings_get_menu_item_borders(&G_Settings) ? 2 : 0, LV_PART_MAIN);
            lv_obj_set_style_border_color(old, app_items[selected_app_index].border_color, LV_PART_MAIN);
        }
        selected_app_index = index;
        if (apps_list_buttons[selected_app_index]) {
            lv_obj_t *btn = apps_list_buttons[selected_app_index];
            lv_obj_set_style_border_width(btn, 4, LV_PART_MAIN);
            lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            lv_obj_scroll_to_view(btn, LV_ANIM_OFF);
        }
        return;
    }

    selected_app_index = index;
    update_app_item(slide_left);
}

/**
 * @brief Handles the selection of app items
 */
static void handle_app_item_selection(int item_index) {
    if (item_index < 0 || item_index >= num_apps) return;

    if (app_items[item_index].view == NULL) {
        display_manager_switch_view(&main_menu_view);
        return;
    }

    ESP_LOGI(TAG, "Launching app: %s (index %d)\n", app_items[item_index].name, item_index);

    if (app_items[item_index].plugin_id[0] != '\0') {
        char msg[64];
        snprintf(msg, sizeof(msg), "Opening %.52s...", app_items[item_index].name);
        toast_show_duration(msg, TOAST_INFO, 1200);
        plugin_runner_set_app(app_items[item_index].plugin_id);
        display_manager_switch_view(&plugin_runner_view);
        return;
    }

    if (app_items[item_index].view == &terminal_view) {
        terminal_set_return_view(&apps_menu_view);
        terminal_set_dualcomm_filter(false);
    }

    display_manager_switch_view(app_items[item_index].view);
}

/**
 * @brief Handles hardware button presses for app navigation
 */
static void handle_apps_button_press(int button) {
    if (apps_layout == APPS_LAYOUT_GRID_CARDS) {
        if (button == 2) {
            select_app_item(selected_app_index - apps_grid_cols, false);
        } else if (button == 4) {
            select_app_item(selected_app_index + apps_grid_cols, false);
        } else if (button == 0) {
            select_app_item(selected_app_index - 1, true);
        } else if (button == 3) {
            select_app_item(selected_app_index + 1, false);
        } else if (button == 1) {
            handle_app_item_selection(selected_app_index);
        }
        return;
    }

    if (apps_layout == APPS_LAYOUT_LIST) {
        if (button == 2) { // Up
            ESP_LOGD(TAG, "Up button pressed\n");
            select_app_item(selected_app_index - 1, false);
        } else if (button == 4) { // Down
            ESP_LOGD(TAG, "Down button pressed\n");
            select_app_item(selected_app_index + 1, false);
        } else if (button == 1) { // Select
            ESP_LOGD(TAG, "Select button pressed\n");
            handle_app_item_selection(selected_app_index);
        } else if (button == 0) { // Back/Left
            ESP_LOGD(TAG, "Back button pressed\n");
            display_manager_switch_view(&main_menu_view);
        }
        return;
    }

    if (button == 0) { // Left
        ESP_LOGD(TAG, "Left button pressed\n");
        select_app_item(selected_app_index - 1, true);
    } else if (button == 3) { // Right
        ESP_LOGD(TAG, "Right button pressed\n");
        select_app_item(selected_app_index + 1, false);
    } else if (button == 1) { // Select
        ESP_LOGD(TAG, "Select button pressed\n");
        handle_app_item_selection(selected_app_index);
    } else if (button == 2) { // Back
        ESP_LOGD(TAG, "Back button pressed\n");
        display_manager_switch_view(&main_menu_view);
    }
}

/**
 * @brief handles keyboard button presses
 */
static void handle_keyboard_interactions(int keyValue){

    // Vim keybinds and Cardputer controls
    if (keyValue == LV_KEY_LEFT || keyValue == 44 || keyValue == ',' || keyValue == 'h') { // Left
        ESP_LOGI(TAG, "Left button or 'h' pressed");
        select_app_item(selected_app_index - 1, true);
    } else if (keyValue == LV_KEY_RIGHT || keyValue == 47 || keyValue == '/' || keyValue == 'l') { // Right
        ESP_LOGI(TAG, "Right button or 'l' pressed");
        select_app_item(selected_app_index + 1, false);
    } else if (keyValue == LV_KEY_UP || keyValue == 'k' || keyValue == ';') { // Up
        ESP_LOGI(TAG, "Up arrow or 'k' pressed");
        select_app_item(selected_app_index - 1, true);
    } else if (keyValue == LV_KEY_DOWN || keyValue == 'j' || keyValue == '.') { // Down
        ESP_LOGI(TAG, "Down arrow or 'j' pressed");
        select_app_item(selected_app_index + 1, false);
    } else if (keyValue == LV_KEY_ENTER || keyValue == 13) { // Select
        ESP_LOGI(TAG, "Enter pressed (select)");
        handle_app_item_selection(selected_app_index);
    } else if (keyValue == LV_KEY_ESC || keyValue == 29 || keyValue == '`') { // Back
        ESP_LOGI(TAG, "Esc or '`' pressed (back)");
        display_manager_switch_view(&main_menu_view);
    }
}

/**
 * @brief Combined handler for app menu events
 */
void apps_menu_event_handler(InputEvent *event) {
    if (event->type == INPUT_TYPE_TOUCH) {
        ESP_LOGD(TAG, "Touch event");
        lv_indev_data_t *data = &event->data.touch_data;
        if (data->state == LV_INDEV_STATE_PR) {
            if (!touch_started) {
                touch_started = true;
                touch_dragged = false;
                touch_drag_axis = 0;
                touch_start_x = data->point.x;
                touch_start_y = data->point.y;
                touch_last_x = data->point.x;
                touch_last_y = data->point.y;
            } else {
                int dx = data->point.x - touch_last_x;
                int dy = data->point.y - touch_last_y;
                touch_last_x = data->point.x;
                touch_last_y = data->point.y;

                if (!touch_dragged) {
                    touch_drag_axis = resolve_drag_axis(data->point.x - touch_start_x, data->point.y - touch_start_y);
                    touch_dragged = touch_drag_axis != 0;
                }

                if (touch_dragged && touch_drag_axis == 1) {
                    dy = clamp_drag_delta(dy);
                    if (dy) {
                        if (apps_layout == APPS_LAYOUT_GRID_CARDS && grid_cards_container) {
                            display_manager_queue_scroll(grid_cards_container, dy);
                        } else if (apps_layout == APPS_LAYOUT_LIST && apps_container) {
                            display_manager_queue_scroll(apps_container, dy);
                        }
                    }
                }
            }
        } else if (data->state == LV_INDEV_STATE_REL && touch_started) {
            int dx = data->point.x - touch_start_x;
            int dy = data->point.y - touch_start_y;
            touch_started = false;

            if (touch_dragged && apps_layout != APPS_LAYOUT_CAROUSEL) {
                touch_dragged = false;
                touch_drag_axis = 0;
                return;
            }
            touch_dragged = false;
            touch_drag_axis = 0;

            if (apps_layout == APPS_LAYOUT_CAROUSEL) {
                if (abs(dx) > SWIPE_THRESHOLD && abs(dx) > abs(dy)) {
                    if (dx < 0) {
                        select_app_item(selected_app_index + 1, true);
                    } else {
                        select_app_item(selected_app_index - 1, false);
                    }
                    return;
                }
            }

            if (left_nav_btn && right_nav_btn) {
                lv_area_t left_area, right_area;
                lv_obj_get_coords(left_nav_btn, &left_area);
                lv_obj_get_coords(right_nav_btn, &right_area);

                bool start_in_left = (touch_start_x >= left_area.x1 && touch_start_x <= left_area.x2 &&
                                      touch_start_y >= left_area.y1 && touch_start_y <= left_area.y2);
                bool end_in_left = (data->point.x >= left_area.x1 && data->point.x <= left_area.x2 &&
                                    data->point.y >= left_area.y1 && data->point.y <= left_area.y2);
                if (start_in_left && end_in_left && abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                    ESP_LOGI(TAG, "Left navigation button tapped (press+release inside)");
                    select_app_item(selected_app_index - 1, true);
                    return;
                }

                bool start_in_right = (touch_start_x >= right_area.x1 && touch_start_x <= right_area.x2 &&
                                       touch_start_y >= right_area.y1 && touch_start_y <= right_area.y2);
                bool end_in_right = (data->point.x >= right_area.x1 && data->point.x <= right_area.x2 &&
                                     data->point.y >= right_area.y1 && data->point.y <= right_area.y2);
                if (start_in_right && end_in_right && abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                    ESP_LOGI(TAG, "Right navigation button tapped (press+release inside)");
                    select_app_item(selected_app_index + 1, false);
                    return;
                }
            }

            if (apps_layout == APPS_LAYOUT_GRID_CARDS && abs(dy) > SWIPE_THRESHOLD && abs(dy) > abs(dx)) {
                if (grid_cards_container) {
                    dy = clamp_drag_delta(dy);
                    if (dy) display_manager_queue_scroll(grid_cards_container, dy);
                }
                return;
            }

            if (apps_layout == APPS_LAYOUT_LIST && abs(dy) > SWIPE_THRESHOLD && abs(dy) > abs(dx)) {
                if (apps_container) {
                    dy = clamp_drag_delta(dy);
                    if (dy) display_manager_queue_scroll(apps_container, dy);
                }
                return;
            }

            if (abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                // Check which app button was actually tapped
                if (apps_layout == APPS_LAYOUT_LIST && apps_list_buttons) {
                    for (int i = 0; i < num_apps; i++) {
                        if (apps_list_buttons[i]) {
                            lv_area_t btn_area;
                            lv_obj_get_coords(apps_list_buttons[i], &btn_area);
                            if (data->point.x >= btn_area.x1 && data->point.x <= btn_area.x2 &&
                                data->point.y >= btn_area.y1 && data->point.y <= btn_area.y2) {
                                select_app_item(i, false);
                                handle_app_item_selection(i);
                                return;
                            }
                        }
                    }
                } else if (apps_layout == APPS_LAYOUT_GRID_CARDS && apps_grid_cards) {
                    for (int i = 0; i < num_apps; i++) {
                        if (apps_grid_cards[i]) {
                            lv_area_t card_area;
                            lv_obj_get_coords(apps_grid_cards[i], &card_area);
                            if (data->point.x >= card_area.x1 && data->point.x <= card_area.x2 &&
                                data->point.y >= card_area.y1 && data->point.y <= card_area.y2) {
                                select_app_item(i, false);
                                handle_app_item_selection(i);
                                return;
                            }
                        }
                    }
                } else {
                    // Carousel layout - use current selection
                    handle_app_item_selection(selected_app_index);
                }
                return;
            }
        }
    } else if (event->type == INPUT_TYPE_JOYSTICK) {
        ESP_LOGI(TAG, "Joystick event");
        handle_apps_button_press(event->data.joystick_index);
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        ESP_LOGW(TAG, "keyboard event");
        handle_keyboard_interactions(event->data.key_value);
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            handle_app_item_selection(selected_app_index);
        } else {
            if (event->data.encoder.direction > 0) {
                select_app_item(selected_app_index + 1, true);
            } else {
                select_app_item(selected_app_index - 1, false);
            }
        }
#ifdef CONFIG_USE_ENCODER
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        ESP_LOGI(TAG, "IO6 exit button pressed, returning to main menu");
        display_manager_switch_view(&main_menu_view);
#endif
    }
}

void get_apps_menu_callback(void **callback) {
    *callback = apps_menu_event_handler;
}

View apps_menu_view = {
    .root = NULL,
    .create = apps_menu_create,
    .destroy = apps_menu_destroy,
    .input_callback = apps_menu_event_handler,
    .name = "Apps Menu",
    .get_hardwareinput_callback = get_apps_menu_callback
};
