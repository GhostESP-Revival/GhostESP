#ifndef GB_PLATFORM_H
#define GB_PLATFORM_H

#include "../../../sdk/ghostesp_plugin_api.h"
#include "gb_layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Store the host API and query the screen once. Returns false when required
 * host functions are missing (app shows a toast and idles). */
bool gb_platform_init(const ghostesp_api_t *api);
const ghostesp_api_t *gb_platform_api(void);

void *gb_platform_psram_malloc(size_t size);
void gb_platform_psram_free(void *ptr);

int gb_platform_content_width(void);
int gb_platform_content_height(void);
bool gb_platform_has_touch(void);
/* Width of the firmware's reserved left-edge gesture strip (0 when absent). */
int gb_platform_touch_reserved_left(void);

/* Layout (recomputed when the scale mode changes). */
void gb_platform_set_scale_mode(gb_scale_mode_t mode);
gb_scale_mode_t gb_platform_scale_mode(void);
const gb_layout_t *gb_platform_layout(void);

/* Frame buffers: two 160x144 RGB565 buffers in PSRAM (the second enables the
 * async zero-copy present path where the host supports it). */
bool gb_platform_alloc_frames(void);
void gb_platform_free_frames(void);
uint16_t *gb_platform_render_frame(void);

/* UI construction. Safe from the LVGL task or the tick task (calls marshal). */
void gb_platform_build_game_screen(const char *title);
void gb_platform_hide_loading(void);
void gb_platform_set_status_text(const char *text);
/* Repaint canvas background + static controls after a layout change. */
void gb_platform_repaint_canvas(void);
/* Show/hide the painted virtual controls and their A/B/START/SELECT labels
 * (the pause menu hides them so they cannot draw over the menu panel). */
void gb_platform_set_hud_visible(bool visible);
/* The app's game screen object (parent for app-created overlays). */
ghostesp_ui_obj_t gb_platform_screen(void);
/* The game canvas. Unlike the screen (a column flex container), a canvas has
 * no layout and so honours explicit child positions. */
ghostesp_ui_obj_t gb_platform_canvas(void);
/* Pause button as an LVGL widget above the canvas: a control drawn into the
 * canvas image would be covered by the game blit whenever the game rectangle
 * overlaps it. */
void gb_platform_set_menu_button(ghostesp_ui_button_cb_t cb, void *user);

/* Present the rendered frame at the layout's game rectangle. Returns true when
 * the frame reached the display queue (false when it was dropped because the
 * previous blit is still in flight). */
bool gb_platform_present(void);

/* Stop any outstanding async blit and release frame buffers. */
void gb_platform_shutdown(void);

/* Convert a firmware touch y coordinate (display space) to content space. */
int32_t gb_platform_touch_local_y(int32_t y);

#ifdef __cplusplus
}
#endif

#endif /* GB_PLATFORM_H */
