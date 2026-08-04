---
title: "Advertiser Scan"
description: "Scan and parse BLE advertisements, including iBeacon packets, without connecting to devices."
weight: 12
---

BLE advertisers broadcast data before any connection is made. This is where non-connectable beacons, iBeacons, Eddystone frames, tracker broadcasts, and many nearby BLE devices appear.

Advertiser scan is different from GATT discovery: GATT requires connecting to a device, while advertiser scan only listens for broadcast packets. Many beacons, including typical iBeacons, will not show useful data in a GATT scan because they are designed to be detected from advertisements.

## What It Shows

The parsed advertiser list includes:

- MAC address and RSSI
- BLE address type, such as public or random
- OUI vendor lookup from GhostESP's embedded OUI database when the address uses a recognizable public OUI
- Advertisement type, such as connectable, scannable, non-connectable, or scan response
- Device name when present
- Manufacturer ID and known vendor name when recognized
- 16-bit service UUID summaries (up to 4 per advertisement)
- 16-bit service-data UUID summaries, including Eddystone frame hints
- BLE Appearance value when present
- iBeacon UUID, major, minor, and measured power when an iBeacon frame is detected
- Seen count for repeated advertisements

## On-Device UI

1. Open **Menu → Bluetooth**.
2. Choose **Advertiser Scan**.
3. Let it run until enough nearby advertisers are found.
4. Press **Back** or another input to stop the scan.
5. Choose **List Advertisers** to browse parsed results.
6. Select an advertiser row to view full parsed details.
7. Use **Track** in the detail view to monitor RSSI for that advertiser.
8. Use **Save to SD** in the detail view to write that advertiser's parsed record to the SD card.

## Command Line

Start the parsed advertiser scan:

```text
blescan -adv
```

Stop scanning:

```text
blescan -s
```

Print parsed results:

```text
listadv
```

## iBeacons

iBeacons are Apple manufacturer-data advertisements. GhostESP identifies the standard iBeacon frame under company ID `0x004C`, beacon type `0x02`, and length `0x15`.

When detected, GhostESP extracts:

- iBeacon UUID
- Major value
- Minor value
- Measured power
- Current RSSI

RSSI can help estimate near/far movement, but it is not precise positioning. A single ESP32 cannot determine exact beacon location or direction without additional measurements or hardware.

## Native App API

Native SD apps with BLE permission can access the advertiser scanner through the plugin API:

```c
api->ble_adv_scan_start();
bool active = api->ble_adv_scan_active();
int count = api->ble_adv_scan_count();

ghostesp_ble_adv_info_t info;
if (api->ble_adv_scan_get(0, &info)) {
    // info contains parsed advertiser fields and iBeacon details when present.
}

api->ble_adv_scan_track(0);
api->ble_adv_scan_stop_tracking();

// index < 0 saves the full advertiser list, >= 0 saves only that index.
if (api->ble_adv_scan_save_to_sd(0)) {
    // scan_file_* handles JIT mount for the Banshee C5 ("Banshee_C5") build
    // and writes to /mnt/ghostesp/scans/ble_advertiser_<n>.txt
}

api->ble_adv_scan_stop();
```

The scanner stores compact parsed records internally and returns a caller-owned snapshot through `ghostesp_ble_adv_info_t`.

## Saving to SD

The **Save to SD** action in the advertiser detail view writes a parsed record for the selected advertiser to `/mnt/ghostesp/scans/ble_advertiser_<n>.txt`. Pass `index < 0` to `ble_adv_scan_save_to_sd` from native apps to save the full list as `ble_advertisers_<n>.txt` instead.

Saving goes through the same `scan_file_*` helper that other scans use, so it:

- Auto-increments the trailing index when previous files already exist.
- Calls `sd_card_mount_for_flush` on the Banshee C5 (`Banshee_C5`) build template so the SD card mounts just-in-time, then unmounts after the write.
- Calls `sd_card_setup_directory_structure` if needed so `/mnt/ghostesp/scans` exists.
- Reports success through the on-screen toast, matching other scan saves.

## Notes

- BLE advertiser scan is not available on ESP32-S2.
- Starting BLE scans temporarily pauses GhostNet Wi-Fi services.
- Results are keyed by BLE address; devices that rotate addresses may appear as multiple advertisers.
- GATT discovery is still the right tool when you need to connect and enumerate services.
