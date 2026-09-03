---
title: "BadBLE"
description: "Bluetooth HID keyboard — DuckyScript over BLE"
weight: 25
keywords: ["BadBLE", "BLE", "HID", "keyboard", "DuckyScript", "Bluetooth keyboard"]
---

BadBLE runs DuckyScript payloads as a standard Bluetooth HID keyboard (HOGP). The device advertises as a BLE keyboard, you pair it with a phone, laptop, or tablet, and payloads (or live typing) play out as keystrokes on the paired host — no USB cable required.

## Supported Devices

Enabled by default on Bluetooth-capable targets:

- ESP32 (original)
- ESP32-C3 / ESP32-C5 / ESP32-C6
- ESP32-S3

ESP32-S2 is not supported (no Bluetooth radio).

## Using BadBLE

Open **BadBLE** from the main menu on the device:

- **Run Script** — pick a payload from `/mnt/ghostesp/badble/` (or the built-in *Ghost Art* script, which needs no SD card). The device advertises and waits; once a host pairs, the popup flips from "Waiting" to "Running" and the script plays out. **Cancel** (tap anywhere, or confirm/back on joystick, Enter/Esc on keyboard, encoder press) stops it.
- **BLE Keyboard** — live typing screen. Every key is forwarded to the paired host as you press it; exiting the screen stops BadBLE.
- **Set Name** — change the advertised name (max 31 characters, saved to NVS).
- **Status** — current state, advertised name, and whether a host is connected.
- **Stop BadBLE** — tear down the BLE profile; WiFi/AP are restored.

The same feature is available from the CLI and the WebUI (BLE page → BadBLE tab).

## Pairing

- Pairing uses a **"Just Works"** exchange — no passkey to enter.
- The device initiates pairing as soon as a host connects; accept the pairing/passkey-optional prompt on the host.
- As with any BLE keyboard, the link must be **encrypted** before the host delivers keystrokes — pairing normally takes care of this automatically.
- Default advertised name: `GhostESP BadBLE`.

> **Note:** Only one host can be paired at a time. If the host drops, the device re-advertises so you can reconnect.

## Script Files

- Place scripts in `/mnt/ghostesp/badble/`
- File extension: `.txt` (only `.txt` files are listed)
- Size limit: 64 KB per script
- The built-in *Ghost Art* script always appears first in the picker and runs without an SD card

## CLI

```
badble status
badble list
badble run <filename|"Ghost Art (Built-in)">
badble stop
badble keyboard_start
badble keyboard_stop
badble type <text>
badble keysend <modifier> <keycode>
badble name
badble set_name <text>
```

## Limitations

- BadBLE owns the BLE radio while active: BLE scans, the BLE bridge, and other BLE tools are unavailable until BadBLE is stopped.
- One host at a time; script execution waits until a host is connected.
- BLE mouse/trackpad reports are not supported.