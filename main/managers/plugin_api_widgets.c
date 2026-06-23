#include "managers/plugin_api_internal.h"
#include "gui/options_view.h"
#include "gui/detail_view.h"
#include "gui/popup.h"
#include "gui/scan_status.h"
#include "gui/paged_menu.h"
#include <stdlib.h>
#include <string.h>

#define WGT_BUTTON_CTX_POOL_SIZE 16
static plugin_ui_button_ctx_t wgt_button_ctx_pool[WGT_BUTTON_CTX_POOL_SIZE];
static uint32_t wgt_button_ctx_pool_mask = 0;
static plugin_ui_button_ctx_t* wgt_button_ctx_alloc(void) {
    for (int i = 0; i < WGT_BUTTON_CTX_POOL_SIZE; i++) {
        if (!(wgt_button_ctx_pool_mask & (1U << i))) {
            wgt_button_ctx_pool_mask |= (1U << i);
            memset(&wgt_button_ctx_pool[i], 0, sizeof(wgt_button_ctx_pool[i]));
            return &wgt_button_ctx_pool[i];
        }
    }
    return NULL;
}
static void wgt_button_ctx_free(plugin_ui_button_ctx_t *ptr) {
    if (!ptr) return;
    int idx = ptr - wgt_button_ctx_pool;
    if (idx >= 0 && idx < WGT_BUTTON_CTX_POOL_SIZE) {
        wgt_button_ctx_pool_mask &= ~(1U << idx);
    }
}

typedef struct {
    void *result;
    const char *str1;
} wgt_str_res_t;

typedef struct {
    void *handle;
    void *result;
    const char *label;
    ghostesp_ui_button_cb_t cb;
    void *user;
} wgt_add_t;

typedef struct {
    void *handle;
    const char *str;
} wgt_set_str_t;

typedef struct {
    void *handle;
    const char *label;
    const char *value;
} wgt_set_str2_t;

typedef struct {
    void *handle;
    int val;
    int val2;
    int result;
} wgt_int_t;

typedef struct {
    void *handle;
    int32_t w;
    int32_t h;
} wgt_wh_t;

typedef struct {
    void *handle;
    int current;
    int total;
} wgt_prog_t;

typedef struct {
    void *handle;
    bool result;
} wgt_bool_t;

typedef struct {
    ghostesp_paged_menu_load_fn load_fn;
    void *load_user;
    ghostesp_paged_menu_select_fn select_fn;
    ghostesp_paged_menu_nav_fn prev_fn;
    ghostesp_paged_menu_nav_fn next_fn;
    void *cb_user;
} pmenu_bridge_t;

typedef struct {
    paged_menu_t *pm;
    pmenu_bridge_t *bridge;
} pmenu_wrap_t;

typedef struct {
    int page_size;
    pmenu_bridge_t *bridge;
    pmenu_wrap_t *wrap;
} pmenu_create_sync_t;

typedef struct {
    pmenu_wrap_t *wrap;
    ghostesp_paged_menu_select_fn select_fn;
    ghostesp_paged_menu_nav_fn prev_fn;
    ghostesp_paged_menu_nav_fn next_fn;
    void *cb_user;
} pmenu_set_cb_sync_t;

static void widget_btn_bridge(lv_event_t *event) {
    plugin_ui_button_ctx_t *ctx = (plugin_ui_button_ctx_t *)lv_event_get_user_data(event);
    if (!ctx) return;
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_CLICKED) {
        if (ctx->cb) ctx->cb(ctx->user);
    } else     if (code == LV_EVENT_DELETE) {
        wgt_button_ctx_free(ctx);
    }
}

static int pmenu_load_bridge(int offset, int page_size, char names[][PAGED_MENU_NAME_MAX], bool *has_more, void *user_data) {
    pmenu_bridge_t *bridge = (pmenu_bridge_t *)user_data;
    if (bridge && bridge->load_fn) {
        bridge->load_fn(offset, page_size, names, has_more, bridge->load_user);
    }
    return 0;
}

static void pmenu_select_bridge(const char *name, void *user_data) {
    pmenu_bridge_t *bridge = (pmenu_bridge_t *)user_data;
    if (bridge && bridge->select_fn) bridge->select_fn(name, bridge->cb_user);
}

