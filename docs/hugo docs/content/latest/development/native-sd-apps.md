---
title: "Native SD Apps"
description: "Build and run native plug-in apps from the SD card with the GhostESP SDK."
weight: 30
toc: true
---

Native SD apps are ESP-IDF shared objects (`.so`) loaded from the SD card at runtime with Espressif's ELF loader. Apps get a stable API surface with access to the UI system, storage, WiFi, BLE, GPS, NFC, IR, SubGHz, BadUSB, RGB LEDs, and more.

> **PSRAM required**: Native SD apps require a board with PSRAM. Boards without PSRAM (e.g. stock ESP32-S3 without octal PSRAM) cannot load or run native SD apps. Native SD apps are hidden from the Apps menu on no-PSRAM boards; a warning toast is shown on entry. The `requires_psram` per-app manifest field is checked separately for apps that need extra PSRAM beyond the baseline — these apps are also hidden from the gallery on no-PSRAM boards.

Enable the system via `CONFIG_ENABLE_NATIVE_SD_APPS=y` in `menuconfig` (on by default for supported targets).

## Quick Start

Scaffold, build, and deploy an app in three steps:

```powershell
python plugins/tools/new_app.py my_tool --name "My Tool"
python plugins/tools/build_app.py plugins/examples/my_tool --target esp32s3
python plugins/tools/package_app.py plugins/examples/my_tool --gapp
```

Copy the resulting `.gapp` file to `/mnt/ghostesp/apps/` or `/mnt/ghostesp/packages/` on your SD card. The app gallery will pick it up on the next reload.

A full build tool (`gbt`) is also available — see [GBT Reference]({{< relref "gbt" >}}).

## SD Card Layout

```
/mnt/ghostesp/
  apps/<app_id>/           Extracted app folders
    manifest.json
    <entry>.so
  packages/                .gapp archive discovery
  app_cache/               Auto-extracted .gapp content
  appdata/<app_id>/        Per-app storage + .state.json
```

## Manifest Format

Every app needs a `manifest.json` at its root. Required fields: `id`, `name`, `entry`, `api_version`. `target` is strongly recommended.

```json
{
  "id": "device_inspector",
  "name": "Device Inspector",
  "version": "2.0.0",
  "author": "GhostESP",
  "description": "Comprehensive hardware and API test suite with responsive native UI. Tests WiFi, BLE, GPS, RGB, storage, canvas drawing, input events, and theme inspection.",
  "category": "System",
  "entry": "ghostesp_device_inspector.so",
  "target": "esp32c5",
  "api_version": 1,
  "manifest_version": 1,
  "package_version": 1,
  "data_version": 1,
  "storage_scope": "ghostesp",
  "icon": "icon.rgb565",
  "icon_source": "device-tablet-svgrepo-com (2).png",
  "icon_format": "rgb565a8",
  "icon_width": 50,
  "icon_height": 50,
  "accent_color": "#56B6F7",
  "permissions": ["ui", "storage", "commands", "wifi", "ble", "rgb", "tasks", "lvgl", "power", "display", "input", "network", "wifi_control", "ethernet", "raw_gpio", "i2c", "spi", "uart", "adc", "pwm", "time", "random", "system", "settings", "nfc", "ir", "subghz", "nrf24", "badusb", "camera", "usb", "audio", "zigbee"],
  "memory_limit": 65536,
  "stack_size": 8192,
  "requires_psram": false
}
```

### Field Reference

| Field | Required | Description |
|-------|----------|-------------|
| `id` | Yes | Unique app identifier (`[A-Za-z0-9_-]+`). Used for `apps run <id>` and storage paths. |
| `name` | Yes | Human-readable display name. |
| `version` | Yes | Semver string (shown in gallery). |
| `entry` | Yes | `.so` filename relative to the app folder. |
| `target` | Strongly recommended | Native SD app target (`esp32`, `esp32s2`, `esp32s3`, `esp32c5`, `esp32c6`, `esp32c61`, `esp32p4`). Required when `NATIVE_SD_APPS_REQUIRE_TARGET_MATCH` is enabled. `esp32c3` firmware builds exist, but native SD `.gapp` apps are not currently supported for C3. |
| `api_version` | Yes | Must be `1`. |
| `author` | No | Attribution string. |
| `description` | No | Short description. |
| `category` | No | Category for future gallery grouping. |
| `manifest_version` | No | Must be `1` if set. |
| `package_version` | No | Integer, minimum `1`. |
| `data_version` | No | Integer, minimum `1`. Bump to trigger data migration. |
| `storage_scope` | No | `"app"` (default — scoped to `/mnt/ghostesp/appdata/<id>/`) or `"ghostesp"` (absolute paths under `/mnt/ghostesp/`). |
| `permissions` | No | Array of permission strings (see below). Unknown permission names make the manifest invalid. |
| `memory_limit` | No | Advisory limit in bytes for `app_malloc`/`app_calloc` tracked allocations. |
| `stack_size` | No | Advisory stack size hint in bytes. |
| `requires_psram` | No | If `true`, loader additionally checks that PSRAM is available (all native SD apps already require PSRAM at a baseline). Apps with this flag are hidden from the gallery on no-PSRAM boards. |
| `icon` | No | Path relative to app folder (raw RGB565 binary). |
| `icon_width` | No | Icon pixel width. |
| `icon_height` | No | Icon pixel height. |
| `icon_format` | No | `"rgb565"` or `"rgb565a8"`. |
| `icon_source` | No | Path to a PNG file (auto-converted to `icon` format at package time by GBT). |
| `accent_color` | No | `#RRGGBB` hex accent for gallery cards. |
| `assets` | No | Array of relative file/folder paths included in the package. |

### Permissions

Apps request permissions in `manifest.json`. The host API gates every subsystem behind the corresponding permission bit:

