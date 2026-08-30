#include "managers/plugin_api_internal.h"
#include "gui/gui_anim.h"
#include "esp_heap_caps.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include "gui/native_canvas_scale.h"
#endif

typedef struct {
    ghostesp_ui_obj_t parent;
    int32_t width;
    int32_t height;
    ghostesp_ui_obj_t result;
} canvas_create_ctx_t;

typedef struct {
    ghostesp_ui_obj_t canvas;
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
    uint32_t hex_color;
} canvas_rect_ctx_t;

typedef struct {
    ghostesp_ui_obj_t obj;
    uint32_t color;
} obj_color_ctx_t;

typedef struct {
    ghostesp_ui_obj_t canvas;
    const uint16_t *pixels;
    int32_t src_width;
    int32_t src_height;
    int32_t src_stride;
    int32_t dst_x;
    int32_t dst_y;
    int32_t dst_width;
    int32_t dst_height;
    bool result;
    uint16_t *owned_pixels;
} canvas_blit_ctx_t;

typedef struct {
    ghostesp_ui_obj_t canvas;
    const ghostesp_point_t *points;
    int count;
    uint32_t hex_color;
    int32_t width;
} canvas_line_draw_ctx_t;

typedef struct {
    ghostesp_ui_obj_t canvas;
    int32_t cx;
    int32_t cy;
    int32_t r;
    int32_t start_angle;
    int32_t end_angle;
    uint32_t hex_color;
    int32_t width;
} canvas_arc_draw_ctx_t;

typedef struct {
    ghostesp_ui_obj_t parent;
    ghostesp_ui_obj_t result;
} simple_create_ctx_t;

typedef struct {
    ghostesp_ui_obj_t obj;
    const ghostesp_point_t *points;
    int count;
} line_points_ctx_t;

typedef struct {
    ghostesp_ui_obj_t obj;
    int32_t value;
} obj_value_ctx_t;

typedef struct {
    ghostesp_ui_obj_t obj;
    int32_t val1;
    int32_t val2;
} obj_pair_ctx_t;

typedef struct {
    ghostesp_ui_obj_t obj;
    const char *path;
    bool result;
} image_src_ctx_t;

typedef struct {
    ghostesp_ui_obj_t obj;
    const lv_img_dsc_t *source;
    bool result;
} builtin_image_src_ctx_t;

typedef struct {
    const char *name;
    const lv_img_dsc_t *source;
} builtin_image_t;

LV_IMG_DECLARE(angry_50x50);
LV_IMG_DECLARE(banshee_50x50);
LV_IMG_DECLARE(cake_50x50);
LV_IMG_DECLARE(evil_50x50);
LV_IMG_DECLARE(happy_50x50);
LV_IMG_DECLARE(love_50x50);
LV_IMG_DECLARE(sleep_50x50);
LV_IMG_DECLARE(speech);
LV_IMG_DECLARE(subghz_50x50);
LV_IMG_DECLARE(surpised_50x50);
LV_IMG_DECLARE(tired_50x50);
LV_IMG_DECLARE(what2_50x50);

static const builtin_image_t s_builtin_images[] = {
    { "ghostchi/angry", &angry_50x50 },
    { "ghostchi/banshee", &banshee_50x50 },
    { "ghostchi/cake", &cake_50x50 },
    { "ghostchi/evil", &evil_50x50 },
    { "ghostchi/happy", &happy_50x50 },
    { "ghostchi/love", &love_50x50 },
    { "ghostchi/sleep", &sleep_50x50 },
    { "ghostchi/speech", &speech },
    { "ghostchi/subghz", &subghz_50x50 },
    { "ghostchi/surprised", &surpised_50x50 },
    { "ghostchi/tired", &tired_50x50 },
    { "ghostchi/what", &what2_50x50 },
};

typedef struct timer_bridge_s {
    ghostesp_ui_timer_cb_t cb;
    void *user;
    lv_timer_t *timer;
    struct timer_bridge_s *next;
} timer_bridge_t;

static timer_bridge_t *s_timer_bridge_head;

typedef struct {
    ghostesp_ui_timer_cb_t cb;
    uint32_t interval_ms;
    void *user;
    ghostesp_ui_timer_t result;
} timer_create_ctx_t;

typedef struct {
    ghostesp_ui_timer_t timer;
} timer_delete_ctx_t;

typedef struct {
    ghostesp_ui_timer_t timer;
    uint32_t interval_ms;
} timer_interval_ctx_t;

typedef struct {
    ghostesp_ui_obj_t obj;
    int direction;
    uint32_t duration_ms;
} anim_slide_ctx_t;

typedef struct {
    ghostesp_ui_obj_t obj;
    int direction;
    uint32_t duration_ms;
    ghostesp_anim_done_cb_t on_done;
    void *user;
} anim_slide_out_ctx_t;

typedef struct {
    ghostesp_anim_done_cb_t cb;
    void *user;
} anim_done_bridge_t;

