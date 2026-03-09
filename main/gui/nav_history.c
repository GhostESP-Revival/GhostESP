#include "gui/nav_history.h"

#define GUI_NAV_HISTORY_DEPTH 16u

static gui_nav_state_t s_nav_stack[GUI_NAV_HISTORY_DEPTH];
static uint8_t s_nav_top = 0;

void gui_nav_history_clear(void) {
    s_nav_top = 0;
}

bool gui_nav_history_push(const gui_nav_state_t *state) {
    if (!state) {
        return false;
    }

    if (s_nav_top >= GUI_NAV_HISTORY_DEPTH) {
        for (uint8_t i = 1; i < GUI_NAV_HISTORY_DEPTH; i++) {
            s_nav_stack[i - 1] = s_nav_stack[i];
        }
        s_nav_top = GUI_NAV_HISTORY_DEPTH - 1;
    }

    s_nav_stack[s_nav_top++] = *state;
    return true;
}

bool gui_nav_history_pop(gui_nav_state_t *out_state) {
    if (s_nav_top <= 0) {
        return false;
    }

    s_nav_top--;
    if (out_state) {
        *out_state = s_nav_stack[s_nav_top];
    }
    return true;
}

bool gui_nav_history_peek(gui_nav_state_t *out_state) {
    if (s_nav_top <= 0 || !out_state) {
        return false;
    }

    *out_state = s_nav_stack[s_nav_top - 1];
    return true;
}
