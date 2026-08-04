---
title: "Installation"
description: "Install the GhostESP companion app on your Flipper Zero device."
weight: 10
keywords: ["install", "setup", "app catalog", "fap"]
---

## Prerequisites

- Flipper Zero device
- GhostESP firmware flashed on an ESP32 device
- Latest version of the companion app (`.fap` file)

## Method 1: Flipper App Catalog (Recommended)

1. Connect your Flipper Zero to your computer (or pair it with the Flipper Mobile App) and open the [Flipper app catalog](https://lab.flipper.net/apps/ghost_esp) in a browser
2. Search for "Ghost ESP" and open the entry (category: GPIO)
3. Click **Install** — the app is uploaded to your Flipper Zero automatically
4. Launch it from **Applications → GPIO** on your Flipper Zero

## Method 2: Manual Installation

1. **Download the latest release**
   - Visit the [releases page](https://github.com/GhostESP-Revival/GhostESP-FlipperCompanion/releases/latest)
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
- **Companion App**: Latest `.fap` from [releases](https://github.com/GhostESP-Revival/GhostESP-FlipperCompanion/releases)

> **Note**: Older app versions may not support newer firmware features. Check the [changelog](https://github.com/GhostESP-Revival/GhostESP-FlipperCompanion/blob/main/CHANGELOG.md) for compatibility information.

## Next Steps

After installing the app, proceed to [Connection Setup]({{< relref "connection.md" >}}) to configure the serial connection between your Flipper Zero and GhostESP device.
