---
title: "Usage Guide"
description: "Basic navigation and common workflows for the Flipper Zero companion app"
weight: 70
keywords: ["usage", "navigation", "workflow", "guide"]
---

## Basic Navigation

The app uses a menu-driven interface:

- **Up/Down**: Navigate through menu items
- **OK**: Select item or confirm action
- **Back**: Return to previous menu or cancel

## Common Workflows

### Wi-Fi Handshake Capture

1. Navigate to **Wi-Fi** → **Scanning & Probing** → **Scan: (APs)**
2. Wait for scan to complete
3. Select target network from list
4. Go to **Packet Capture** → **EAPOL**
5. Start capture and wait for handshake
6. Export PCAP file when complete

### Beacon Spam Attack

1. Navigate to **Wi-Fi** → **Attacks** → **Beacon Spam (List)**
2. Select spam mode (List, Random, Rickroll, or Custom)
3. If using List mode, manage your beacon list first
4. Start the spam attack
5. Stop when desired

### BLE Device Scanning

See [BLE Features]({{< relref "ble-features.md" >}}) for the BLE scanning workflow under **Bluetooth** → **Scanning & Detection** and the actions available for discovered devices.

### GPS Wardriving

1. Ensure GPS module is connected and receiving signal
2. Navigate to **GPS** → **Wardriving** → **Start Wardriving**
3. Start wardriving session
4. Drive around to collect data
5. Stop and export CSV file with GPS coordinates

## File Management

See [File Storage]({{< relref "configuration.md#file-storage" >}}) for where captured files are saved on the Flipper Zero and how to transfer them to your computer.
