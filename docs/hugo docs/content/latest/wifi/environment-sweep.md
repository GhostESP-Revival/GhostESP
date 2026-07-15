---
title: "Run a Full Environment Sweep"
description: "Collect nearby Wi-Fi, BLE, 802.15.4, and GPS observations into a CSV file."
weight: 14
toc: true
---

## What this does

An environment sweep runs a sequence of wireless observations and saves the results to the SD card. The available data depends on the board, connected hardware, and enabled radios.

## Before you start

- A mounted SD card with free space.
- A board with the radios you want to observe. 802.15.4 capture is available on ESP32-C5 and C6 only.
- A GPS module if you want location fields in the export.

## On-device steps

1. Open **Menu → WiFi → Scanning → Sweep**.
2. Wait for each phase to finish. Progress appears on screen.
3. Open `/mnt/ghostesp/sweeps/sweep_N.csv` on the SD card to review the export.

## CLI steps

1. Run `sweep` to use the default 10-second timing for each phase.
2. Run `sweep -w 15` to use a 15-second Wi-Fi scan, or `sweep -b 20` for a 20-second BLE scan.
3. Combine options, such as `sweep -w 15 -b 20`, to adjust both phases.
4. Run `sweep -h` to view all options.

## Expected result

GhostESP saves a CSV containing available Wi-Fi access points and clients, BLE observations, optional 802.15.4 packets, and optional GPS coordinates.

## Troubleshooting

- **No CSV is created:** Confirm that the SD card is mounted and has free space.
- **A radio type is missing:** Check that the board supports it and that no conflicting Wi-Fi or BLE task is active.
- **GPS fields are empty:** Confirm the GPS module is connected and has a location fix before starting the sweep.

## Related tasks

- [Scan nearby Wi-Fi networks]({{< relref "survey.md" >}})
- [GPS and Wardriving]({{< relref "../gps" >}})