| Permission | Unlocks |
|------------|---------|
| `ui` | Screen creation, widgets, popups, detail views, options menus, scan status, canvas, animations |
| `storage` | `app_storage_*` functions, plus `storage_*` absolute functions only when `storage_scope` is `"ghostesp"` |
| `commands` | CLI command execution via `command_exec` |
| `tasks` | `delay_ms`, any future task creation |
| `wifi` | `wifi_start_scan`, `wifi_stop_scan`, AP enumeration |
| `ble` | `ble_start_scan`, `ble_stop_scan`, device enumeration, BLE detection |
| `nfc` | `nfc_is_available`, `nfc_read_start`, `nfc_stop` |
| `ir` | `ir_send_file`, `ir_stop` |
| `subghz` | `subghz_is_available`, `subghz_load_snapshot`, `subghz_transmit_loaded`, `subghz_stop` |
| `badusb` | `badusb_run_script`, `badusb_stop` |
| `raw_gpio` | Direct GPIO access |
| `lvgl` | Raw `lv_scr_act()` and `display_get_current_view()` |
| `rgb` | `rgb_set_all` and any LED APIs |
| `nrf24` | `nrf24_start`, `nrf24_stop`, pause/state queries |

## Entry Point

Every app exports one function:

```c
const ghostesp_app_t *ghostesp_app_init(const ghostesp_api_t *api);
```

Use the convenience macro to define the app descriptor:

```c
#include "ghostesp_plugin_api.h"

static void my_start(void) { /* setup */ }
static void my_stop(void)  { /* cleanup */ }

GHOSTESP_APP_DEFINE("my_app", "My App", my_start, my_stop, NULL, NULL)
```

The host calls `ghostesp_app_init(api)`, validates the returned descriptor, then calls `on_start()`.

Always set:
- `api_version = GHOSTESP_APP_API_VERSION` (currently `1`)
- `struct_size = GHOSTESP_APP_STRUCT_SIZE_V1`
- `flags` to `0` unless the host has advertised a capability you need

The host advertises its capabilities through the `flags` field of `ghostesp_api_t`. Recognized bits:

| Macro | Meaning |
|-------|---------|
| `GHOSTESP_APP_FLAG_PERMISSIONS_ENFORCED` | Host enforces the `permissions` array in the manifest. Calls to gated APIs without the matching permission return false / -1. |
| `GHOSTESP_APP_FLAG_ABSOLUTE_STORAGE_ALLOWED` | Host allows the `storage_*` (non-`app_storage_*`) calls to operate on paths outside `/mnt/ghostesp/appdata/<id>/`. Required for any app that writes to `/mnt/ghostesp/...` directly. |

The `GHOSTESP_API_STRUCT_SIZE_V1` macro is the host-side equivalent of the app-side `GHOSTESP_APP_STRUCT_SIZE_V1`; both expand to `sizeof(ghostesp_api_t)` / `sizeof(ghostesp_app_t)`. Apps rarely need it — only when comparing against a `struct_size` field the host itself publishes.

Future v1-compatible additions to the API struct are append-only; the host uses `struct_size` to detect which fields are valid.

## App Lifecycle

```
Load → on_start() → [on_tick() / on_input()] → on_pause() / on_resume() → on_stop() → Unload
```

| Callback | When |
|----------|------|
| `on_start()` | After successful load and validation. Set up UI here. |
| `on_tick(uint32_t elapsed_ms)` | Periodic tick (interval set by host UI). Update animations, polls. |
| `on_input(const ghostesp_input_event_t *event)` | User input (keys, encoder, touch). |
| `on_pause()` | App loses foreground (e.g. settings overlay). |
| `on_resume()` | App regains foreground. |
| `on_stop()` | App is being unloaded. Free resources, save state. Callback runs before `dlclose`. |

Call `api->app_exit()` from your app to request a clean shutdown.

## Input Events

`on_input` receives a `ghostesp_input_event_t`:

```c
typedef struct {
    ghostesp_input_type_t type;
    int32_t value;
    int32_t x;
    int32_t y;
    bool pressed;
} ghostesp_input_event_t;
```

| Field | Meaning |
|-------|---------|
| `type` | One of the `GHOSTESP_INPUT_*` values below. |
| `value` | For `GHOSTESP_INPUT_KEY`, the keycode; otherwise 0. |
| `x`, `y` | For `GHOSTESP_INPUT_TOUCH`, the touch coordinates in screen space (origin top-left); otherwise 0. |
| `pressed` | `true` on press, `false` on release. For `GHOSTESP_INPUT_TOUCH`, also `true` while a finger is held. |

Input types:

| Value | Description |
|-------|-------------|
| `GHOSTESP_INPUT_NONE` | Reserved / placeholder. |
| `GHOSTESP_INPUT_LEFT` | D-pad left. |
| `GHOSTESP_INPUT_RIGHT` | D-pad right. |
| `GHOSTESP_INPUT_UP` | D-pad up. |
| `GHOSTESP_INPUT_DOWN` | D-pad down. |
| `GHOSTESP_INPUT_SELECT` | Primary action (center / OK / Enter). |
| `GHOSTESP_INPUT_BACK` | Cancel / back. |
| `GHOSTESP_INPUT_KEY` | Keyboard event; check `value` for the keycode. |
| `GHOSTESP_INPUT_TOUCH` | Touch event; check `x` and `y` for screen coordinates. |

## API Reference

The `ghostesp_api_t` struct is passed to `ghostesp_app_init`. All function pointers through the SDK header `plugins/sdk/ghostesp_plugin_api.h`.

### System

```c
void        (*log)(const char *message);
void        (*delay_ms)(uint32_t ms);
size_t      (*system_free_heap)(void);
size_t      (*system_free_internal_heap)(void);
uint32_t    (*system_uptime_ms)(void);
const char *(*system_firmware_version)(void);
const char *(*system_target)(void);
const char *(*app_id)(void);
const char *(*app_data_path)(void);
uint8_t     (*settings_get_theme)(void);
const char *(*settings_get_device_name)(void);
void        (*app_exit)(void);
bool        (*has_permission)(const char *permission);
bool        (*has_feature)(const char *feature);
```

`has_permission` checks the current app manifest permissions by name, for example `"wifi"` or `"nrf24"`. `has_feature` checks host capabilities such as `"touchscreen"`, `"compact_screen"`, `"absolute_storage"`, `"subghz"`, `"nrf24"`, `"camera"`, `"usb"`, `"badusb"`, `"ir"`, or `"ble"`.

### Memory (Tracked)

Use the `app_` allocators for memory that counts against your `memory_limit`:

```c
void   *(*app_malloc)(size_t size);
void   *(*app_calloc)(size_t count, size_t size);
void    (*app_free)(void *ptr);
size_t  (*app_memory_used)(void);
size_t  (*app_memory_limit)(void);
```

