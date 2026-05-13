---
title: "Scanning"
description: "Discover and analyze nearby Bluetooth Low Energy devices."
weight: 10
---

Discover nearby BLE devices and gather information about them.

> **Wi-Fi impact:** Starting any BLE scan temporarily suspends the GhostNet access point. Wi-Fi services resume automatically once you stop scanning (for example by running `stop` or pressing **Back** in the UI).

## Prerequisites

- GhostESP flashed device, powered on with a wireless antenna.
- Device must support Bluetooth (not available on ESP32-S2).

## Scanning for devices

### On-device UI

1. Open **Menu → Bluetooth**.
2. Choose a scanning mode from the options below.
   The device will start scanning. Leave it running until you have enough data.
3. Back out of the terminal view to stop scanning.
   The device will show a summary of discovered devices.

### Command line

1. Open the GhostESP terminal.
2. Run `blescan [OPTION]` where the option is one of the modes below (for example, `blescan -f`).
   The device will start scanning.
3. Run `blescan -s` when you're done.
   The device will stop scanning and show a summary.

## Scanning modes

### Find Flippers
- **UI**: Menu → Bluetooth → Find Flippers
- **CLI**: `blescan -f`
- Scans for nearby Flipper Zero devices and displays their names and signal strength.
- After scanning, use **Select Flipper** to track a specific device's RSSI (signal strength) in real time as you move around.

### AirTag Scanner
- **UI**: Menu → Bluetooth → Start AirTag Scanner
- **CLI**: `blescan -a`
- Scans for Apple AirTags and other Find My devices using active scanning to find more devices.
- Allows duplicate advertisements to be reported for better tracking.
- While scanning, RSSI for already discovered AirTags is logged every few seconds to help you see proximity changes over time.
- Use **List AirTags** to see discovered devices, or **Select AirTag** to prepare for spoofing.

### BLE Skimmer Detection
- **UI**: Menu → Bluetooth → BLE Skimmer Detect
- **CLI**: `capture -skimmer`
- Scans for payment terminal skimmers that use BLE to exfiltrate card data.
- Logs detected skimmers to a PCAP file for analysis.

### GATT Service Enumeration
- **UI**: Menu → Bluetooth → GATT Scan → Start GATT Scan
- **CLI**: `blescan -g`
- Connect to BLE devices and discover what services they offer (e.g., heart rate, battery level, custom services).
- Track devices by signal strength to physically locate them.

See the dedicated [GATT Discovery]({{< relref "gatt" >}}) page for a full walkthrough, command reference, and service UUID tables.

## Listing and selecting devices

After scanning, you can interact with discovered devices:

### List discovered devices
- **Flippers**: Menu → Bluetooth → Flipper → List Flippers or CLI: `listflippers`
- **AirTags**: Menu → Bluetooth → AirTag → List AirTags or CLI: `listairtags`
- **GATT Devices**: Menu → Bluetooth → GATT Scan → List GATT Devices or CLI: `listgatt`

### Select a device for further action
- **Flipper**: Menu → Bluetooth → Flipper → Select Flipper
  - Once selected, the device will continuously track and display the Flipper's RSSI (signal strength).
  - Use this to locate the Flipper by moving around and watching the signal strength change.
- **AirTag**: Menu → Bluetooth → AirTag → Select AirTag (prepares for spoofing)
- **GATT Device**: Menu → Bluetooth → GATT Scan → Select GATT Device
  - Once selected, use **Enumerate Services** to connect and discover the device's GATT services.
  - Use **Track Device** to locate it using real-time signal strength updates.

## Notes

- BLE scanning is not available on ESP32-S2 devices.
- Scanning modes are mutually exclusive; starting a new scan will stop the previous one.
- Signal strength (RSSI) is displayed in dBm; higher values (closer to 0) indicate stronger signals.
- Some devices may not respond to all scanning modes depending on their BLE implementation.

## Troubleshooting

- **No devices found**: Move closer to BLE devices and try scanning again.
- **Scanning stops immediately**: Check that your device has Bluetooth enabled. Remember that the AP pauses during scans—wait a moment after stopping for Wi-Fi to return.
- **Device not responding**: Some devices may be in sleep mode or have BLE disabled. Try scanning again or move closer.
- **Bluetooth not supported**: Ensure you're using a device other than ESP32-S2, which does not have Bluetooth support.