static void canvas_buf_delete_cb(lv_event_t *e) {
    void *buf = lv_event_get_user_data(e);
    if (buf) free(buf);
}

static void line_points_delete_cb(lv_event_t *e) {
    lv_obj_t *line = lv_event_get_target(e);
    lv_point_t *pts = (lv_point_t *)lv_obj_get_user_data(line);
    if (pts) free(pts);
    lv_obj_set_user_data(line, NULL);
}

static void timer_bridge_cb(lv_timer_t *timer) {
    timer_bridge_t *bridge = (timer_bridge_t *)timer->user_data;
    if (bridge && bridge->cb) bridge->cb(bridge->user);
}

static void anim_ready_bridge(lv_anim_t *a) {
    anim_done_bridge_t *bridge = (anim_done_bridge_t *)a->user_data;
    if (bridge) {
        if (bridge->cb) bridge->cb(bridge->user);
        free(bridge);
    }
}

static void plugin_api_ui_canvas_create_now(void *arg) {
    canvas_create_ctx_t *ctx = (canvas_create_ctx_t *)arg;
    lv_obj_t *parent = plugin_api_internal_parent_or_current(ctx->parent);
    lv_obj_t *canvas = lv_canvas_create(parent);
    if (!canvas) return;

    const size_t bytes_per_pixel = (LV_COLOR_SIZE + 7u) / 8u;
    const size_t width = (size_t)ctx->width;
    const size_t height = (size_t)ctx->height;
    if (width > SIZE_MAX / height || width * height > SIZE_MAX / bytes_per_pixel) {
        lv_obj_del(canvas);
        return;
    }
    size_t buf_size = width * height * bytes_per_pixel;
    void *buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = calloc(1, buf_size);
    if (!buf) {
        lv_obj_del(canvas);
        return;
    }

    lv_canvas_set_buffer(canvas, buf, ctx->width, ctx->height, LV_IMG_CF_TRUE_COLOR);
    lv_obj_add_event_cb(canvas, canvas_buf_delete_cb, LV_EVENT_DELETE, buf);
    ctx->result = (ghostesp_ui_obj_t)canvas;
}

ghostesp_ui_obj_t plugin_api_ui_canvas_create(ghostesp_ui_obj_t parent, int32_t width, int32_t height) {
    if (!plugin_api_internal_has_ui_permission()) return NULL;
    if (width <= 0 || height <= 0) return NULL;
    canvas_create_ctx_t ctx = { .parent = parent, .width = width, .height = height };
    return plugin_api_internal_run_sync(plugin_api_ui_canvas_create_now, &ctx) ? ctx.result : NULL;
}

static void plugin_api_ui_canvas_draw_rect_now(void *arg) {
    canvas_rect_ctx_t *ctx = (canvas_rect_ctx_t *)arg;
    lv_obj_t *canvas = (lv_obj_t *)ctx->canvas;
    if (!canvas || !lv_obj_is_valid(canvas)) return;

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(ctx->hex_color);
    dsc.bg_opa = LV_OPA_COVER;

    lv_canvas_draw_rect(canvas, ctx->x, ctx->y, ctx->w, ctx->h, &dsc);
}

void plugin_api_ui_canvas_draw_rect(ghostesp_ui_obj_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t hex_color) {
    if (!plugin_api_internal_has_ui_permission()) return;
    canvas_rect_ctx_t ctx = { .canvas = canvas, .x = x, .y = y, .w = w, .h = h, .hex_color = hex_color };
    plugin_api_internal_run_sync(plugin_api_ui_canvas_draw_rect_now, &ctx);
}

static void plugin_api_ui_canvas_fill_now(void *arg) {
    obj_color_ctx_t *ctx = (obj_color_ctx_t *)arg;
    lv_obj_t *canvas = (lv_obj_t *)ctx->obj;
    if (!canvas || !lv_obj_is_valid(canvas)) return;
    lv_canvas_fill_bg(canvas, lv_color_hex(ctx->color), LV_OPA_COVER);
}

void plugin_api_ui_canvas_fill(ghostesp_ui_obj_t canvas, uint32_t hex_color) {
    if (!plugin_api_internal_has_ui_permission()) return;
    obj_color_ctx_t ctx = { .obj = canvas, .color = hex_color };
    plugin_api_internal_run_sync(plugin_api_ui_canvas_fill_now, &ctx);
}

static void plugin_api_ui_canvas_draw_line_now(void *arg) {
    canvas_line_draw_ctx_t *ctx = (canvas_line_draw_ctx_t *)arg;
    lv_obj_t *canvas = (lv_obj_t *)ctx->canvas;
    if (!canvas || !lv_obj_is_valid(canvas) || !ctx->points || ctx->count <= 0) return;

    lv_point_t *lv_pts = malloc(sizeof(lv_point_t) * ctx->count);
    if (!lv_pts) return;
    for (int i = 0; i < ctx->count; i++) {
        lv_pts[i].x = ctx->points[i].x;
        lv_pts[i].y = ctx->points[i].y;
    }

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = lv_color_hex(ctx->hex_color);
    dsc.width = ctx->width;

    lv_canvas_draw_line(canvas, lv_pts, (uint32_t)ctx->count, &dsc);
    free(lv_pts);
}