Raw `malloc`/`free` and `api->malloc`/`api->free` are also available but untracked.

### Runner UI (Simple Text Output)

```c
void (*ui_set_title)(const char *title);
void (*ui_print)(const char *text);
void (*ui_clear)(void);
void (*toast)(const char *message);
void (*ui_show_text)(const char *title, const char *text);
void (*ui_set_status)(const char *text);
```

### Full UI (Requires `ui` Permission)

```c
ghostesp_ui_obj_t (*ui_screen_create)(const char *title);
ghostesp_ui_obj_t (*ui_card_create)(ghostesp_ui_obj_t parent);
ghostesp_ui_obj_t (*ui_label_create)(ghostesp_ui_obj_t parent, const char *text);
ghostesp_ui_obj_t (*ui_button_create)(ghostesp_ui_obj_t parent, const char *text, ghostesp_ui_button_cb_t on_click, void *user);
void (*ui_label_set_text)(ghostesp_ui_obj_t label, const char *text);
void (*ui_button_set_text)(ghostesp_ui_obj_t button, const char *text);
void (*ui_button_set_selected)(ghostesp_ui_obj_t button, bool selected);
void (*ui_obj_set_visible)(ghostesp_ui_obj_t obj, bool visible);
void (*ui_obj_delete)(ghostesp_ui_obj_t obj);
void (*ui_show_popup)(const char *title, const char *text);
int32_t (*ui_screen_get_width)(void);
int32_t (*ui_screen_get_height)(void);
int32_t (*ui_screen_get_content_width)(void);
int32_t (*ui_screen_get_content_height)(void);
```

### Widget Styling

```c
void (*ui_obj_set_bg_color)(ghostesp_ui_obj_t obj, uint32_t hex_color);
void (*ui_obj_set_text_color)(ghostesp_ui_obj_t obj, uint32_t hex_color);
void (*ui_obj_set_border_color)(ghostesp_ui_obj_t obj, uint32_t hex_color);
void (*ui_obj_set_border_width)(ghostesp_ui_obj_t obj, int32_t width);
void (*ui_obj_set_radius)(ghostesp_ui_obj_t obj, int32_t radius);
void (*ui_obj_set_pad)(ghostesp_ui_obj_t obj, int32_t l, int32_t r, int32_t t, int32_t b);
void (*ui_obj_set_font)(ghostesp_ui_obj_t obj, ghostesp_font_size_t size);
void (*ui_obj_set_opa)(ghostesp_ui_obj_t obj, uint8_t opa);
void (*ui_obj_set_pos)(ghostesp_ui_obj_t obj, int32_t x, int32_t y);
void (*ui_obj_set_size)(ghostesp_ui_obj_t obj, int32_t w, int32_t h);
void (*ui_obj_set_width)(ghostesp_ui_obj_t obj, int32_t w);
void (*ui_obj_set_height)(ghostesp_ui_obj_t obj, int32_t h);
void (*ui_obj_align)(ghostesp_ui_obj_t obj, ghostesp_align_t align, int32_t x_ofs, int32_t y_ofs);
int32_t (*ui_obj_get_width)(ghostesp_ui_obj_t obj);
int32_t (*ui_obj_get_height)(ghostesp_ui_obj_t obj);
int32_t (*ui_obj_get_x)(ghostesp_ui_obj_t obj);
int32_t (*ui_obj_get_y)(ghostesp_ui_obj_t obj);
```

### Flex Layout

```c
void (*ui_obj_set_flex_flow)(ghostesp_ui_obj_t obj, ghostesp_flex_flow_t flow);
void (*ui_obj_set_flex_align)(ghostesp_ui_obj_t obj, ghostesp_flex_align_t main, ghostesp_flex_align_t cross, ghostesp_flex_align_t track);
void (*ui_obj_set_flex_grow)(ghostesp_ui_obj_t obj, uint8_t grow);
void (*ui_obj_set_pad_row)(ghostesp_ui_obj_t obj, int32_t pad);
void (*ui_obj_set_pad_column)(ghostesp_ui_obj_t obj, int32_t pad);
```

Flex flow values: `COLUMN`, `ROW`, `COLUMN_WRAP`, `ROW_WRAP`, `COLUMN_REVERSE`, `ROW_REVERSE`, and their `_WRAP_REVERSE` variants.

### Scrolling

```c
void (*ui_obj_set_scrollable)(ghostesp_ui_obj_t obj, bool scrollable);
void (*ui_obj_set_scrollbar)(ghostesp_ui_obj_t obj, bool enabled);
void (*ui_obj_scroll_by)(ghostesp_ui_obj_t obj, int32_t dx, int32_t dy, bool animated);
```

`ui_obj_set_scrollable` enables scroll gestures (drag / wheel) on any container. `ui_obj_set_scrollbar` toggles the scrollbar indicator (visible by default once the content overflows). `ui_obj_scroll_by` programmatically nudges the viewport — pass `animated = true` for a tween, `false` for an instant jump (useful when resetting to the top of a list).

### Theme Colors

```c
uint32_t (*ui_theme_get_background)(void);
uint32_t (*ui_theme_get_surface)(void);
uint32_t (*ui_theme_get_surface_alt)(void);
uint32_t (*ui_theme_get_text)(void);
uint32_t (*ui_theme_get_text_muted)(void);
uint32_t (*ui_theme_get_accent)(void);
bool     (*ui_theme_is_bright)(void);
```

Returns 24-bit hex colors (`0xRRGGBB`).

### Options Menu

```c
ghostesp_options_t (*ui_options_create)(const char *title);
ghostesp_ui_obj_t  (*ui_options_add_item)(ghostesp_options_t opts, const char *label, ghostesp_ui_button_cb_t cb, void *user);
ghostesp_ui_obj_t  (*ui_options_add_back)(ghostesp_options_t opts, ghostesp_ui_button_cb_t cb, void *user);
void (*ui_options_set_selected)(ghostesp_options_t opts, int index);
void (*ui_options_move_selection)(ghostesp_options_t opts, int delta);
int  (*ui_options_get_selected)(ghostesp_options_t opts);
void (*ui_options_clear)(ghostesp_options_t opts);
void (*ui_options_destroy)(ghostesp_options_t opts);
```

