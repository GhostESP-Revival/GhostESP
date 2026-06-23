#include "gui/main_menu_layout.h"
#include "gui/design_tokens.h"

static int clamp_int(int value, int min, int max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

main_menu_layout_kind_t main_menu_layout_from_setting(uint8_t setting) {
    switch (setting) {
        case 1:
            return MAIN_MENU_LAYOUT_CARD_GRID;
        case 2:
            return MAIN_MENU_LAYOUT_LIST;
        default:
            return MAIN_MENU_LAYOUT_CAROUSEL;
    }
}

void main_menu_layout_get_metrics(main_menu_layout_kind_t kind, int item_count, main_menu_layout_metrics_t *metrics) {
    if (!metrics) return;

    int screen_width = LV_HOR_RES;
    int screen_height = LV_VER_RES;
    int status_bar_height = GUI_STATUS_BAR_H;
    int content_height = screen_height - status_bar_height;
    if (content_height < 60) content_height = screen_height;

    *metrics = (main_menu_layout_metrics_t){
        .screen_width = screen_width,
        .screen_height = screen_height,
        .status_bar_height = status_bar_height,
        .content_height = content_height,
        .container_align = LV_ALIGN_CENTER,
        .container_x = 0,
        .container_y = status_bar_height / 2,
        .container_width = screen_width,
        .container_height = content_height,
    };

    int min_dim = LV_MIN(screen_width, screen_height);
    int carousel_size = (int)(min_dim * 0.55f);
    if (min_dim <= 128) {
        carousel_size = (int)(min_dim * 0.62f);
    } else if (min_dim >= 320) {
        carousel_size = (int)(min_dim * 0.42f);
    }
    metrics->carousel_button_size = clamp_int(carousel_size, 64, 160);
    metrics->carousel_icon_target = clamp_int((int)(metrics->carousel_button_size * 0.38f), 20, 56);
    metrics->carousel_show_label = screen_width > 150;

    metrics->nav_button_size = 52;
    metrics->nav_button_margin = 15;
    if (screen_width <= 128) {
        metrics->nav_button_size = 40;
        metrics->nav_button_margin = 10;
    } else if (screen_width >= 320) {
        metrics->nav_button_size = 60;
        metrics->nav_button_margin = 20;
    }

    metrics->list_button_height = (screen_height <= 160 || screen_width <= 160) ? 32 : 44;
    metrics->list_icon_target = metrics->list_button_height <= 38 ? 20 : 26;
    metrics->list_pad = screen_width > 200 ? 16 : 10;
    metrics->list_row_gap = 6;
    metrics->list_column_gap = 12;

    bool portrait = screen_height > screen_width;
    int columns = (screen_width >= 320) ? 4 : (screen_width >= 240) ? 3 : 2;
    if (item_count > 0 && columns > item_count) columns = item_count;
    if (columns <= 0) columns = 1;

    metrics->columns = columns;
    metrics->rows = item_count > 0 ? (item_count + columns - 1) / columns : 1;
    if (metrics->rows <= 0) metrics->rows = 1;
    metrics->visible_rows = portrait && screen_width >= 200 ? 4 : 2;
    metrics->margin = (screen_width <= 240 || content_height <= 120) ? 4 : GUI_GRID;
    if (portrait && screen_width >= 200) {
        metrics->margin = 6;
    }
    metrics->card_width = (screen_width - (columns + 1) * metrics->margin) / columns;
    metrics->card_height = (content_height - (metrics->visible_rows - 1) * metrics->margin) / metrics->visible_rows;
    if (metrics->card_width < 1) metrics->card_width = 1;
    if (metrics->card_height < 1) metrics->card_height = 1;

    if (kind == MAIN_MENU_LAYOUT_CARD_GRID) {
        metrics->container_align = LV_ALIGN_TOP_MID;
        metrics->container_y = status_bar_height;
    }
}
