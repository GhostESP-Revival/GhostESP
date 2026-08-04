# GhostESP Native SD Apps v1

Native SD apps are trusted ESP-IDF shared objects loaded from SD with Espressif `elf_loader`. They run inside the GhostESP firmware with access to the full hardware & UI API surface.

The SDK header is at `plugins/sdk/ghostesp_plugin_api.h`. Always rebuild apps against the SDK that ships with your firmware version; binaries built against earlier draft headers are not supported.

## Directory Layout

```
plugins/
  sdk/                      SDK header for app developers
  examples/                 Example apps
    device_inspector/       Full example exercising the API
  templates/basic_app/      Template for gbt create / new_app.py
  tools/                    Build tooling
    ghostbt/                GBT Python package
    new_app.py              Standalone scaffold script
    build_app.py            Standalone build script
    package_app.py          Standalone packaging script
  package.schema.json       manifest.json JSON Schema
  README.md                 This file
```

## SD Card Layout

```
/mnt/ghostesp/
  apps/<app_id>/            Extracted app folders
    manifest.json
    <entry>.so
  app_cache/                Auto-extracted .gapp content
  appdata/<app_id>/         Per-app persistent storage
    .state.json             Quarantine / failure state
```

## Manifest Format

```json
{
  "id": "device_inspector",
  "name": "Device Inspector",
  "version": "2.0.0",
  "author": "GhostESP",
  "description": "Comprehensive hardware and API test suite with responsive native UI.",
  "category": "System",
  "entry": "ghostesp_device_inspector.so",
  "target": "esp32c5",
  "api_version": 1,
  "manifest_version": 1,
  "package_version": 1,
  "data_version": 1,
  "storage_scope": "ghostesp",
  "icon": "icon.rgb565",
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

Required fields: `id`, `name`, `entry`, `api_version`. `target` is strongly recommended and required when `NATIVE_SD_APPS_REQUIRE_TARGET_MATCH` is enabled (default).

### Hardware Requirements

Use `"requires_features"` to prevent discovery/launch on unsupported input hardware. Supported values are `touchscreen`, `joystick` (or `dpad`), `encoder`, and `keyboard`.

```json
"requires_features": ["joystick"]
```

The App Gallery reports the missing requirement, and the loader rejects CLI/direct launches as well.

### Tick Rate

Apps with an `on_tick` callback run every 100 ms by default. Set `"tick_interval_ms"` to request a per-app interval from 16 to 1000 ms. Use this only for apps that need regular updates; Doom uses `28` ms to match its 35 Hz game tic.

### Permissions

| Permission | Unlocks |
|------------|---------|
| `ui` | Screen creation, widgets, popups, detail views, canvas, animations |
| `storage` | App-scoped file I/O; absolute `/mnt/ghostesp` file I/O only when `storage_scope` is `"ghostesp"` |
| `commands` | CLI command execution |
| `tasks` | Reserved for future task APIs |
| `wifi` | WiFi scan, AP enumeration, monitor receive |
| `ble` | BLE scan, device enumeration, BLE detection, reserved GATT hooks |
| `nfc` | NFC read/stop |
| `ir` | IR send file/stop |
| `subghz` | SubGHz snapshot load, transmit, stop |
| `nrf24` | NRF24 start/stop/pause and state queries |
| `badusb` | BadUSB run script/stop |
| `raw_gpio` | GPIO mode/read/write/pulls/drive/interrupts |
| `lvgl` | Raw `lv_scr_act()`, `display_get_current_view()` |
| `rgb` | `rgb_set_all`, LED control |
| `uart` | UART open/read/write/close; UART0 is blocked |
| `i2c` | Board I2C probe/read/write/write-read |
| `spi` | App-owned SPI device handles and transfers |
| `adc` | ADC raw/millivolt reads |
| `pwm` | LEDC/PWM attach/write/detach |
| `network` | HTTP GET/POST |
| `wifi_control` | WiFi connect/disconnect, channel control, raw TX helpers |
| `power` | Battery percentage/voltage/charging state |
| `input` | Button-state snapshot |
| `display` | Backlight brightness get/set |
| `time` | Microsecond uptime and delay helpers |
| `random` | Hardware random helpers |
| `system` | Reboot and privileged system controls |
| `camera` | Reserved for camera APIs |
| `usb` | Reserved for USB APIs |
| `ethernet` | Reserved for Ethernet APIs |
| `audio` | Reserved for microphone/audio APIs |
| `settings` | Limited settings read/write/save APIs |
| `zigbee` | Reserved for IEEE 802.15.4/Zigbee APIs |
| `espnow` | Nearby ESP-NOW discovery and messaging |

## Entry Point

Every app exports one function:

```c
const ghostesp_app_t *ghostesp_app_init(const ghostesp_api_t *api);
```

Use the convenience macro:

```c
#include "ghostesp_plugin_api.h"

static void my_start(void) { /* setup */ }
static void my_stop(void)  { /* cleanup */ }

