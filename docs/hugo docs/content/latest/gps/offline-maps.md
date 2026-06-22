---
title: "Offline maps (SD tiles)"
description: "View cached Slippy map tiles from the SD card in the Maps app."
weight: 8
---

Display builds can show **raster map tiles** loaded from the SD card. This is separate from wardriving CSV uploads to WiGLE: it is a local tile viewer for map imagery you copy onto the card.

## Open the viewer

1. Insert an SD card with a compatible tile tree (see below).
2. On the device: **Apps** (app gallery) → **Maps**.

If the SD card is missing, not mounted, or no tiles are found under the active style path, the screen explains the error.

## Map styles (folders under `maps/`)

Each **style** is a subfolder of `maps/` on the SD card that contains its own **`tiles/`** directory (Slippy layout inside that folder). Style names are the folder basenames (for example `satellite`, `streets`).

- Paths look like: `ghostesp/maps/<style>/tiles/{z}/{x}/{y}.<ext>` on the card (mounted as `/mnt/ghostesp/...` on device).
- Styles are discovered at runtime, **sorted alphabetically** by folder name.
- **Cycle style**: use the on-map **style control** (touch where supported), keyboard **`M`**, or **encoder button press** (turning the encoder zooms in or out instead).

Example for a style named `satellite`:

```text
/mnt/ghostesp/maps/satellite/tiles/{z}/{x}/{y}.<ext>
```

## Tile layout

Tiles use the usual **Slippy map** / XYZ layout (same idea as `tdeck-meshtastic_tiles` and many desktop tools):

```text
/mnt/ghostesp/maps/<style>/tiles/{z}/{x}/{y}.<ext>
```

Each zoom level must be a **numeric directory** `z`, with **numeric** `x` column directories under it, and tile files named by tile **`y`** (see extensions below). Layouts that omit the `x/` level cannot be scanned for zoom extent or focused the same way.

### File extensions

For each cell the viewer tries, in order:

1. `{y}.png`
2. `{y}.jpg`
3. `{y}.jpeg`

**PNG** tiles are passed to LVGL as compressed PNG in RAM (decoded by the PNG decoder). **JPEG** tiles are handled differently:

- With **16-bit color** (`LV_COLOR_DEPTH` 16), JPEG bytes are decoded with **stb_image** to uncompressed **RGB565** in RAM, then shown as a normal true-color image. This supports **baseline and progressive** JPEG.
- If that decode fails, the firmware can fall back to LVGL’s **SJPG** path (compressed JPEG in `lv_img_dsc_t`), which is suitable for **baseline** JPEG only.
- Some caches store JPEG data in a file named **`.png`**. The viewer detects JPEG by the file header (`FF D8 FF`) and treats it as JPEG, not PNG.

### Optional `metadata.json`

Place a file next to the zoom folders:

```text
/mnt/ghostesp/maps/<style>/tiles/metadata.json
```

Supported fields (when valid JSON):

| Field | Meaning |
|-------|--------|
| `minzoom`, `maxzoom` | Optional hints from many export tools |
| `bounds` | Array `[west, south, east, north]` in degrees; used to pick an initial center tile |

**Zoom range in the viewer:** whenever the firmware can read the SD layout, it **derives allowed zoom from the numeric `z/` directories** under `tiles/` (levels that have at least one numeric `x/` subdirectory). That **on-disk pyramid overrides** `minzoom` / `maxzoom` in `metadata.json`, which are often a small template range (for example 10–12) while the real cache spans a wider band. If the card cannot be read, metadata or built-in defaults apply.

If `metadata.json` is missing, built-in defaults apply and the view can still **auto-focus** on zoom levels that contain tiles (bounded scan so large caches stay responsive).

## Build and runtime requirements

Full **Maps** tile rendering needs LVGL **PNG** support and the **stdio filesystem** driver (same pattern as the rest of GhostESP for `S:` paths), for example:

- `CONFIG_LV_USE_PNG=y`
- `CONFIG_LV_USE_FS_STDIO=y`
- `CONFIG_LV_FS_STDIO_LETTER=83` (ASCII `S` for `S:`)

**JPEG misnamed as `.png`** or raw `.jpg` / `.jpeg` tiles work best with **16-bit color** so the stb decode path is available; **SJPG** (`CONFIG_LV_USE_SJPG=y`) remains useful as a fallback for baseline JPEG when the RGB565 path does not apply.

## Shared SPI / “stream” mode

On boards where the display and SD card share SPI, the UI may **mount the SD only to read tiles**, copy tile bytes into RAM, then **unmount** so the panel keeps working. In that mode you still need real tile files on the card at the paths above; nothing is fetched over the network on device. Opening **Maps**, switching **style**, or refreshing tiles may **JIT-mount** the card briefly to read metadata, discover zoom levels, or reload the 3×3 grid.

## Pan and zoom

Controls depend on the device (touch, keyboard, joystick, encoder). Typical bindings where a keyboard is exposed:

- **`[`** / **`]`** — zoom out / in (map stays centered when possible)
- **Arrow keys**, **`h` `j` `k` `l`**, or related scancodes — pan
- **`M`** — cycle map style
- **Esc**, **`q`**, backtick — leave **Maps** for the app gallery

The viewer keeps a **3×3** grid of tiles, **reuses tiles already in RAM** when you pan so cells that stay on screen avoid extra SD reads, and keeps the map **geographically centered** when the zoom level changes when possible.

## See also

- [GPS features in the Flipper companion](../flipper-app/gps-features) — high-level GPS and mapping features
- [Wardriving](wardriving) — logging networks to CSV (WiGLE workflow)
- [SD card configuration](../getting-started/sd-card-config) — `config.cfg` and SD usage
