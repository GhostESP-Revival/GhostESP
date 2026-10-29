# GhostESP → Attaky port (2.2.2-A001)

Provenance and bring-up notes for the Attaky target added to this tree. The
implementation contract is [../port/plan.md](../port/plan.md); the approved
survey is [../port/survey.md](../port/survey.md).

- **Upstream**: GhostESP `v2.2.2` (GPL-3.0 — this port keeps the upstream
  licence and stays inside upstream's board seams so it can be contributed
  back).
- **Placeholders**: `firmware-name` `GhostESP`, `version` `2.2.2-A001`
  (upstream version normalized to its numeric core + Attaky port counter),
  `build-SKU` `Cyber Deck Plus 1.0`.
- **Toolchain**: **ESP-IDF v6.1**, matching `build.py`'s `ESP_IDF_VERSION`
  and `.github/workflows/compile_all.yml`. Target `esp32s3`, 16 MB flash
  (**qio**, 80 MHz), 8 MB octal PSRAM — pinned in
  `configs/sdkconfig.attaky`. No shared IDF install is mutated.

## Boot banner contract

Emitted by `attaky_board_emit_banner()` from `app_main()`, directly after
`serial_manager_init()` and before any feature init, so a boot that dies
mid-init still shows it:

```
[INFO] [APP] GhostESP v2.2.2-A001 on Cyber Deck Plus 1.0
[INFO] [APP] Build: <date> <time> / toolchain: ESP-IDF
[INFO] [APP] milestone: M<N> (<short name>)
[INFO] [APP] upstream: GhostESP v2.2.2 (GPL-3.0)
```

`CONFIG_ATTAKY_MILESTONE` (profile value 8) selects the milestone line; the
ladder is plan §4. `attaky_board_emit_presence()` follows it with one line
naming every module the probe found, which is the bring-up log to read first
when something is missing.

## Hardware map this port implements

From the attached Cyber Deck Plus 1.0 datasheet (module sections), which the
public hardware catalog agrees with cell for cell:

| Device | Bus / address | Notes |
| --- | --- | --- |
| ST7789 240×320 | SPI2: SCLK `IO11`, MOSI `IO13`, CS `IO45`, DC `IO12`, RST `IO10`, BL `IO14` | write-only, no MISO |
| FT6636 touch | I²C `0x38` | INT on expander `0x58` P04, reset on expander `0x59` P13 |
| AW9523B | I²C `0x58` | interrupt aggregator, all pins inputs |
| AW9523B | I²C `0x59` | buttons + RGB LED + touch reset |
| QMI8658A IMU | I²C `0x6B` | accel + gyro |
| STK3311-X ambient light | I²C `0x48` | present on the Core, unused by this port |
| MAX17048 gauge | I²C `0x36` | Power module |
| microSD | SPI3: CLK `IO5`, MOSI `IO6`, MISO `IO7`, CS `IO8` | Media I/O module |
| NS4168 speaker | BCLK `IO17`, LRCLK `IO21`, SDATA `IO18` | Media I/O module, fixed-wired amp |
| ICS-43434 mic | SCK `IO39`, WS `IO9`, SD `IO16` | Media I/O module |
| AW9523B ×2 | I²C `0x5A` / `0x5B` | Keyboard module, two 5×5 matrices |
| TCA9555 ×2 | I²C `0x22` / `0x26` | Gamepad module |

Core buttons on expander `0x59` P0: Down P00, Left P01, Enter P02, Right P03,
Up P04, L1 P05, R1 P06, POWER P07 (short press only — a long press is the
hardware power switch). P1: RGB status LED P10 red / P11 green / P12 blue,
touch reset P13, all active-low outputs driven high at rest. Boot is a direct
`IO38` input and is not driven by firmware.

Keyboard matrices (rows driven low on P10–P14, columns read on P00–P04, both
zones 5×5, active-low): left zone `0x5A` = `1 2 3 4 5 / Q W E R T / A S D F G
/ Shift Z X C V / Language Tab Alt , Space`; right zone `0x5B` = `6 7 8 9 0 /
Y U I O P / H J K L Delete / B N M — Enter / — . Mic # Fn`, with ROW3×COL3 and
ROW4×COL0 routed but keyless (48 keys total). Gamepad: left `0x22` = Up P05,
Down P02, Left P04, Right P03, L1 P06, L2 P07, FN_L1 P00, FN_L2 P01; right
`0x26` = A P03, B P04, X P02, Y P05, R1 P01, R2 P00, FN_R2 P06, FN_R1 P07.
Per-half RGB LEDs sit on P17 red / P16 green / P15 blue, active-low.

## Files this port touches

See plan §2 for the full list. Short version:

- **New**: `configs/sdkconfig.attaky`, `main/vendor/attaky/` (board, keyboard,
  gamepad), `include/vendor/attaky/`, `main/vendor/drivers/qmi8658.c` +
  `include/vendor/drivers/qmi8658.h`, this file.
