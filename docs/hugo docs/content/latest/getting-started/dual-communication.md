---
title: "GhostLink"
description: "GhostLink connects two ESP32 devices for remote control while maintaining continuous AP hosting."
weight: 30
---

GhostLink is GhostESP's dual communication link between two ESP32 devices. It lets one device host the access point continuously while the other performs attacks or scans, all controllable from the web interface.

## Prerequisites

- Two compatible ESP32 boards.
- Three wires (TX, RX, GND) to connect them.
- Both devices flashed with GhostESP firmware.

## How it works

- Wire two ESP32s together via UART.
- They auto-discover and connect on boot.
- Send any GhostESP command from one device to the other.
- No manual connection commands needed in most cases.

## Wiring

Connect the devices with 3 wires:

- **TX of Device A** → **RX of Device B** (GPIO 6 → GPIO 7 by default, 17 → 16 on base ESP32 models)
- **RX of Device A** → **TX of Device B** (GPIO 7 → GPIO 6 by default, 16 → 17 on base ESP32 models)
- **GND** → **GND**

Both devices need power.

## Basic usage

Once wired and powered on, devices automatically find each other:

- `commstatus` - Check if connected
- `commsend <command>` - Send any command to the other device
- `commdisconnect` - Disconnect if needed

### Examples

- `commstatus` - Check connection
- `commsend scanap` - Send command to other device
- `commsend attack -d` - Start attack on other device
- `commsend beaconspam -r` - Start beacon spam on other device
- `commsend capture -probe` - Start probe capture on other device

## Changing pins

Default pins are TX: GPIO 6, RX: GPIO 7 (or 17 and 16 on base ESP32 models). To change them:

Run `commsetpins <TX_PIN> <RX_PIN>` (for example, `commsetpins 4 5`).

Pin changes are saved and persist across reboots. You cannot change pins while connected.

## Manual connection

If auto-connection fails, you can manually connect:

- `commdiscovery` - Check discovery status
- `commconnect ESP_A1B2C3` - Connect to specific device

Device names are auto-generated based on MAC address (format: `ESP_XXXXXX`).

## On-screen UI

When GhostLink is active (peer discovered and connected), the Options menu exposes a dedicated GhostLink section with submenus for session control, scanning, WiFi, attacks, capture, tools, BLE, and GPS. These entries open a split-view terminal so you can see what the local device is doing and how the remote peer responds at the same time.

- Left side: normal terminal logs from the local device (status messages, WiFi scans, attacks, etc.).
- Right side: GhostLink-focused messages and responses from the remote peer (discovery status, handshake/connect messages, and `commsend` command output).

Any GhostLink menu item that sends a `commsend ...` command automatically opens this split terminal view. Launching the generic Terminal app from the Apps menu still shows the standard single-column terminal with all logs combined.

## Troubleshooting

- **Not connecting**: Check wiring (TX→RX, RX→TX, GND connected). Make sure both devices are powered. Reboot them simultaneously and wait 30 seconds for discovery. Run `commstatus` to check.
- **Commands not working**: Verify connection with `commstatus`. Use exact GhostESP command syntax.

## Use cases

- **Hidden device control**: Hide one ESP32 and control it remotely with full functionality through the web interface.
- **Coordinated attacks**: Both devices attack different targets simultaneously.
- **Continuous AP hosting**: One device maintains the access point while the other performs intensive operations.

## Notes

- Auto-discovery happens every 3 seconds.
- Connection uses UART at 115,200 baud.
- The device with the "larger" name becomes master, but commands can be sent from both directions.
- Auto-reconnect is enabled; the connection will automatically restore if interrupted.
- Physical wired connection is required.
