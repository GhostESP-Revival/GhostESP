---
title: "GBT (Ghost Build Tool)"
description: "Build, package, and flash native SD apps and GhostESP firmware with the Ghost Build Tool."
weight: 35
toc: true
---

`gbt` is the CLI toolchain for GhostESP native SD app development. It can scaffold projects, build app `.so` files with ESP-IDF, package `.gapp` archives, build & flash firmware, and manage serial devices.

## Installation

GBT is a Python package located at `plugins/tools/ghostbt/` in the repo.

```powershell
cd plugins/tools/ghostbt
pip install -e .
```

This installs `gbt` (and `ghostbt`) as a command on your PATH. Both names invoke the same tool.

### Requirements

- Python 3.8+
- Git (for `gbt setup`)
- ESP-IDF or `gbt setup` to install it automatically

## Subcommands

| Command | Description |
|---------|-------------|
| `gbt create` | Scaffold a new app from template |
| `gbt build` | Build an app `.so` with ESP-IDF |
| `gbt package` | Package a built app (folder or `.gapp`) |
| `gbt dist` | Build + package in one step |
| `gbt asset` | Convert images and build SD-ready asset packs (`.gtheme`) |
| `gbt setup` | Install/configure ESP-IDF toolchain |
| `gbt boards` | List available firmware board configs |
| `gbt firmware` | Build GhostESP firmware for a board |
| `gbt flash` | Flash firmware or app to device |
| `gbt monitor` | Open serial monitor |
| `gbt ports` | List available serial ports |

## Environment

Run from inside the GhostESP repo (auto-detected via `plugins/`, `main/`, `components/` markers). GBT resolves the SDK header, templates, and board configs relative to the repo root.

Set these environment variables for custom paths:

| Variable | Overrides |
|----------|-----------|
| `GHOSTBT_ROOT` | Repo root path |
| `GHOSTBT_SDK` | Path to `ghostesp_plugin_api.h` |
| `IDF_PATH` | ESP-IDF installation path |

### Configuration File

GBT stores discovered ESP-IDF paths in `~/.ghostbt/config.json`. This file is maintained automatically by `gbt setup` and read on each invocation.

---

## `gbt create` — Scaffold an App

```
gbt create <app_id> [--name "Display Name"] [--template basic_app] [--out .]
```

Creates a new app project from `plugins/templates/basic_app/`. Placeholders `{{APP_ID}}`, `{{APP_NAME}}`, and `{{APP_SYMBOL}}` are substituted in all source files.

The `app_id` may only contain letters, numbers, `_` and `-`.

```powershell
gbt create my_scanner --name "WiFi Scanner"
# Creates: ./my_scanner/ with CMakeLists.txt, manifest.json, main/*.c
```

The template includes:
- `CMakeLists.txt` — references the repo's `elf_loader` component
- `main/{{APP_SYMBOL}}.c` — minimal app with `GHOSTESP_APP_DEFINE`
- `manifest.json` — filled with placeholder values
- `sdkconfig.defaults` — enables ELF loader support

---

## `gbt build` — Build an App

```
gbt build [app_dir] [--target esp32s3] [--skip-set-target]
```

Runs `idf.py set-target <target>` then `idf.py build` in the app directory. Default target is read from `manifest.json`, falling back to `esp32s3`.

```powershell
gbt build ./my_scanner --target esp32s3
# Output: .../my_scanner/build/ghostesp_my_scanner.so
```

Use `--skip-set-target` to skip the `idf.py set-target` step if already configured.

---

## `gbt package` — Package an App

```
gbt package [app_dir] [--out dist] [--gapp]
```

Copies the built `.so`, `manifest.json`, icons, and assets into `dist/<app_id>/`. Auto-converts PNG icon sources (`icon_source` in manifest) to raw RGB565 or RGB565A8 at the dimensions specified by `icon_width`/`icon_height`.

Generates `checksums.json` with FNV-1a 64-bit hashes for every file.

With `--gapp`, also creates a compressed `.gapp` archive:

```powershell
gbt package ./my_scanner --gapp
# Output: ./my_scanner/dist/my_scanner-1.0.0-esp32s3.gapp
```

Copy the `.gapp` to `/mnt/ghostesp/apps/` or `/mnt/ghostesp/packages/` on your SD card.

---

## `gbt dist` — Build + Package

```
gbt dist [app_dir] [--target esp32s3] [--out dist] [--gapp]
```

Runs `build` then `package` in sequence. Equivalent to:

```powershell
gbt build ./my_scanner --target esp32s3
gbt package ./my_scanner --gapp
```

---

## `gbt asset` — Build Asset Images and Packs

```powershell
gbt asset image ./icon.png --out ./icon.gimg --width 64 --height 64 --format rgb565a8
gbt asset pack ./my_pack --out ./dist --archive
```