### Detail View

```c
ghostesp_detail_t (*ui_detail_create)(const char *title);
void (*ui_detail_add_info)(ghostesp_detail_t dv, const char *label, const char *value);
void (*ui_detail_add_action)(ghostesp_detail_t dv, const char *label, ghostesp_ui_button_cb_t on_click, void *user);
void (*ui_detail_add_header)(ghostesp_detail_t dv, const char *text);
void (*ui_detail_add_divider)(ghostesp_detail_t dv);
ghostesp_ui_obj_t (*ui_detail_add_back)(ghostesp_detail_t dv, ghostesp_ui_button_cb_t on_click, void *user);
void (*ui_detail_set_selected)(ghostesp_detail_t dv, int index);
void (*ui_detail_move_selection)(ghostesp_detail_t dv, int delta);
int  (*ui_detail_get_selected)(ghostesp_detail_t dv);
int  (*ui_detail_get_count)(ghostesp_detail_t dv);
bool (*ui_detail_step_up)(ghostesp_detail_t dv);
bool (*ui_detail_step_down)(ghostesp_detail_t dv);
void (*ui_detail_activate_selected)(ghostesp_detail_t dv);
void (*ui_detail_clear)(ghostesp_detail_t dv);
void (*ui_detail_destroy)(ghostesp_detail_t dv);
ghostesp_ui_obj_t (*ui_detail_finish)(ghostesp_detail_t dv, ghostesp_ui_button_cb_t on_back, void *user);
```

`ui_detail_finish` adds a back button — a convenient shorthand. Add a divider first if desired via `ui_detail_add_divider`.

### Popup Dialog

```c
ghostesp_popup_t  (*ui_popup_create)(int32_t width, int32_t height);
void (*ui_popup_set_title)(ghostesp_popup_t p, const char *title);
void (*ui_popup_set_body)(ghostesp_popup_t p, const char *body);
ghostesp_ui_obj_t (*ui_popup_add_button)(ghostesp_popup_t p, const char *label, ghostesp_ui_button_cb_t on_click, void *user);
void (*ui_popup_show)(ghostesp_popup_t p);
void (*ui_popup_hide)(ghostesp_popup_t p);
void (*ui_popup_destroy)(ghostesp_popup_t p);
```

### Scan Status Overlay

```c
ghostesp_scan_t (*ui_scan_status_create)(const char *message);
void (*ui_scan_status_update)(ghostesp_scan_t ss, const char *message);
void (*ui_scan_status_set_progress)(ghostesp_scan_t ss, int current, int total);
void (*ui_scan_status_close)(ghostesp_scan_t ss);
```

### Canvas Drawing

```c
ghostesp_ui_obj_t (*ui_canvas_create)(ghostesp_ui_obj_t parent, int32_t w, int32_t h);
void (*ui_canvas_draw_rect)(ghostesp_ui_obj_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t hex_color);
void (*ui_canvas_fill)(ghostesp_ui_obj_t canvas, uint32_t hex_color);
void (*ui_canvas_draw_line)(ghostesp_ui_obj_t canvas, const ghostesp_point_t *pts, int count, uint32_t hex_color, int32_t width);
void (*ui_canvas_draw_arc)(ghostesp_ui_obj_t canvas, int32_t cx, int32_t cy, int32_t r, int32_t start_angle, int32_t end_angle, uint32_t hex_color, int32_t width);
```

### Animations

```c
void (*ui_anim_slide_in)(ghostesp_ui_obj_t obj, int direction, uint32_t duration_ms);
void (*ui_anim_slide_out)(ghostesp_ui_obj_t obj, int direction, uint32_t duration_ms, ghostesp_anim_done_cb_t on_done, void *user);
void (*ui_anim_pop_in)(ghostesp_ui_obj_t obj);
void (*ui_anim_press_pulse)(ghostesp_ui_obj_t obj);
```

### Arc & Line Widgets

```c
ghostesp_ui_obj_t (*ui_arc_create)(ghostesp_ui_obj_t parent);
void (*ui_arc_set_value)(ghostesp_ui_obj_t arc, int32_t value);
void (*ui_arc_set_range)(ghostesp_ui_obj_t arc, int32_t min, int32_t max);
void (*ui_arc_set_angles)(ghostesp_ui_obj_t arc, int32_t start, int32_t end);
void (*ui_arc_set_bg_angles)(ghostesp_ui_obj_t arc, int32_t start, int32_t end);
void (*ui_arc_set_bg_color)(ghostesp_ui_obj_t arc, uint32_t hex_color);
void (*ui_arc_set_indicator_color)(ghostesp_ui_obj_t arc, uint32_t hex_color);

ghostesp_ui_obj_t (*ui_line_create)(ghostesp_ui_obj_t parent);
void (*ui_line_set_points)(ghostesp_ui_obj_t line, const ghostesp_point_t *pts, int count);
void (*ui_line_set_color)(ghostesp_ui_obj_t line, uint32_t hex_color);
void (*ui_line_set_width)(ghostesp_ui_obj_t line, int32_t width);
```

### Image

```c
ghostesp_ui_obj_t (*ui_image_create)(ghostesp_ui_obj_t parent);
bool (*ui_image_set_src)(ghostesp_ui_obj_t img, const char *app_relative_path);
```

### Paged Menu

```c
ghostesp_paged_menu_t (*ui_paged_menu_create)(int page_size, ghostesp_paged_menu_load_fn load_fn, void *user);
void (*ui_paged_menu_set_callbacks)(ghostesp_paged_menu_t pm, ghostesp_paged_menu_select_fn select, ghostesp_paged_menu_nav_fn prev, ghostesp_paged_menu_nav_fn next, void *user);
void (*ui_paged_menu_reset)(ghostesp_paged_menu_t pm);
void (*ui_paged_menu_destroy)(ghostesp_paged_menu_t pm);
bool (*ui_paged_menu_has_prev)(ghostesp_paged_menu_t pm);
bool (*ui_paged_menu_has_next)(ghostesp_paged_menu_t pm);
```

### Touch Bar

Touch-optimized action bar with built-in back/up/down buttons. Use on devices with a touchscreen for primary navigation:

