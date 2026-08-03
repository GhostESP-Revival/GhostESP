#ifndef GUI_NAV_HISTORY_H
#define GUI_NAV_HISTORY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t scope;
    int32_t value;
    int32_t selection;
    int32_t scroll_y;
    int32_t wifi_state;
    int32_t bluetooth_state;
    int32_t dualcomm_state;
    int32_t settings_root;
    int32_t settings_category;
} gui_nav_state_t;

void gui_nav_history_clear(void);
bool gui_nav_history_push(const gui_nav_state_t *state);
bool gui_nav_history_pop(gui_nav_state_t *out_state);
bool gui_nav_history_peek(gui_nav_state_t *out_state);

#endif
