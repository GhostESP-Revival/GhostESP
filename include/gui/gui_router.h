#ifndef GUI_ROUTER_H
#define GUI_ROUTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "managers/display_manager.h"

typedef enum {
    GUI_ROUTE_VIEW,
    GUI_ROUTE_OPTIONS,
    GUI_ROUTE_TERMINAL,
    GUI_ROUTE_KEYBOARD,
} gui_route_id_t;

typedef struct {
    gui_route_id_t id;
    View *view;
    int32_t selected;
    int32_t scroll_y;
    int32_t item_index;
    int32_t parent_id;
    int32_t state[5];
} gui_route_t;

void gui_router_navigate(const gui_route_t *route);
void gui_router_replace(const gui_route_t *route);
void gui_router_back(void);
void gui_router_reset(const gui_route_t *route);

/* These immediate variants are for display-manager callbacks already running
 * on the LVGL task. Normal callers should use the operations above. */
void gui_router_navigate_immediate(const gui_route_t *route);
void gui_router_replace_immediate(const gui_route_t *route);
void gui_router_back_immediate(void);
void gui_router_reset_immediate(const gui_route_t *route);

const gui_route_t *gui_router_current(void);
View *gui_router_previous_view(void);
size_t gui_router_depth(void);

/* Temporary adapter for callers that still use direct view switching. New
 * navigation code must use one of the four explicit operations above. */
void gui_router_legacy_switch_immediate(View *view);

#endif
