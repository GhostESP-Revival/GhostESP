---
title: "Adjusting build options"
description: "Use the build helper to edit GhostESP configuration options."
weight: 10
---

The `build.py` helper wraps Espressif's build tools so you can change GhostESP feature flags without setting up the environment manually. Follow the steps below to run `menuconfig`, persist your changes, and rebuild with the updated settings.

## Prerequisites

- Python 3 installed with access to the command line.
- ESP-IDF v5.4.1, v5.5 or v5.5.1 on disk. `build.py` can auto-detect common locations or also download it for you.
- GhostESP repository cloned locally. Clone from GitHub and navigate into the project directory before running the script.

## 1. Launch the helper

From the project root, run:

```
python build.py
```

If you already know which boards you want to modify, you can skip the interactive picker:

```
python build.py --targets 2 3
```

Use `--targets all` to queue every board profile.

When prompted for ESP-IDF, confirm a detected path or allow the script to download v5.5 automatically. Supplying `--idf-path <path>` bypasses the prompt entirely.

## 2. Select a build target

GhostESP ships configs for multiple boards inside `configs/`. The helper lists them with numeric IDs. Enter the index for the target whose Kconfig you want to edit (for example `esp32s3-generic`).

The script copies that entry's `sdkconfig.*` file into `sdkconfig` and `sdkconfig.defaults`, then runs `idf.py set-target` so menuconfig matches the board.

## 3. Run menuconfig for GhostESP options

After applying the base config, `build.py` launches:

```
idf.py menuconfig
```

Use the curses UI to browse `GhostESP Options → GhostESP Features` and any other menus you need. When finished, save and exit menuconfig.

### Enabling NFC features

Open **Ghost ESP Options → NFC Options** to toggle the backends:

- **Enable PN532 NFC** turns on the PN532 reader and exposes pin settings for SCL/SDA/IRQ/RST.
- **Enable Chameleon Ultra NFC** lets GhostESP use a Chameleon Ultra over BLE as the NFC front-end. This option defaults to enabled when BLE is compiled in.

Turning on either backend automatically sets the internal `HAS_NFC` flag, which enables the UI and shared NFC code paths.

### Enabling displays

Navigate to **Ghost ESP Options → Display Options** for screen hardware:

- Toggle **Enable Screen Support** to bring the LCD driver into the build.
- Adjust resolution via **TFT Width** and **TFT Height**, or enable board-specific panels such as **Enable Waveshare LCD**, **Enable Crowtech LCD**, **Enable Sunton LCD**, or **Enable JC3248W535EN LCD** depending on your hardware.
- Optional helpers like **Enable Touchscreen**, **Use 7-inch Display**, and **Enable Status Display** expose extra pins and I2C settings when needed.

### Configuring LVGL display driver

After enabling **Enable Screen Support**, navigate to **Component config → LVGL** to configure the display driver:

- Under **LVGL TFT Display**, select your display controller (e.g., ILI9341, ST7789, etc.).
- Configure **LVGL Touch Input** if your screen has a touchscreen.
- Adjust **LVGL Display Settings** for rotation, color depth, and refresh rate as needed for your hardware.

## 4. Decide where to store your changes

Once menuconfig exits, `build.py` asks whether to write the new configuration back into the source tree:

- **Yes** — Copies the generated `sdkconfig` into the original `configs/sdkconfig.*` file _and_ into `sdkconfig.defaults`. Choose this when you want the board profile to permanently carry the new settings.
- **No** — Keeps `sdkconfig.defaults` updated for the current build only without altering the source file. This is useful when experimenting.

Regardless of your choice, the helper syncs `sdkconfig.defaults` so the next build uses the options you just saved.

## 5. Rebuild firmware (optional)

If you continue through the prompts, `build.py` can clean, rebuild, and package artifacts automatically. Otherwise, rerun later with:

```
python build.py --targets <index>
```

The script will reuse the modified `sdkconfig.defaults` unless you overwrite it.

## Tips

- Running `python build.py --no-auto-download` disables the ESP-IDF fetch prompt if you prefer manual installs only.
- Delete `sdkconfig` and `sdkconfig.defaults` to revert to the pristine profile before another run.
- Use separate directories or virtual environments per ESP-IDF version to avoid mixing toolchains when switching targets.
