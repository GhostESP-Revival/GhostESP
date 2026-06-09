#include "gui/screen_layout.h"
#include "gui/asset_pack.h"
#include "gui/design_tokens.h"
#include "managers/display_manager.h"

/* Tracks the last asset-pack version for which we built a bg widget on a
 * given root, so gui_screen_apply_background() can early-out when called
 * repeatedly (e.g. from the 1Hz menu refresh timer) without doing work. */
static uint32_t s_last_applied_bg_version = 0xFFFFFFFFu;
static lv_obj_t *s_last_applied_bg_root = NULL;
static const lv_img_dsc_t *s_last_applied_bg_src = NULL;
static lv_obj_t *s_last_applied_bg_widget = NULL;
static const lv_img_dsc_t *s_indexed_scaled_bg_src = NULL;

/* Drop the short-circuit cache (e.g. after lv_obj_clean on the root). */
static void invalidate_bg_shortcut(void) {
    s_last_applied_bg_root = NULL;
    s_last_applied_bg_widget = NULL;
    s_last_applied_bg_src = NULL;
    s_last_applied_bg_version = 0xFFFFFFFFu;
    s_indexed_scaled_bg_src = NULL;
}

static lv_color_t indexed4_px(const lv_img_dsc_t *src, uint32_t x, uint32_t y) {
    const lv_color32_t *palette = (const lv_color32_t *)src->data;
    const uint8_t *pixels = src->data + 64;
    uint32_t i = (uint32_t)src->header.w * y + x;
    uint8_t packed = pixels[i / 2];
    uint8_t index = (i & 1) ? (packed >> 4) : (packed & 0x0F);
    lv_color32_t c = palette[index & 0x0F];
    return lv_color_make(c.ch.red, c.ch.green, c.ch.blue);
}

static void indexed_scaled_bg_draw_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
    const lv_img_dsc_t *src = s_indexed_scaled_bg_src;
    if (!src || src->header.cf != LV_IMG_CF_INDEXED_4BIT || !src->data) return;

    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    if (!obj || !draw_ctx || !draw_ctx->clip_area) return;

    lv_area_t draw_area;
    if (!_lv_area_intersect(&draw_area, &obj->coords, draw_ctx->clip_area)) return;

    uint32_t src_w = src->header.w;
    uint32_t src_h = src->header.h;
    if (src_w == 0 || src_h == 0) return;

    uint32_t zoom_w = (LV_HOR_RES * 256u) / src_w;
    uint32_t zoom_h = (LV_VER_RES * 256u) / src_h;
    uint32_t zoom = zoom_w > zoom_h ? zoom_w : zoom_h;
    if (zoom == 0) return;

    lv_color_t line[LV_HOR_RES];
    lv_draw_img_dsc_t img_dsc;
    lv_draw_img_dsc_init(&img_dsc);
    img_dsc.opa = LV_OPA_COVER;

    for (lv_coord_t y = draw_area.y1; y <= draw_area.y2; ++y) {
        lv_coord_t line_w = lv_area_get_width(&draw_area);
        if (line_w <= 0 || line_w > LV_HOR_RES) return;
        uint32_t dst_y = (uint32_t)(y - obj->coords.y1);
        uint32_t sy = (dst_y * 256u) / zoom;
        if (sy >= src_h) sy = src_h - 1;

        for (lv_coord_t x = draw_area.x1; x <= draw_area.x2; ++x) {
            uint32_t dst_x = (uint32_t)(x - obj->coords.x1);
            uint32_t sx = (dst_x * 256u) / zoom;
            if (sx >= src_w) sx = src_w - 1;
            line[x - draw_area.x1] = indexed4_px(src, sx, sy);
        }

        lv_img_dsc_t line_img = {0};
        line_img.header.cf = LV_IMG_CF_TRUE_COLOR;
        line_img.header.w = (uint16_t)line_w;
        line_img.header.h = 1;
        line_img.data_size = (uint32_t)line_w * sizeof(lv_color_t);
        line_img.data = (const uint8_t *)line;
        lv_area_t line_area = {.x1 = draw_area.x1, .y1 = y, .x2 = draw_area.x2, .y2 = y};
        lv_draw_img(draw_ctx, &img_dsc, &line_area, &line_img);
    }
}