static void pmenu_prev_bridge(void *user_data) {
    pmenu_bridge_t *bridge = (pmenu_bridge_t *)user_data;
    if (bridge && bridge->prev_fn) bridge->prev_fn(bridge->cb_user);
}

static void pmenu_next_bridge(void *user_data) {
    pmenu_bridge_t *bridge = (pmenu_bridge_t *)user_data;
    if (bridge && bridge->next_fn) bridge->next_fn(bridge->cb_user);
}

static void opts_create_sync(void *arg) {
    wgt_str_res_t *ctx = (wgt_str_res_t *)arg;
    ctx->result = options_view_create(plugin_api_internal_parent_or_current(NULL), ctx->str1);
}

ghostesp_options_t plugin_api_ui_options_create(const char *title) {
    if (!plugin_api_internal_has_ui_permission()) return NULL;
    wgt_str_res_t ctx = { .str1 = title };
    return plugin_api_internal_run_sync(opts_create_sync, &ctx) ? ctx.result : NULL;
}

static void opts_add_item_sync(void *arg) {
    wgt_add_t *ctx = (wgt_add_t *)arg;
    options_view_t *ov = (options_view_t *)ctx->handle;
    plugin_ui_button_ctx_t *bctx = wgt_button_ctx_alloc();
    if (!bctx) return;
    bctx->cb = ctx->cb;
    bctx->user = ctx->user;
    ctx->result = options_view_add_item(ov, ctx->label, widget_btn_bridge, bctx);
    if (ctx->result) {
        lv_obj_add_event_cb((lv_obj_t *)ctx->result, widget_btn_bridge, LV_EVENT_DELETE, bctx);
    } else {
        wgt_button_ctx_free(bctx);
    }
}

ghostesp_ui_obj_t plugin_api_ui_options_add_item(ghostesp_options_t opts, const char *label, ghostesp_ui_button_cb_t on_click, void *user) {
    if (!plugin_api_internal_has_ui_permission()) return NULL;
    if (!opts) return NULL;
    wgt_add_t ctx = { .handle = opts, .label = label, .cb = on_click, .user = user };
    return plugin_api_internal_run_sync(opts_add_item_sync, &ctx) ? ctx.result : NULL;
}

static void opts_add_back_sync(void *arg) {
    wgt_add_t *ctx = (wgt_add_t *)arg;
    options_view_t *ov = (options_view_t *)ctx->handle;
    plugin_ui_button_ctx_t *bctx = wgt_button_ctx_alloc();
    if (!bctx) return;
    bctx->cb = ctx->cb;
    bctx->user = ctx->user;
    ctx->result = options_view_add_back_row(ov, widget_btn_bridge, bctx);
    if (ctx->result) {
        lv_obj_add_event_cb((lv_obj_t *)ctx->result, widget_btn_bridge, LV_EVENT_DELETE, bctx);
    } else {
        wgt_button_ctx_free(bctx);
    }
}

ghostesp_ui_obj_t plugin_api_ui_options_add_back(ghostesp_options_t opts, ghostesp_ui_button_cb_t on_click, void *user) {
    if (!plugin_api_internal_has_ui_permission()) return NULL;
    if (!opts) return NULL;
    wgt_add_t ctx = { .handle = opts, .cb = on_click, .user = user };
    return plugin_api_internal_run_sync(opts_add_back_sync, &ctx) ? ctx.result : NULL;
}

static void opts_set_sel_sync(void *arg) {
    wgt_int_t *ctx = (wgt_int_t *)arg;
    options_view_set_selected((options_view_t *)ctx->handle, ctx->val);
}

void plugin_api_ui_options_set_selected(ghostesp_options_t opts, int index) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!opts) return;
    wgt_int_t ctx = { .handle = opts, .val = index };
    plugin_api_internal_run_sync(opts_set_sel_sync, &ctx);
}

static void opts_move_sel_sync(void *arg) {
    wgt_int_t *ctx = (wgt_int_t *)arg;
    options_view_move_selection((options_view_t *)ctx->handle, ctx->val);
}

