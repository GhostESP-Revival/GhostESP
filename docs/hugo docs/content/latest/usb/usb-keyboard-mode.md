---
title: "USB Keyboard Mode"
description: "Use your ESP32-S3 as a real USB keyboard"
weight: 25
---

USB Keyboard Mode turns your ESP32-S3 into a live USB HID keyboard. Instead of executing pre-written DuckyScript files, keystrokes are forwarded in real time from a paired display controller (C5) to the host PC.

## How It Works

1. The S3 installs as a USB HID keyboard (composite device with keyboard + mouse interfaces)
2. The C5 on-screen keyboard sends each pressed key as a `badusb type_char` command over GhostLink
3. The S3 receives the command and sends it as a USB HID keyboard report to the host PC

This effectively turns the C5 display into a wireless keyboard relay.

## Usage

### From the Display Menu (Remote)

1. Navigate to **BadUSB**
2. Select **USB Keyboard**
3. The C5 sends `badusb keyboard_start` to the S3
4. The S3 activates USB HID keyboard mode
5. Key events from the C5 are forwarded to the host PC in real time
6. Select **Done** or **Exit** to leave the keyboard screen, then select **Cancel** on the status popup to stop keyboard mode

### Via CLI (Standalone S3)

Start:
```
badusb keyboard_start
```

Stop:
```
badusb keyboard_stop
```

Type text through the active keyboard mode:
```
badusb type <text>
```

Type one ASCII character by numeric code:
```
badusb type_char <ascii>
```

Send a single keypress (HID modifier + keycode):
```
badusb keysend <modifier> <keycode>
```

Example — send Ctrl+C:
```
badusb keysend 1 6
```

(Modifier `1` = Left Ctrl, keycode `6` = 'c' in HID)

## HID Modifier Values

| Value | Modifier |
|-------|----------|
| 0x01 | Left Ctrl |
| 0x02 | Left Shift |
| 0x04 | Left Alt |
| 0x08 | Left GUI (Windows) |
| 0x10 | Right Ctrl |
| 0x20 | Right Shift |
| 0x40 | Right Alt |
| 0x80 | Right GUI |

Modifiers can be combined with bitwise OR. For example, Ctrl+Shift = `0x03`.

## HID Keycode Reference

Common keycodes (hex):

| Key | Code |
|-----|------|
| a-z | 0x04-0x1D |
| 1-9 | 0x1E-0x26 |
| 0 | 0x27 |
| Enter | 0x28 |
| Escape | 0x29 |
| Backspace | 0x2A |
| Tab | 0x2B |
| Space | 0x2C |
| F1-F12 | 0x3A-0x45 |
| Arrow Right | 0x4F |
| Arrow Left | 0x50 |
| Arrow Down | 0x51 |
| Arrow Up | 0x52 |

Full reference: [USB HID Usage Tables](https://usb.org/document-library/hid-usage-tables-14)

## Notes

- **USB required**: The S3 must be plugged into the host PC. If VSENSE is configured, it waits for USB connection automatically.
- **Mutually exclusive**: USB Keyboard Mode cannot run at the same time as BadUSB scripts or Mouse Jiggler — they share the same USB HID device.
- **Composite device**: When active, the S3 presents as both a keyboard and mouse (composite HID). The mouse interface is idle unless the Mouse Jiggler is used.
- **Immediate display typing**: The C5 on-screen keyboard sends each key as it is pressed; **Done** only exits the keyboard screen.
- **CDC Console unavailable**: The USB serial console is not available while HID mode is active.
