#ifndef STATUS_DISPLAY_ANIMATIONS_H
#define STATUS_DISPLAY_ANIMATIONS_H

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "managers/settings_manager.h"

typedef struct StatusAnimGfx {
    void (*clear)(void *user);
    void (*flush)(void *user);
    void (*plot_pixel)(void *user, int x, int y, bool on);
    void (*draw_text)(void *user, int x, int y, const char *text);
    void (*draw_char_rot90_right)(void *user, int x, int y, char c);
    int width;
    int height;
    int scale_y;
    int font_char_width;
    int font_char_height;
    void *user;
} StatusAnimGfx;

void status_display_animations_step(IdleAnimation anim, TickType_t now, int frame, const StatusAnimGfx *gfx);
void status_display_animations_reset(void);

#endif // STATUS_DISPLAY_ANIMATIONS_H