```c
ghostesp_ui_obj_t (*ui_touch_bar_create)(ghostesp_ui_obj_t parent);
ghostesp_ui_obj_t (*ui_touch_bar_add_back)(ghostesp_ui_obj_t bar, ghostesp_ui_button_cb_t on_click, void *user);
ghostesp_ui_obj_t (*ui_touch_bar_add_up)(ghostesp_ui_obj_t bar, ghostesp_ui_button_cb_t on_click, void *user);
ghostesp_ui_obj_t (*ui_touch_bar_add_down)(ghostesp_ui_obj_t bar, ghostesp_ui_button_cb_t on_click, void *user);
```

`ui_touch_bar_create` returns a bar widget; add up to three buttons with the `add_*` helpers, in any order. The bar lays out as a horizontal flex row.

### Timers

```c
ghostesp_ui_timer_t (*ui_timer_create)(ghostesp_ui_timer_cb_t cb, uint32_t interval_ms, void *user);
void (*ui_timer_delete)(ghostesp_ui_timer_t timer);
void (*ui_timer_set_interval)(ghostesp_ui_timer_t timer, uint32_t interval_ms);
```

### Input Dialog

```c
void (*ui_input_dialog)(const char *title, const char *default_text, ghostesp_input_submit_cb_t on_submit, void *user);
```

### Screen Queries

Capability queries for adapting UI to the host device:

```c
bool (*ui_screen_is_compact)(void);
bool (*ui_has_touchscreen)(void);
```

`ui_screen_is_compact` reports whether the host considers the current screen a "compact" layout (narrow, single-column — e.g. cardputer, M5StickC) versus a wide layout (gallery / grid). Use it to swap list views for grids, hide labels, or pick a denser font. `ui_has_touchscreen` reports whether the device has a touch panel; combine with `GHOSTESP_INPUT_TOUCH` events in `on_input` to add touch-only affordances.

### Storage (Absolute, `storage` Permission + `storage_scope: "ghostesp"`)

These calls only work when the manifest sets `storage_scope` to `"ghostesp"`. Paths must stay under `/mnt/ghostesp` and traversal such as `..` is rejected.

```c
bool (*storage_exists)(const char *path);
int  (*storage_read)(const char *path, void *buffer, size_t buffer_len);
bool (*storage_write)(const char *path, const void *data, size_t len);
bool (*storage_append)(const char *path, const void *data, size_t len);
bool (*storage_delete)(const char *path);
bool (*storage_mkdir)(const char *path);
int  (*storage_list)(const char *path, ghostesp_storage_entry_t *out, int max_entries);
```

### Storage (App-Scoped, `storage` Permission)

Maps to `/mnt/ghostesp/appdata/<app_id>/`:

```c
bool (*app_storage_exists)(const char *path);
int  (*app_storage_read)(const char *path, void *buffer, size_t buffer_len);
bool (*app_storage_write)(const char *path, const void *data, size_t len);
bool (*app_storage_append)(const char *path, const void *data, size_t len);
bool (*app_storage_delete)(const char *path);
bool (*app_storage_mkdir)(const char *path);
int  (*app_storage_list)(const char *path, ghostesp_storage_entry_t *out, int max_entries);
```

### Storage Utilities (`storage` Permission)

```c
typedef struct {
    uint64_t size;
    bool is_directory;
} ghostesp_storage_stat_t;

bool    (*storage_stat)(const char *path, ghostesp_storage_stat_t *out);
int64_t (*storage_size)(const char *path);
bool    (*storage_rename)(const char *from, const char *to);
bool    (*storage_mkdir_recursive)(const char *path);
bool    (*app_storage_stat)(const char *path, ghostesp_storage_stat_t *out);
int64_t (*app_storage_size)(const char *path);
bool    (*app_storage_rename)(const char *from, const char *to);
bool    (*app_storage_mkdir_recursive)(const char *path);
```

Absolute storage utilities follow the same absolute-storage manifest rules as `storage_*`; app-scoped utilities stay under `/mnt/ghostesp/appdata/<app_id>/`.

### WiFi

```c
bool      (*wifi_start_scan)(void);
bool      (*wifi_stop_scan)(void);
uint16_t  (*wifi_ap_count)(void);
bool      (*wifi_scan_get_ap)(uint16_t index, ghostesp_wifi_ap_info_t *out);
```

### WiFi Control (`wifi_control` Permission)

```c
bool (*wifi_connect)(const char *ssid, const char *password, uint32_t timeout_ms);
bool (*wifi_disconnect)(void);
bool (*wifi_is_connected)(void);
int  (*wifi_rssi)(void);
bool (*wifi_ip)(char *out, size_t out_len);
```

`wifi_is_connected`, `wifi_rssi`, and `wifi_ip` require `wifi`; changing connection state requires `wifi_control`.

### HTTP (`network` Permission)

```c
int (*http_get)(const char *url, void *buffer, size_t buffer_len, uint32_t timeout_ms);
int (*http_post)(const char *url, const void *body, size_t body_len, void *buffer, size_t buffer_len, uint32_t timeout_ms);
```

Returns bytes read into `buffer`, or `-1` on failure.

### Low-Level GPIO (`raw_gpio` Permission)

```c
typedef void (*ghostesp_gpio_intr_cb_t)(int pin, int level, void *user);

bool (*gpio_set_mode)(int pin, uint32_t mode);
bool (*gpio_write)(int pin, int level);
int  (*gpio_read)(int pin);
bool (*gpio_set_pull)(int pin, bool pullup, bool pulldown);
bool (*gpio_set_drive_strength)(int pin, int strength);
bool (*gpio_set_intr)(int pin, int edge, ghostesp_gpio_intr_cb_t cb, void *user);
bool (*gpio_clear_intr)(int pin);
```

Reserved board pins are rejected. Interrupts are cleared automatically when the app exits.

### Low-Level Buses

```c
bool (*uart_open)(int uart_num, int tx_pin, int rx_pin, uint32_t baud);
int  (*uart_write)(int uart_num, const void *data, size_t len);
int  (*uart_read)(int uart_num, void *buffer, size_t len, uint32_t timeout_ms);
bool (*uart_close)(int uart_num);

bool (*i2c_probe)(uint8_t addr, uint32_t timeout_ms);
bool (*i2c_write)(uint8_t addr, const void *data, size_t len, uint32_t timeout_ms);
int  (*i2c_read)(uint8_t addr, void *buffer, size_t len, uint32_t timeout_ms);
bool (*i2c_write_read)(uint8_t addr, const void *tx, size_t tx_len, void *rx, size_t rx_len, uint32_t timeout_ms);

int  (*spi_open)(int host, int sclk, int miso, int mosi, int cs, uint32_t hz, int mode);
int  (*spi_transfer)(int handle, const void *tx, void *rx, size_t len);
bool (*spi_close)(int handle);
```