- **Edited**: `main/Kconfig.projbuild` (one `Attaky Options` menu plus a
  generic AW9523/QMI8658 selector each), `components/io_manager/io_manager.c`
  (chip-conditional register set, data-driven button map), `main/main.c`
  (banner + board init + audio `#if` chains),  `main/managers/display_manager.c`
  (Control-module init **and the three unguarded `view:badusb` / `view:compass`
  / `view:enviii` menu paths, now feature-guarded**),
  `main/managers/joystick_manager.c` (a negative pin is now "slot not
  present" instead of a GPIO to configure),
  `main/managers/views/accelerometer_screen.c` (QMI8658A branch), three audio
  managers, `build.py` (board table entry).
- **Edited, profile only**: `configs/sdkconfig.attaky` — the Chameleon-over-BLE
  NFC flags above, plus the flash-mode comment rewording.

## Deviations from the plan, and why

1. **`components/lvgl_esp32_drivers/lvgl_touch/ft6x36.c` was left untouched.**
   The plan expected FT6636 tolerance to be added there, but the driver already
   probes non-fatally: `ft6x06_init()` fails only if the chip does not ACK the
   panel-ID read (the FT6636 does), and the chip/firmware IDs are logged, never
   validated. A smaller diff, not a behaviour change.
2. **QMI8658A register values come from the datasheet, not from
   `shared/imu-driver-patterns.md`.** That doc's "enable bit is CTRL1 bit0
   (`ACC_EN`)" and `CTRL2 = 0x54` disagree with the datasheet bit list, where
   `CTRL1` bit0 is **`SensorDisable`** (`0` = internal oscillator running) and
   `CTRL2` bits 6:4 are `aFS` (`000`=±2 g … `011`=±16 g) with `aODR` in bits
   3:0. Mainline Zephyr (`qmi8658a.c`) and ESPHome (`qmi8658.cpp`) both
   implement the datasheet reading, so `qmi8658.c` writes `CTRL1 = 0x40`
   (ADDR_AI only), `CTRL2 = 0x16` (±4 g, 125 Hz), `CTRL3 = 0x56` (±512 dps,
   112 Hz), `CTRL5 = 0x00` (LPF off, as the patterns doc requires for shake/
   transient use) and `CTRL7 = 0x03` (accel + gyro enable). The gain is that
   `CTRL1 = 0x43` would set `SensorDisable` and freeze the data registers. If a
   unit ever reads frozen, `qmi8658_init()` already fails loudly: it reads the
   accelerometer twice and refuses to report success when the values are byte
   identical.
3. **The keyboard pushes `INPUT_TYPE_KEYBOARD` events** onto
   `display_manager`'s shared `input_queue` — the same seam the T-Deck matrix
   path uses — instead of implementing `keyboard_handler.h`. Press goes out
   with `is_touch_move = false`, release with `true`, which is exactly what the
   global `kb_repeat_*` machinery keys on, so key repeat and view handling work
   unchanged. That machinery only ever stops on a release, so the release has to
   be *delivered*: the adaptive-poll path (`kb_presence()`, entered after 3 s of
   typing silence) can only see keys that are down, and a release reads exactly
   like an idle bus. Skipping the full scan there left the cell pressed forever
   and the held key re-injected itself from `display_manager` — one `h` press
   typing a row of `h`, backspace wiping a whole field — until the next key was
   pressed. The scan task now keeps full-scanning while `held_count > 0`, and
   `kb_repeat_start()` is no longer re-armed by the repeat events it produced
   itself (a queued repeat outliving its own release was the same runaway, a few
   lines away in the same file).
4. **The gamepad polls** (15 ms, both halves) rather than consuming each half's
   own `INT#` line: the interrupt lines land on the Core's `0x58` aggregator
   (P14 / P07) and nothing else in the tree consumes aggregator interrupts
   either, so polling keeps the module driver self-contained. With both halves
   present that is two 2-byte reads per tick on a 400 kHz bus. D-pad maps to
   the shared joystick indices (0 left, 1 select, 2 up, 3 right, 4 down); A =
   select, B = Esc (back), X = Tab, Y = Backspace, R1 = Enter. L1/L2/R2 and the
   four FN keys are read and logged at debug level, unmapped until a feature
   claims them.
5. **`main/vendor/drivers/aw9523.c` was not reused.** It is gated on the
   CoreS3 board template and only drives that board's 5 V rail pins; the Attaky
   modules need their own explicit reset/direction/output sequences.
6. **`CONFIG_USE_HW_KB=y` was added to the profile.** It is upstream's generic
   "this device has a physical keyboard" flag: it advertises the `keyboard`
   feature to plugins and switches two views to their hardware-keyboard layout.
   It pulls in no driver.
7. **The accelerometer screen gained a third branch.** Plan §2 listed the
   QMI8658A driver but not its call site; `accel_init_hw()` and
   `accel_timer_cb()` in `accelerometer_screen.c` are the only consumers, so
   the branch (`#elif defined(CONFIG_ACCEL_USE_QMI8658)`) had to be added there
   for the option to do anything. `qmi8658.c` `#error`s if both `BMI270` and
   `QMI8658A` selectors are set, because the screen compiles BMI270 first.
8. **`components/io_manager/io_manager.c` keeps upstream behaviour on every
   other board**: the map table's `#else` branch reproduces the previous
   TCA9535 wiring (D-pad + select on P0, b1–b3 on P1) exactly, and the only
   new behaviour is the AW9523 register set and init sequence behind
   `CONFIG_IO_EXPANDER_CHIP_AW9523`.

## Verification status

**Built and linked clean with ESP-IDF v6.1 (M1 gate closed).**

- ESP-IDF v6.1 is installed locally at `C:\esp\esp-idf-v6.1` with the esp32s3
  toolchain (`install.bat esp32s3` + `export.bat`), matching `build.py`'s
  `ESP_IDF_VERSION`. The build seeded the profile the documented way
  (`cp configs/sdkconfig.attaky sdkconfig.defaults && rm -f sdkconfig`) so it
  exercised the tracked profile and not a stale generated config.
- `idf.py build` → `Project build complete.` (2113/2113 on the first pass,
  2,138/2,138 after the input-config fix below), no errors, and no warnings
  originating in the port's own sources.
- `build/Ghost_ESP_IDF.bin` is 3,607,200 bytes (`0x370aa0`; 3,607,264 before
  the fix) against the 6 MB `ota_0` slot: `0x28f560` bytes / 43 % free.
- `esptool image-info` on both the app image and the merged image reports
  ESP32-S3, flash size 16 MB, 80 MHz, valid checksum and valid validation hash,
  with the ELF SHA-256 recorded for later OTA checks.
- Flashable artifacts, produced with upstream's own offsets and naming
  (`scripts/partition_table.py` + the conventions in `build.py`):
  - `local_builds/Attaky-Cyber-Deck-Plus-merged-gesp.bin` — 3,738,272 bytes
    (`0x390aa0`, flash mode DIO), bootloader @ `0x0`, partition table @
    `0x8000`, `ota_data` @ `0x10000`, app @ `0x20000`; flash it at `0x0` in one
    shot.
  - `local_builds/Attaky Cyber Deck Plus/` + `Attaky-Cyber-Deck-Plus.zip` —
    `bootloader.bin` / `partitions.bin` / `firmware.bin`, the same artifact set
    `build.py` packages for every board.

Two link failures the first build surfaced were fixed at the cause rather than
worked around:

1. **Menu entries with no feature guard.** Inside the IO-expander
   button-command blocks, `display_manager.c` referenced `badusb_view`,
   `compass_view` and `enviii_view` unconditionally. Those three symbols only
   exist when their feature is compiled in, so a build with the features off
   died at link. Each is now guarded exactly like the neighbouring `view:ir` and
   `view:subghz` entries already were — the same latent bug affects any
   IO-expander board without those three apps, so the fix is upstream-worthy as
   is.
2. **Chameleon NFC backend.** `nfc_view.c` compiles on every board and calls
   `chameleon_manager_*`, which only exist when the Chameleon backend is on.
   The Attaky has no local NFC frontend, so the profile now sets
   `CONFIG_NFC_CHAMELEON=y` and `CONFIG_HAS_NFC=y`: the NFC app talks to a
   Chameleon Ultra over BLE via the NimBLE stack the profile already enables.
   Both flags are set by hand because the Kconfig default keys off
   `BT_BLE_ENABLED`, a symbol nothing in this tree defines — every upstream BLE
   board profile sets them the same way.

Static checks from the first pass still hold: every `CONFIG_*` symbol
referenced by the new/changed sources resolves in `main/Kconfig.projbuild` or a
component Kconfig, both keyboard zones were transcribed from the datasheet table
and re-checked cell by cell (48 keys, two keyless positions), all 16 gamepad
buttons plus both LED groups match the datasheet pin table, and the diff was
reviewed hunk by hunk. Existing seams are reused rather than duplicated:
`input_queue` event contract (`is_touch_move` = release), joystick index
meanings, `i2c_shared_*` bus helpers, `i2c_bus_lock`, the audio `#if` chains and
`partitions_ota_16mb.csv`.

**Flashed and run on hardware (M2 partly closed), and the two defects that
first flash surfaced are fixed here.**

What the device showed on this run, and what the code and the generated
configuration confirm about each symptom:

1. **Mirrored image.** The profile's trailing "LVGL component settings" block
   is an alphabetical dump of a whole generated sdkconfig, so it restated
   three Kconfig *choices* after the display and touch sections had already
   made the right call — and for a choice the **last** assignment in the file
   wins. The generated `sdkconfig.h` of the flashed image proves it:
   `CONFIG_LV_TFT_DISPLAY_CONTROLLER_ILI9341=y`, so the panel was driven by
   the ILI9341 driver, whose index-0 MADCTL is `0x48` (mirror-X + BGR) — a
   left-to-right mirror — and `CONFIG_LV_DISPLAY_ORIENTATION=0` (portrait,
   not the landscape this mount needs). The duplicate lines are gone; the
   ST7789 driver and `LANDSCAPE_INVERTED` (index 3) are now the only members
   left.
   The same dump's `CONFIG_LV_TFT_SPI_CLK_DIVIDER_1` (80 MHz, out of spec) was
   removed with it: the profile now runs divider 2, the 40 MHz it always
   documented.
   That index-3 choice was wrong, and the second flash is what proved it — see
   "Display orientation" below.
2. **Dead touch.** The dump also restated `CONFIG_LV_I2C_TOUCH_PORT_1=y`, and
   that is the effective value the flashed image carries. Port 1 has no bus in
   this profile, so `lvgl_i2c_read()` fails on every poll, `ft6x06_init()`
   never marks itself inited, and `touch_driver_read()` reports
   `LV_INDEV_STATE_REL` for ever: the panel was dead from boot, not
   mis-mapped. `LV_I2C_TOUCH_PORT_0` (the shared main bus, SDA `IO2` / SCL
   `IO1`) is now the only member.
3. **Buttons that look held.** The two candidate paths were read end to end in
   the code, and one of them is now unreachable. The D-pad mapping is correct
   (P00 Down, P01 Left, P02 Enter, P03 Right, P04 Up → joystick slots 4, 0, 1,
   3, 2 in `joystick_get_button_state()`), and `io_manager_init()` writes the
   AW9523B's directions the way the driver notes demand — software reset, then
   LEDMODE, then `0x04`/`0x05` = all inputs and `0x02` = all released — so the
   expander is not left driving its own button lines low, which is the classic
   "button stuck pressed" failure for this chip (its POR state on a `0x59` part
   is P0_4-P0_7 driving LOW, i.e. Up / L1 / R1 / POWER). The path that *could*
   hold every D-pad slot down is the GPIO fallback: `L_BTN`…`D_BTN` default to
   `0`, and `main.c` binds those five pins for the five slots whenever
   `io_manager_init()` fails — so **GPIO0**, one pin, stands in for five buttons
   and a pin held or floating low reads as five presses. The profile now sets
   them to `-1` and `joystick_init()` treats a negative pin as "slot absent",
   which closes that path for good. If the buttons still misbehave on the next
   flash, the serial log names the live path in one line: `Joystick: IO
   expander` (intended) versus `IO Expander initialization failed, falling back
   to GPIO mode` (the fallback), next to `IO Manager initialized successfully`
   and the AW9523 ID check.

The rebuild that carries those fixes links clean (2,138/2,138 tasks) and
regenerates the configuration with the corrected values — read back from the
*generated* `sdkconfig.h` rather than taken on trust from the profile:
`CONFIG_LV_TFT_DISPLAY_CONTROLLER_ST7789`, `LV_DISPLAY_ORIENTATION 3`,
`LV_TFT_SPI_CLK_DIVIDER_2`, `LV_I2C_TOUCH_PORT 0` (with
`LV_I2C_TOUCH_PORT_1` now unset), `LV_FT6X36_SWAPXY`/`INVERT_X`,
`L_BTN`…`D_BTN = -1`, `IO_EXPANDER_CHIP_AW9523`. `st7789.c.obj` is built and
`ili9341.c.obj` is not in the build at all. App image `0x370aa0` bytes
(3,607,200) against the 6 MB `ota_0` slot — 43 % free; merged image `0x390aa0`
bytes (3,738,272) at `esptool image-info`: ESP32-S3, 16 MB, 80 MHz, **DIO**,
valid checksum, valid validation hash. DIO is what the profile really gets:
`CONFIG_ESPTOOLPY_FLASHMODE_QIO` is a no-op on this part, because octal PSRAM
takes the flash's IO4-IO7 (`ESPTOOLPY_FLASHMODE="dio"`,
`FLASHMODE_VAL=1`), so the merged image is built with `--flash-mode dio` to
match the bootloader and app headers instead of forcing QIO on top of them.
The image is on the SD card as `GhostESP-2.2.2-A001-merged.bin`, SHA-256
`04222d6485674b2bcb450f2e874189159cae4085c25f2af4c6961075afba4b4e`.

