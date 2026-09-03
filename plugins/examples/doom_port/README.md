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

The large P4 build centers a viewport up to 640x400 below the status bar, with
aspect-correct nearest-neighbor scaling. Direction arrows and fire/use icons
sit in the black margins, outside the game. The 800x480 panel uses a 480x300
viewport to reserve usable side controls; 1024x600 retains 640x400. The controls
are drawn once into a full-content canvas, then only the game subrectangle is
updated each frame. P4 firmware scales without per-pixel 64-bit division and
reuses repeated rows; PPA still handles eligible LVGL fills/blends, not Doom's
software renderer. Actual device FPS depends on the scene and panel. The P4 package
is separate because native ELF binaries are architecture-specific; the existing
C5/S3 manifest and controls are unchanged.

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

The standard package declares `requires_features: ["joystick"]`. The P4 package
declares `requires_features: ["touchscreen"]` and does not require a joystick.

## Controls

- D-pad or encoder: movement and turning
- Select: fire; hold 0.6 seconds to use/open, or 2 seconds to exit
- Back: Doom menu / escape
- Keyboard: Doom's normal keyboard controls

On P4: touch the margin arrows to move/turn, the red crosshair to
fire/start/confirm, and the door icon above it to use/open. Holding virtual fire
does not invoke the physical Select button's use/exit shortcuts. The current
native input API supplies one touch point, so these are single-finger controls.
The left edge remains reserved for the firmware's joystick-left gesture.

P4 requires the firmware change that forwards the whole touch
sequence to native apps over a non-scrolling canvas. Older firmware can consume
the release as a click on the canvas's parent; updating the GAPP alone does not
fix that. The app merges latched touch buttons with physical input on its game
task, so joystick snapshots cannot cancel touch holds and taps between frames
are retained.

For 0.1.6, also update the P4 firmware for the faster scaler and safe Home-swipe
shutdown. Home lets the native app finish its current tick while LVGL continues
processing pending blits, then uses the normal exit cleanup. Physical joystick
hold and other exit methods are unchanged. The Home fix and scaler are firmware
changes: installing the GAPP alone cannot update them.

Host regression checks (using the vendored LVGL and actual app handlers):

```powershell
cmake -S tests/doom_p4 -B build-doom-p4-tests -G Ninja
cmake --build build-doom-p4-tests
ctest --test-dir build-doom-p4-tests --output-on-failure
```

The Doom engine is GPL. Do not package commercial WAD data without a license.

## Build

```powershell
python plugins/tools/build_app.py plugins/examples/doom_port --target esp32c5
python plugins/tools/package_app.py plugins/examples/doom_port --gapp
```

For the large P4 display, build and package with the P4 manifest:

```powershell
python plugins/tools/build_app.py plugins/examples/doom_port --target esp32p4
python plugins/tools/package_app.py plugins/examples/doom_port --manifest manifest.p4.json --gapp
```

Use the matching target for the flashed firmware. The app requires a PSRAM-capable
native-app board and firmware containing the RGB565 canvas-blit API. DoomGeneric
is an upstream submodule pinned by this repository; its license is included in
that directory.