UART requires `uart` and blocks UART0. I2C requires `i2c` and uses the board I2C bus. SPI requires `spi` and returns an app-local handle.

### ADC, PWM, Timing, Random

```c
int  (*adc_read_raw)(int channel);
int  (*adc_read_mv)(int channel);

bool (*pwm_attach)(int pin, uint32_t freq_hz, uint8_t resolution_bits);
bool (*pwm_write)(int pin, uint32_t duty);
bool (*pwm_detach)(int pin);

uint64_t (*system_uptime_us)(void);
void     (*delay_us)(uint32_t us);
uint32_t (*random_u32)(void);
bool     (*random_bytes)(void *buffer, size_t len);
```

Permissions: `adc`, `pwm`, `time`, and `random` respectively. `adc_read_mv` currently returns the raw reading when board calibration is unavailable.

### Power, Display, Input

```c
int      (*battery_percent)(void);
int      (*battery_voltage_mv)(void);
bool     (*battery_is_charging)(void);
uint8_t  (*display_get_brightness)(void);
bool     (*display_set_brightness)(uint8_t percent);
uint32_t (*input_buttons_state)(void);
```

Permissions: `power`, `display`, and `input`. `input_buttons_state` returns bit 0-4 for left/select/up/right/down.

### App Tasks (`tasks` Permission)

```c
typedef void (*ghostesp_task_fn_t)(void *user);
typedef void *ghostesp_task_t;

ghostesp_task_t (*task_create)(const char *name, ghostesp_task_fn_t fn, void *user, uint32_t stack_size, int priority);
bool (*task_delete)(ghostesp_task_t task);
void (*task_yield)(void);
```

Tasks created through the API are tracked and deleted when the app exits.

### TCP/UDP Sockets (`network` Permission)

```c
int  (*tcp_connect)(const char *host, uint16_t port, uint32_t timeout_ms);
int  (*socket_send)(int sock, const void *data, size_t len);
int  (*socket_recv)(int sock, void *buffer, size_t len, uint32_t timeout_ms);
bool (*socket_close)(int sock);
int  (*udp_open)(uint16_t local_port);
int  (*udp_send_to)(int sock, const char *host, uint16_t port, const void *data, size_t len);
int  (*udp_recv_from)(int sock, void *buffer, size_t len, char *host_out, size_t host_out_len, uint16_t *port_out, uint32_t timeout_ms);
```

Sockets opened through the API are closed when the app exits.

### Wall Clock And System

```c
int64_t (*time_unix)(void);
bool    (*time_set_unix)(int64_t unix_time);
void    (*system_reboot)(void);
```

Time APIs require `time`. Reboot requires `system`.

### WiFi Monitor And Raw TX

```c
typedef void (*ghostesp_wifi_packet_cb_t)(const uint8_t *data, size_t len, int8_t rssi, uint8_t channel, void *user);

bool    (*wifi_set_channel)(uint8_t channel);
uint8_t (*wifi_get_channel)(void);
bool    (*wifi_monitor_start)(ghostesp_wifi_packet_cb_t cb, void *user);
bool    (*wifi_monitor_stop)(void);
bool    (*wifi_raw_tx)(const void *data, size_t len);
```

Monitor receive requires `wifi`. Channel changes and raw TX require `wifi_control`. Monitor mode is stopped automatically when the app exits.

### WiFi Async and Live Scan

The basic `wifi_start_scan` blocks the calling task until the scan completes. For non-blocking or continuous scans:

```c
bool (*wifi_start_scan_async)(void);
bool (*wifi_scan_check_done)(void);
void (*wifi_finish_scan)(void);
```

`wifi_start_scan_async` kicks off a scan and returns immediately. Poll `wifi_scan_check_done` from `on_tick`; when it returns `true`, read results with `wifi_scan_get_ap` (same as the blocking variant), then call `wifi_finish_scan` to release the scan buffer. Calling `wifi_start_scan_async` while a scan is already in progress returns `false`.

For continuous monitoring (a UI that updates as new APs appear without explicit rescan calls):

```c
bool (*wifi_live_scan_start)(void);
void (*wifi_live_scan_stop)(void);
bool (*wifi_live_scan_active)(void);
```

`wifi_live_scan_start` begins a repeating scan loop in the background; the AP list refreshes automatically between calls to `wifi_ap_count` and `wifi_scan_get_ap`. `wifi_live_scan_active` reports whether the background loop is currently running. Always call `wifi_live_scan_stop` from `on_stop` to release the worker task — it is not auto-stopped on app exit.

### Protocol Native Hooks

```c
bool (*nfc_get_last_uid)(uint8_t *uid, size_t *uid_len);
bool (*nfc_write_file)(const char *app_relative_path);
bool (*ir_send_raw)(uint32_t carrier_hz, const uint16_t *durations, size_t count);
bool (*ir_receive_start)(void);
bool (*ir_receive_stop)(void);
int  (*ir_receive_read)(uint16_t *durations, size_t max_count);
bool (*subghz_transmit_raw)(uint32_t frequency_hz, const uint16_t *durations, size_t count);
bool (*ble_adv_start)(const uint8_t *data, size_t len);
bool (*ble_adv_stop)(void);
```

Raw IR, raw SubGHz, and BLE advertising are wired to existing managers where the target supports them. NFC UID read is implemented (tracked from PN532 scans). NFC write/read-result beyond UID is reserved until the NFC manager exposes reusable result hooks. Unsupported firmware builds return `false` or `-1`.

### Advanced Native Hooks

