#ifndef APPS_MENU_SCREEN_H
#define APPS_MENU_SCREEN_H

#include "lvgl.h"
#include "managers/display_manager.h"

void apps_menu_create(void);
void apps_menu_destroy(void);
void get_apps_menu_callback(void **callback);
void update_app_item_styles(void);

extern View apps_menu_view;

#endif /* APPS_MENU_SCREEN_H */
