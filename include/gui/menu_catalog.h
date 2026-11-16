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
    /* Folder this app lives under. Built-ins use a key from the firmware
     * folder table; native SD apps use the "category" from their manifest.
     * Empty means the app stays at the gallery root. */
    char category[PLUGIN_APP_CATEGORY_MAX];
} menu_catalog_item_t;

int menu_catalog_count(void);
bool menu_catalog_get(int index, menu_catalog_item_t *item);
bool menu_catalog_available(const menu_catalog_item_t *item, bool connected);

/* Folders are defined by the firmware table, never discovered from the card,
 * which is what keeps their order and their labels fixed. Only these keys are
 * recognised; a native app declaring anything else stays at the root. A
 * folder with no members is not shown at all. */
#define MENU_CATALOG_MAX_FOLDERS 12
int menu_catalog_folder_count(void);
const char *menu_catalog_folder_key(int index);
const char *menu_catalog_folder_label(int index);
int menu_catalog_folder_index(const char *category_key);

/* True when the gallery files this app inside a folder instead of showing it
 * at the root. An explicit menu_config entry pins the app back to the root. */
bool menu_catalog_is_grouped(const menu_catalog_item_t *item);

/* Returns a heap-owned snapshot, in saved order. menu=0 includes all items for editing. */
menu_catalog_item_t *menu_catalog_collect(uint8_t menu, bool available_only, int *count);
void menu_catalog_launch(const char *id, View *return_view);
