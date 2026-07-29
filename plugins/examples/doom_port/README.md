# Doom Port

This native SD app embeds the GPL DoomGeneric engine and adapts its video,
input, timing, and WAD loading platform layer to GhostESP. It includes no game
data.

## Responsive Rendering

The engine keeps its usual 320x200 render buffer. The app centers a direct-sized
viewport when the display can accommodate it, avoiding an extra per-pixel scale
on the 320x240 target. Smaller displays use nearest-neighbor clipping/scaling.
Low-detail rendering halves the expensive world-rendering columns while keeping
the HUD and menus at native resolution.

## Included Game Data

The `.gapp` contains Freedoom Phase 1, a freely licensed Doom-compatible IWAD,
split into installer-safe package assets. The app reads the parts as one virtual
WAD directly from its assets, so installation needs no separate WAD. Its GPL
license is included as `assets/COPYING.txt`.

The app starts DoomGeneric at a fixed 320x200 render resolution. It converts
the Doom palette to RGB565 and transfers the completed frame through the native
canvas blit API.

On boards where the display and SD card share one SPI host, Doom reads WAD
lumps through offset-based asset storage calls. The firmware briefly hands the
bus to SD only on cache misses, then restores the display for rendering.

The manifest requests a 28 ms tick interval, targeting Doom's native 35 Hz game tic.

This app declares `requires_features: ["joystick"]`, so GhostESP will reject
launch on encoder-only hardware.

## Controls

- D-pad or encoder: movement and turning
- Select: fire; hold 0.6 seconds to use/open, or 2 seconds to exit
- Back: Doom menu / escape
- Keyboard: Doom's normal keyboard controls

The Doom engine is GPL. Do not package commercial WAD data without a license.

## Build

```powershell
python plugins/tools/build_app.py plugins/examples/doom_port --target esp32c5
python plugins/tools/package_app.py plugins/examples/doom_port --gapp
```

Use the matching target for the flashed firmware. The app requires a PSRAM-capable
native-app board and firmware containing the RGB565 canvas-blit API. DoomGeneric
is an upstream submodule pinned by this repository; its license is included in
that directory.