```c
bool (*ble_gatt_connect)(const uint8_t mac[6]);
bool (*ble_gatt_disconnect)(void);
int  (*ble_gatt_read)(uint16_t service_uuid, uint16_t char_uuid, void *buffer, size_t buffer_len);
bool (*ble_gatt_write)(uint16_t service_uuid, uint16_t char_uuid, const void *data, size_t len);
bool (*ble_gatt_server_start)(const char *name);
bool (*ble_gatt_server_stop)(void);

bool (*wifi_deauth)(const uint8_t bssid[6], const uint8_t sta[6], uint8_t reason);
bool (*wifi_send_beacon)(const char *ssid, const uint8_t bssid[6], uint8_t channel);
bool (*wifi_pcap_start)(const char *app_relative_path);
bool (*wifi_pcap_stop)(void);

bool (*ethernet_is_connected)(void);
bool (*ethernet_ip)(char *out, size_t out_len);

int  (*camera_capture_jpeg)(void *buffer, size_t buffer_len);
bool (*camera_capture_jpeg_file)(const char *app_relative_path);

bool (*usb_hid_keyboard_send)(const char *text);
bool (*usb_hid_mouse_move)(int dx, int dy, uint8_t buttons);

bool  (*audio_mic_is_available)(void);
int   (*audio_mic_read)(int32_t *samples, size_t max_samples, uint32_t timeout_ms);
float (*audio_mic_rms)(const int32_t *samples, size_t count);

bool (*zigbee_capture_start)(uint8_t channel);
bool (*zigbee_capture_stop)(void);
bool (*zigbee_is_capturing)(void);
int  (*zigbee_device_count)(void);
```
### Settings, NVS, Events, Parsers

```c
bool (*settings_get_u8)(const char *key, uint8_t *out);
bool (*settings_set_u8)(const char *key, uint8_t value);
bool (*settings_get_string)(const char *key, char *out, size_t out_len);
bool (*settings_set_string)(const char *key, const char *value);
bool (*settings_save)(void);

bool (*nvs_get_u32)(const char *key, uint32_t *out);
bool (*nvs_set_u32)(const char *key, uint32_t value);
int  (*nvs_get_blob)(const char *key, void *buffer, size_t buffer_len);
bool (*nvs_set_blob)(const char *key, const void *data, size_t len);
bool (*nvs_delete)(const char *key);

ghostesp_event_sub_t (*event_subscribe)(const char *topic, ghostesp_event_cb_t cb, void *user);
bool (*event_unsubscribe)(ghostesp_event_sub_t sub);
bool (*event_publish)(const char *topic, const void *data, size_t len);

bool (*parser_nfc_summary)(const char *app_relative_path, char *out, size_t out_len);
bool (*parser_ir_summary)(const char *app_relative_path, char *out, size_t out_len);
bool (*parser_subghz_summary)(const char *app_relative_path, char *out, size_t out_len);
```

Settings keys currently supported: `theme`, `max_brightness`, `nav_buttons`, `neopixel_brightness`, `power_save`, and string key `device_name`. NVS keys are app-scoped and hashed into the firmware NVS namespace. There is no OTA API.

### BLE

```c
bool (*ble_start_scan)(void);
bool (*ble_stop_scan)(void);
int  (*ble_device_count)(void);
bool (*ble_get_device)(int index, ghostesp_ble_device_info_t *out);

void (*ble_detect_start)(void);
void (*ble_detect_stop)(void);
bool (*ble_detect_is_active)(void);
int  (*ble_detect_count)(void);
bool (*ble_detect_get_device)(int index, ghostesp_ble_detect_info_t *out);
const char *(*ble_detect_type_name)(uint8_t type);
bool (*ble_detect_start_tracking)(int index);
bool (*ble_detect_start_airtag_spoof)(int index);
```

### BLE Advertiser Scan

```c
typedef struct {
    uint8_t  mac[6];
    uint8_t  addr_type;            // 0 = Public, 1 = Random, other = Unknown
    int8_t   rssi;
    uint8_t  event_type;
    uint32_t seen_count;
    char     name[24];
    bool     has_flags;
    uint8_t  flags;
    bool     has_tx_power;
    int8_t   tx_power;
    bool     has_manufacturer_id;
    uint16_t manufacturer_id;
    bool     is_ibeacon;
    char     ibeacon_uuid[37];
    uint16_t ibeacon_major;
    uint16_t ibeacon_minor;
    int8_t   ibeacon_measured_power;
    char     adv_type[16];
    char     manufacturer[24];
    char     oui_vendor[32];      // Empty if MAC has no recognized OUI
    char     services[96];        // Comma-separated 16-bit service UUIDs
    char     service_data[64];    // Includes Eddystone frame hints
    bool     has_appearance;
    uint16_t appearance;
} ghostesp_ble_adv_info_t;

void (*ble_adv_scan_start)(void);
void (*ble_adv_scan_stop)(void);
bool (*ble_adv_scan_active)(void);
int  (*ble_adv_scan_count)(void);
bool (*ble_adv_scan_get)(int index, ghostesp_ble_adv_info_t *out);
bool (*ble_adv_scan_track)(int index);
void (*ble_adv_scan_stop_tracking)(void);
bool (*ble_adv_scan_save_to_sd)(int index);
```

`ble_adv_scan_start` begins a parsed BLE advertisement scan that does not require a connection. Use `ble_adv_scan_get` to enumerate results, `ble_adv_scan_track` to follow a single advertiser's RSSI on the terminal, and `ble_adv_scan_save_to_sd` to write the parsed record(s) to `/mnt/ghostesp/scans/ble_advertiser*.txt`. Pass `index < 0` to save the full list as `ble_advertisers_*.txt`. Requires the `ble` permission. Not available on ESP32-S2.

### NRF24

```c
bool (*nrf24_start)(bool stream_to_peer);
void (*nrf24_stop)(void);
bool (*nrf24_is_running)(void);
bool (*nrf24_is_paused)(void);
void (*nrf24_set_paused)(bool paused);
```

Requires `nrf24` permission. Available on targets with NRF24 radio module support.

### NFC

```c
bool (*nfc_is_available)(void);
bool (*nfc_read_start)(void);
bool (*nfc_stop)(void);
```

### Infrared

```c
bool (*ir_send_file)(const char *app_relative_path);
bool (*ir_stop)(void);
```

### SubGHz

```c
bool (*subghz_is_available)(void);
bool (*subghz_load_snapshot)(const char *app_relative_path);
bool (*subghz_transmit_loaded)(void);
bool (*subghz_transmit_file)(const char *app_relative_path);
bool (*subghz_stop)(void);
```

