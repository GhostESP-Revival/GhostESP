// m5gfx_wrapper.cpp
#include <M5GFX.h>
#include <lgfx/v1/panel/Panel_ST7789.hpp>
#include "vendor/m5/m5gfx_wrapper.h"

M5GFX display;

static lgfx::Panel_ST7789 *g_panel = NULL;

extern "C" void init_m5gfx_display() {
    if (g_panel) {
        delete g_panel;
        g_panel = NULL;
    }
    g_panel = new lgfx::Panel_ST7789();
    g_panel->setRotation(1);
    display.setPanel(g_panel);
    display.init();
    display.fillScreen(TFT_BLACK);
}

extern "C" void m5gfx_write_pixels(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint16_t *color_p) {
    display.startWrite();
    display.setAddrWindow(x1, y1, (x2 - x1 + 1), (y2 - y1 + 1));
    display.pushPixels(color_p, (x2 - x1 + 1) * (y2 - y1 + 1));
    display.endWrite();
}

extern "C" int get_m5gfx_width() { return display.width(); }
extern "C" int get_m5gfx_height() { return display.height(); }