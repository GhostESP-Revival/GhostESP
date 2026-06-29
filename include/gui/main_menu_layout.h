#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MAIN_MENU_LAYOUT_CAROUSEL = 0,
    MAIN_MENU_LAYOUT_CARD_GRID = 1,
    MAIN_MENU_LAYOUT_LIST = 2,
} main_menu_layout_kind_t;

typedef struct {
    int screen_width;
    int screen_height;
    int status_bar_height;
    int content_height;

    int margin;
    int columns;
    int rows;
    int visible_rows;
    int card_width;
    int card_height;

    int carousel_button_size;
    int carousel_icon_target;
    bool carousel_show_label;

    int list_button_height;
    int list_icon_target;
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
void main_menu_layout_get_metrics(main_menu_layout_kind_t kind, int item_count, main_menu_layout_metrics_t *metrics);

#ifdef __cplusplus
}
#endif
