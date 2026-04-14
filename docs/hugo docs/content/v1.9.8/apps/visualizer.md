---
title: "Visualizer (Rave Mode)"
description: "Stream desktop audio to GhostESP and render a live spectrum visualizer"
weight: 5
toc: true
---

`Visualizer` is the on-device music visualizer app (previously labeled Rave Mode). It renders a live 15-band spectrum from audio sent over USB serial or UDP.

## Recommended Download (Windows)

Start here for the easiest setup:

- [`rave_tray.exe`](https://github.com/GhostESP-Revival/GhostESP/blob/Development-deki/scripts/Audio%20Visualizers/rave_tray.exe)

The current transport supports:

- High-rate USB serial streaming with compact binary frames
- UDP streaming on port `6677`
- Auto-discovery service over UDP `6678` (device advertises, helper auto-targets)

## Requirements

- A GhostESP build with screen support
- Desktop helper from `scripts/Audio Visualizers/`
- For USB mode: data-capable USB cable
- For UDP mode: host and GhostESP on the same network

## Open the App

From the device UI, open **Apps -> Visualizer**.

You can also open it from CLI:

```
rave on
```

Close with:

```
rave off
```

## Quick Start (USB Serial)

USB is usually the smoothest path for realtime updates.

1. Connect GhostESP over USB
2. Open **Apps -> Visualizer** on the device
3. Run the launcher:

```
rave_helper.bat
```

4. Choose `USB serial`
5. Pick a source (optional) and port when prompted

## Quick Start (Wi-Fi / UDP)

1. Connect GhostESP to Wi-Fi
2. Open **Apps -> Visualizer**
3. Run the launcher:

```
rave_helper.bat
```

4. Choose `Wi-Fi / UDP`
5. Leave device IP blank for auto-discovery (recommended), or enter a specific IP

When IP is blank:

- GhostESP broadcasts discovery beacons on `6678`
- The helper listens for those beacons and switches to that device automatically
- If discovery does not resolve a target, broadcast fallback still works on normal LANs

You can disable discovery and use classic routing:

```
python "_internal\Display_Visualizer.py" --no-discovery 255.255.255.255
```

## Recommended Usage

For normal use, run the launcher and use its menu:

```
rave_helper.bat
```

The launcher already handles:

- USB vs Wi-Fi mode selection
- Port detection/probing
- Audio source selection
- common defaults

### Optional: Tray App (Rust)

If you want a persistent desktop helper, you can build and run the tray binary:

```
_dev\build_rave_tray.bat
rave_tray.exe
```

```
chmod +x "_dev/build_rave_tray.sh"
./_dev/build_rave_tray.sh
./rave_tray
```

Build output is a single distributable with the worker embedded (`rave_tray.exe` on Windows, `rave_tray` on Linux).
End users only need that tray binary and do not need Python or Rust installed.

Tray menu actions:

- `Start Wi-Fi (Auto)` starts background stream with discovery
- `Start Wi-Fi (Set IP...)` prompts for a fixed device IP
- `Start USB (Auto Port)` auto-selects first detected GhostESP serial port
- `Start USB (Manual Port...)` prompts for serial port (for example `COM3`)
- `Set Audio Source...`, `Set FPS...`, and `Set USB Baud...` provide quick popup configuration
- `Open Interactive Helper` opens `rave_helper.bat`
- `Stop Stream` stops the background stream

## Advanced (optional)

Direct script arguments are optional and mainly for troubleshooting or automation.

```
python "_internal\Display_Visualizer.py" --list-sources
python "_internal\Display_Visualizer.py" --list-ports
```

## Troubleshooting

- If device frames stay near `0`, verify firmware and helper are both updated and `Visualizer` is open.
- If movement is choppy, prefer USB serial and a higher baud rate.
- If level is always low, run `--list-sources` and select the correct playback loopback source.
- If Wi-Fi auto-discovery does not find a device, try direct IP mode. Some guest networks/VLANs block local broadcast traffic.
- If audio capture drops with runtime errors (for example `0x88890004`), the helper now retries and auto-recovers; if it repeats often, reselect the playback source and update audio drivers.
- `raveport` should return protocol details for serial helper probe.
