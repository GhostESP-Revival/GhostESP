#ifndef WARDRIVING_SCREEN_H
#define WARDRIVING_SCREEN_H

#include "managers/display_manager.h"

extern View wardriving_view;

void wardriving_view_create(void);
void wardriving_view_destroy(void);
void wardriving_view_set_scan_mode(bool enabled);
void wardriving_view_set_ble_mode(bool enabled);

#endif