### Display orientation (second flash, and why it is now set on the device)

The second flash put the rebuilt image on the glass and the orientation the
first fix chose (`LANDSCAPE_INVERTED`, i.e. `CONFIG_LV_DISPLAY_ORIENTATION=3`,
whose table entry is MADCTL `0xA0` = swap_xy + mirror_y) came up 90 degrees out
and unreadable. `0xA0` is not an arbitrary guess: it is the byte the vendor's
own bring-up uses for this 180-degree mount (`LCD_SWAP_XY true`,
`LCD_MIRROR_Y true` in their esp-idf bring-up template, "the esp_lcd equivalent
of the TFT_eSPI setRotation(3) the Arduino bring-up uses", agreeing byte for
byte with `setRotation(3) = TFT_MAD_MV | TFT_MAD_MY` in TFT_eSPI's
`ST7789_Rotation.h`). With a 320x240 LVGL frame it still fails here, because
MV addresses the GRAM along the 240px axis while LVGL pushes 320-wide rows —
the failure `ili9341.c` documents for the CoreS3's native-landscape ILI9342C.
Combined with the first flash's evidence (the ILI9341 table's `0x48` = MX|BGR
gave a *readable* landscape frame that was mirrored left to right), the panel
answers natively as 320x240 landscape and the remaining unknowns are only the
mirror and element-order bits — twelve plausible bytes in all.

