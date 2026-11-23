---
title: "GATT Discovery"
description: "Connect to BLE devices and enumerate their services and characteristics"
weight: 15
---

Connect to a BLE device and enumerate its services and characteristics. You need a Bluetooth-capable GhostESP (not ESP32-S2) and the target device powered on and in range.

## What Is GATT?

When a BLE device advertises, it broadcasts basic info. To learn what it actually *does*, you must connect and read its GATT profile. A GATT profile contains:

- **Services** - Logical groupings of related functionality (e.g., "Heart Rate Service")
- **Characteristics** - Individual data points within a service (e.g., "Heart Rate Measurement")
- **UUIDs** - Unique identifiers for each service and characteristic

For example, a fitness tracker might expose:
- Battery Service (UUID 0x180F) with a Battery Level characteristic
- Heart Rate Service (UUID 0x180D) with Heart Rate Measurement characteristic
- Device Information Service (UUID 0x180A) with Manufacturer Name, Model Number, etc.

{{< flow title="Enumeration sequence" caption="Only connectable advertisers are listed; pairing is attempted when the link is not already encrypted." >}}
Scan for connectable devices | Only `ADV_IND` and `DIR_IND` are listed (up to 20 devices)
Select a device by index | `selectgatt <index>`
Connect and pair if needed | Pairing starts when the link is not encrypted
Discover services | Up to 8 services per device, with UUID and handle range
Read known characteristics | Device Information, Battery Level, and Current Time are decoded
Track RSSI (optional) | `trackgatt` logs live signal strength
{{< /flow >}}

## Walkthrough

### Step 1: Scan for connectable devices

Start a GATT scan to find BLE devices that accept connections:

**UI:** BLE → GATT Scan (starts scanning directly)  
**CLI:** `blescan -g`

The scan runs continuously, discovering devices as they advertise. Leave it running for 10-30 seconds to find nearby devices.

### Step 2: List discovered devices

View the devices found during the scan:

**UI:** BLE → GATT Scan → List GATT Devices  
**CLI:** `listgatt`

Example output:
```
[0] Name: <unknown>,
     MAC: 5C:94:F2:xx:xx:xx,
     RSSI: -67,
     Type: AirTag,
[1] Name: SmartTag,
     MAC: DC:71:96:xx:xx:xx,
     RSSI: -72,
     Type: SmartTag,
[2] Name: <unknown>,
     MAC: F1:23:xx:xx:xx:xx,
     RSSI: -81,
     Type: Tile,
```

The index number (e.g., `[0]`) is used to select a device. When a device advertises no name, `Name:` shows `<unknown>`. The `Type:` line appears only when a tracker type was auto-detected.

### Step 3: Select a device

Choose a device for enumeration:

**UI:** BLE → GATT Scan → Select GATT Device → Enter the index number  
**CLI:** `selectgatt <index>` (e.g., `selectgatt 0`)

### Step 4: Enumerate services

Connect to the selected device and read its GATT profile:

**UI:** BLE → GATT Scan → Enumerate Services  
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

### Step 5: Track by signal strength (optional)

Use RSSI tracking to physically locate a device:

**UI:** BLE → GATT Scan → Track Device  
**CLI:** `trackgatt`

The display prints one line per RSSI update:
```
[####] RSSI: -67 dBm, Min: -89, Max: -61, Close: 45%, CLOSER
```

- **Signal bars** - Visual strength indicator (1-5 `#` characters)
- **RSSI** - Current signal strength (closer to 0 = stronger)
- **Min/Max** - Range seen during tracking
- **Close** - How close the current reading is to the strongest reading seen, as a percentage
- **Direction** - `CLOSER` or `FARTHER` when the trend has moved more than 5 dBm; otherwise omitted

Walk around while watching the signal strength to locate the device. See the [CLI Reference]({{< relref "../getting-started/command-line-reference.md" >}}) for the full BLE command list.

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

Detected types are shown on the `Type:` line when listing devices, e.g., `Type: AirTag`.

## Service UUID Recognition

GhostESP automatically identifies known GATT services:

### Standard BLE Services (0x18xx)

| UUID | Service Name |
|------|--------------|
| 0x1800 | Generic Access |
| 0x1801 | Generic Attribute |
| 0x1805 | Current Time Service |
| 0x180A | Device Information |
| 0x180F | Battery Service |
| 0x1811 | Alert Notification |
| 0x1812 | Human Interface Device (HID) |
| 0x1802 | Immediate Alert |
| 0x1803 | Link Loss |
| 0x1804 | Tx Power |

### Vendor Services

GhostESP recognizes services from:
- **Huawei** - 0xFEE0, 0xFEE1
- **Tencent** - 0xFEE7
- **Xiaomi** - 0xFEE8
- **Tile** - 0xFEED, 0xFEEC

Unknown services display their raw UUID for manual lookup. 128-bit UUIDs are shortened to their first four bytes followed by `-...` (for example `AABBCCDD-...`).

When present, the Device Information characteristics (Manufacturer, Model, Serial, Firmware, Hardware, Software), the Battery Level characteristic, and the Current Time characteristic are read and decoded and printed beneath the service.

## Tips

- **Connection timeouts:** Some devices sleep aggressively. Try moving closer or interacting with the device to wake it.
- **Privacy-focused devices:** Some trackers rotate their MAC addresses. The device you see may have a different address on the next scan.
- **Multiple scans:** Run several short scans rather than one long scan to catch devices that advertise infrequently.
- **Signal strength:** RSSI values are approximate. Walls, interference, and antenna orientation all affect readings.
- **Device limit:** GATT scan stores up to **20 devices**, each with up to **8 services**; devices found after the limit is reached are not added.

## Troubleshooting

- **"No devices found"** - Move closer to BLE devices, or wait longer for devices to advertise.
- **"Connection failed"** - The device may be paired to another host, sleeping, or out of range. Try again.
- **"Service enumeration empty"** - Some devices require bonding/pairing before exposing services. GhostESP actively initiates pairing during GATT enumeration (auto-confirming numeric comparison) to read protected services.
- **Bluetooth not available** - ESP32-S2 devices do not have Bluetooth hardware.
