#include "gui/menu_item_style.h"

void gui_menu_card_apply(lv_obj_t *obj, bool background_enabled,
                         lv_color_t surface, lv_color_t border,
                         int border_width, int shadow_width) {
    if (!obj) return;

    if (background_enabled) {
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(obj, surface, LV_PART_MAIN);
        lv_obj_set_style_border_width(obj, border_width, LV_PART_MAIN);
        lv_obj_set_style_border_color(obj, border, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(obj, shadow_width, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(obj, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(obj, LV_OPA_50, LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    }
}

void gui_menu_card_apply_selected(lv_obj_t *obj, bool background_enabled,
                                  lv_color_t accent) {
    if (!obj) return;

    // Focus remains visible even when users disable card backgrounds.
    lv_obj_set_style_border_width(obj, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, accent, LV_PART_MAIN);
    if (background_enabled) {
        lv_obj_set_style_shadow_width(obj, 8, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(obj, accent, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(obj, LV_OPA_20, LV_PART_MAIN);
    } else {
        lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    }
}

void gui_menu_launcher_tile_apply(lv_obj_t *obj, bool background_enabled,
                                   lv_color_t surface) {
    if (!obj) return;
    (void)background_enabled;

    lv_obj_set_style_bg_color(obj, surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
}

void gui_menu_launcher_tile_apply_selected(lv_obj_t *obj, bool background_enabled,
                                           lv_color_t accent) {
    if (!obj) return;
    (void)accent;

    // Selection is a neutral focus plate; color remains on the icon and page dot.
    lv_obj_set_style_bg_opa(obj, background_enabled ? LV_OPA_30 : LV_OPA_10, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
}

void gui_menu_compact_tile_apply(lv_obj_t *obj, lv_obj_t *label, bool selected,
                                 bool background_enabled, lv_color_t surface,
                                 lv_color_t text, lv_color_t accent,
                                 lv_color_t accent_text) {
    if (!obj) return;

    lv_obj_set_style_bg_color(obj, selected ? accent : surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, selected ? LV_OPA_COVER :
                            (background_enabled ? LV_OPA_COVER : LV_OPA_TRANSP), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    if (label) lv_obj_set_style_text_color(label, selected ? accent_text : text, 0);
}

void gui_menu_page_indicator_update(lv_obj_t *indicator, int current_page, int page_count,
                                    lv_color_t active, lv_color_t inactive) {
    if (!indicator) return;
    if (page_count < 1) page_count = 1;
    if (current_page < 0) current_page = 0;
    if (current_page >= page_count) current_page = page_count - 1;

    lv_obj_clean(indicator);
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    lv_obj_set_size(indicator, LV_SIZE_CONTENT, 16);
#else
    lv_obj_set_size(indicator, LV_SIZE_CONTENT, 10);
#endif
    lv_obj_set_flex_flow(indicator, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(indicator, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(indicator, 0, LV_PART_MAIN);
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    lv_obj_set_style_pad_column(indicator, 8, LV_PART_MAIN);
#else
    lv_obj_set_style_pad_column(indicator, 4, LV_PART_MAIN);
#endif
    lv_obj_set_style_bg_opa(indicator, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(indicator, 0, LV_PART_MAIN);
    lv_obj_clear_flag(indicator, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (int page = 0; page < page_count; ++page) {
        bool selected = page == current_page;
        lv_obj_t *dot = lv_obj_create(indicator);
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
        /* The active page becomes a short pill. It reads more clearly at
         * arm's length than a slightly larger dot on a 800px+ display. */
        int dot_w = selected ? 26 : 8;
        int dot_h = selected ? 10 : 8;
#else
        int dot_w = selected ? 6 : 4;
        int dot_h = selected ? 6 : 4;
#endif
        lv_obj_set_size(dot, dot_w, dot_h);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, selected ? active : inactive, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, selected ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }
}

static void menu_scroll_x_anim(void *obj, int32_t value) {
    lv_obj_scroll_to_x((lv_obj_t *)obj, value, LV_ANIM_OFF);
}

void gui_menu_scroll_to_x(lv_obj_t *obj, int target_x, bool animate) {
    if (!obj) return;
    lv_anim_del(obj, menu_scroll_x_anim);
    int current_x = lv_obj_get_scroll_x(obj);
    if (!animate || current_x == target_x) {
        lv_obj_scroll_to_x(obj, target_x, LV_ANIM_OFF);
        return;
    }

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_values(&anim, current_x, target_x);
    /* Ease-out reaches the target sooner perceptually than a linear tween of
     * the same length, and a shorter total time means fewer redraw ticks of
     * the (icon-heavy) grid have to land before the animation is done. */
    lv_anim_set_time(&anim, 45);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, menu_scroll_x_anim);
    lv_anim_start(&anim);
}

void gui_menu_image_fit(lv_obj_t *image, const lv_img_dsc_t *source,
                        int target_size, int max_zoom) {
    if (!image || !source || target_size <= 0) return;

    lv_coord_t image_width = source->header.w;
    lv_coord_t image_height = source->header.h;
    if (image_width <= 0 || image_height <= 0) return;

    int zoom_width = (target_size * 256) / image_width;
    int zoom_height = (target_size * 256) / image_height;
    int zoom = LV_MIN(zoom_width, zoom_height);
    if (max_zoom < 64) max_zoom = 64;
    if (zoom > max_zoom) zoom = max_zoom;
    if (zoom < 64) zoom = 64;

    lv_img_set_zoom(image, zoom);
    lv_img_set_size_mode(image, LV_IMG_SIZE_MODE_REAL);
    lv_obj_refresh_self_size(image);
}