`subghz_transmit_file` replays an app-scoped Flipper-style `.sub` file directly. `subghz_load_snapshot` plus `subghz_transmit_loaded` is still useful when an app wants to remember or inspect the loaded file before transmitting.

### BadUSB

```c
bool (*badusb_run_script)(const char *app_relative_path);
bool (*badusb_stop)(void);
```

### RGB LEDs

```c
bool (*rgb_set_all)(uint8_t red, uint8_t green, uint8_t blue);
```

### GPS

```c
bool   (*gps_is_available)(void);
bool   (*gps_has_fix)(void);
double (*gps_get_latitude)(void);
double (*gps_get_longitude)(void);
double (*gps_get_altitude)(void);
int    (*gps_get_satellites)(void);
float  (*gps_get_speed)(void);
float  (*gps_get_heading)(void);
```

### CLI Commands

```c
bool (*command_exec)(const char *command);
```

### Raw LVGL Access (`lvgl` Permission)

```c
void *(*lv_scr_act)(void);
void *(*display_get_current_view)(void);
void *(*raw_symbol)(const char *name);
```

## Application Template

The SDK includes a template project:

```
plugins/templates/basic_app/
  CMakeLists.txt
  main/
    CMakeLists.txt
    idf_component.yml
    {{APP_SYMBOL}}.c
  manifest.json
  sdkconfig.defaults
```

### Example App (Minimal)

```c
#include "ghostesp_plugin_api.h"

static const ghostesp_api_t *g_api;

static void on_start(void) {
    g_api->ui_print("Hello from SD app!\n");
}

GHOSTESP_APP_DEFINE("my_app", "My App", on_start, NULL, NULL, NULL)
```

### CMake Project Structure

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(myapp)

set(EXTRA_COMPONENT_DIRS
    ../../components
    ../../components/espressif__elf_loader
)
```

## Build Targets

Build one `.so` per supported native SD app target. Xtensa (esp32/s2/s3) and RISC-V (c5/c6/c61/p4) binaries are not interchangeable.

| Target | Architecture |
|--------|-------------|
| `esp32` | Xtensa LX6 |
| `esp32s2` | Xtensa LX7 |
| `esp32s3` | Xtensa LX7 |
| `esp32c5` | RISC-V |
| `esp32c6` | RISC-V |
| `esp32c61` | RISC-V |
| `esp32p4` | RISC-V |

`esp32c3` is not listed because the current ELF loader configuration does not enable native SD `.gapp` shared-object loading for C3.

## .gapp Archive Format

The `.gapp` format is a custom streaming archive (not ZIP):

- **Header**: 4-byte magic `GAPP`, 2-byte version, 2-byte flags, 4-byte file count.
- **Per-file entry**: 4-byte magic `FILE`, 2-byte compression method (0=store, 1=raw-deflate), 2-byte path length, 4-byte uncompressed size, 4-byte compressed size, 8-byte FNV-1a 64-bit checksum, then UTF-8 relative path, then payload.
- Firmware extracts `.gapp` files into `/mnt/ghostesp/app_cache/<name>-<hash>/` because `elf_loader` needs a real `.so` file path.

## App State

State is tracked per-app in `/mnt/ghostesp/appdata/<app_id>/.state.json`:

```json
{
  "launch_failure_count": 0,
  "quarantined": false,
  "launch_pending": false,
  "last_error": ""
}
```

Apps that crash or fail to load keep a failure count and last error for diagnostics. They are not blocked from launching automatically. Reset the diagnostic state with:

```
apps reset <id>
```

A clean exit (normal `on_stop` -> `dlclose`) resets the failure count to 0.

## Memory & Load Constraints

### Executable Sections Must Use Internal RAM

The ELF loader requires **internal RAM for executable sections** (`.text`, `.rodata` mapped as executable). This is enforced on **ESP32-C5** (and any future target where `MALLOC_CAP_EXEC` only exists in internal memory). PSRAM **cannot** hold executable code.

The load will fail with `ELC: exec alloc N -> 0x0 internal=0 exec=0` if the app's executable segment cannot fit in the available internal pool.

**Practical limits** for executable data on C5 vary by firmware build, but a rough guideline is ~20-30 KB of internal heap available for ELF loading after firmware init. Apps with large `.text` sections or many translation units may need to:
- Use `memory_limit` in `manifest.json` conservatively (the tracked limit covers `app_malloc`/`app_calloc` usage, but the .so's executable sections consume separate internal heap).
- Reduce code size (link-time optimization, strip unused functions, enable `-Os`).

### Non-Executable Data PSRAM Preference

The loader allocates non-executable sections (`.data`, `.bss`) with **PSRAM-first** on targets that have PSRAM, falling back to internal RAM. The firmware itself also prefers PSRAM for its internal structures:
- Plugin manager app registry
- Gallery card data and task stacks
- `.gapp` extraction buffers

This conserves internal RAM for executable allocations.

### Stack

The app's stack is allocated from internal RAM (or PSRAM where the board supports it and PSRAM stacks are stable). The `stack_size` field in `manifest.json` is advisory — the firmware allocates a fixed runner task stack. Deep call chains or large stack locals may exhaust it.

### Firmware-Side Internal RAM Budget

The firmware's ELF loading infrastructure does not reserve a dedicated pool; it shares internal heap with other subsystems. Free internal RAM at load time depends on which features are active (WiFi, BLE, LVGL buffers, etc.). Use `api->system_free_internal_heap()` from your app to see the remaining budget at runtime.

A boot-time message in the monitor log shows free internal RAM after init:
```
Free INTERNAL RAM after init: 27160 / 118204 bytes (23.0% free)
```

If your app fails to load with an exec-memory error, check this value. Disabling non-essential firmware features in `menuconfig` can free internal heap for app loading.

## CLI Commands

| Command | Description |
|---------|-------------|
| `apps list` | List all discovered SD apps |
| `apps reload` | Rescan `/mnt/ghostesp/apps/` and `/mnt/ghostesp/packages/` |
| `apps info <id>` | Show manifest details and failure count |
| `apps run <id>` | Launch app (UI mode if screen available, headless otherwise) |
| `apps stop` | Stop the currently running app |
| `apps reset <id>` | Clear failure diagnostic state |
