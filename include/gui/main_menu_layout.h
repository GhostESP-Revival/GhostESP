#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MAIN_MENU_LAYOUT_CAROUSEL = 0,
    MAIN_MENU_LAYOUT_LAUNCHER = 1,
    MAIN_MENU_LAYOUT_LIST = 2,
} main_menu_layout_kind_t;

// Source compatibility for callers that still refer to the former Grid layout.
#define MAIN_MENU_LAYOUT_CARD_GRID MAIN_MENU_LAYOUT_LAUNCHER

typedef enum {
    MAIN_MENU_DENSITY_COMPACT = 0,
    MAIN_MENU_DENSITY_REGULAR,
    MAIN_MENU_DENSITY_COMFORTABLE,
} main_menu_density_t;

typedef struct {
    int screen_width;
    int screen_height;
    int status_bar_height;
    int content_height;
    main_menu_density_t density;

    int margin;
    int columns;
    int rows;
    int visible_rows;
    int page_capacity;
    int page_count;
    int page_indicator_height;
    int card_width;
    int card_height;

    int carousel_button_size;
    int carousel_icon_target;
    int carousel_icon_y_offset;
    bool carousel_show_label;
    bool carousel_show_previews;
    int carousel_preview_size;
    int carousel_preview_icon_target;
    int carousel_preview_offset;
    int carousel_transition_distance;

    int list_button_height;
    int list_icon_target;
    int list_button_pad;
    int list_pad;
    int list_row_gap;
    int list_column_gap;

    int nav_button_size;
    int nav_button_margin;

    lv_align_t container_align;
    int container_x;
    int container_y;
    int container_width;
    int container_height;
} main_menu_layout_metrics_t;

main_menu_layout_kind_t main_menu_layout_from_setting(uint8_t setting);
main_menu_layout_kind_t main_menu_layout_resolve_for_size(main_menu_layout_kind_t kind,
                                                          int screen_width,
                                                          int screen_height);
void main_menu_layout_get_metrics_for_size(main_menu_layout_kind_t kind, int item_count,
                                           int screen_width, int screen_height,
                                           int status_bar_height,
                                           main_menu_layout_metrics_t *metrics);
void main_menu_layout_get_metrics(main_menu_layout_kind_t kind, int item_count, main_menu_layout_metrics_t *metrics);

#ifdef __cplusplus
}
#endif
