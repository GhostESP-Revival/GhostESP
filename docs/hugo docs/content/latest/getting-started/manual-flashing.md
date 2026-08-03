---
title: "Manual USB Flashing"
description: "Flash a downloaded GhostESP release when the web flasher is unavailable."
weight: 50
toc: true
---

## What this does

Manual USB flashing installs a downloaded release bundle with ESP Huhn Tool. Use it when you need a specific release file or the recommended web flasher is unavailable.

## Before you start

- A compatible board and USB data cable.
- The firmware `.zip` for your board from [GhostESP Releases](https://github.com/GhostESP-Revival/GhostESP/releases).
- [7-Zip](https://www.7-zip.org/download.html) or another tool that can extract the release archive.
- Chrome, Brave, or Edge for [ESP Huhn Tool](https://esp.huhn.me/).

## On-device steps

1. Hold **BOOT**, connect USB, then release **BOOT**.
2. If that does not work, hold **BOOT**, tap **RESET**, wait 1-2 seconds, then release **BOOT**.

## Flashing steps

1. Extract the downloaded firmware archive.
2. Open [ESP Huhn Tool](https://esp.huhn.me/) and click **Connect**.
3. Select the board's serial port.
4. Add the three release binaries using the offsets below.

| Chip | `bootloader.bin` | `partitions.bin` | `firmware.bin` |
|---|---:|---:|---:|
| ESP32-S2 | `0x1000` | `0x8000` | `0x10000` |
| ESP32-S3 / C3 / C5 / C6 | `0x0` | `0x8000` | `0x10000` |

5. Click **Flash** and wait for it to finish.
6. Unplug and reconnect the board.

## Expected result

GhostESP starts and creates the `GhostNet` access point. You can verify boot output with a [serial console](https://ghostesp.net/serial) at 115200 baud.

## Troubleshooting

- **The board is not detected:** Use a data-capable cable, close other serial apps, and retry bootloader mode.
- **Flash fails:** Confirm that each binary came from the same release archive and uses the matching offset.
- **The board loops after flashing:** Check that the downloaded release matches the exact board and chip.

## Related tasks

- [Installation Guide]({{< relref "installation-guide.md" >}})
- [Flash with Flipper Zero]({{< relref "flipper-flashing.md" >}})
- [Connect to GhostESP]({{< relref "control-methods.md" >}})