void plugin_api_ui_canvas_draw_line(ghostesp_ui_obj_t canvas, const ghostesp_point_t *points, int count, uint32_t hex_color, int32_t width) {
    if (!plugin_api_internal_has_ui_permission()) return;
    canvas_line_draw_ctx_t ctx = { .canvas = canvas, .points = points, .count = count, .hex_color = hex_color, .width = width };
    plugin_api_internal_run_sync(plugin_api_ui_canvas_draw_line_now, &ctx);
}

static void plugin_api_ui_canvas_draw_arc_now(void *arg) {
    canvas_arc_draw_ctx_t *ctx = (canvas_arc_draw_ctx_t *)arg;
    lv_obj_t *canvas = (lv_obj_t *)ctx->canvas;
    if (!canvas || !lv_obj_is_valid(canvas)) return;

    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = lv_color_hex(ctx->hex_color);
    dsc.width = ctx->width;

    lv_canvas_draw_arc(canvas, ctx->cx, ctx->cy, (uint32_t)ctx->r, ctx->start_angle, ctx->end_angle, &dsc);
}

void plugin_api_ui_canvas_draw_arc(ghostesp_ui_obj_t canvas, int32_t cx, int32_t cy, int32_t r, int32_t start_angle, int32_t end_angle, uint32_t hex_color, int32_t width) {
    if (!plugin_api_internal_has_ui_permission()) return;
    canvas_arc_draw_ctx_t ctx = { .canvas = canvas, .cx = cx, .cy = cy, .r = r, .start_angle = start_angle, .end_angle = end_angle, .hex_color = hex_color, .width = width };
    plugin_api_internal_run_sync(plugin_api_ui_canvas_draw_arc_now, &ctx);
}

static void plugin_api_ui_line_create_now(void *arg) {
    simple_create_ctx_t *ctx = (simple_create_ctx_t *)arg;
    lv_obj_t *parent = plugin_api_internal_parent_or_current(ctx->parent);
    lv_obj_t *line = lv_line_create(parent);
    if (!line) return;
    lv_obj_set_user_data(line, NULL);
    lv_obj_add_event_cb(line, line_points_delete_cb, LV_EVENT_DELETE, NULL);
    ctx->result = (ghostesp_ui_obj_t)line;
}

ghostesp_ui_obj_t plugin_api_ui_line_create(ghostesp_ui_obj_t parent) {
    if (!plugin_api_internal_has_ui_permission()) return NULL;
    simple_create_ctx_t ctx = { .parent = parent };
    return plugin_api_internal_run_sync(plugin_api_ui_line_create_now, &ctx) ? ctx.result : NULL;
}

static void plugin_api_ui_line_set_points_now(void *arg) {
    line_points_ctx_t *ctx = (line_points_ctx_t *)arg;
    lv_obj_t *line = (lv_obj_t *)ctx->obj;
    if (!line || !lv_obj_is_valid(line) || !ctx->points || ctx->count <= 0) return;

    lv_point_t *old = (lv_point_t *)lv_obj_get_user_data(line);
    free(old);

    lv_point_t *pts = malloc(sizeof(lv_point_t) * ctx->count);
    if (!pts) return;
    for (int i = 0; i < ctx->count; i++) {
        pts[i].x = ctx->points[i].x;
        pts[i].y = ctx->points[i].y;
    }

    lv_obj_set_user_data(line, pts);
    lv_line_set_points(line, pts, (uint16_t)ctx->count);
}

void plugin_api_ui_line_set_points(ghostesp_ui_obj_t line, const ghostesp_point_t *points, int count) {
    if (!plugin_api_internal_has_ui_permission()) return;
    line_points_ctx_t ctx = { .obj = line, .points = points, .count = count };
    plugin_api_internal_run_sync(plugin_api_ui_line_set_points_now, &ctx);
}

static void plugin_api_ui_line_set_color_now(void *arg) {
    obj_color_ctx_t *ctx = (obj_color_ctx_t *)arg;
    lv_obj_t *line = (lv_obj_t *)ctx->obj;
    if (!line || !lv_obj_is_valid(line)) return;
    lv_obj_set_style_line_color(line, lv_color_hex(ctx->color), LV_PART_MAIN);
}

void plugin_api_ui_line_set_color(ghostesp_ui_obj_t line, uint32_t hex_color) {
    if (!plugin_api_internal_has_ui_permission()) return;
    obj_color_ctx_t ctx = { .obj = line, .color = hex_color };
    plugin_api_internal_run_sync(plugin_api_ui_line_set_color_now, &ctx);
}

