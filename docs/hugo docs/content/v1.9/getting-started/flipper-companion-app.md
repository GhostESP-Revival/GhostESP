---
title: "Flipper Zero Companion App"
description: "Control GhostESP from your Flipper Zero device using the official companion app."
weight: 30
keywords: ["flipper zero", "companion app", "mobile control", "gui", "fap"]
---

The GhostESP Flipper Zero companion app provides a comprehensive graphical interface to control and interact with your GhostESP device directly from your Flipper Zero. This app enables you to perform Wi-Fi operations, Bluetooth scanning, GPS tracking, and more without needing a computer or web browser.

## Overview

The companion app is a Flipper Application Package (`.fap`) that runs on Flipper Zero devices. It communicates with GhostESP firmware via serial/UART connection, providing an intuitive menu-driven interface for all major GhostESP features.

**Repository**: [github.com/jaylikesbunda/ghost_esp_app](https://github.com/jaylikesbunda/ghost_esp_app)

## Prerequisites

- Flipper Zero device
- GhostESP firmware flashed on an ESP32 device
- Serial/UART connection between Flipper Zero and ESP32
- Latest version of the companion app (`.fap` file)

## Installation

### Method 1: Flipper App Store (Recommended)

1. Open the **Flipper App Store** on your Flipper Zero
2. Search for "GhostESP" or "ghost_esp"
3. Install the app directly from the store
4. The app will appear in your Applications menu

### Method 2: Manual Installation

1. **Download the latest release**
   - Visit the [releases page](https://github.com/jaylikesbunda/ghost_esp_app/releases/latest)
   - Download the `.fap` file for your Flipper Zero firmware version

2. **Transfer to Flipper Zero**
   - Use **qFlipper** (desktop or mobile app) to transfer the file
   - Or copy the `.fap` file directly to your Flipper's SD card under `apps/` directory

3. **Launch the app**
   - Navigate to **Applications** → **GhostESP** on your Flipper Zero
   - The app will start and attempt to connect to your GhostESP device

## Connection Setup

### Hardware Connection

Connect your ESP32 device to your Flipper Zero via UART:

- **ESP32 TX** → **Flipper GPIO pin** (typically GPIO 13 or 15)
- **ESP32 RX** → **Flipper GPIO pin** (typically GPIO 14 or 16)
- **ESP32 GND** → **Flipper GND**
- **ESP32 VCC** → **Flipper 3.3V** (if needed, check your board's power requirements)

> **Note**: Pin configurations may vary depending on your Flipper Zero setup. Refer to your specific hardware documentation or the companion app's connection guide.

### Software Connection

1. Power on your GhostESP device
2. Launch the GhostESP app on your Flipper Zero
3. The app will automatically attempt to connect via serial

## Features

### Wi-Fi Operations

#### Scanning & Probing
- **Scan Access Points**: Discover and list nearby Wi-Fi networks
- **Scan Stations**: Find connected devices on networks
- **Probe Requests**: Listen for probe requests with channel hopping or fixed channel
- **Pineapple Detection**: Identify rogue access points
- **Channel Congestion Analysis**: View network activity per channel
- **Port Scanning**: Scan ports on local network or specific IP addresses
- **ARP Scanning**: Discover devices on local network
- **SSH Scanning**: Scan for SSH services on target IPs

#### Packet Capture
- **Multiple Capture Modes**:
  - WPS capture
  - Raw packet capture
  - Probe request capture
  - Deauthentication capture
  - Beacon capture
  - EAPOL handshake capture
  - Pwnagotchi capture
- **PCAP Export**: Save captures as PCAP files for analysis in Wireshark
- **Cycling Menu**: Easily switch between capture types

#### Beacon Spam & Attacks
- **Beacon Spam Modes**:
  - List mode (spam from predefined list)
  - Random mode (random SSIDs)
  - Rickroll mode (entertainment)
  - Custom SSID mode
- **Deauthentication Attacks**: Disconnect devices from networks
- **EAPOL Logoff Attacks**: Force re-authentication
- **SAE Handshake Flood**: WPA3 attack method
- **Karma Rogue AP**: Create fake access points with custom SSIDs
- **DHCP Starvation**: Exhaust DHCP server resources
- **Beacon List Management**: Add, remove, clear, show, and spam from lists

#### Evil Portal & Network
- **Captive Portal**: Create fake login pages
- **Portal HTML Management**: List, set, and clear custom portal HTML files
- **Wi-Fi Connection**: Connect to Wi-Fi networks
- **Network Disconnect**: Disconnect from current network
- **Network Scanning**: Scan local network for devices
- **Media Casting**: Cast random videos to Cast/DIAL devices
- **Printer Control**: Control network printers
- **Smart Plug Control**: Control TP-Link smart plugs
- **WebUI Credentials**: Configure WebUI authentication

### Bluetooth (BLE) Operations

#### Scanning & Detection
- **Skimmer Detection**: Find credit card skimmers
- **Flipper Discovery**: Locate nearby Flipper Zero devices
- **AirTag Scanning**: Scan for Apple AirTags
- **BLE Spam Detection**: Identify BLE spam attacks
- **Traffic Viewing**: View all BLE advertising traffic
- **RSSI Tracking**: Track Flipper device signal strength

#### Attacks & Spoofing
- **BLE Spam Modes**:
  - Apple device spam
  - Microsoft device spam
  - Samsung device spam
  - Google device spam
  - Random device spam
- **AirTag Spoofing**: Spoof selected AirTag devices
- **Device Selection**: Choose AirTag and Flipper devices from discovered lists

#### Packet Capture
- **Raw BLE Capture**: Capture all BLE advertising packets
- **PCAP Export**: Export BLE captures for analysis

### GPS

#### GPS Information
- **Real-time Position**: View current latitude and longitude
- **Altitude Monitoring**: Track elevation above sea level
- **Speed Tracking**: Monitor current movement speed
- **Direction**: Display heading/bearing
- **Signal Quality**: View GPS signal strength and satellite count
- **Satellite Status**: See connected satellites

#### Wardriving Capabilities
- **Wi-Fi Wardriving**: Log Wi-Fi networks with GPS coordinates (CSV export)
- **BLE Wardriving**: Log BLE devices with GPS coordinates (CSV export)
- **Combined Mapping**: Map both networks and devices on a single view

### Configuration & System

#### LED Control
- **LED Effects**:
  - Rainbow effect
  - Police strobe
  - Strobe effect
  - Fixed colors (Red, Green, Blue, Yellow, Purple, Cyan, Orange, White, Pink)
  - LED off mode
- **Custom RGB Pins**: Configure RGB LED pin assignments
- **NeoPixel Brightness**: Control brightness from 0-100%

#### SD Card Configuration
- **View Configuration**: Show current SD card pin settings
- **MMC Mode**: Set SD pins for MMC mode
- **SPI Mode**: Set SD pins for SPI mode
- **Save Configuration**: Persist settings to SD card

#### System Settings
- **Timezone Configuration**: Set device timezone
- **Wi-Fi Country Code**: Configure regulatory domain
- **Web Authentication**: Enable/disable WebUI authentication
- **Access Point Control**: Enable/disable AP across reboots
- **RGB Profile**: Select lighting profile (normal, rainbow, stealth)

#### Device Management
- **Chip Information**: View ESP32 chip details and memory usage
- **Settings Management**: List, get, set, and reset device settings
- **Device Reboot**: Restart the GhostESP device
- **Help Documentation**: Access built-in help for commands and features

## Usage Guide

### Basic Navigation

The app uses a menu-driven interface:

- **Up/Down**: Navigate through menu items
- **OK**: Select item or confirm action
- **Back**: Return to previous menu or cancel

### Common Workflows

#### Wi-Fi Handshake Capture

1. Navigate to **WiFi** → **Scanning & Probing** → **Scan APs**
2. Wait for scan to complete
3. Select target network from list
4. Go to **Packet Capture** → **EAPOL**
5. Start capture and wait for handshake
6. Export PCAP file when complete

#### Beacon Spam Attack

1. Navigate to **WiFi** → **Beacon Spam & Attacks** → **Beacon Spam**
2. Select spam mode (List, Random, Rickroll, or Custom)
3. If using List mode, manage your beacon list first
4. Start the spam attack
5. Stop when desired

#### BLE Device Scanning

1. Navigate to **Bluetooth** → **Scanning & Detection** → **Scan BLE**
2. Wait for scan to complete
3. View discovered devices
4. Select device for additional actions (spoof, track, etc.)

#### GPS Wardriving

1. Ensure GPS module is connected and receiving signal
2. Navigate to **GPS** → **Wardriving** → **WiFi Wardriving**
3. Start wardriving session
4. Drive around to collect data
5. Stop and export CSV file with GPS coordinates

### File Management

Captured files are stored on your Flipper Zero's SD card:

- **PCAP files**: `/ext/apps_data/ghost_esp/pcaps/`
- **CSV files**: `/ext/apps_data/ghost_esp/csv/`
- **Portal HTML**: `/ext/apps_data/ghost_esp/portals/`

Use **qFlipper** or remove the SD card to transfer files to your computer.

## Troubleshooting

### Connection Issues

**App can't connect to GhostESP**
- Verify serial wiring (TX, RX, GND)
- Check that GhostESP device is powered on
- Try restarting both devices
- Check serial port configuration in app settings

**Connection drops during use**
- Check wiring connections for loose connections
- Verify power supply is stable
- Move away from sources of electrical interference
- Try reducing baud rate if using high-speed connection

### Feature Not Working

**Wi-Fi scan returns no results**
- Ensure GhostESP firmware supports Wi-Fi operations
- Check that device is not in a restricted regulatory region
- Verify antenna is properly connected
- Try scanning from a different location

**GPS not working**
- Verify GPS module is connected and powered
- Ensure GPS has clear view of sky
- Wait for GPS to acquire satellite lock (may take 1-2 minutes)
- Check GPS module wiring and configuration

**BLE scan shows no devices**
- Ensure BLE is enabled in GhostESP settings
- Move closer to target devices
- Check that target devices are advertising (not in sleep mode)
- Verify BLE antenna connection

### File Export Issues

**PCAP files not saving**
- Ensure Flipper Zero SD card is inserted
- Check SD card has free space
- Verify SD card is properly formatted (FAT32)
- Check file path permissions

**CSV export fails**
- Verify SD card is mounted
- Check available storage space
- Ensure GPS data is being received (for GPS-related exports)

### App Crashes or Freezes

**App freezes during operation**
- Restart the Flipper Zero
- Update to latest app version
- Check for firmware compatibility issues
- Reduce capture size or scan duration

**App won't launch**
- Verify `.fap` file is compatible with your Flipper firmware version
- Reinstall the app from app store or latest release
- Check Flipper Zero has sufficient memory
- Update Flipper Zero firmware if outdated

## Advanced Configuration


### Custom Portal HTML

1. Create your HTML file (max 2048 bytes for app upload)
2. Navigate to **WiFi** → **Evil Portal & Network** → **Set Evil Portal HTML**
3. Select upload method
4. Enter HTML content or select file
5. Portal will use custom HTML on next start

### Beacon List Management

1. Navigate to **WiFi** → **Beacon Spam & Attacks** → **Beacon List**
2. Choose action:
   - **Add**: Add new SSID to list
   - **Remove**: Remove SSID from list
   - **Clear**: Remove all SSIDs
   - **Show**: Display current list
   - **Spam**: Start spamming from list

## Version Compatibility

The companion app is regularly updated to support new GhostESP firmware features. Always use the latest version of both:

- **GhostESP Firmware**: Latest release from main repository
- **Companion App**: Latest `.fap` from [releases](https://github.com/jaylikesbunda/ghost_esp_app/releases)

> **Note**: Older app versions may not support newer firmware features. Check the [changelog](https://github.com/jaylikesbunda/ghost_esp_app/blob/main/CHANGELOG.md) for compatibility information.

## Support and Resources

- **GitHub Repository**: [github.com/jaylikesbunda/ghost_esp_app](https://github.com/jaylikesbunda/ghost_esp_app)
- **Issues**: Report bugs or request features on the [GitHub Issues page](https://github.com/jaylikesbunda/ghost_esp_app/issues)
- **Discord**: Join the community on [Discord](https://discord.gg/5cyNmUMgwh)
- **Documentation**: Checkout the docs at [docs.ghostesp.net](docs.ghostesp.net)

## Credits

- **Original Developer**: Spooky ([Spooks4576](https://github.com/Spooks4576))
- **Maintainer**: Jay Candel ([jaylikesbunda](https://github.com/jaylikesbunda))
- **Contributor**: @tototo31 ([tototo31](https://github.com/tototo31))

## Next Steps

- Learn about [GhostESP Installation]({{< relref "installation-guide.md" >}}) if you haven't set up your device yet
- Explore [Wi-Fi Features]({{< relref "../wifi/_index.md" >}}) to understand available attacks and scans
- Check out [BLE Operations]({{< relref "../ble/_index.md" >}}) for Bluetooth capabilities
- Review the [Command Line Reference]({{< relref "command-line-reference.md" >}}) to understand underlying commands
