---
title: "Karma Attack"
description: "Automatically respond to device probes with fake networks."
weight: 50
---

Create fake Wi-Fi networks based on SSIDs that nearby devices are searching for. When devices search for a network, Karma broadcasts it back to them.

> **Note**: Only test this on networks you own or have explicit permission to test. Unauthorized access to networks is illegal in most jurisdictions.

## Prerequisites

- GhostESP flashed device, powered on with a wireless antenna.
- Optional: SD card for evil portal integration.

## Legal and ethical

- Only test on your own devices or with written permission.
- Never use Karma on networks you don't own.


## Starting Karma

### On-device UI

1. Open **Menu → WiFi → Attacks → Start Karma Attack**.
   The device will begin learning SSIDs from probe requests.
2. To use specific SSIDs instead, choose **Start Karma Attack (Custom SSIDs)**.
   Enter the SSIDs you want to broadcast (separated by commas).
3. The device will start broadcasting fake networks. Leave it running to catch devices.
4. To stop, go back to the menu or select **Stop Karma Attack**.

### Command line

1. Run `karma start` to begin automatic SSID learning.
   The device will cache SSIDs from probe requests.
2. Or run `karma start SSID1 SSID2 SSID3` to use specific SSIDs.
   Example: `karma start FreeWiFi Starbucks McDonald's`
3. Run `karma stop` when you're done.

## How it works

**Automatic mode**: Karma listens for Wi-Fi probe requests from nearby devices and broadcasts fake networks with the SSIDs they're searching for.

**Custom mode**: You specify which SSIDs to broadcast instead of learning them automatically.

When a device connects to a fake network, a captive portal starts automatically to capture credentials.

## Troubleshooting

- **Fake networks not appearing**: Try restarting with `karma stop` then `karma start`.
- **No devices connecting**: Ensure devices are actually searching for networks. Try moving closer to the ESP32.

