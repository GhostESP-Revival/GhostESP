---
title: "Installation Guide"
description: "Flash GhostESP with the recommended web flasher"
weight: 20
toc: true
---

## What this does

The web flasher installs the GhostESP firmware on a compatible ESP32 board directly from your browser. It is the recommended method for most users.

## Before you start

- A compatible ESP32 board (see [Supported Hardware]({{< relref "supported-hardware.md" >}}))
- A USB cable (Micro USB or USB-C; must be a data cable, not charge-only)
- A modern web browser (Chrome, Brave, or Edge; Firefox doesn't support WebSerial)

## Flash with the web flasher

1. Open [ghostesp.net/flasher](https://ghostesp.net/flasher) in Chrome, Brave, or Edge.
2. Close any app that is using the board's serial port.
3. Enter bootloader mode: hold **BOOT**, connect USB, then release **BOOT**.
4. If that does not work, hold **BOOT**, tap **RESET**, wait 1-2 seconds, then release **BOOT**.
5. Select the matching ESP32 variant, click **Connect**, and follow the browser prompts.
6. When flashing finishes, unplug and reconnect the board.

## Expected result

GhostESP boots and creates a Wi-Fi access point named `GhostNet` with password `GhostNet`.

## Troubleshooting

- **The board is not detected:** Confirm the cable supports data, close other serial apps, then enter bootloader mode again.
- **The web flasher times out:** Reconnect the board and repeat the bootloader steps. Clear the browser cache only if the flasher page itself fails to load correctly.
- **`GhostNet` does not appear after flashing:** Verify the selected firmware matches your board and restart the device. Use a [serial console](https://ghostesp.net/serial) at 115200 baud to inspect boot output.

## Related tasks

- [Connect to GhostESP]({{< relref "control-methods.md" >}})
- [Try your first scan]({{< relref "first-scan.md" >}})
- [Manual USB flashing]({{< relref "manual-flashing.md" >}})
- [Flash with Flipper Zero]({{< relref "flipper-flashing.md" >}})