void plugin_api_ui_options_move_selection(ghostesp_options_t opts, int delta) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!opts) return;
    wgt_int_t ctx = { .handle = opts, .val = delta };
    plugin_api_internal_run_sync(opts_move_sel_sync, &ctx);
}

static void opts_get_sel_sync(void *arg) {
    wgt_int_t *ctx = (wgt_int_t *)arg;
    ctx->result = options_view_get_selected((const options_view_t *)ctx->handle);
}

int plugin_api_ui_options_get_selected(ghostesp_options_t opts) {
    if (!plugin_api_internal_has_ui_permission()) return -1;
    if (!opts) return -1;
    wgt_int_t ctx = { .handle = opts };
    return plugin_api_internal_run_sync(opts_get_sel_sync, &ctx) ? ctx.result : -1;
}

static void opts_clear_sync(void *arg) {
    options_view_clear((options_view_t *)arg);
}

void plugin_api_ui_options_clear(ghostesp_options_t opts) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!opts) return;
    plugin_api_internal_run_sync(opts_clear_sync, opts);
}

static void opts_destroy_sync(void *arg) {
    options_view_destroy((options_view_t *)arg);
}

void plugin_api_ui_options_destroy(ghostesp_options_t opts) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!opts) return;
    plugin_api_internal_run_sync(opts_destroy_sync, opts);
}

static void dv_create_sync(void *arg) {
    wgt_str_res_t *ctx = (wgt_str_res_t *)arg;
    ctx->result = detail_view_create(plugin_api_internal_parent_or_current(NULL), ctx->str1);
}

ghostesp_detail_t plugin_api_ui_detail_create(const char *title) {
    if (!plugin_api_internal_has_ui_permission()) return NULL;
    wgt_str_res_t ctx = { .str1 = title };
    return plugin_api_internal_run_sync(dv_create_sync, &ctx) ? ctx.result : NULL;
}

static void dv_add_info_sync(void *arg) {
    wgt_set_str2_t *ctx = (wgt_set_str2_t *)arg;
    detail_view_add_info((detail_view_t *)ctx->handle, ctx->label, ctx->value);
}

void plugin_api_ui_detail_add_info(ghostesp_detail_t dv, const char *label, const char *value) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!dv) return;
    wgt_set_str2_t ctx = { .handle = dv, .label = label, .value = value };
    plugin_api_internal_run_sync(dv_add_info_sync, &ctx);
}

static void dv_add_action_sync(void *arg) {
    wgt_add_t *ctx = (wgt_add_t *)arg;
    detail_view_t *d = (detail_view_t *)ctx->handle;
    plugin_ui_button_ctx_t *bctx = wgt_button_ctx_alloc();
    if (!bctx) return;
    bctx->cb = ctx->cb;
    bctx->user = ctx->user;
    lv_obj_t *btn = detail_view_add_action(d, ctx->label, widget_btn_bridge, bctx);
    if (btn) {
        lv_obj_add_event_cb(btn, widget_btn_bridge, LV_EVENT_DELETE, bctx);
    } else {
        wgt_button_ctx_free(bctx);
    }
}

void plugin_api_ui_detail_add_action(ghostesp_detail_t dv, const char *label, ghostesp_ui_button_cb_t on_click, void *user) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!dv) return;
    wgt_add_t ctx = { .handle = dv, .label = label, .cb = on_click, .user = user };
    plugin_api_internal_run_sync(dv_add_action_sync, &ctx);
}

static void dv_add_header_sync(void *arg) {
    wgt_set_str_t *ctx = (wgt_set_str_t *)arg;
    detail_view_add_header((detail_view_t *)ctx->handle, ctx->str);
}

void plugin_api_ui_detail_add_header(ghostesp_detail_t dv, const char *text) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!dv) return;
    wgt_set_str_t ctx = { .handle = dv, .str = text };
    plugin_api_internal_run_sync(dv_add_header_sync, &ctx);
}

static void dv_add_divider_sync(void *arg) {
    detail_view_add_divider((detail_view_t *)arg);
}

void plugin_api_ui_detail_add_divider(ghostesp_detail_t dv) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!dv) return;
    plugin_api_internal_run_sync(dv_add_divider_sync, dv);
}

