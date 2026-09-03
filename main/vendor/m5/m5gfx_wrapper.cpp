// m5gfx_wrapper.cpp
#include <M5GFX.h>
#include <lgfx/v1/panel/Panel_ST7789.hpp>
#ifdef CONFIG_IS_ATOMS3R
#include <lgfx/v1/panel/Panel_GC9A01.hpp>
#include <lgfx/v1/platforms/esp32/Bus_SPI.hpp>
#endif
#include "vendor/m5/m5gfx_wrapper.h"

M5GFX display;

static lgfx::Panel_ST7789 *g_panel = NULL;
#ifdef CONFIG_IS_ATOMS3R
static lgfx::Bus_SPI g_atoms3r_bus;
static lgfx::Panel_GC9107 *g_atoms3r_panel = NULL;
#endif

extern "C" void init_m5gfx_display() {
#ifdef CONFIG_IS_ATOMS3R
    auto bus_cfg = g_atoms3r_bus.config();
    bus_cfg.spi_host = SPI3_HOST;
    bus_cfg.spi_mode = 0;
    bus_cfg.spi_3wire = true;
    bus_cfg.freq_write = 40000000;
    bus_cfg.freq_read = 16000000;
    bus_cfg.pin_sclk = 15;
    bus_cfg.pin_mosi = 21;
    bus_cfg.pin_miso = -1;
    bus_cfg.pin_dc = 42;
    g_atoms3r_bus.config(bus_cfg);
    g_atoms3r_panel = new lgfx::Panel_GC9107();
    g_atoms3r_panel->bus(&g_atoms3r_bus);
    auto panel_cfg = g_atoms3r_panel->config();
    panel_cfg.pin_cs = 14;
    panel_cfg.pin_rst = 48;
    panel_cfg.panel_width = 128;
    panel_cfg.panel_height = 128;
    // This panel's visible 128x128 window sits inside a larger (ST7735-class,
    // ~132 wide) GRAM, offset from the origin -- so writing [0..127] leaves the
    // panel scanning out unwritten columns/rows as noise at the bottom/right.
    // The offsets are asymmetric (classic for these controllers): +1 in y clears
    // the top/bottom lines, +2 in x clears the right line. At rotation 0,
    // setWindow adds the offset after setAddrWindow has clamped to panel_width,
    // so the shifted columns/rows are addressed fine (the GRAM extends there).
    panel_cfg.offset_x = 2;
    panel_cfg.offset_y = 1;
    panel_cfg.readable = false;
    panel_cfg.bus_shared = false;
    panel_cfg.invert = true;
    g_atoms3r_panel->config(panel_cfg);
    // Use the explicit-panel overload; plain init() runs board autodetection
    // and can reconfigure the SPI bus with unrelated default pins.
    display.init(g_atoms3r_panel);
    display.setColorDepth(16);
    // LVGL is built with LV_COLOR_16_SWAP=y, so it already emits byte-swapped
    // RGB565. Do NOT swap again here or the double swap scrambles every pixel's
    // byte order (green status bar / pink background). This matches the working
    // cardputer path, which also runs LV_COLOR_16_SWAP=y with no panel swap.
    display.setSwapBytes(false);
    display.setRotation(0);
    display.fillScreen(TFT_BLACK);
    // The AtomS3R backlight is controlled by its LP5562 over I2C.
    lgfx::i2c::init(0, GPIO_NUM_45, GPIO_NUM_0);
    lgfx::i2c::writeRegister8(0, 48, 0x00, 0b01000000, 0, 400000);
    lgfx::delay(1);
    lgfx::i2c::writeRegister8(0, 48, 0x08, 0b00000001, 0, 400000);
    lgfx::i2c::writeRegister8(0, 48, 0x70, 0, 0, 400000);
    lgfx::i2c::writeRegister8(0, 48, 0x0e, 0, 0, 400000);
    return;
#else
    if (g_panel) {
        delete g_panel;
        g_panel = NULL;
    }
    g_panel = new lgfx::Panel_ST7789();
    g_panel->setRotation(1);
    display.setPanel(g_panel);
    display.init();
    display.fillScreen(TFT_BLACK);
#endif
}

extern "C" void m5gfx_set_brightness(uint8_t brightness) {
#ifdef CONFIG_IS_ATOMS3R
    lgfx::i2c::writeRegister8(0, 48, 0x0e, brightness, 0, 400000);
#else
    (void)brightness;
#endif
}

extern "C" void m5gfx_write_pixels(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint16_t *color_p) {
    display.startWrite();
    display.setAddrWindow(x1, y1, (x2 - x1 + 1), (y2 - y1 + 1));
    display.pushPixels(color_p, (x2 - x1 + 1) * (y2 - y1 + 1));
    display.endWrite();
}

extern "C" int get_m5gfx_width() { return display.width(); }
extern "C" int get_m5gfx_height() { return display.height(); }