static void apply_bg_widget(lv_obj_t *root, const lv_img_dsc_t *src) {
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);
    bool custom_indexed_scale = asset_pack_background_should_scale() && src->header.cf == LV_IMG_CF_INDEXED_4BIT;

    lv_obj_t *bg_img = NULL;
    if (s_last_applied_bg_widget && lv_obj_is_valid(s_last_applied_bg_widget) &&
        lv_obj_get_parent(s_last_applied_bg_widget) == root &&
        lv_obj_has_flag(s_last_applied_bg_widget, LV_OBJ_FLAG_USER_1)) {
        bg_img = s_last_applied_bg_widget;
    }
    if (!bg_img) {
        bg_img = custom_indexed_scale ? lv_obj_create(root) : lv_img_create(root);
        if (custom_indexed_scale) lv_obj_add_event_cb(bg_img, indexed_scaled_bg_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    } else if (custom_indexed_scale && lv_obj_check_type(bg_img, &lv_img_class)) {
        lv_obj_del(bg_img);
        bg_img = lv_obj_create(root);
        lv_obj_add_event_cb(bg_img, indexed_scaled_bg_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    } else if (!custom_indexed_scale && !lv_obj_check_type(bg_img, &lv_img_class)) {
        lv_obj_del(bg_img);
        bg_img = lv_img_create(root);
    }
    lv_obj_move_to_index(bg_img, 0);
    lv_obj_add_flag(bg_img, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_USER_1);
    lv_obj_clear_flag(bg_img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_last_applied_bg_widget = bg_img;
    s_indexed_scaled_bg_src = custom_indexed_scale ? src : NULL;
    if (custom_indexed_scale) {
        lv_obj_set_size(bg_img, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_pos(bg_img, 0, 0);
        lv_obj_set_style_bg_opa(bg_img, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(bg_img, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(bg_img, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(bg_img, 0, LV_PART_MAIN);
        return;
    }

    lv_img_set_src(bg_img, src);
    lv_img_set_antialias(bg_img, false);
    lv_img_set_size_mode(bg_img, LV_IMG_SIZE_MODE_VIRTUAL);

    /* If src is the pre-baked fullscreen desc, it already matches the
     * display exactly. No zoom, no tiling style, just a flat blit. */
    if (src->header.w == (uint16_t)LV_HOR_RES && src->header.h == (uint16_t)LV_VER_RES) {
        lv_obj_set_size(bg_img, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_bg_img_src(bg_img, NULL, LV_PART_MAIN);
        lv_obj_set_style_bg_img_tiled(bg_img, false, LV_PART_MAIN);
        lv_obj_set_style_img_opa(bg_img, LV_OPA_COVER, LV_PART_MAIN);
        lv_img_set_pivot(bg_img, 0, 0);
        lv_img_set_zoom(bg_img, 256);
        lv_obj_set_pos(bg_img, 0, 0);
        return;
    }

    lv_coord_t img_w = src->header.w;
    lv_coord_t img_h = src->header.h;
    bool tile = (img_w < LV_HOR_RES || img_h < LV_VER_RES) && !asset_pack_background_should_scale();

    if (tile) {
        lv_obj_set_size(bg_img, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_bg_img_src(bg_img, src, LV_PART_MAIN);
        lv_obj_set_style_bg_img_tiled(bg_img, true, LV_PART_MAIN);
        lv_obj_set_style_bg_img_opa(bg_img, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bg_img, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(bg_img, 0, LV_PART_MAIN);
        lv_obj_set_style_img_opa(bg_img, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_img_set_pivot(bg_img, 0, 0);
        lv_img_set_zoom(bg_img, 256);
        lv_obj_set_pos(bg_img, 0, 0);
    } else {
        lv_img_set_size_mode(bg_img, LV_IMG_SIZE_MODE_REAL);
        lv_obj_set_size(bg_img, img_w, img_h);
        lv_obj_set_style_bg_img_src(bg_img, NULL, LV_PART_MAIN);
        lv_obj_set_style_bg_img_tiled(bg_img, false, LV_PART_MAIN);
        int zoom_w = (img_w > 0) ? (LV_HOR_RES * 256) / img_w : 256;
        int zoom_h = (img_h > 0) ? (LV_VER_RES * 256) / img_h : 256;
        int zoom = (zoom_w > zoom_h) ? zoom_w : zoom_h;
        if (zoom < 64) zoom = 64;
        lv_img_set_pivot(bg_img, 0, 0);
        lv_img_set_zoom(bg_img, zoom);
        lv_obj_set_pos(bg_img, 0, 0);
        lv_obj_set_style_img_opa(bg_img, LV_OPA_COVER, LV_PART_MAIN);
    }
}

static lv_obj_t *create_root_internal(lv_obj_t *parent, const char *title,
                                     lv_color_t bg_color, lv_opa_t bg_opa,
                                     bool use_asset_pack_bg) {
    if (!parent) parent = lv_scr_act();

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_HOR_RES, LV_VER_RES);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);

    lv_obj_set_style_bg_color(root, bg_color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, bg_opa, LV_PART_MAIN);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(root, 0, LV_PART_MAIN);

    if (use_asset_pack_bg) {
        /* Prefer the pre-baked fullscreen bg (single blit) over the smaller tile. */
        const lv_img_dsc_t *bg_src = asset_pack_get_background_fullscreen();
        if (!bg_src) bg_src = asset_pack_get_background_tile();
        if (bg_src) {
            apply_bg_widget(root, bg_src);
            s_last_applied_bg_root = root;
            s_last_applied_bg_src = bg_src;
            s_last_applied_bg_version = asset_pack_get_version();
        }
    }

    if (title && title[0]) {
        display_manager_add_status_bar(title);
    }

    return root;
}

lv_obj_t *gui_screen_create_root(lv_obj_t *parent, const char *title, lv_color_t bg_color, lv_opa_t bg_opa) {
    return create_root_internal(parent, title, bg_color, bg_opa, true);
}

lv_obj_t *gui_screen_create_root_no_bg(lv_obj_t *parent, const char *title, lv_color_t bg_color, lv_opa_t bg_opa) {
    return create_root_internal(parent, title, bg_color, bg_opa, false);
}

void gui_screen_apply_background(lv_obj_t *root) {
    if (!root || !lv_obj_is_valid(root)) return;

    /* Fast path: if the bg widget on this root already shows the current
     * pack's image, do nothing. This skips a 150KB re-blit when the 1Hz
     * menu refresh timer or any other periodic caller invokes us. The
     * widget-presence check is required because callers like the 1Hz
     * menu rebuild do lv_obj_clean(root) which destroys the bg widget
     * even though the version didn't change. */
    uint32_t cur_ver = asset_pack_get_version();
    const lv_img_dsc_t *expected = asset_pack_get_background_fullscreen();
    if (!expected) expected = asset_pack_get_background_tile();
    if (s_last_applied_bg_root == root && s_last_applied_bg_version == cur_ver &&
        s_last_applied_bg_widget && lv_obj_is_valid(s_last_applied_bg_widget) &&
        expected == s_last_applied_bg_src) {
        return;
    }

    const lv_img_dsc_t *bg_src = asset_pack_get_background_fullscreen();
    if (!bg_src) bg_src = asset_pack_get_background_tile();
    if (!bg_src) {
        /* No background available - drop any existing bg widget. */
        for (int i = 0; i < (int)lv_obj_get_child_cnt(root); i++) {
            lv_obj_t *child = lv_obj_get_child(root, i);
            if (child && lv_obj_has_flag(child, LV_OBJ_FLAG_USER_1)) {
                lv_obj_del(child);
                break;
            }
        }
        s_last_applied_bg_root = NULL;
        s_last_applied_bg_src = NULL;
        s_last_applied_bg_version = cur_ver;
        return;
    }

    apply_bg_widget(root, bg_src);
    s_last_applied_bg_root = root;
    s_last_applied_bg_src = bg_src;
    s_last_applied_bg_version = cur_ver;
}

void gui_screen_invalidate_bg_cache(void) {
    /* Call after lv_obj_clean or destroy on a root that had a cached bg. */
    invalidate_bg_shortcut();
}

lv_obj_t *gui_screen_create_content(lv_obj_t *root, lv_coord_t status_bar_h) {
    if (!root) return NULL;

    if (status_bar_h < 0) status_bar_h = 0;
    if (status_bar_h > LV_VER_RES) status_bar_h = LV_VER_RES;

    lv_obj_t *content = lv_obj_create(root);
    lv_obj_set_pos(content, 0, status_bar_h);
    lv_obj_set_size(content, LV_HOR_RES, LV_VER_RES - status_bar_h);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(content, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(content, 0, LV_PART_MAIN);

    return content;
}
