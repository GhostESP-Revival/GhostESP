---
title: "BadUSB"
description: "Run DuckyScript over USB HID"
weight: 20
---

BadUSB runs scripts from the SD card as a USB HID keyboard. Scripts live in `/mnt/ghostesp/badusb/`.

## Supported Devices

**Note:** Configs with VSENSE support will wait for a USB connection before starting the script.

With VSENSE support:
- The Wired Hatters Banshee **S3**

Without VSENSE support:
- Cardputer
- Cardputer ADV
- CrowPanel 1.28" Rotary
- LilyGo T-Dongle-S3
- LilyGo TEmbed C1101

**Note:** The Banshee **C5** is a remote-HID build. It has no local BadUSB and instead drives BadUSB on a paired S3 over GhostLink. VSENSE (wait-for-USB) is present only on the Banshee **S3**.

## Supported DuckyScript Commands

- `REM`
- `DEFAULT_DELAY` / `DEFAULTDELAY`
- `DELAY`
- `STRING`
- `REPEAT`
- modifier combos like `CTRL`, `SHIFT`, `ALT`, `GUI` / `WINDOWS` with a key
- named keys like `ENTER`, `TAB`, `ESC`, arrows, and `F1`-`F12`

## Script Files

- Place scripts in `/mnt/ghostesp/badusb/`
- File extension: `.txt` (only `.txt` files from the SD card are listed; the built-in script is always listed first)
- Remote streaming size limit: 64 KB per script
- SD card must be inserted and mounted to list or run scripts

To add your own files, create the `badusb` folder on the SD card and copy your `.txt` scripts into it.

## CLI

Script commands:

```
badusb list
badusb run <filename|builtin>
badusb stop
badusb exec <size>
```

`badusb run builtin` runs the built-in **"Ghost Art (Built-in)"** script, which opens Notepad and types ASCII art. The built-in entry is always listed first by `badusb list` and does not need to exist on the SD card.

Settings (applies to the next run):

```
badusb set_vid <hex>
badusb set_pid <hex>
badusb set_mfr <text>
badusb set_prod <text>
badusb set_rand <0|1>
badusb set_layout <n>
```

Mouse, keyboard, and trackpad commands:

```
badusb jiggle_start
badusb jiggle_stop
badusb keyboard_start
badusb keyboard_stop
badusb type <text>
badusb type_char <ascii>
badusb keysend <modifier> <keycode>
badusb trackpad_start
badusb trackpad_stop
badusb trackpad_move <dx> <dy>
badusb trackpad_button <mask>
badusb trackpad_wheel <delta>
```

See [Mouse Jiggler]({{< relref "mouse-jiggler.md" >}}) and [USB Keyboard Mode]({{< relref "usb-keyboard-mode.md" >}}) for details.

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

When BadUSB is used over Dual Comm, the controller sends the current settings to the peer right before streaming the script. The peer uses those settings for that run. These remote settings apply only for that run and do not persist to NVS on the peer. For setup details, see [GhostLink]({{< relref "../getting-started/dual-communication.md" >}}).
