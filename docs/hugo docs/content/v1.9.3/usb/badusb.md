---
title: "BadUSB"
description: "Run DuckyScript over USB HID"
weight: 20
---

BadUSB runs scripts from the SD card as a USB HID keyboard. Scripts live in `/mnt/ghostesp/badusb/`.

## Supported Devices

**Note:** Configs with VSENSE support will wait for a USB connection before starting the script.

With VSENSE support:
- The Wired Hatter's Banshee

Without VSENSE support:
- Cardputer
- Cardputer ADV
- LilyGo T-Deck/T-Deck Plus

## Supported DuckyScript Commands

Supported DuckyScript commands:

- `REM`
- `DEFAULT_DELAY` / `DEFAULTDELAY`
- `DELAY`
- `STRING`
- `REPEAT`
- modifier combos like `CTRL`, `SHIFT`, `ALT`, `GUI` / `WINDOWS` with a key
- named keys like `ENTER`, `TAB`, `ESC`, arrows, and `F1`-`F12`

## Script Files

- Place scripts in `/mnt/ghostesp/badusb/`
- File extension: `.txt` (only `.txt` files are listed)
- Remote streaming size limit: 64 KB per script
- SD card must be inserted and mounted to list or run scripts

To add your own files, create the `badusb` folder on the SD card and copy your `.txt` scripts into it.

## CLI

Commands:

```
badusb list
badusb run <filename>
badusb stop
badusb exec <size>
```

Settings (applies to the next run):

```
badusb set_vid <hex>
badusb set_pid <hex>
badusb set_mfr <text>
badusb set_prod <text>
badusb set_rand <0|1>
badusb set_layout <n>
```

Keyboard layout values:

- `0` = US
- `1` = DE
- `2` = FR
- `3` = UK
- `4` = ES

## On-Display (Standalone)

From the BadUSB view:

1. Open **Settings**
2. Edit **VID**, **PID**, **Manufacturer**, **Product**
3. Toggle **Randomize** for per-run USB details
4. Select **Layout** to cycle the keyboard layout

These values are saved on the local device and used on the next run.

## Remote

When BadUSB is used over Dual Comm, the controller sends the current settings to the peer right before streaming the script. The peer uses those settings for that run. These remote settings apply only for that run and do not persist to NVS on the peer. For setup details, see [GhostLink](/latest/getting-started/dual-communication/).