Guessing one per rebuild costs a build and a flash each time, so the choice is
made on the device:

- `st7789.c` now exposes `st7789_set_madctl()` / `st7789_set_madctl_override()`
  / `st7789_get_madctl()`. `st7789_set_orientation()` still picks from its
table, but an override (from NVS) wins, and a byte can be applied live.
- `main/vendor/attaky/attaky_display_tuner.c` (`CONFIG_ATTAKY_DISPLAY_TUNER`,
  on in this profile) draws an asymmetric test pattern on `lv_layer_top()` —
  four differently coloured corner blocks, magenta top/bottom bars against a
  cyan left bar, and R/G/B swatches for the element order — and walks the
  twelve candidates. With nothing stored it advances by itself every 6 s, so
  the panel can be identified even with no working keys; any D-pad key steps
  early and ENTER stores the byte on screen.
- The stored byte lives in NVS (namespace `attaky`, key `madctl2`). Once it is
  set, later boots start on it and the tuner stays out of the way; hold a D-pad
  key during boot to re-tune. Serial names every step:
  `ATK_ORIENT: candidate N/12 on screen: MADCTL 0xNN (...)`.
- Resetting the screen config: hold ENTER (or a side button) during boot —
  `attaky_display_tuner_reset_saved()` erases the key, clears the driver
  override so the compiled `LANDSCAPE` byte (`0x08`) is used again, and the
  tuner re-arms from candidate 1. The key name carries a generation suffix
  (`madctl2`), and the pre-reset key (`madctl`) is erased on first boot, so
  flashing this image also drops any byte saved by the previous one. For a
  total wipe instead, erase the NVS partition and reflash:
  `python -m esptool --chip esp32s3 erase-region 0x9000 0x7000` (the `nvs`
  row of `partitions_ota_16mb.csv`).