static void dv_add_back_sync(void *arg) {
    wgt_add_t *ctx = (wgt_add_t *)arg;
    detail_view_t *d = (detail_view_t *)ctx->handle;
    plugin_ui_button_ctx_t *bctx = wgt_button_ctx_alloc();
    if (!bctx) return;
    bctx->cb = ctx->cb;
    bctx->user = ctx->user;
    ctx->result = detail_view_add_back(d, widget_btn_bridge, bctx);
    if (ctx->result) {
        lv_obj_add_event_cb((lv_obj_t *)ctx->result, widget_btn_bridge, LV_EVENT_DELETE, bctx);
    } else {
        wgt_button_ctx_free(bctx);
    }
}

ghostesp_ui_obj_t plugin_api_ui_detail_add_back(ghostesp_detail_t dv, ghostesp_ui_button_cb_t on_click, void *user) {
    if (!plugin_api_internal_has_ui_permission()) return NULL;
    if (!dv) return NULL;
    wgt_add_t ctx = { .handle = dv, .cb = on_click, .user = user };
    return plugin_api_internal_run_sync(dv_add_back_sync, &ctx) ? ctx.result : NULL;
}

static void dv_finish_sync(void *arg) {
    wgt_add_t *ctx = (wgt_add_t *)arg;
    detail_view_t *d = (detail_view_t *)ctx->handle;
    plugin_ui_button_ctx_t *bctx = wgt_button_ctx_alloc();
    if (!bctx) return;
    bctx->cb = ctx->cb;
    bctx->user = ctx->user;
    ctx->result = detail_view_add_back(d, widget_btn_bridge, bctx);
    if (ctx->result) {
        lv_obj_add_event_cb((lv_obj_t *)ctx->result, widget_btn_bridge, LV_EVENT_DELETE, bctx);
    } else {
        wgt_button_ctx_free(bctx);
    }
}

ghostesp_ui_obj_t plugin_api_ui_detail_finish(ghostesp_detail_t dv, ghostesp_ui_button_cb_t on_back, void *user) {
    if (!plugin_api_internal_has_ui_permission()) return NULL;
    if (!dv) return NULL;
    wgt_add_t ctx = { .handle = dv, .cb = on_back, .user = user };
    return plugin_api_internal_run_sync(dv_finish_sync, &ctx) ? ctx.result : NULL;
}

static void dv_set_sel_sync(void *arg) {
    wgt_int_t *ctx = (wgt_int_t *)arg;
    detail_view_set_selected((detail_view_t *)ctx->handle, ctx->val);
}

void plugin_api_ui_detail_set_selected(ghostesp_detail_t dv, int index) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!dv) return;
    wgt_int_t ctx = { .handle = dv, .val = index };
    plugin_api_internal_run_sync(dv_set_sel_sync, &ctx);
}

static void dv_move_sel_sync(void *arg) {
    wgt_int_t *ctx = (wgt_int_t *)arg;
    detail_view_move_selection((detail_view_t *)ctx->handle, ctx->val);
}

void plugin_api_ui_detail_move_selection(ghostesp_detail_t dv, int delta) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!dv) return;
    wgt_int_t ctx = { .handle = dv, .val = delta };
    plugin_api_internal_run_sync(dv_move_sel_sync, &ctx);
}

static void dv_get_sel_sync(void *arg) {
    wgt_int_t *ctx = (wgt_int_t *)arg;
    ctx->result = detail_view_get_selected((const detail_view_t *)ctx->handle);
}

int plugin_api_ui_detail_get_selected(ghostesp_detail_t dv) {
    if (!plugin_api_internal_has_ui_permission()) return -1;
    if (!dv) return -1;
    wgt_int_t ctx = { .handle = dv };
    return plugin_api_internal_run_sync(dv_get_sel_sync, &ctx) ? ctx.result : -1;
}

static void dv_get_count_sync(void *arg) {
    wgt_int_t *ctx = (wgt_int_t *)arg;
    ctx->result = detail_view_get_count((const detail_view_t *)ctx->handle);
}

int plugin_api_ui_detail_get_count(ghostesp_detail_t dv) {
    if (!plugin_api_internal_has_ui_permission()) return 0;
    if (!dv) return 0;
    wgt_int_t ctx = { .handle = dv };
    return plugin_api_internal_run_sync(dv_get_count_sync, &ctx) ? ctx.result : 0;
}

