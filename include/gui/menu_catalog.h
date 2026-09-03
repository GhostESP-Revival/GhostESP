#pragma once

#include "gui/menu_config.h"
#include "managers/display_manager.h"
#include "managers/views/options_screen.h"
#include "managers/plugin_manager.h"

typedef struct {
    char id[MENU_CONFIG_ID_LEN];
    char name[PLUGIN_APP_NAME_MAX];
    const char *asset_key;
    const lv_img_dsc_t *icon;
    lv_color_t border_color;
    View *view;
    EOptionsMenuType options_type;
    uint8_t default_placement;
} menu_catalog_item_t;

int menu_catalog_count(void);
bool menu_catalog_get(int index, menu_catalog_item_t *item);
bool menu_catalog_available(const menu_catalog_item_t *item, bool connected);
bool menu_catalog_is_grouped_plugin(const menu_catalog_item_t *item);
/* Returns a heap-owned snapshot, in saved order. menu=0 includes all items for editing. */
menu_catalog_item_t *menu_catalog_collect(uint8_t menu, bool available_only, int *count);
void menu_catalog_launch(const char *id, View *return_view);