static void plugin_api_ui_line_set_width_now(void *arg) {
    obj_value_ctx_t *ctx = (obj_value_ctx_t *)arg;
    lv_obj_t *line = (lv_obj_t *)ctx->obj;
    if (!line || !lv_obj_is_valid(line)) return;
    lv_obj_set_style_line_width(line, (lv_coord_t)ctx->value, LV_PART_MAIN);
}

void plugin_api_ui_line_set_width(ghostesp_ui_obj_t line, int32_t width) {
    if (!plugin_api_internal_has_ui_permission()) return;
    obj_value_ctx_t ctx = { .obj = line, .value = width };
    plugin_api_internal_run_sync(plugin_api_ui_line_set_width_now, &ctx);
}

static void plugin_api_ui_arc_create_now(void *arg) {
    simple_create_ctx_t *ctx = (simple_create_ctx_t *)arg;
    lv_obj_t *parent = plugin_api_internal_parent_or_current(ctx->parent);
    lv_obj_t *arc = lv_arc_create(parent);
    if (!arc) return;
    lv_arc_set_mode(arc, LV_ARC_MODE_NORMAL);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(arc, 0, LV_PART_INDICATOR);
    lv_obj_set_size(arc, 100, 100);
    ctx->result = (ghostesp_ui_obj_t)arc;
}

ghostesp_ui_obj_t plugin_api_ui_arc_create(ghostesp_ui_obj_t parent) {
    if (!plugin_api_internal_has_ui_permission()) return NULL;
    simple_create_ctx_t ctx = { .parent = parent };
    return plugin_api_internal_run_sync(plugin_api_ui_arc_create_now, &ctx) ? ctx.result : NULL;
}

static void plugin_api_ui_arc_set_value_now(void *arg) {
    obj_value_ctx_t *ctx = (obj_value_ctx_t *)arg;
    lv_obj_t *arc = (lv_obj_t *)ctx->obj;
    if (!arc || !lv_obj_is_valid(arc)) return;
    lv_arc_set_value(arc, ctx->value);
}

void plugin_api_ui_arc_set_value(ghostesp_ui_obj_t arc, int32_t value) {
    if (!plugin_api_internal_has_ui_permission()) return;
    obj_value_ctx_t ctx = { .obj = arc, .value = value };
    plugin_api_internal_run_sync(plugin_api_ui_arc_set_value_now, &ctx);
}

static void plugin_api_ui_arc_set_range_now(void *arg) {
    obj_pair_ctx_t *ctx = (obj_pair_ctx_t *)arg;
    lv_obj_t *arc = (lv_obj_t *)ctx->obj;
    if (!arc || !lv_obj_is_valid(arc)) return;
    lv_arc_set_range(arc, ctx->val1, ctx->val2);
}

void plugin_api_ui_arc_set_range(ghostesp_ui_obj_t arc, int32_t min, int32_t max) {
    if (!plugin_api_internal_has_ui_permission()) return;
    obj_pair_ctx_t ctx = { .obj = arc, .val1 = min, .val2 = max };
    plugin_api_internal_run_sync(plugin_api_ui_arc_set_range_now, &ctx);
}

static void plugin_api_ui_arc_set_angles_now(void *arg) {
    obj_pair_ctx_t *ctx = (obj_pair_ctx_t *)arg;
    lv_obj_t *arc = (lv_obj_t *)ctx->obj;
    if (!arc || !lv_obj_is_valid(arc)) return;
    lv_arc_set_start_angle(arc, (uint16_t)ctx->val1);
    lv_arc_set_end_angle(arc, (uint16_t)ctx->val2);
}

void plugin_api_ui_arc_set_angles(ghostesp_ui_obj_t arc, int32_t start, int32_t end) {
    if (!plugin_api_internal_has_ui_permission()) return;
    obj_pair_ctx_t ctx = { .obj = arc, .val1 = start, .val2 = end };
    plugin_api_internal_run_sync(plugin_api_ui_arc_set_angles_now, &ctx);
}

static void plugin_api_ui_arc_set_bg_angles_now(void *arg) {
    obj_pair_ctx_t *ctx = (obj_pair_ctx_t *)arg;
    lv_obj_t *arc = (lv_obj_t *)ctx->obj;
    if (!arc || !lv_obj_is_valid(arc)) return;
    lv_arc_set_bg_start_angle(arc, (uint16_t)ctx->val1);
    lv_arc_set_bg_end_angle(arc, (uint16_t)ctx->val2);
}

void plugin_api_ui_arc_set_bg_angles(ghostesp_ui_obj_t arc, int32_t start, int32_t end) {
    if (!plugin_api_internal_has_ui_permission()) return;
    obj_pair_ctx_t ctx = { .obj = arc, .val1 = start, .val2 = end };
    plugin_api_internal_run_sync(plugin_api_ui_arc_set_bg_angles_now, &ctx);
}

