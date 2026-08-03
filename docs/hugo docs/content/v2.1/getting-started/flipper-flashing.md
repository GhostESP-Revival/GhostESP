---
title: "Flash with Flipper Zero"
description: "Use a Flipper Zero and the GhostESP app to flash a compatible ESP32 board."
weight: 60
toc: true
---

## What this does

This method uses a Flipper Zero as the programmer for a compatible ESP32 board. Use it when you prefer a portable flashing workflow.

## Before you start

- A Flipper Zero with an SD card.
- The GhostESP Flipper app from the [Flipper app store](https://lab.flipper.net/apps) or [latest release](https://github.com/GhostESP-Revival/GhostESP-FlipperCompanion/releases/latest).
- The GhostESP firmware `.zip` that matches the ESP32 chip.
- A way to copy the extracted firmware files to the Flipper SD card.
- Correct GPIO wiring between the Flipper and ESP32 board. Refer to the GhostESP Flipper app for the pinout.

## On-device steps

1. Install the GhostESP `.fap` on the Flipper SD card.
2. Download and extract the matching GhostESP firmware archive.
3. Copy its three `.bin` files to `SDCard/apps_data/esp_flasher/`. Do not place them in `assets/`.
4. Wire the ESP32 to the Flipper GPIO pins.
5. Put the ESP32 into bootloader mode: hold **BOOT**, connect USB, then release **BOOT**.
6. Open the **ESP flasher** app on the Flipper and choose **Manual Flash**.
7. Select `bootloader.bin`, `partitions.bin`, and `GhostESP.bin`, then confirm the target variant.
8. Start the flash and reset the ESP32 when it completes.

## Expected result

GhostESP boots and exposes `GhostNet`. Continue with a connection method or run a first passive Wi-Fi scan.

## Troubleshooting

- **Flashing fails:** Recheck the GPIO wiring, bootloader mode, and board variant.
- **Firmware files are not shown:** Confirm all three files are in `SDCard/apps_data/esp_flasher/`, not `assets/`.
- **The board does not boot:** Download the release matching its ESP32 chip and flash again.

## Related tasks

- [Installation Guide]({{< relref "installation-guide.md" >}})
- [Manual USB Flashing]({{< relref "manual-flashing.md" >}})
- [Try your first scan]({{< relref "first-scan.md" >}})