`gbt asset image` converts a single PNG to GhostESP's `.gimg` runtime format. `--format` accepts `rgb565`, `rgb565a8` (default), or `indexed_4bpp` — the last quantizes the source to a 16-color palette and packs 4-bit pixels, ideal for internal-only devices where RAM is tight. `gbt asset pack` reads an asset-pack manifest, converts icon/background sources, writes an SD-ready folder, and optionally creates a `.gtheme` archive. Set the pack-wide icon format with the `icon_format` field in the source manifest, or override per-background via the `format` field on each `background_sources` entry.

For backgrounds, a source named `background` with `"variants": true` generates the default runtime set: `full` (`240x320 rgb565`), `half` (`120x160 indexed_4bpp`), `tiny` (`80x107 indexed_4bpp`), and `tile` (`32x32 indexed_4bpp`). Firmware prefers `full` on PSRAM boards and scaled `half`/`tiny` variants on internal-only boards before falling back to the tile.

---

## `gbt setup` — Install ESP-IDF

```
gbt setup [--target esp32s3 esp32c6 ...] [--idf-version v6.0.1] [--install-dir ~/esp-idf]
```

If `idf.py` or `$IDF_PATH` is already available, GBT saves the path and exits. Otherwise, clones ESP-IDF from GitHub and runs the installer:

```powershell
gbt setup
# Detects existing ESP-IDF, or clones v6.0.1 to ~/.ghostbt/esp-idf
# Runs install.bat/install.sh with requested targets
```

Supported setup targets include firmware targets such as `esp32c3`. Native SD `.gapp` app builds currently support `esp32`, `esp32s2`, `esp32s3`, `esp32c5`, `esp32c6`, `esp32c61`, and `esp32p4`; `esp32c3` is not supported for `.gapp` shared-object apps.

---

## `gbt boards` — List Board Configs

```
gbt boards
```

Scans `configs/sdkconfig.*` files in the repo and prints each board ID with its detected target chip:

```
Board                          Target
------------------------------------------
cardputer                      esp32s3
cardputer_adv                  esp32s3
ghostboard_esp32s3             esp32s3
...
```

---

## `gbt firmware` — Build Firmware

```
gbt firmware <board> [--repo /path/to/repo] [--skip-set-target]
```

Builds GhostESP firmware for a specific board config. Copies the board's `sdkconfig.*` to `sdkconfig.defaults`, runs `idf.py set-target` then `idf.py build`.

```powershell
gbt firmware cardputer
# Builds firmware in ./build/
```

Use `gbt boards` to see all available board IDs.

---

## `gbt flash` — Flash to Device

```
gbt flash [firmware|app] [options]
```

### Flash firmware

```
gbt flash firmware [--port COM3] [--baud 460800] [--board cardputer] [--verify] [--erase] [--monitor]
```

Auto-detects the serial port if only one is connected. If the firmware isn't built yet, `--board` triggers a build first.

```powershell
gbt flash firmware --board cardputer -m
# Builds (if needed), flashes, then opens monitor
```

### Flash app (SD card instructions)

```
gbt flash app --app-dir ./my_scanner [--port COM3]
```

Prints instructions for loading the app via SD card. Apps are not flashed directly — they're loaded from SD at runtime.

---

## `gbt monitor` — Serial Monitor

```
gbt monitor [--port COM3] [--baud 115200]
```

Opens the IDF serial monitor. If a firmware `.elf` exists in `build/`, it passes it to `idf_monitor.py` for address decoding.

Press `Ctrl+]` to exit.

---

## `gbt ports` — List Serial Ports

```
gbt ports
```

```
Port                 Description
------------------------------------------------------------
COM3                 USB Serial Device (COM3)
```

Uses `pyserial` if available, falling back to ESP-IDF's `serial.tools.list_ports`.

---

## Standalone Scripts

The repo also includes standalone Python scripts for simple workflows (no GBT install required):

| Script | Equivalent |
|--------|------------|
| `plugins/tools/new_app.py` | `gbt create` |
| `plugins/tools/build_app.py` | `gbt build` |
| `plugins/tools/package_app.py` | `gbt package` |

```powershell
# Quick start without installing GBT:
python plugins/tools/new_app.py my_tool --name "My Tool"
python plugins/tools/build_app.py plugins/examples/my_tool --target esp32s3
python plugins/tools/package_app.py plugins/examples/my_tool --gapp
```

---

## Full Workflow Example

```powershell
# 1. One-time setup
gbt setup

# 2. Create app
gbt create wifi_scanner --name "WiFi Scanner"

# 3. Build, package, and create .gapp
gbt dist ./wifi_scanner --gapp

# 4. Copy .gapp to SD card
#    wifi_scanner/dist/wifi_scanner-1.0.0-esp32s3.gapp → /mnt/ghostesp/apps/

# 5. Optional: build and flash firmware for your board
gbt firmware cardputer
gbt flash firmware --board cardputer -m
```
