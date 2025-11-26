---
title: "Attacks"
description: "Send BLE advertisement spam and spoof AirTags."
weight: 20
---

Broadcast fake BLE advertisements to simulate nearby devices or spoof Apple AirTags and other Find My devices.

## Prerequisites

- GhostESP flashed device, powered on with a wireless antenna.
- Device must support Bluetooth (not available on ESP32-S2).


## Legal and ethical

### Patches and Mitigations

BLE spam attacks have been largely patched in recent device updates:
- **iOS**: Apple addressed the issue in iOS 17.2 and later with stability improvements and crash fixes.
- **Android**: Google patched BLE spam vulnerabilities in Android 14 and later.
- **Windows**: Microsoft has implemented protections in recent Windows versions.

However, older devices and firmware versions remain vulnerable. These attacks are primarily useful for security research and testing on controlled devices.

### Safety Warnings

**Do not use BLE attacks in public or near other people.** BLE spam can:
- Cause nearby devices to crash or become unresponsive.
- Interfere with medical devices (pacemakers, insulin pumps, hearing aids, etc.) that rely on Bluetooth connectivity.
- Disrupt emergency communication systems.
- Cause unexpected behavior in connected vehicles and smart home systems.
- Violate local regulations regarding electromagnetic interference.

Only use these features:
- In isolated test environments with devices you own or have explicit permission to test.
- For authorized security research and penetration testing.
- With full understanding of the legal implications in your jurisdiction.

## BLE Advertisement Spam

Broadcast fake BLE advertisements to simulate nearby devices.

### On-device UI

1. Open **Menu → Bluetooth → Spam**.
   You should see the spam options.
2. Choose a spam type from the options below (for example, **BLE Spam - Apple**).
   The device will start broadcasting advertisements. Leave it running as long as you want.
3. Select **Stop BLE Spam** to stop broadcasting.
   The device will show a summary of packets sent.

### Command line

1. Open the GhostESP terminal.
2. Run `blespam [TYPE]` where the type is one of the modes below (for example, `blespam -apple`).
   The device will start broadcasting.
3. Run `stop` when you're done.
   The device will stop and show a summary.

### Spam modes

#### Apple Device Spam
- **UI**: Menu → Bluetooth → Spam → BLE Spam - Apple
- **CLI**: `blespam -apple`
- Broadcasts fake Apple device advertisements (AirPods, Apple TV, HomePod, AirTags, etc.).
- Uses Apple's Continuity Protocol with randomized device types and colors.
- Nearby Apple devices may show pairing prompts or connection notifications.

#### Microsoft Swift Pair Spam
- **UI**: Menu → Bluetooth → Spam → BLE Spam - Microsoft
- **CLI**: `blespam -ms` or `blespam -microsoft`
- Broadcasts fake Microsoft device advertisements with random device names.
- Targets Windows devices with Swift Pair notifications.

#### Samsung Device Spam
- **UI**: Menu → Bluetooth → Spam → BLE Spam - Samsung
- **CLI**: `blespam -samsung`
- Broadcasts fake Samsung Galaxy Watch and other Samsung device advertisements.
- Targets Android devices with Samsung pairing prompts.

#### Google Fast Pair Spam
- **UI**: Menu → Bluetooth → Spam → BLE Spam - Google
- **CLI**: `blespam -google`
- Broadcasts fake Google device advertisements using Google's Fast Pair protocol.
- Targets Android devices with Google pairing notifications.

#### Random Spam
- **UI**: Menu → Bluetooth → Spam → BLE Spam - Random
- **CLI**: `blespam -random`
- Cycles through all spam types (Apple, Microsoft, Samsung, Google) randomly.
- Broadcasts a mix of different device types to maximize disruption.

## AirTag Spoofing

Spoof Apple AirTags and other Find My devices to broadcast their location. This makes your device appear as a legitimate AirTag to nearby Apple devices.

### Workflow

1. **Scan for AirTags**
   - Open **Menu → Bluetooth → AirTag → Start AirTag Scanner** or run `blescan -a`.
   - Wait for the scan to complete.

2. **List discovered AirTags**
   - Open **Menu → Bluetooth → AirTag → List AirTags** or run `list -airtags`.
   - You should see a list of discovered AirTags with their index numbers.

3. **Select an AirTag to spoof**
   - Open **Menu → Bluetooth → AirTag → Select AirTag**.
   - Enter the index number of the AirTag you want to spoof.

4. **Start spoofing**
   - Open **Menu → Bluetooth → AirTag → Spoof Selected AirTag** or run `spoofairtag`.
   - The device will broadcast as the selected AirTag.
   - Nearby Apple devices will see your device as that AirTag.

5. **Stop spoofing**
   - Open **Menu → Bluetooth → AirTag → Stop Spoofing** or run `stopspoof`.
   - The device will stop broadcasting the AirTag advertisement.


## Notes

- BLE attacks are not available on ESP32-S2 devices.
- Spam attacks and spoofing are mutually exclusive; starting one will stop the other.
- The device broadcasts continuously until you explicitly stop it.
- Spam packet counts are logged every 5 seconds to the terminal.
- Apple spam uses different advertising intervals (~100ms) than other spam types for better compatibility.
- Spoofing captures the full AirTag advertisement payload during scanning for accurate reproduction.


## Troubleshooting

- **Attacks not working**: Check that your device has Bluetooth enabled and sufficient free memory. Try turning off the GhostNet AP and rebooting to conserve memory.
- **No AirTags found**: Move closer to Apple devices with Find My enabled or try scanning again.
- **Spoofing doesn't appear on Apple devices**: Ensure you've selected a valid AirTag before starting spoofing. Try stopping and restarting the spoof.
- **Bluetooth not supported**: Ensure you're using a device other than ESP32-S2, which does not have Bluetooth support.