static void dv_step_up_sync(void *arg) {
    wgt_int_t *ctx = (wgt_int_t *)arg;
    ctx->result = detail_view_step_up((detail_view_t *)ctx->handle) ? 1 : 0;
}

bool plugin_api_ui_detail_step_up(ghostesp_detail_t dv) {
    if (!plugin_api_internal_has_ui_permission()) return false;
    if (!dv) return false;
    wgt_int_t ctx = { .handle = dv };
    return plugin_api_internal_run_sync(dv_step_up_sync, &ctx) && ctx.result != 0;
}

static void dv_step_down_sync(void *arg) {
    wgt_int_t *ctx = (wgt_int_t *)arg;
    ctx->result = detail_view_step_down((detail_view_t *)ctx->handle) ? 1 : 0;
}

bool plugin_api_ui_detail_step_down(ghostesp_detail_t dv) {
    if (!plugin_api_internal_has_ui_permission()) return false;
    if (!dv) return false;
    wgt_int_t ctx = { .handle = dv };
    return plugin_api_internal_run_sync(dv_step_down_sync, &ctx) && ctx.result != 0;
}

static void dv_activate_selected_sync(void *arg) {
    detail_view_t *dv = (detail_view_t *)arg;
    lv_obj_t *obj = detail_view_get_selected_obj(dv);
    if (obj && lv_obj_is_valid(obj)) lv_event_send(obj, LV_EVENT_CLICKED, NULL);
}

void plugin_api_ui_detail_activate_selected(ghostesp_detail_t dv) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!dv) return;
    plugin_api_internal_run_sync(dv_activate_selected_sync, dv);
}

static void dv_clear_sync(void *arg) {
    detail_view_clear((detail_view_t *)arg);
}

void plugin_api_ui_detail_clear(ghostesp_detail_t dv) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!dv) return;
    plugin_api_internal_run_sync(dv_clear_sync, dv);
}

static void dv_destroy_sync(void *arg) {
    detail_view_destroy((detail_view_t *)arg);
}

void plugin_api_ui_detail_destroy(ghostesp_detail_t dv) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!dv) return;
    plugin_api_internal_run_sync(dv_destroy_sync, dv);
}

static void popup_create_sync(void *arg) {
    wgt_wh_t *ctx = (wgt_wh_t *)arg;
    ctx->handle = popup_create(plugin_api_internal_parent_or_current(NULL), (int)ctx->w, (int)ctx->h);
}

ghostesp_popup_t plugin_api_ui_popup_create(int32_t width, int32_t height) {
    if (!plugin_api_internal_has_ui_permission()) return NULL;
    wgt_wh_t ctx = { .w = width, .h = height };
    return plugin_api_internal_run_sync(popup_create_sync, &ctx) ? ctx.handle : NULL;
}

static void popup_set_title_sync(void *arg) {
    wgt_set_str_t *ctx = (wgt_set_str_t *)arg;
    popup_set_title((popup_t *)ctx->handle, ctx->str);
}

void plugin_api_ui_popup_set_title(ghostesp_popup_t p, const char *title) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!p) return;
    wgt_set_str_t ctx = { .handle = p, .str = title };
    plugin_api_internal_run_sync(popup_set_title_sync, &ctx);
}

static void popup_set_body_sync(void *arg) {
    wgt_set_str_t *ctx = (wgt_set_str_t *)arg;
    popup_set_body((popup_t *)ctx->handle, ctx->str);
}

void plugin_api_ui_popup_set_body(ghostesp_popup_t p, const char *body) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!p) return;
    wgt_set_str_t ctx = { .handle = p, .str = body };
    plugin_api_internal_run_sync(popup_set_body_sync, &ctx);
}