static void plugin_api_ui_arc_set_bg_color_now(void *arg) {
    obj_color_ctx_t *ctx = (obj_color_ctx_t *)arg;
    lv_obj_t *arc = (lv_obj_t *)ctx->obj;
    if (!arc || !lv_obj_is_valid(arc)) return;
    lv_obj_set_style_arc_color(arc, lv_color_hex(ctx->color), LV_PART_MAIN);
}

void plugin_api_ui_arc_set_bg_color(ghostesp_ui_obj_t arc, uint32_t hex_color) {
    if (!plugin_api_internal_has_ui_permission()) return;
    obj_color_ctx_t ctx = { .obj = arc, .color = hex_color };
    plugin_api_internal_run_sync(plugin_api_ui_arc_set_bg_color_now, &ctx);
}

static void plugin_api_ui_arc_set_indicator_color_now(void *arg) {
    obj_color_ctx_t *ctx = (obj_color_ctx_t *)arg;
    lv_obj_t *arc = (lv_obj_t *)ctx->obj;
    if (!arc || !lv_obj_is_valid(arc)) return;
    lv_obj_set_style_arc_color(arc, lv_color_hex(ctx->color), LV_PART_INDICATOR);
}

void plugin_api_ui_arc_set_indicator_color(ghostesp_ui_obj_t arc, uint32_t hex_color) {
    if (!plugin_api_internal_has_ui_permission()) return;
    obj_color_ctx_t ctx = { .obj = arc, .color = hex_color };
    plugin_api_internal_run_sync(plugin_api_ui_arc_set_indicator_color_now, &ctx);
}

static void plugin_api_ui_image_create_now(void *arg) {
    simple_create_ctx_t *ctx = (simple_create_ctx_t *)arg;
    lv_obj_t *parent = plugin_api_internal_parent_or_current(ctx->parent);
    lv_obj_t *img = lv_img_create(parent);
    if (!img) return;
    ctx->result = (ghostesp_ui_obj_t)img;
}

ghostesp_ui_obj_t plugin_api_ui_image_create(ghostesp_ui_obj_t parent) {
    if (!plugin_api_internal_has_ui_permission()) return NULL;
    simple_create_ctx_t ctx = { .parent = parent };
    return plugin_api_internal_run_sync(plugin_api_ui_image_create_now, &ctx) ? ctx.result : NULL;
}

static void plugin_api_ui_image_set_src_now(void *arg) {
    image_src_ctx_t *ctx = (image_src_ctx_t *)arg;
    lv_obj_t *img = (lv_obj_t *)ctx->obj;
    if (!img || !lv_obj_is_valid(img) || !ctx->path) return;
    lv_img_set_src(img, ctx->path);
    ctx->result = true;
}

bool plugin_api_ui_image_set_src(ghostesp_ui_obj_t img, const char *app_relative_path) {
    if (!plugin_api_internal_has_ui_permission()) return false;
    if (!img || !app_relative_path) return false;
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_internal_build_app_path(app_relative_path, full_path, sizeof(full_path))) return false;
    image_src_ctx_t ctx = { .obj = img, .path = full_path, .result = false };
    plugin_api_internal_run_sync(plugin_api_ui_image_set_src_now, &ctx);
    return ctx.result;
}

