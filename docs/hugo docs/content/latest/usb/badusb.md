---
title: "BadUSB"
description: "Run DuckyScript over USB HID"
weight: 20
---

BadUSB runs scripts from the SD card as a USB HID keyboard. Scripts live in `/mnt/ghostesp/badusb/`.

For other USB features, see [Mouse Jiggler]({{< relref "mouse-jiggler.md" >}}) and [USB Keyboard Mode]({{< relref "usb-keyboard-mode.md" >}}).

## Supported Devices

**Note:** Configs with VSENSE support will wait for a USB connection before starting the script.

With VSENSE support:
- The Wired Hatters Banshee

Without VSENSE support:
- Cardputer
- Cardputer ADV
- LilyGo T-Deck/T-Deck Plus
- Banshee S3 (`Banshee_S3` build; S3 worker paired with the C5 display)

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

Script commands:

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
