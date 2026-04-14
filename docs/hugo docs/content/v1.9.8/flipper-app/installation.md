---
title: "Installation"
description: "Install the GhostESP companion app on your Flipper Zero device."
weight: 10
keywords: ["install", "setup", "app store", "fap"]
---

## Prerequisites

- Flipper Zero device
- GhostESP firmware flashed on an ESP32 device
- Latest version of the companion app (`.fap` file)

## Method 1: Flipper App Store (Recommended)

1. Open the **Flipper App Store** on your Flipper Zero
2. Search for "GhostESP" or "ghost_esp"
3. Install the app directly from the store
4. The app will appear in your Applications menu

## Method 2: Manual Installation

1. **Download the latest release**
   - Visit the [releases page](https://github.com/jaylikesbunda/ghost_esp_app/releases/latest)
   - Download the `.fap` file for your Flipper Zero firmware version

2. **Transfer to Flipper Zero**
   - Use **qFlipper** (desktop or mobile app) to transfer the file
   - Or copy the `.fap` file directly to your Flipper's SD card under `apps/` directory

3. **Launch the app**
   - Navigate to **Applications** → **GhostESP** on your Flipper Zero
   - The app will start and attempt to connect to your GhostESP device

## Version Compatibility

The companion app is regularly updated to support new GhostESP firmware features. Always use the latest version of both:

- **GhostESP Firmware**: Latest release from main repository
- **Companion App**: Latest `.fap` from [releases](https://github.com/jaylikesbunda/ghost_esp_app/releases)

> **Note**: Older app versions may not support newer firmware features. Check the [changelog](https://github.com/jaylikesbunda/ghost_esp_app/blob/main/CHANGELOG.md) for compatibility information.

## Next Steps

After installing the app, proceed to [Connection Setup]({{< relref "connection.md" >}}) to configure the serial connection between your Flipper Zero and GhostESP device.
