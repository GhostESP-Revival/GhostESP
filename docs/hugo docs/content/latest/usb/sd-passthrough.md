---
title: "USB SD Passthrough"
description: "Use your device as a USB SD card reader"
weight: 40
---

USB SD Passthrough exposes the device's SD card to a host PC as a USB mass-storage drive, turning the board into a USB SD card reader. Requires an ESP32-S3 board with a physical SD card.

## How It Works

When started, the firmware releases the SD card from its own filesystem, hands the card to the TinyUSB mass-storage class, and enumerates on the host PC as a USB drive. The card stays owned by the USB host until passthrough is stopped, at which point the card is remounted and the serial console restored.

While active:

- The firmware **cannot** access the SD card (captures, logging, and other SD writes fail cleanly until passthrough stops).
- The **USB serial console is unavailable** — use WiFi or the WebUI terminal to run `usbsd off`.
- WiFi and the WebUI stay up.

## Usage

### Via CLI

```
usbsd on      # start passthrough (card becomes a USB drive)
usbsd off     # stop passthrough and remount the card
usbsd toggle  # start if stopped, stop if active
usbsd status  # show passthrough state and saved preference
```

### From the Display Menu

Navigate to **Settings → Power → USB SD Passthrough** and toggle it on. Toggling off stops passthrough and remounts the card.

### Saved Preference

The toggle state is remembered across reboots (`settings get/set usb_msc`), but passthrough **never auto-starts at boot** — the device always boots with a normal console and mounted SD card.

## Requirements and Limitations

- ESP32-S3 board with an SD card (Cardputer, Cardputer ADV, T-Deck, TEmbed C1101, T-Dongle-S3, and other S3 builds with SD support).
- Cannot run at the same time as BadUSB, USB Keyboard Mode, or USB host keyboard — stop those first.
- Boards whose SD card is wired to GPIO19/20 (the native USB pins) are unsupported.
- The disk identifies itself to the host with test vendor strings from the TinyUSB stack; the USB device itself shows as "Ghost ESP SD".
- Always eject the drive on the host PC before running `usbsd off`.
