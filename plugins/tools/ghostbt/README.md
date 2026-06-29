# gbt — Ghost Build Tool

Build and package [GhostESP](https://github.com/GhostESP-Revival/GhostESP) native SD apps from the command line.

## Install

```bash
pip install ghostbt
```

Requires Python 3.8+ and [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) (install via `gbt setup`).

## Quick Start

```bash
# Set up ESP-IDF (one-time)
gbt setup

# Create a new app
gbt create my_app --name "My App"

# Build and package
gbt dist ./my_app --gapp

# Flash firmware to device
gbt firmware cardputer
gbt flash firmware --board cardputer --monitor
```

## Commands

| Command | Description |
|---------|-------------|
| `gbt create <id>` | Scaffold a new native SD app |
| `gbt build [dir]` | Build app with ESP-IDF |
| `gbt package [dir] --gapp` | Package as folder or `.gapp` archive |
| `gbt dist [dir] --gapp` | Build + package in one step |
| `gbt setup` | Install/configure ESP-IDF toolchain |
| `gbt boards` | List available firmware board configs |
| `gbt firmware <board>` | Build GhostESP firmware |
| `gbt flash firmware` | Flash firmware to device |
| `gbt flash app` | Instructions for loading app via SD card |
| `gbt asset image` | Convert a PNG into a GhostESP `.gimg` asset image |
| `gbt asset pack` | Build an SD-ready asset pack folder or `.gtheme` bundle |
| `gbt monitor` | Serial monitor |
| `gbt ports` | List serial ports |

## Asset Packs

Create a source folder with a `manifest.json` and PNG artwork. `gbt asset pack` converts source images into compact `.gimg` files and writes a runtime manifest for `/mnt/ghostesp/themes/<pack id>/`.

```json
{
  "id": "cyberpack",
  "name": "Cyber Pack",
  "version": 1,
  "colors": {
    "accent": "0xFF00FF",
    "background": "0x0A0A0A",
    "surface": "0x1A0A1A",
    "surface_alt": "0x2A1A2A",
    "text": "0xFFFFFF",
    "text_muted": "0xAA88CC"
  },
  "icon_variants": [32, 64],
  "icon_sources": {
    "wifi": "art/icons/wifi.png",
    "bluetooth": "art/icons/bluetooth.png",
    "settings_icon": "art/icons/settings.png"
  },
  "background_sources": {
    "bg_tile": {
      "source": "art/bg_tile.png",
      "width": 64,
      "height": 64,
      "format": "rgb565"
    }
  }
}
```

Build it:

```bash
gbt asset pack ./cyberpack --out ./dist
gbt asset pack ./cyberpack --out ./dist --archive
```

Install a generated pack folder by copying it to SD as:

```text
/mnt/ghostesp/themes/cyberpack/manifest.json
/mnt/ghostesp/themes/cyberpack/icons/...
```

Or install an archive by copying the `.gtheme` to SD as:

```text
/mnt/ghostesp/themes/cyberpack.gtheme
```

Then use `Settings > Appearance > Asset Pack` and press left/right to cycle installed packs. Firmware scans `/mnt/ghostesp/themes/` for pack folders with `manifest.json` and for `.gtheme` archives. The selected pack is saved in NVS.

When loading a `.gtheme`, firmware streams entries into `/mnt/ghostesp/themes/active/` before loading. New `.gtheme` archives store raw `.gimg` payloads without deflate compression for safe runtime decoding. Theme SD access uses the same short mount/unmount flow as other SD operations; decoded icons and background tiles stay cached in RAM/PSRAM after each read.

### PSRAM vs internal RAM

**PSRAM devices:** Up to 32 icons cached in PSRAM, tiled or scaled backgrounds supported. Background images smaller than the screen tile automatically; larger images scale to fill using LVGL zoom. A 128x128 background will scale up to fill any screen. Background images are always loaded into PSRAM. Any image format works; deflate-compressed payloads decode into PSRAM.

**Internal-only devices:** 2 icons cached in internal RAM. Background tile (≤32x32) supported — LVGL tiles the small image across the screen. Icon size capped at 32x32 RGB565A8; deflate-compressed payloads rejected. Use the `indexed_4bpp` format for icons and the background tile to fit in internal RAM: a 32x32 indexed icon is 576 bytes vs 3,072 bytes for RGB565A8 (~5x smaller). Icons fall back to compiled-in artwork when the cache is full. The pack still loads colors and icon mappings; only the image cache is limited.

### Image formats

| Format | Code | Size at 32x32 | Notes |
|--------|------|---------------|-------|
| `rgb565` | 1 | 2,048 B | No alpha. Best for opaque backgrounds. |
| `rgb565a8` | 2 | 3,072 B | RGB565 + separate alpha plane. Default for icons. |
| `indexed_4bpp` | 3 | 576 B | 16-color palette, packed 4-bit pixels. Ideal for internal-RAM devices; the `gbt` tool quantizes the source PNG at build time. |

Set the pack-wide icon format with `icon_format` in the source manifest, or override per-background via the `format` field on each `background_sources` entry. Indexed payloads are always stored uncompressed.

Convert one image directly:

```bash
# Default — RGB565A8, 3 KB for 32x32
gbt asset image ./wifi.png --out ./wifi_l.gimg --width 64 --height 64 --format rgb565a8

# Internal-RAM friendly — indexed 4bpp, 576 B for 32x32
gbt asset image ./wifi.png --out ./wifi_l.gimg --width 64 --height 64 --format indexed_4bpp
```

The generated pack folder contains `manifest.json`, `checksums.json`, and generated `.gimg` files. Asset packs store raw `.gimg` payloads by default so firmware does not run deflate decompression from small UI task stacks. Missing icons are fine; firmware falls back to built-in artwork for any icon not present in the selected pack. Full-screen background images are loaded into PSRAM only; the smaller tiled background path works on internal-RAM devices at ≤32x32.

## Requirements

- Python 3.8 or later
- ESP-IDF (auto-installed by `gbt setup` if missing)
- Git (for `gbt setup`)

## Links

- [GhostESP GitHub](https://github.com/GhostESP-Revival/GhostESP)
- [Full documentation](https://github.com/GhostESP-Revival/GhostESP/tree/apps/docs/hugo%20docs/content/latest/development/gbt.md)