static void plugin_api_ui_canvas_blit_rgb565_now(void *arg) {
    canvas_blit_ctx_t *ctx = (canvas_blit_ctx_t *)arg;
    lv_obj_t *canvas = (lv_obj_t *)ctx->canvas;
    if (!canvas || !lv_obj_is_valid(canvas) || !ctx->pixels || ctx->src_width <= 0 ||
        ctx->src_height <= 0 || ctx->src_stride < ctx->src_width || ctx->dst_width <= 0 ||
        ctx->dst_height <= 0 || LV_COLOR_SIZE != 16) return;

    lv_img_dsc_t *image = lv_canvas_get_img(canvas);
    if (!image || !image->data || image->header.w <= 0 || image->header.h <= 0) return;

    const int32_t canvas_width = image->header.w;
    const int32_t canvas_height = image->header.h;
    if ((size_t)(ctx->src_height - 1) > (SIZE_MAX - (size_t)ctx->src_width) / (size_t)ctx->src_stride ||
        (size_t)canvas_height > SIZE_MAX / (size_t)canvas_width) return;
    const int64_t dst_right = (int64_t)ctx->dst_x + ctx->dst_width;
    const int64_t dst_bottom = (int64_t)ctx->dst_y + ctx->dst_height;
    const int32_t left = ctx->dst_x < 0 ? 0 : ctx->dst_x;
    const int32_t top = ctx->dst_y < 0 ? 0 : ctx->dst_y;
    const int32_t right = dst_right > canvas_width ? canvas_width : (int32_t)dst_right;
    const int32_t bottom = dst_bottom > canvas_height ? canvas_height : (int32_t)dst_bottom;
    if (left >= right || top >= bottom) return;

    uint16_t *destination = (uint16_t *)image->data;
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    native_canvas_scale_rgb565(destination, canvas_width, ctx->pixels,
        ctx->src_width, ctx->src_height, ctx->src_stride,
        ctx->dst_x, ctx->dst_y, ctx->dst_width, ctx->dst_height,
        left, top, right, bottom, LV_COLOR_16_SWAP);
#else
    const bool copy_rows = left == ctx->dst_x && right == dst_right &&
                           ctx->dst_width == ctx->src_width;
    for (int32_t y = top; y < bottom; y++) {
        int32_t source_y = (int32_t)(((int64_t)y - ctx->dst_y) * ctx->src_height / ctx->dst_height);
        const uint16_t *source_row = ctx->pixels + (size_t)source_y * (size_t)ctx->src_stride;
        uint16_t *destination_row = destination + (size_t)y * (size_t)canvas_width;
        if (copy_rows) {
#if LV_COLOR_16_SWAP
            int32_t x = 0;
            for (; x + 1 < ctx->src_width; x += 2) {
                uint32_t pixels;
                memcpy(&pixels, source_row + x, sizeof(pixels));
                pixels = ((pixels & 0x00ff00ffu) << 8) | ((pixels & 0xff00ff00u) >> 8);
                memcpy(destination_row + left + x, &pixels, sizeof(pixels));
            }
            for (; x < ctx->src_width; ++x) {
                uint16_t pixel = source_row[x];
                destination_row[left + x] = (uint16_t)(pixel << 8 | pixel >> 8);
            }
#else
            memcpy(destination_row + left, source_row, (size_t)ctx->src_width * sizeof(uint16_t));
#endif
            continue;
        }
        for (int32_t x = left; x < right; x++) {
            int32_t source_x = (int32_t)(((int64_t)x - ctx->dst_x) * ctx->src_width / ctx->dst_width);
#if LV_COLOR_16_SWAP
            uint16_t pixel = source_row[source_x];
            destination_row[x] = (uint16_t)(pixel << 8 | pixel >> 8);
#else
            destination_row[x] = source_row[source_x];
#endif
        }
    }
#endif
    lv_area_t area;
    lv_obj_get_coords(canvas, &area);
    area.x1 += left;
    area.y1 += top;
    area.x2 = area.x1 + (right - left) - 1;
    area.y2 = area.y1 + (bottom - top) - 1;
    lv_obj_invalidate_area(canvas, &area);
    ctx->result = true;
}

bool plugin_api_ui_canvas_blit_rgb565(ghostesp_ui_obj_t canvas, const uint16_t *pixels,
                                      int32_t src_width, int32_t src_height, int32_t src_stride,
                                      int32_t dst_x, int32_t dst_y, int32_t dst_width, int32_t dst_height) {
    if (!plugin_api_internal_has_ui_permission()) return false;
    canvas_blit_ctx_t ctx = {
        .canvas = canvas, .pixels = pixels, .src_width = src_width, .src_height = src_height,
        .src_stride = src_stride, .dst_x = dst_x, .dst_y = dst_y,
        .dst_width = dst_width, .dst_height = dst_height,
    };
    return plugin_api_internal_run_sync(plugin_api_ui_canvas_blit_rgb565_now, &ctx) && ctx.result;
}

/* Async blit: a rendering app that blocks on every frame's blit serializes
   its own render against the UI task's compositing+flush, which roughly
   doubles frame time. These let the app hand off a finished frame and get
   straight back to rendering the next one. Only one blit may be in flight
   (the app is expected to double-buffer), which also keeps the queued
   context to a single allocation. */
static volatile bool s_async_blit_pending = false;
static SemaphoreHandle_t s_async_blit_done = NULL;

static void plugin_api_ui_canvas_blit_async_cb(void *arg) {
    canvas_blit_ctx_t *ctx = (canvas_blit_ctx_t *)arg;
    plugin_api_ui_canvas_blit_rgb565_now(ctx);
    free(ctx->owned_pixels);
    free(ctx);
    s_async_blit_pending = false;
    if (s_async_blit_done) xSemaphoreGive(s_async_blit_done);
}

bool plugin_api_ui_canvas_blit_async_wait(uint32_t timeout_ms) {
    if (!s_async_blit_pending) return true;
    if (!s_async_blit_done) return false;
    TickType_t timeout = timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s_async_blit_done, timeout) != pdTRUE) return false;
    /* Give it back so a second waiter (or the drain below) still sees a
       consistent state; pending is the authoritative flag. */
    xSemaphoreGive(s_async_blit_done);
    return !s_async_blit_pending;
}