- All LVGL work happens in `display_manager_lvgl_async_call()` callbacks, so
  the MADCTL write can never land in the middle of a flush's
  CASET/RASET/RAMWR sequence.
- The glass on this unit is mounted 180 degrees from the byte that renders the
  UI correctly, so `st7789_set_madctl()` toggles MX and MY (`^= 0xC0`) on every
  byte it sends — default, NVS override and tuner candidate alike
  (`ATTAKY_PANEL_FLIP_180` in `st7789.c`). Doing it at that one choke point
  keeps the tuner's numbering and the stored byte meaning the same thing they
  did on the glass. It is a *display-side* transform only: an intermediate build
  mirrored both touch axes in `ft6x36.c` to "follow" it, and touch came up
  inverted on both axes on the glass — every tap landing diagonally opposite —
  so that mirror is gone and plain `SWAPXY` + `INVERT_X` is the
  measured-correct mapping. The tuner overlay prints the driver's own point
  (`touch: DOWN x=.. y=..`) plus a red dot where it lands, so a dead or
  mirrored touch panel is visible without serial.

The profile's default is now `LANDSCAPE` (index 2) → `0x08` (MV clear, BGR,
no mirror): the byte that follows from the two hardware readings above. The
tuner is what turns that from an inference into a measurement.

The first build of it links clean (2,139 tasks, no warnings from the port's own
sources; `attaky_display_tuner.c` is in the glob only because the profile build
regenerates the CMake file list). App image 3,611,008 bytes (`0x371980`)
against the 6 MB `ota_0` slot — `scripts/partition_table.py check-fit` reports
2,680,448 bytes (42 %) free (it wants the app image plus the *binary* table,
`build/partition_table/partition-table.bin`, not the CSV). Merged image
3,742,080 bytes (`0x391980`) with `esptool image-info`: ESP32-S3, 16 MB, 80 MHz,
**DIO**, valid checksum, valid validation hash. On the SD card as
`GhostESP-2.2.2-A001-merged.bin`, SHA-256
`dd0e0e5ead34379316be1143ccd19618f17543c7a25220ba9c745f0a0de30e53` — the copy
is byte-identical to `local_builds/Attaky-Cyber-Deck-Plus-merged-gesp.bin`
(verified by hashing both).

