# GB Emulator

This native SD app embeds the GPL-2.0 [gnuboy](https://github.com/ducalex/retro-go)
core (as modernized by retro-go) and adapts its video, input, timing, cartridge
storage and save states to GhostESP. It emulates the original Game Boy (DMG)
**and the Game Boy Color (CGB)**, and includes no game data — you provide your
own legally-obtained ROMs.

## ROMs, saves and states

Copy `.gb`/`.gbc` files to the app's SD folder and pick one from the in-app
list:

```
/mnt/ghostesp/appdata/gb_emulator/roms/     your .gb / .gbc files
/mnt/ghostesp/appdata/gb_emulator/saves/    battery saves (<rom>.sav), automatic
/mnt/ghostesp/appdata/gb_emulator/states/   save states (<rom>.st0), via pause menu
```

The whole ROM image is loaded into PSRAM (32 KB–8 MB), so the SD card is never
touched mid-game. Battery saves are flushed every 30 s when changed, on pause
and on exit. **Save states** are a single slot per game: pause → *Save state* /
*Load state* (the load option is only meaningful once a state exists).
Cartridges with a real-time clock (Pokémon Gold/Silver/Crystal) are seeded
from the device clock and persist their RTC with the save file.

### Supported games

- **Original Game Boy (DMG)** — full support, colorized by default the same
  way a real Game Boy Color colorizes DMG cartridges.
- **Game Boy Color (CGB)** — full support: color, palettes, VRAM/WRAM banking,
  double-speed mode, HDMA.
- **Dual-mode cartridges** (black carts) run in CGB mode, as on real hardware.
- **Cartridge types with working banking**: MBC1 (most early games), MBC2,
  MBC3 + RTC (Pokémon Gold/Silver/Crystal clocks), MBC5 (most later games,
  up to 8 MB) and plain ROM-only carts (types 0x00).
- **Partial**: HuC1/HuC3 share a best-effort path in the core; HuC3's
  RTC/IR hardware is not emulated. Very rare MBC-less RAM carts (types 0x08/
  0x09) boot, but the core leaves their save RAM inaccessible.
- **Not supported**: link cable (no trading/battling), MBC7 tilt cartridges
  (Kirby Tilt 'n' Tumble loads but its accelerometer does nothing), Game Boy
  Camera, TAMA5 (unmapped cartridge type), and MMM01/MBC6 carts (detected but
  no banking logic exists in the core, so they misbehave). Rumble output has
  no haptic to drive.
- **File rules enforced by the app**: `.gb`/`.gbc` extension, at least 512
  bytes, at most 8 MB, and the file size must match the size declared in the
  ROM header (truncated dumps are rejected, overdumps are clamped). GBS music
  files and other non-ROM files are rejected.
- **Accuracy**: gnuboy is a mature, widely used core (retro-go ships it for
  GB/GBC on ESP32-class hardware) but it is not cycle-accurate; a small number
  of timing-sensitive titles can show glitches.

## Colors

CGB games always use their hardware colors. DMG games get a colorization
palette, selectable in the pause menu and persisted:

- **GBC auto** (default) — the exact palette the real Game Boy Color boot ROM
  picks for that cartridge, where known
- **DMG green**, **Pocket grey**, **Light grey**, **SGB**

## Responsive rendering

The core renders a fixed 160x144 LCD into a PSRAM RGB565 frame buffer; the
frame is handed to the host's nearest-neighbour `ui_canvas_blit_rgb565` at a
per-device destination rectangle, so the same binary scales from a 320x240
handheld to a 1024x600 panel:

| Screen | Layout |
|---|---|
| 320x240 (no touch) | "Fit" 266x240 (default), pixel-perfect 160x144 |
| 240x320 portrait touch | game above a bottom control band |
| 480x320 | integer 2x (320x288) |
| 800x480 / 1024x600 touch | game centred, virtual controls in the side margins |

Three scale modes (pixel perfect / fit / fill) are cycled from the pause menu
and persisted. On boards where profiling showed a benefit (Banshee-class C5)
the frame is double buffered and presented through the async blit, so
emulation never waits on the display. When a tick runs several catch-up
frames, only the last frame is converted to RGB565.

## Input

The app merges physical buttons, keyboard keys and touch on its game task
using atomic latches with press-latch bits, so a tap that lands entirely
between two 16 ms ticks still registers as held for a full frame.

The ROM picker lists your games, then **Controls & info** (an About-style page
with the full controls table, ROM/save/state locations and version details),
then **Refresh list**. The pause menu's Controls item repeats the table
in-game.

| Action | Joystick / d-pad | Keyboard | Touch |
|---|---|---|---|
| Move | D-pad | arrows, WASD | chevron d-pad (drag to steer) |
| A | Select tap | Z, comma, space | A circle |
| B | Select hold (≥0.6 s) | X, period | B circle |
| Start | Back tap | Enter | Start pill |
| Select | Back hold (≥0.6 s) | Backspace | Select pill |
| **Menu** | Back 2 s, or Select + Down (≥0.8 s) | Esc | pause icon (top corner) |
| Exit app | menu → Exit (saves); hold Select ≥4 s (firmware) | Esc in the picker | — |

**Back is forwarded to the app on firmware with the `forward_back` manifest
API** (this build declares it), so Back behaves like native `go_back()`: tap =
Start, hold = Game Boy Select, 2 s = menu. On older firmware Back is
intercepted by the runner and exits the app instead — Select+Down is the
fallback menu gesture there, and the manifest flag is simply ignored.

Encoder-only boards are intentionally excluded: the manifest requires a
joystick/D-pad (`requires_features: ["joystick"]`), so the firmware refuses to
install the app there with an "app requires D-pad" message. A Game Boy needs
8 buttons plus a direction pad, which an encoder cannot express. On boards
that happen to combine a joystick with an encoder, encoder rotation arrives
as press-only LEFT/RIGHT pulses; the app treats those as one-frame taps (they
also move menu selection) so a direction can never stick held.

Touch panels use a single touch point, so one control is active at a time
(matching the Doom port's P4 policy), and the firmware's reserved edge strip
is left clear: no control is placed inside it and dragging into it releases
the d-pad.

The pause menu offers Continue, Save state, Load state, Reset, Scaling,
Palette, Controls and Exit. Scaling and palette choices persist via NVS.

## Timing and performance

The manifest requests a 16 ms tick. Each tick runs the number of whole GB
frames owed (70224 dots at 4194304 Hz = 16.743 ms per frame), capped at 4
frames per tick so a slow frame never spirals. gnuboy is a more accurate
emulator than the DMG-only core this app started with, so it costs more CPU:
S3 and P4 targets have ample headroom (including CGB double-speed scenes),
while the single-core C5 may show occasional dips that the frame-skip path
absorbs. A 5 s perf log (`fps`, average emulation and present times) goes to
the serial log, mirroring the Doom port.

## Memory budget

| Region | Size | Placement |
|---|---|---|
| gnuboy core state (WRAM 32 KB, VRAM 16 KB, OAM/IO/palettes, CPU/APU) | ~50 KB | strict PSRAM |
| Cartridge RAM (battery save) | 512 B–128 KB | strict PSRAM |
| 2x RGB565 frame buffers | 92 KB | strict PSRAM |
| ROM image | 32 KB–8 MB | strict PSRAM, app-owned |
| Save-state scratch (transient) | ~4 KB + blocks | stack/heap during save |
| App statics (ROM list, state) | ~29 KB | app ELF |

All core buffers are allocated once at load through strict PSRAM and freed at
exit; nothing is allocated per frame, and math is integer-only (the app may
not import 64-bit division helpers beyond the allowed `__divdi3`).

## Vendored core patches

`main/gnuboy/` is a vendored copy of retro-go's gnuboy. Search
"GHOSTESP VENDOR PATCH" for the full set:

1. File-based BIOS/ROM loading removed (the app always supplies a preloaded
   PSRAM image); this also drops `rand()`/`feof()`/`abort()` imports that are
   not on the firmware's symbol whitelist for native SD apps.
2. `memcmp` replaced with a local helper (same whitelist reason).
3. All core allocations routed through `gnuboy_port_*` hooks implemented in
   `gb_core.c`, so cartridge and work RAM always land in PSRAM.
4. `gb_hw_updatemap` falls back to bank 0 instead of streaming banks from SD.
5. Integer audio-rate math (upstream used `double`, which pulled in
   `__adddf3`/`__divdf3`/`__fixdfsi`/`__floatsidf`; none are on the firmware's
   import whitelist). A zero samplerate disables synthesis entirely.

## Audio

The host API exposes no audio output for apps (microphone capture only), so
this version is silent. gnuboy's APU can be wired to a future firmware
`audio_out_*` API without app architecture changes.

## Build

```powershell
python plugins/tools/build_app.py plugins/examples/gb_emulator --target esp32s3
python plugins/tools/package_app.py plugins/examples/gb_emulator --gapp
```

For the large P4 panels, build and package with the P4 manifest (native ELF
binaries are architecture-specific):

```powershell
python plugins/tools/build_app.py plugins/examples/gb_emulator --target esp32p4
python plugins/tools/package_app.py plugins/examples/gb_emulator --manifest manifest.p4.json --gapp
```

Requires a PSRAM-capable native-app board and firmware with the RGB565
canvas-blit API. Host regression tests for layout, input and button mapping:

```powershell
cmake -S tests/gb_layout -B build-gb-layout-tests -G Ninja
cmake --build build-gb-layout-tests
ctest --test-dir build-gb-layout-tests --output-on-failure
```

## Licensing

gnuboy is GPL-2.0 (see `assets/COPYING.gnuboy.txt` and
`assets/CREDITS.gnuboy.txt`). Because the app links this core, the distributed
app package as a whole is subject to the GPL; the complete corresponding
source is this directory in the GhostESP repository. Do not distribute
copyrighted ROMs.