bool plugin_api_ui_canvas_blit_rgb565_async(ghostesp_ui_obj_t canvas, const uint16_t *pixels,
                                            int32_t src_width, int32_t src_height, int32_t src_stride,
                                            int32_t dst_x, int32_t dst_y, int32_t dst_width, int32_t dst_height) {
    if (!plugin_api_internal_has_ui_permission()) return false;
    if (!pixels || src_width <= 0 || src_height <= 0 || src_stride < src_width ||
        dst_width <= 0 || dst_height <= 0) return false;
    if (s_async_blit_pending) return false;
    if (!s_async_blit_done) {
        s_async_blit_done = xSemaphoreCreateBinary();
        if (!s_async_blit_done) return false;
    }
    const size_t width = (size_t)src_width;
    const size_t height = (size_t)src_height;
    const size_t max_pixels = (size_t)LV_HOR_RES * (size_t)LV_VER_RES;
    if (width > SIZE_MAX / height || width * height > max_pixels ||
        width * height > SIZE_MAX / sizeof(uint16_t) ||
        (size_t)(src_height - 1) > (SIZE_MAX - width) / (size_t)src_stride) return false;
    const size_t pixel_count = width * height;
    canvas_blit_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return false;
    ctx->owned_pixels = malloc(pixel_count * sizeof(uint16_t));
    if (!ctx->owned_pixels) {
        free(ctx);
        return false;
    }
    for (size_t y = 0; y < height; ++y) {
        memcpy(ctx->owned_pixels + y * width,
               pixels + y * (size_t)src_stride,
               width * sizeof(uint16_t));
    }
    *ctx = (canvas_blit_ctx_t){
        .canvas = canvas, .pixels = ctx->owned_pixels, .src_width = src_width, .src_height = src_height,
        .src_stride = src_width, .dst_x = dst_x, .dst_y = dst_y,
        .dst_width = dst_width, .dst_height = dst_height,
        .owned_pixels = ctx->owned_pixels,
    };
    while (xSemaphoreTake(s_async_blit_done, 0) == pdTRUE) { /* drain stale signal */ }
    s_async_blit_pending = true;
    /* Runs inline if we're already on the UI task, which clears pending
       before this returns — that's fine, it just degrades to synchronous. */
    plugin_api_internal_run_async(plugin_api_ui_canvas_blit_async_cb, ctx);
    return true;
}

bool plugin_api_ui_canvas_is_rgb565_native_byte_order(void) {
    /* LV_COLOR_16_SWAP exchanges bytes on read/write so the canvas storage already
       matches what the display DMA pushes to the panel. Apps may then fill their
       RGB565 framebuffer directly in screen byte order. */
#if LV_COLOR_16_SWAP
    return true;
#else
    return false;
#endif
}

static void plugin_api_ui_image_set_builtin_now(void *arg) {
    builtin_image_src_ctx_t *ctx = (builtin_image_src_ctx_t *)arg;
    lv_obj_t *img = (lv_obj_t *)ctx->obj;
    if (!img || !lv_obj_is_valid(img) || !ctx->source) return;
    lv_img_set_src(img, ctx->source);
    ctx->result = true;
}

bool plugin_api_ui_image_set_builtin(ghostesp_ui_obj_t img, const char *image_name) {
    if (!plugin_api_internal_has_ui_permission() || !img || !image_name) return false;
    for (size_t i = 0; i < sizeof(s_builtin_images) / sizeof(s_builtin_images[0]); ++i) {
        if (strcmp(image_name, s_builtin_images[i].name) != 0) continue;
        builtin_image_src_ctx_t ctx = { .obj = img, .source = s_builtin_images[i].source, .result = false };
        plugin_api_internal_run_sync(plugin_api_ui_image_set_builtin_now, &ctx);
        return ctx.result;
    }
    return false;
}

static void plugin_api_ui_timer_create_now(void *arg) {
    timer_create_ctx_t *ctx = (timer_create_ctx_t *)arg;
    timer_bridge_t *bridge = calloc(1, sizeof(timer_bridge_t));
    if (!bridge) return;
    bridge->cb = ctx->cb;
    bridge->user = ctx->user;
    bridge->next = s_timer_bridge_head;
    s_timer_bridge_head = bridge;
    lv_timer_t *timer = lv_timer_create(timer_bridge_cb, ctx->interval_ms, bridge);
    if (!timer) {
        s_timer_bridge_head = bridge->next;
        free(bridge);
        return;
    }
    bridge->timer = timer;
    ctx->result = (ghostesp_ui_timer_t)timer;
}

ghostesp_ui_timer_t plugin_api_ui_timer_create(ghostesp_ui_timer_cb_t cb, uint32_t interval_ms, void *user) {
    if (!plugin_api_internal_has_ui_permission() || !cb) return NULL;
    timer_create_ctx_t ctx = { .cb = cb, .interval_ms = interval_ms, .user = user };
    return plugin_api_internal_run_sync(plugin_api_ui_timer_create_now, &ctx) ? ctx.result : NULL;
}

