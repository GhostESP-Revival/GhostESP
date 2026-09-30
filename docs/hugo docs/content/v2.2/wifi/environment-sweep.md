---
title: "Run a Full Environment Sweep"
description: "Collect nearby Wi-Fi, BLE, 802.15.4, and GPS observations into a CSV file"
weight: 14
---

An environment sweep runs a sequence of wireless observations and saves the results to the SD card. The data you get depends on the board, connected hardware, and enabled radios: 802.15.4 capture is available on ESP32-C5 and C6 only, and GPS fields require a connected GPS module. Start with a mounted SD card that has free space.

## Running a sweep

1. Open **Menu → Wi-Fi → Monitor → Environment Sweep**.
2. Wait for each phase to finish. Progress appears on screen. The phases are: Wi-Fi AP scan, Wi-Fi station scan, BLE Flipper scan, BLE GATT scan, BLE raw packet scan, and (ESP32-C5/C6 only) 802.15.4 scan.
3. Open `/mnt/ghostesp/sweeps/sweep_N.csv` on the SD card to review the export.

From the CLI, run `sweep` to use the default 10-second timing for each phase. Run `sweep -w 15` to use a 15-second Wi-Fi scan, or `sweep -b 20` for a 20-second BLE scan, and combine options such as `sweep -w 15 -b 20`. Run `sweep -h` to view all options.

{{< flow title="Sweep phases" caption="Each phase runs for the configured time; the results land in one CSV." >}}
Wi-Fi AP scan | Beacons and probe responses
Wi-Fi station scan | Clients seen while hopping channels
BLE Flipper scan | Flipper Zero advertisements
BLE GATT scan | Connectable devices and their services
BLE raw scan | Raw advertising packets
802.15.4 scan (ESP32-C5/C6 only) | Zigbee/Thread-style frames
{{< /flow >}}

The export contains available Wi-Fi access points and clients, BLE observations, optional 802.15.4 packets, and optional GPS coordinates.

## Troubleshooting

- **No CSV is created:** Confirm that the SD card is mounted and has free space.
- **A radio type is missing:** Check that the board supports it and that no conflicting Wi-Fi or BLE task is active.
- **GPS fields are empty:** Confirm the GPS module is connected and has a location fix before starting the sweep.

## Related tasks

- [Scan nearby Wi-Fi networks]({{< relref "survey.md" >}})
- [GPS and Wardriving]({{< relref "../gps" >}})