### Third flash: touch mapping and the repeating keyboard

Two fixes, both from the same increment. **Touch**: the both-axis mirror that
was meant to follow the panel flip is gone. **Keyboard**: the scanner's idle
shortcut could not see a *release* — `kb_presence()` only detects keys that are
*down* — so the cell stayed pressed for good and `display_manager`'s held-key
repeat kept re-injecting it (one `h` press typing a row of `h`, backspace
wiping a field), until the next key was pressed. The scan task now stays on full
scans while anything is held, and `kb_repeat_start()` no longer re-arms itself
from the repeat events it produced (a queued repeat outliving its release did
the same thing from the timer side).

The rebuild links clean (2,139 tasks; the single warning in the log, `unused
variable 'registered_disp'` at `display_manager.c:2486`, is upstream and appears
in every earlier build log). App image 3,612,128 bytes (`0x371de0`), 2,679,328
bytes (42 %) free in the 6 MB `ota_0`; merged image 3,743,200 bytes (`0x391de0`)
with `esptool image-info`: ESP32-S3, 16 MB, 80 MHz, **DIO**, valid checksum,
valid validation hash. `st7789_set_madctl` still disassembles to `movi a8, -64` /
`xor a2, a2, a8`, so the display flip is intact in the image. On the SD card as
`GhostESP-2.2.2-A001-merged.bin`, SHA-256
`ad63e68d23efe8be30b1dcc8c5024b1c279dd5b84b13c3148d574614d3136485`, byte-identical
to `local_builds/Attaky-Cyber-Deck-Plus-merged-gesp.bin` (both hashed).

The remaining M2 gates (IMU orientation, audio path) are still open with the
device in hand. **Touch is no longer one of them**: the `SWAPXY` + `INVERT_X`
pair carried over from the CrowPanel 2.4/2.8 profile is measured, because the
only competing mapping — the same pair plus a mirror of both axes — was tried on
the glass and put every tap in the diagonally opposite corner.
`sdkconfig.defaults` is gitignored upstream, so the copy is local-only;
`configs/sdkconfig.attaky` stays the tracked source of truth.

### Fourth flash: B backs out anywhere, mic-driven Visualizer, RGB colour order

Three fixes, all reported from the glass.

**B is Escape, and nothing more.** The gamepad already emits `LV_KEY_ESC` for B
(`attaky_gamepad.c`) - the same key every view's own back path is wired to (the
touch back button, the Escape fallbacks, the hardware exit button's fallback).
`display_manager.c` hands that event straight to the view through
`dm_dispatch_to_view()` and adds nothing of its own.

That is the fix, not an omission. Two earlier attempts tried to be helpful - if a
view looked "idle" after a back press, back it out anyway - and each made it
worse in a different direction:

- The first fired after 160 ms and looked at `dm.current_view`. A view's back is
  asynchronous (its swap lands when the fade completes) and the Terminal runs
  `stop_all_operations()` first, so the screen was still up and B popped a
  second level: Terminal to Main Menu instead of Terminal to Apps Gallery.