static void plugin_api_ui_timer_delete_now(void *arg) {
    timer_delete_ctx_t *ctx = (timer_delete_ctx_t *)arg;
    lv_timer_t *timer = (lv_timer_t *)ctx->timer;
    if (!timer) return;
    timer_bridge_t *bridge = (timer_bridge_t *)timer->user_data;
    lv_timer_del(timer);
    if (bridge) {
        timer_bridge_t **pp = &s_timer_bridge_head;
        while (*pp && *pp != bridge) pp = &(*pp)->next;
        if (*pp) *pp = bridge->next;
        free(bridge);
    }
}

void plugin_api_ui_timer_delete(ghostesp_ui_timer_t timer) {
    if (!plugin_api_internal_has_ui_permission() || !timer) return;
    timer_delete_ctx_t ctx = { .timer = timer };
    plugin_api_internal_run_sync(plugin_api_ui_timer_delete_now, &ctx);
}

static void plugin_api_ui_timer_set_interval_now(void *arg) {
    timer_interval_ctx_t *ctx = (timer_interval_ctx_t *)arg;
    lv_timer_t *timer = (lv_timer_t *)ctx->timer;
    if (timer) lv_timer_set_period(timer, ctx->interval_ms);
}

void plugin_api_ui_timer_set_interval(ghostesp_ui_timer_t timer, uint32_t interval_ms) {
    if (!plugin_api_internal_has_ui_permission() || !timer) return;
    timer_interval_ctx_t ctx = { .timer = timer, .interval_ms = interval_ms };
    plugin_api_internal_run_sync(plugin_api_ui_timer_set_interval_now, &ctx);
}

static void plugin_api_ui_anim_slide_in_now(void *arg) {
    anim_slide_ctx_t *ctx = (anim_slide_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    gui_anim_slide_in(obj, (gui_anim_dir_t)ctx->direction, ctx->duration_ms);
}

void plugin_api_ui_anim_slide_in(ghostesp_ui_obj_t obj, int direction, uint32_t duration_ms) {
    if (!plugin_api_internal_has_ui_permission()) return;
    anim_slide_ctx_t ctx = { .obj = obj, .direction = direction, .duration_ms = duration_ms };
    plugin_api_internal_run_sync(plugin_api_ui_anim_slide_in_now, &ctx);
}

static void plugin_api_ui_anim_slide_out_now(void *arg) {
    anim_slide_out_ctx_t *ctx = (anim_slide_out_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;

    lv_anim_ready_cb_t ready_cb = NULL;
    void *ready_user = NULL;

    if (ctx->on_done) {
        anim_done_bridge_t *bridge = calloc(1, sizeof(anim_done_bridge_t));
        if (bridge) {
            bridge->cb = ctx->on_done;
            bridge->user = ctx->user;
            ready_cb = anim_ready_bridge;
            ready_user = bridge;
        }
    }

    gui_anim_slide_out(obj, (gui_anim_dir_t)ctx->direction, ctx->duration_ms, ready_cb, ready_user);
}

void plugin_api_ui_anim_slide_out(ghostesp_ui_obj_t obj, int direction, uint32_t duration_ms, ghostesp_anim_done_cb_t on_done, void *user) {
    if (!plugin_api_internal_has_ui_permission()) return;
    anim_slide_out_ctx_t ctx = { .obj = obj, .direction = direction, .duration_ms = duration_ms, .on_done = on_done, .user = user };
    plugin_api_internal_run_sync(plugin_api_ui_anim_slide_out_now, &ctx);
}

static void plugin_api_ui_anim_pop_in_now(void *arg) {
    obj_color_ctx_t *ctx = (obj_color_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    gui_anim_pop_in(obj);
}

void plugin_api_ui_anim_pop_in(ghostesp_ui_obj_t obj) {
    if (!plugin_api_internal_has_ui_permission()) return;
    obj_color_ctx_t ctx = { .obj = obj };
    plugin_api_internal_run_sync(plugin_api_ui_anim_pop_in_now, &ctx);
}

static void plugin_api_ui_anim_press_pulse_now(void *arg) {
    obj_color_ctx_t *ctx = (obj_color_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    gui_anim_press_pulse(obj);
}

void plugin_api_ui_anim_press_pulse(ghostesp_ui_obj_t obj) {
    if (!plugin_api_internal_has_ui_permission()) return;
    obj_color_ctx_t ctx = { .obj = obj };
    plugin_api_internal_run_sync(plugin_api_ui_anim_press_pulse_now, &ctx);
}

static void plugin_api_canvas_cleanup_timers_now(void *arg) {
    (void)arg;
    timer_bridge_t *bridge = s_timer_bridge_head;
    s_timer_bridge_head = NULL;
    while (bridge) {
        timer_bridge_t *next = bridge->next;
        if (bridge->timer) lv_timer_del(bridge->timer);
        free(bridge);
        bridge = next;
    }
}

void plugin_api_canvas_cleanup_timers(void) {
    plugin_api_internal_run_sync(plugin_api_canvas_cleanup_timers_now, NULL);
}
