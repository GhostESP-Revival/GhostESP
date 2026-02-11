---
title: "GATT Discovery"
description: "Connect to BLE devices and enumerate their services and characteristics."
weight: 15
---

GATT defines how BLE devices expose data. This guide shows how to discover services on a device.

## What is GATT?

When a BLE device advertises, it broadcasts basic info. To learn what it actually *does*, you must connect and read its GATT profile. A GATT profile contains:

- **Services** — Logical groupings of related functionality (e.g., "Heart Rate Service")
- **Characteristics** — Individual data points within a service (e.g., "Heart Rate Measurement")
- **UUIDs** — Unique identifiers for each service and characteristic

For example, a fitness tracker might expose:
- Battery Service (UUID 0x180F) with a Battery Level characteristic
- Heart Rate Service (UUID 0x180D) with Heart Rate Measurement characteristic
- Device Information Service (UUID 0x180A) with Manufacturer Name, Model Number, etc.

## Prerequisites

- GhostESP device with Bluetooth support (not ESP32-S2)
- Target BLE device powered on and in range

## Quick Start

### On-Device UI

1. **Menu → Bluetooth → GATT Scan → Start GATT Scan** to discover devices
2. **List GATT Devices** to see what was found
3. **Select GATT Device** to pick one by index
4. **Enumerate Services** to connect and read its GATT profile
5. **Track Device** to locate it by signal strength

### Command Line

```
blescan -g              # Start GATT scan
listgatt                # List discovered devices
selectgatt 0            # Select device at index 0
enumgatt                # Connect and enumerate services
trackgatt               # Track by signal strength
blescan -s              # Stop scanning
```

## Step-by-Step Walkthrough

### Step 1: Scan for Connectable Devices

Start a GATT scan to find BLE devices that accept connections:

**UI:** Menu → Bluetooth → GATT Scan → Start GATT Scan  
**CLI:** `blescan -g`

The scan runs continuously, discovering devices as they advertise. Leave it running for 10-30 seconds to find nearby devices.

### Step 2: List Discovered Devices

View the devices found during the scan:

**UI:** Menu → Bluetooth → GATT Scan → List GATT Devices  
**CLI:** `listgatt`

Example output:
```
[0] [AirTag] 5C:94:F2:xx:xx:xx (-67 dB)
[1] [SmartTag] DC:71:96:xx:xx:xx (-72 dB)  
[2] [Tile] F1:23:xx:xx:xx:xx (-81 dB)
[3] Unknown 4A:B2:xx:xx:xx:xx (-85 dB)
```

The index number (e.g., `[0]`) is used to select a device. Tracker types are auto-detected and shown in brackets.

### Step 3: Select a Device

Choose a device for enumeration:

**UI:** Menu → Bluetooth → GATT Scan → Select GATT Device → Enter the index number  
**CLI:** `selectgatt <index>` (e.g., `selectgatt 0`)

### Step 4: Enumerate Services

Connect to the selected device and read its GATT profile:

**UI:** Menu → Bluetooth → GATT Scan → Enumerate Services  
**CLI:** `enumgatt`

GhostESP will:
1. Establish a BLE connection
2. Query all services
3. Display each service with its UUID and handle range
4. Identify known services by name

Example output:
```
Connecting to 5C:94:F2:xx:xx:xx...
Connected!

Service: Generic Access (0x1800) [handles 1-7]
Service: Generic Attribute (0x1801) [handles 8-11]
Service: Battery Service (0x180F) [handles 12-15]
Service: Device Information (0x180A) [handles 16-28]
Service: Unknown (0xFD44) [handles 29-35]
```

### Step 5: Track by Signal Strength (Optional)

Use RSSI tracking to physically locate a device:

**UI:** Menu → Bluetooth → GATT Scan → Track Device  
**CLI:** `trackgatt`

The display shows:
```
Tracking: 5C:94:F2:xx:xx:xx
█████░░░░░  -67 dB  ↑ CLOSER
Min: -89 dB | Max: -61 dB
```

- **Signal bars** — Visual strength indicator
- **dB value** — Current signal strength (closer to 0 = stronger)
- **Direction** — ↑ CLOSER or ↓ FARTHER based on recent trend
- **Min/Max** — Range seen during tracking

Walk around while watching the signal strength to locate the device.

## Automatic Tracker Detection

During scanning, GhostESP identifies common tracker types by their manufacturer data:

| Tracker Type | Description |
|--------------|-------------|
| **AirTag** | Apple AirTags |
| **Apple FindMy** | Other Apple Find My devices |
| **SmartTag** | Samsung SmartTag trackers |
| **Tile** | Tile Bluetooth trackers |
| **Chipolo** | Chipolo trackers |
| **FindMy Clone** | Third-party Find My compatible devices |

Detected types appear in brackets when listing devices, e.g., `[AirTag]`.

## Service UUID Recognition

GhostESP automatically identifies hundreds of known GATT services:

### Standard BLE Services (0x18xx)

| UUID | Service Name |
|------|--------------|
| 0x1800 | Generic Access |
| 0x1801 | Generic Attribute |
| 0x180A | Device Information |
| 0x180D | Heart Rate |
| 0x180F | Battery Service |
| 0x1812 | Human Interface Device (HID) |
| 0x1826 | Fitness Machine |

### Vendor Services

GhostESP recognizes services from:
- **Apple** — AirDrop, HomeKit, Siri, AirPlay, AirPods, Nearby
- **Google** — Fast Pair, Chromecast, Eddystone, Nearby
- **Samsung** — SmartThings, Gear
- **Tile** — Core, Ring, Identity, Firmware
- **Nordic** — UART Service (serial over BLE)
- **Flipper Zero** — Custom serial service
- **Fitbit** — Fitness tracker services
- **Xiaomi** — MiHome, Amazfit
- **Others** — Sonos, Meta/Oculus, Spotify, Tesla, Amazon Alexa

Unknown services display their raw UUID for manual lookup.

## CLI Command Reference

| Command | Description |
|---------|-------------|
| `blescan -g` | Start GATT device scan |
| `blescan -s` | Stop any active BLE scan |
| `listgatt` | List discovered GATT devices |
| `selectgatt <index>` | Select a device by index |
| `enumgatt` | Connect and enumerate services |
| `trackgatt` | Track selected device by RSSI |

## Tips

- **Connection timeouts:** Some devices sleep aggressively. Try moving closer or interacting with the device to wake it.
- **Privacy-focused devices:** Some trackers rotate their MAC addresses. The device you see may have a different address on the next scan.
- **Multiple scans:** Run several short scans rather than one long scan to catch devices that advertise infrequently.
- **Signal strength:** RSSI values are approximate. Walls, interference, and antenna orientation all affect readings.

## Troubleshooting

- **"No devices found"** — Move closer to BLE devices, or wait longer for devices to advertise.
- **"Connection failed"** — The device may be paired to another host, sleeping, or out of range. Try again.
- **"Service enumeration empty"** — Some devices require bonding/pairing before exposing services. GhostESP does passive enumeration.
- **Bluetooth not available** — ESP32-S2 devices do not have Bluetooth hardware.