- The second watched a navigation-request counter and waited 1.2 s. That covered
  slow navigations but not the views that consume Escape *without* navigating:
  the BLE scan overlays cancel the scan and stay on their menu ("Use any input
  to finish scan"), so B cancelled a scan and then, a second later, walked out
  of Options by itself.

"Handled" cannot be told from "ignored" from the outside, so the guess is gone.
A view that needs Escape gets it in the view, next to the rest of its back
handling. The one behavioural change kept is that back does not auto-repeat
(`kb_repeat_start()` refuses 27/29/'`'), so a held B walks out exactly one level
instead of machine-gunning the route stack.

**Visualizer runs from the Media I/O microphone.** The I2S capture was already
live on this profile (`CONFIG_MIC_I2S_BCLK_PIN=39` / `WS=9` / `DIN=16`, the
ICS-43434's SCK/WS/SD), but the mic task only shipped its spectrum to a
GhostLink peer, and the Visualizer waited for a USB-serial/UDP host stream.

The first cut of the mic path was silent on the glass, and the cause was in the
driver, not the profile: `mic_read_samples()` read a **fixed channel** of a
stereo frame (`samples[i * 2]`), copied from an SPH0645/INMP441 bring-up. A mono
MEMS mic drives only one half of the frame, and which half is decided by its L/R
pin - here `ICS-43434.LR` on `M-BTB37` / host `IO46`, a strapping pin this driver
cannot read back. On the other wiring the sampled half is permanently zero, which
is exactly "the microphone sees no noise". The driver now measures both halves
each frame, latches onto the one carrying signal (the dead half is exactly zero,
and a latch keeps it from flickering on noise), and logs the slot it settled on:
`Mic data on the right I2S slot (peak L=0 R=...)`.
`mic_visualizer` now publishes its last four Goertzel bands plus level
(`mic_visualizer_get_bands()`), and the Visualizer stretches those across its
15 bars whenever the host stream is stale, labelling the source "MIC INPUT /
Media I/O microphone". A host stream still takes over the moment it arrives.

**Red/blue were swapped.** MADCTL's D3 (RGB/BGR order) was asserted - the
profile's landscape byte 0x08 reached the panel as 0xC8 after the 180-degree
mount correction - and the glass answered it by drawing red as blue. The
Attaky table now carries D3 clear: `0x00` for landscape, i.e. `0xC0` (MX|MY)
on the wire, which reads upright *and* draws red as red on this unit.

**The screen tuner is gone.** Orientation is settled, so the on-device
orientation tuner - the MADCTL walker with the test pattern, its NVS-stored
byte, and `st7789_set_madctl_override()` - was removed outright rather than
left switched off: `main/vendor/attaky/attaky_display_tuner.c` and its header
are deleted, `CONFIG_ATTAKY_DISPLAY_TUNER` is out of Kconfig and out of the
profile, and `main.c` no longer calls it. `st7789.c`'s orientation table is now
the only thing that decides the byte, so a boot cannot land on a stray saved
value, and there is nothing to hold a D-pad key for. The earlier sections above
that describe finding the byte with the tuner are the history of how it was
measured, not a mode this build has.

Rebuild after the tuner removal links clean, no new warnings (the `registered_disp`
and `popup.c` ones are upstream and in every earlier log). App image 3,608,368
bytes, merged image 3,739,440 bytes (`0x390f30`) with `esptool image-info`:
ESP32-S3, 16 MB, 80 MHz, **DIO**, valid checksum and validation hash. Merged
image at **`0x0`**, SHA-256
`d44304170ab4dc2f9f6ed290f95546bc390519fe99bb762382bc0c7e248bcd80`; app-only
`build/Ghost_ESP_IDF.bin` (3,608,368 bytes, SHA-256
`5cb2e4ed74ca91f82d219c7a4bdbf90458b17b2a259488fc18f4d0b3e4f8ae42`) still
belongs at `0x20000`. Both images are built, not run: the deck was not
reflashed from this host. The card copy is `D:\GhostESP-2.2.2-A001.bin` - the
`-merged` suffix is gone and the two older GhostESP images were deleted, so the
launcher now sees exactly one GhostESP entry.

## Known gaps (deliberately out of this increment)

- Expander-interrupt-driven scanning for the Control modules (polling today).
- Core RGB LED and per-module status LEDs are configured but never lit; the
  STK3311-X ambient-light sensor is unused (no auto-brightness).
- Module-detection safe-mode pin parking (holding module pins benign while
  probing) is not implemented; probing uses single-byte ACK reads only.
- No LoRa / GPS / FM-AM profile paths — this build has none of that hardware.
- Axis/orientation of the QMI8658A is unverified on hardware: the IMU axes are
  the sensor's own, to be corrected at M2/M4 with the device in hand. The touch
  axes (`LV_FT6X36_SWAPXY` + `INVERT_X`) come from this panel's mount plus the
  display rotation rather than from a measurement on the glass, so confirm
  them with a four-corner crosshair.
- **L1 / R1 / POWER (expander `0x59` P05/P06/P07) still do nothing.** The
  IO-expander B1/B2/B3 block in `display_manager.c` pushes
  `INPUT_TYPE_JOYSTICK` with `joystick_index = 5/6/7` and `joystick_pressed`
  left at its zero value — i.e. a *release* — and
  `dm_joystick_event_matches_view()` rejects any index above 4 as well. Both
  are upstream behaviour on every IO-expander board, not port damage; making
  the three top buttons work needs a press reported as a press, a matching
  release on the falling edge, and `s_joystick_press_owner[]` grown to 8
  entries, so it belongs in its own increment.

## USB / serial link (the only wired host path)

The Core's USB-C is a **CH340X USB-UART bridge**, not the ESP32-S3's own USB
(datasheet §USB: "USB-C through the CH340X USB-to-UART bridge … used for
firmware flashing and the serial console"; catalog: `CH340X` ↔ `TXD0`/`RXD0`).
Everything that talks to a host — the serial console, serial-based companion / Control-App /
Web-Serial tooling, and every flasher — therefore rides **UART0**, and the
profile is built for exactly that:

| Link | Detail |
| --- | --- |
| Bridge | CH340X, USB-C ↔ UART0 `TXD0` (IO43, MCU TX) / `RXD0` (IO44, MCU RX) |
| Console | `CONFIG_ESP_CONSOLE_UART_DEFAULT`, `UART_NUM=0`, 115200 8N1 (`CONFIG_ESP_CONSOLE_ROM_SERIAL_PORT_NUM=0` for the ROM path) |
| No second console | `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`; no USB-CDC and no USB-Serial-JTAG console, because the S3's native USB pins never reach the USB-C |
| Auto-reset | CH340X `DTR`/`RTS` → `EN`/`IO0` (catalog §USB-UART bridge), so a flasher can reset and enter download mode without holding BOOT |
| UART0 pins untouched | no `uart_set_pin()` in the tree targets UART0: the dual-comm link defaults to UART1 (`esp_comm_manager.c`), GPS picks UART1/UART2, and GPS is not in this profile — so nothing can steal the console at runtime |
| Host driver (Windows) | WCH `CH341SER`; the board enumerates as `USB-SERIAL CH340 (COM8)`, `VID_1A86&PID_7523`, bound by `oem17.inf` → `ch341ser.inf_amd64_12fd7c1278610ffb` |

`JTAG_SUPPORTED` is 1 on this target (it is for every S3), so
`serial_manager_init()` also installs the USB-Serial-JTAG driver and every log
line is mirrored there — harmless here, since IO19/IO20 appear nowhere in the
profile or the port's vendor files, and nothing enumerates on them.

A companion tool that instead scans for an *Espressif* USB device (`VID_303A`)
will not find this board: the native USB is unconnected, only the UART bridge
is. That is the one USB mistake this hardware invites.

### Why the COM number keeps changing (host side, not firmware)

The CH340X reports `VID_1A86&PID_7523` with **no USB serial number**. Windows
binds a COM port to a device *instance*, and with no serial string the instance
is identified by the port path it enumerated on - so plugging into a different
USB port, or re-enumerating after the bridge has dropped (power dip, hub reset,
cable), hands out a new instance and a new `COMn`. Nothing in this firmware can
change that: it never touches the bridge, which is a separate chip on the other
side of the connector, and its drivers are fixed-pin. Resetting the ESP32 does
not reset the bridge either.

What actually settles it:

- **Use one physical USB port on the host, always.** Windows caches the instance
  per port path; a different socket is a different device.
- **Clear the stale instances once.** Device Manager -> View -> *Show hidden
  devices* -> Ports, delete every greyed-out `USB-SERIAL CH340 (COMn)`, then
  replug. Windows reuses one number instead of adding the next one each time.
- **Pin the number.** Port Settings -> Advanced -> COM Port Number, pick a free
  one and keep it; the companion tools then find the same port every boot.

If a genuinely stable identity is needed across ports and machines, that is a
bridge with its own serial number (CH343/CP2102N class) - a hardware change, not
a firmware one.

## Flash addresses (SD store / fwstore publishing)

Read back from the built `partitions.bin` (not from the CSV), so these are the
bytes the device actually uses:

| Offset | Partition | Size |
| --- | --- | --- |
| `0x000000` | bootloader | — |
| `0x008000` | partition table | `0x1000` |
| `0x009000` | nvs | `0x7000` |
| `0x010000` | **otadata** | `0x2000` |
| `0x020000` | **ota_0 (the app)** | `0x600000` |
| `0x620000` | ota_1 | `0x600000` |
| `0xC20000` | coredump | `0x20000` |

| Artifact | Write at | Size | SHA-256 |
| --- | --- | --- | --- |
| `GhostESP-2.2.2-A001.bin` (whole flash, on the SD card) | **`0x0`** | 3,739,440 | `d44304170ab4dc2f9f6ed290f95546bc390519fe99bb762382bc0c7e248bcd80` |
| `bootloader.bin` | `0x0` | 22,560 | `aba6c942595614afd3c585bb3a4dcaaec1c6f0b546275e98317973f68cf5ed26` |
| `partitions.bin` | `0x8000` | 3,072 | `eb24c944af20a4a8d95837593454d1b843be6e55f0e3d9cfbc1c50e0924b3478` |
| `Ghost_ESP_IDF.bin` (app only) | **`0x20000`** | 3,608,368 | `5cb2e4ed74ca91f82d219c7a4bdbf90458b17b2a259488fc18f4d0b3e4f8ae42` |

The app offset is **`0x20000`, not `0x10000`** — the builder's
`build-manifest` template and the Arduino 16 MB layouts put the app at
`0x10000`, but in this table `0x10000` is *otadata*. An app written there
corrupts otadata, leaves `ota_0` empty and does not boot; the two regions are a
whole partition apart, so there is nothing "close" about it. One whole-flash
image at `0x0` removes the question, and it is the shape the SD store's other
whole-flash entries (including the known-good id 42) already have. The file
name carries no `-merged`: it is the build this tree produces, one file, `0x0`.

The same numbers, in the shape a store manifest wants, are written beside the
MicroPython note in the SD store's text folder
(`<Downloads>\Attaky-All\Firmwares\text\GhostESP-FLASH-ADDRESS.txt`). The SD
copy is `D:\GhostESP-2.2.2-A001.bin`, hashing identically to
`build/GhostESP-2.2.2-A001-merged.bin` (that build-side name is just what the
merge step writes); it is the only GhostESP image on the card.