GHOSTESP_APP_DEFINE("my_app", "My App", my_start, my_stop, NULL, NULL)
```

Set `api_version = GHOSTESP_APP_API_VERSION` and `struct_size = GHOSTESP_APP_STRUCT_SIZE_V1`. Future v1-compatible additions are append-only; the host reads `struct_size` to detect available fields.

## UI API

Apps with `ui` permission can build GhostESP-styled screens using opaque handles:

```c
ghostesp_ui_obj_t root = api->ui_screen_create("My App");
api->ui_label_create(root, "Ready");
api->ui_button_create(root, "Run", on_run_clicked, NULL);
api->ui_show_popup("My App", "Done");
```

The UI layer provides screens, cards, labels, buttons, popups, detail views, options menus, scan status overlays, canvas drawing, RGB565 framebuffer blits with nearest-neighbor scaling, arc/line widgets, animations, paged menus, input dialogs, flex layout, and theme color access. See the SDK header for the full surface.

The stable UI layer is intentionally decoupled from GhostESP's internal view code — apps stay source-compatible as firmware views change.

## Capability Helpers

Apps can call `api->has_permission("wifi")` or `api->has_feature("touchscreen")` to gracefully hide unavailable actions. Useful feature names include `touchscreen`, `compact_screen`, `absolute_storage`, `subghz`, `nrf24`, `camera`, `usb`, `badusb`, `ir`, and `ble`.

## Storage

| Scope | Path | Functions |
|-------|------|-----------|
| `"app"` (default) | `/mnt/ghostesp/appdata/<app_id>/` | `app_storage_*` |
| `"ghostesp"` | `/mnt/ghostesp/...` | `storage_*` |

App-scoped storage uses `api->app_storage_read/write/list/...`. The firmware auto-creates the appdata directory on discovery.

## Memory & Limits

`api->app_malloc` / `api->app_calloc` / `api->app_free` are tracked against the `memory_limit` in the manifest. Query usage with `api->app_memory_used()` and `api->app_memory_limit()`. Raw `malloc`/`free` remain available for C compatibility but are not tracked.

## App State

State is persisted to `/mnt/ghostesp/appdata/<app_id>/.state.json`:

```json
{
  "launch_failure_count": 0,
  "quarantined": false,
  "launch_pending": false,
  "last_error": ""
}
```

Apps that crash or fail to load keep a failure count and last error for diagnostics. Apps are not blocked from launching automatically; `apps reset <id>` clears the diagnostic state. Clean exits (normal `on_stop` -> `dlclose`) reset the count to zero.

## Build Targets

Xtensa and RISC-V app binaries are not interchangeable. Build one `.so` per target:

| Target | Architecture |
|--------|-------------|
| `esp32` | Xtensa LX6 |
| `esp32s2` | Xtensa LX7 |
| `esp32s3` | Xtensa LX7 |
| `esp32c3` | RISC-V |
| `esp32c5` | RISC-V |
| `esp32c6` | RISC-V |
| `esp32c61` | RISC-V |
| `esp32p4` | RISC-V |

## .gapp Archive Format

Custom streaming archive (not ZIP). Header: 4-byte magic `GAPP`, version, flags, file count. Each file entry: `FILE` magic, compression method (0=store, 1=raw-deflate), path, sizes, FNV-1a 64-bit checksum, then payload. Firmware extracts `.gapp` files into `/mnt/ghostesp/app_cache/` because `elf_loader` needs a real `.so` file path for `dlopen()`.

Drop `.gapp` files into `/mnt/ghostesp/apps/`, then reboot the device. GhostESP discovers new packages during startup, extracts them into cache, and registers the app.

## Quick Start

```powershell
# Scaffold
python plugins/tools/new_app.py my_tool --name "My Tool"

# Build
python plugins/tools/build_app.py plugins/examples/my_tool --target esp32s3

# Package (folder)
python plugins/tools/package_app.py plugins/examples/my_tool

# Package (.gapp archive)
python plugins/tools/package_app.py plugins/examples/my_tool --gapp
```

Copy `manifest.json` and the `.so` to `/mnt/ghostesp/apps/<id>/` on the SD card, or drop the `.gapp` into `/mnt/ghostesp/apps/` and reboot.

## GBT (Ghost Build Tool)

A full CLI toolchain is available at `plugins/tools/ghostbt/`:

```powershell
cd plugins/tools/ghostbt
pip install -e .
```

| Command | Description |
|---------|-------------|
| `gbt create <id>` | Scaffold a new app |
| `gbt build <dir>` | Build with ESP-IDF |
| `gbt package <dir> --gapp` | Package folder or .gapp |
| `gbt dist <dir> --gapp` | Build + package in one step |
| `gbt setup` | Install ESP-IDF toolchain |
| `gbt boards` | List board configs |
| `gbt firmware <board>` | Build firmware |
| `gbt flash firmware --board <board>` | Flash to device |
| `gbt monitor` | Serial monitor |
| `gbt ports` | List serial ports |

Full docs: `docs/hugo docs/content/latest/development/gbt.md`

## CLI Commands (on-device)

| Command | Description |
|---------|-------------|
| `apps list` | List discovered SD apps |
| `apps reload` | Rescan SD for new/removed apps |
| `apps info <id>` | Show manifest details and state |
| `apps run <id>` | Launch an app |
| `apps stop` | Stop the running app |
| `apps reset <id>` | Clear failure diagnostic state |