static void popup_add_btn_sync(void *arg) {
    wgt_add_t *ctx = (wgt_add_t *)arg;
    popup_t *pop = (popup_t *)ctx->handle;
    plugin_ui_button_ctx_t *bctx = wgt_button_ctx_alloc();
    if (!bctx) return;
    bctx->cb = ctx->cb;
    bctx->user = ctx->user;
    ctx->result = popup_add_button(pop, ctx->label, widget_btn_bridge, bctx);
    if (ctx->result) {
        lv_obj_add_event_cb((lv_obj_t *)ctx->result, widget_btn_bridge, LV_EVENT_DELETE, bctx);
    } else {
        wgt_button_ctx_free(bctx);
    }
}

ghostesp_ui_obj_t plugin_api_ui_popup_add_button(ghostesp_popup_t p, const char *label, ghostesp_ui_button_cb_t on_click, void *user) {
    if (!plugin_api_internal_has_ui_permission()) return NULL;
    if (!p) return NULL;
    wgt_add_t ctx = { .handle = p, .label = label, .cb = on_click, .user = user };
    return plugin_api_internal_run_sync(popup_add_btn_sync, &ctx) ? ctx.result : NULL;
}

static void popup_show_sync(void *arg) {
    popup_show((popup_t *)arg);
}

void plugin_api_ui_popup_show(ghostesp_popup_t p) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!p) return;
    plugin_api_internal_run_sync(popup_show_sync, p);
}

static void popup_hide_sync(void *arg) {
    popup_hide((popup_t *)arg);
}

void plugin_api_ui_popup_hide(ghostesp_popup_t p) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!p) return;
    plugin_api_internal_run_sync(popup_hide_sync, p);
}

static void popup_destroy_sync(void *arg) {
    popup_destroy((popup_t *)arg);
}

void plugin_api_ui_popup_destroy(ghostesp_popup_t p) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!p) return;
    plugin_api_internal_run_sync(popup_destroy_sync, p);
}

static void scan_create_sync(void *arg) {
    wgt_str_res_t *ctx = (wgt_str_res_t *)arg;
    ctx->result = scan_status_create(ctx->str1);
}

ghostesp_scan_t plugin_api_ui_scan_status_create(const char *message) {
    if (!plugin_api_internal_has_ui_permission()) return NULL;
    wgt_str_res_t ctx = { .str1 = message };
    return plugin_api_internal_run_sync(scan_create_sync, &ctx) ? ctx.result : NULL;
}

static void scan_update_sync(void *arg) {
    wgt_set_str_t *ctx = (wgt_set_str_t *)arg;
    scan_status_update((scan_status_t *)ctx->handle, ctx->str);
}

void plugin_api_ui_scan_status_update(ghostesp_scan_t ss, const char *message) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!ss) return;
    wgt_set_str_t ctx = { .handle = ss, .str = message };
    plugin_api_internal_run_sync(scan_update_sync, &ctx);
}

static void scan_set_prog_sync(void *arg) {
    wgt_prog_t *ctx = (wgt_prog_t *)arg;
    scan_status_set_progress((scan_status_t *)ctx->handle, ctx->current, ctx->total);
}

void plugin_api_ui_scan_status_set_progress(ghostesp_scan_t ss, int current, int total) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!ss) return;
    wgt_prog_t ctx = { .handle = ss, .current = current, .total = total };
    plugin_api_internal_run_sync(scan_set_prog_sync, &ctx);
}

static void scan_close_sync(void *arg) {
    scan_status_close((scan_status_t *)arg);
}

void plugin_api_ui_scan_status_close(ghostesp_scan_t ss) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!ss) return;
    plugin_api_internal_run_sync(scan_close_sync, ss);
}

static void pmenu_create_sync(void *arg) {
    pmenu_create_sync_t *ctx = (pmenu_create_sync_t *)arg;
    paged_menu_t *pm = paged_menu_create(ctx->page_size, pmenu_load_bridge, ctx->bridge);
    if (!pm) return;
    ctx->wrap = calloc(1, sizeof(*ctx->wrap));
    if (!ctx->wrap) {
        paged_menu_destroy(pm);
        return;
    }
    ctx->wrap->pm = pm;
    ctx->wrap->bridge = ctx->bridge;
}

