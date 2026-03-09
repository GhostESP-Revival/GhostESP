#pragma once

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct detail_view_t detail_view_t;

typedef enum {
    DETAIL_ROW_INFO,
    DETAIL_ROW_ACTION,
    DETAIL_ROW_HEADER,
    DETAIL_ROW_DIVIDER
} detail_row_type_t;

detail_view_t *detail_view_create(lv_obj_t *parent, const char *title);

void detail_view_destroy(detail_view_t *dv);

void detail_view_add_info(detail_view_t *dv, const char *label, const char *value);

void detail_view_add_infof(detail_view_t *dv, const char *label, const char *fmt, ...);

void detail_view_add_action(detail_view_t *dv, const char *label, lv_event_cb_t on_click, void *user_data);

void detail_view_add_header(detail_view_t *dv, const char *text);

void detail_view_add_divider(detail_view_t *dv);

lv_obj_t *detail_view_add_back(detail_view_t *dv, lv_event_cb_t on_click, void *user_data);

void detail_view_set_selected(detail_view_t *dv, int index);

void detail_view_move_selection(detail_view_t *dv, int delta);

bool detail_view_step_up(detail_view_t *dv);

bool detail_view_step_down(detail_view_t *dv);

int detail_view_get_selected(const detail_view_t *dv);

int detail_view_get_count(const detail_view_t *dv);

detail_row_type_t detail_view_get_row_type(const detail_view_t *dv, int index);

void detail_view_clear(detail_view_t *dv);

lv_obj_t *detail_view_get_list(detail_view_t *dv);

lv_obj_t *detail_view_get_info_panel(detail_view_t *dv);

lv_obj_t *detail_view_get_selected_obj(detail_view_t *dv);

void detail_view_refresh_styles(detail_view_t *dv);

#ifdef __cplusplus
}
#endif
