---
title: "Aerial Detection"
description: "Detect drones, controllers, and OpenDroneID broadcasts with sequential Wi-Fi/BLE sweeps"
weight: 60
---

GhostESP's aerial detector watches both Wi-Fi and BLE airspace to find Remote ID beacons, DJI links, and drone-branded networks. It alternates radios so scans run reliably on every ESP32 board. You need a board with both Wi-Fi and BLE (ESP32-S2 can only run the Wi-Fi phase) and serial, GhostLink display, or WebUI terminal access; a GPS module is optional for pairing detections with coordinates.

## How it works

- **Phase 1 - Wi-Fi sniffing**: Promiscuous capture with channel hopping looks for OpenDroneID NAN frames, DJI OUIs, and drone SSIDs.
- **Phase 2 - BLE scanning**: Wi-Fi is suspended so BLE can search for OpenDroneID (UUID `0xFFFA`) and DJI advertisements (UUID `0xFFE0`).
- **Automatic recovery**: When BLE stops, GhostNet AP and STA sessions return without user interaction.

{{< flow title="Order of an aerial sweep" caption="The radios take turns; Wi-Fi comes back automatically after the BLE phase." >}}
Wi-Fi phase: promiscuous sniff + channel hop | Looks for OpenDroneID NAN frames, DJI OUIs, and drone SSIDs
Suspend Wi-Fi, start the BLE scan | Frees the radio for BLE
BLE phase: listen for Remote ID adverts | OpenDroneID (`0xFFFA`) and DJI (`0xFFE0`)
Restore AP and STA | GhostNet and any STA session return automatically
{{< /flow >}}

## How to use

### Via terminal

1. **Scan** for aerial devices (default 30 seconds):
   ```
   aerialscan 30
   ```
2. **Review** captured drones:
   ```
   aeriallist
   ```
3. **Track** one device by index or MAC:
   ```
   aerialtrack 0
   ```
4. **Stop** scanning or tracking at any time:
   ```
   aerialstop
   ```

### Via display

1. **Open** the on-device **BLE** menu.
2. **Select** **Aerial Detector**.
3. **Choose** an action such as `Scan Aerial Devices`, `List Aerial Devices`, or `Track Aerial Device`.
4. **Wait** for the command to finish in the terminal pane, then review the results card.

> **Note for Flipper app:** The Aerial Detector submenu is under the **Wi-Fi** category (not BLE) in the Flipper UI. On-device displays keep it under BLE.

## What gets detected

- **OpenDroneID Remote ID (ASTM F3411)** - Wi-Fi NAN frames and BLE service `0xFFFA` decode BasicID, Location, System, SelfID, and OperatorID messages.
- **DJI protocols** - MAC OUI matching and BLE service `0xFFE0` flag Mavic, Phantom, Inspire, and other DJI links.
- **Drone SSIDs** - Pattern matching for DJI, Parrot, Autel, Skydio, and FPV networks.

Each device record stores MAC, vendor, RSSI, GPS/altitude (if broadcast), operator position, and flight status so you can triage real aircraft versus test spoofers.

## Spoofing Remote ID for testing

Use the spoof workflow to broadcast a test drone and confirm your detector (or another receiver) reacts:

```
aerialspoof DRONE-TEST 37.7749 -122.4194 120
```

- Sends alternating BasicID and Location frames once per second.
- Wi-Fi remains suspended until you run `aerialspoofstop`.
- Pair a second GhostESP, run `aerialscan`, and verify it lists `DRONE-TEST`.

## Troubleshooting

- **“Wi-Fi suspended” message**: Normal during BLE phase or spoofing. It clears after the command finishes or you run `aerialstop`.
- **No detections**: Increase scan time (`aerialscan 60`) so each phase lasts longer, or move closer to the suspected drone.
- **ESP32-S2 board**: Only Wi-Fi detections work; BLE phase is skipped.
- **Heap errors during repeated scans**: Restart your device.
