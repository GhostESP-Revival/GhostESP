#include "managers/plugin_api_internal.h"
#include "gui/gui_anim.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>

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

    size_t buf_size = (size_t)ctx->width * ctx->height * LV_COLOR_SIZE / 8;
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
