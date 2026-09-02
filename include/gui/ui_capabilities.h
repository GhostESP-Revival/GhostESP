#ifndef GUI_UI_CAPABILITIES_H
#define GUI_UI_CAPABILITIES_H

/* Presentation capabilities are deliberately separate from SoC checks.
 * CrowPanel P4 and the 800x480 CrowPanel S3 boards can share large-screen LVGL
 * geometry, typography and touch affordances. P4 hardware acceleration,
 * native canvas, C6-hosted radio and 128px asset paths remain P4-only. */
#if defined(CONFIG_CROWPANEL_ADVANCED_P4) || defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
#define GUI_LARGE_SCREEN 1
#define GUI_LARGE_TOUCH_UI 1
#else
#define GUI_LARGE_SCREEN 0
#define GUI_LARGE_TOUCH_UI 0
#endif

/* The S3 RGB panel is bandwidth-bound. Avoid full-screen transition
 * animations for system overlays while retaining the richer static UI. */
#if defined(CONFIG_CROWPANEL_ADVANCED_P4)
#define GUI_SYSTEM_OVERLAY_ANIMATIONS 1
#define GUI_IMAGE_ZOOM_MAX 1024
#elif defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
#define GUI_SYSTEM_OVERLAY_ANIMATIONS 0
#define GUI_IMAGE_ZOOM_MAX 512
#else
#define GUI_SYSTEM_OVERLAY_ANIMATIONS 0
#define GUI_IMAGE_ZOOM_MAX 256
#endif

#endif