ghostesp_paged_menu_t plugin_api_ui_paged_menu_create(int page_size, ghostesp_paged_menu_load_fn load_fn, void *user) {
    if (!plugin_api_internal_has_ui_permission()) return NULL;
    if (!load_fn) return NULL;
    pmenu_bridge_t *bridge = calloc(1, sizeof(*bridge));
    if (!bridge) return NULL;
    bridge->load_fn = load_fn;
    bridge->load_user = user;
    pmenu_create_sync_t ctx = { .page_size = page_size, .bridge = bridge };
    if (!plugin_api_internal_run_sync(pmenu_create_sync, &ctx) || !ctx.wrap) {
        free(bridge);
        return NULL;
    }
    return (ghostesp_paged_menu_t)ctx.wrap;
}

static void pmenu_set_cb_sync(void *arg) {
    pmenu_set_cb_sync_t *ctx = (pmenu_set_cb_sync_t *)arg;
    paged_menu_set_callbacks(
        ctx->wrap->pm,
        ctx->select_fn ? pmenu_select_bridge : NULL,
        ctx->prev_fn ? pmenu_prev_bridge : NULL,
        ctx->next_fn ? pmenu_next_bridge : NULL,
        ctx->wrap->bridge
    );
}

void plugin_api_ui_paged_menu_set_callbacks(ghostesp_paged_menu_t pm, ghostesp_paged_menu_select_fn select_fn, ghostesp_paged_menu_nav_fn prev_fn, ghostesp_paged_menu_nav_fn next_fn, void *user) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!pm) return;
    pmenu_wrap_t *wrap = (pmenu_wrap_t *)pm;
    if (!wrap->bridge) return;
    wrap->bridge->select_fn = select_fn;
    wrap->bridge->prev_fn = prev_fn;
    wrap->bridge->next_fn = next_fn;
    wrap->bridge->cb_user = user;
    pmenu_set_cb_sync_t ctx = {
        .wrap = wrap,
        .select_fn = select_fn,
        .prev_fn = prev_fn,
        .next_fn = next_fn,
        .cb_user = user
    };
    plugin_api_internal_run_sync(pmenu_set_cb_sync, &ctx);
}

static void pmenu_reset_sync(void *arg) {
    pmenu_wrap_t *wrap = (pmenu_wrap_t *)arg;
    paged_menu_reset(wrap->pm);
}

void plugin_api_ui_paged_menu_reset(ghostesp_paged_menu_t pm) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!pm) return;
    plugin_api_internal_run_sync(pmenu_reset_sync, pm);
}

static void pmenu_destroy_sync(void *arg) {
    pmenu_wrap_t *wrap = (pmenu_wrap_t *)arg;
    paged_menu_destroy(wrap->pm);
}

void plugin_api_ui_paged_menu_destroy(ghostesp_paged_menu_t pm) {
    if (!plugin_api_internal_has_ui_permission()) return;
    if (!pm) return;
    pmenu_wrap_t *wrap = (pmenu_wrap_t *)pm;
    plugin_api_internal_run_sync(pmenu_destroy_sync, wrap);
    free(wrap->bridge);
    free(wrap);
}

static void pmenu_has_prev_sync(void *arg) {
    wgt_bool_t *ctx = (wgt_bool_t *)arg;
    pmenu_wrap_t *wrap = (pmenu_wrap_t *)ctx->handle;
    ctx->result = paged_menu_has_prev(wrap->pm);
}

bool plugin_api_ui_paged_menu_has_prev(ghostesp_paged_menu_t pm) {
    if (!plugin_api_internal_has_ui_permission()) return false;
    if (!pm) return false;
    wgt_bool_t ctx = { .handle = pm };
    return plugin_api_internal_run_sync(pmenu_has_prev_sync, &ctx) ? ctx.result : false;
}

static void pmenu_has_next_sync(void *arg) {
    wgt_bool_t *ctx = (wgt_bool_t *)arg;
    pmenu_wrap_t *wrap = (pmenu_wrap_t *)ctx->handle;
    ctx->result = paged_menu_has_next(wrap->pm);
}

bool plugin_api_ui_paged_menu_has_next(ghostesp_paged_menu_t pm) {
    if (!plugin_api_internal_has_ui_permission()) return false;
    if (!pm) return false;
    wgt_bool_t ctx = { .handle = pm };
    return plugin_api_internal_run_sync(pmenu_has_next_sync, &ctx) ? ctx.result : false;
}
