---
title: "Keyboard Control"
description: "Using a USB keyboard to control GhostESP"
weight: 10
---

ESP32-S3 devices can use USB Host mode to accept input from a USB keyboard.

## Requirements

- **ESP32-S3 based device** (Cardputer ADV, T-Deck, etc.)
- **USB OTG adapter** (Type-C to USB-A)
- **USB keyboard**
- **CONFIG_USE_USB_KEYBOARD** enabled in build config

## Enabling USB Host Mode

USB Host mode can be enabled in two ways:

### Via Settings Menu

1. Navigate to **Settings → System & Hardware**
2. Find **USB Host Mode**
3. Toggle to **On**

### Via Command

```
usbkbd on
```

To disable:
```
usbkbd off
```

To check status:
```
usbkbd status
```

## Keyboard Controls

Once enabled, the following keys control navigation:

| Key | Action |
|-----|--------|
| ↑ Arrow | Navigate up |
| ↓ Arrow | Navigate down |
| ← Arrow | Back / Left |
| → Arrow | Right |
| Enter | Select |
| ESC | Back / Exit |

All other keys work as normal keyboard input for text fields.

## Remote Keyboard

When two GhostESP devices are connected via Dual Comm, keyboard events are transmitted between them. This allows one device with a USB keyboard to control another device remotely.

## Important Notes

- **CDC Console:** USB console is unavailable while in Host mode (port switches from Device to Host).
- **Flashing**: You must disable USB Host mode (or reboot) to flash new firmware via USB
- **Not Persistent**: The USB Host mode setting is runtime-only and resets on reboot
