---
title: "Configuration & System"
description: "LED control, SD card configuration, and system settings in the Flipper Zero companion app."
weight: 60
keywords: ["configuration", "settings", "led", "rgb", "sd card", "system"]
---

## LED Control

- **LED Effects**:
  - Rainbow effect
  - Police strobe
  - Strobe effect
  - Fixed colors (Red, Green, Blue, Yellow, Purple, Cyan, Orange, White, Pink)
  - LED off mode
- **Custom RGB Pins**: Configure RGB LED pin assignments
- **NeoPixel Brightness**: Control brightness from 0-100%

## SD Card Configuration

- **View Configuration**: Show current SD card pin settings
- **MMC Mode**: Set SD pins for MMC mode
- **SPI Mode**: Set SD pins for SPI mode
- **Save Configuration**: Persist settings to SD card

## System Settings

- **Timezone Configuration**: Set device timezone
- **Wi-Fi Country Code**: Configure regulatory domain
- **Web Authentication**: Enable/disable WebUI authentication
- **Access Point Control**: Enable/disable AP across reboots
- **RGB Profile**: Select lighting profile (normal, rainbow, stealth)

## Device Management

- **Chip Information**: View ESP32 chip details and memory usage
- **Settings Management**: List, get, set, and reset device settings
- **Device Reboot**: Restart the GhostESP device
- **Help Documentation**: Access built-in help for commands and features

## File Storage

Captured files are stored on your Flipper Zero's SD card:

| File Type | Location |
|-----------|----------|
| PCAP files | `/ext/apps_data/ghost_esp/pcaps/` |
| CSV files | `/ext/apps_data/ghost_esp/csv/` |
| Portal HTML | `/ext/apps_data/ghost_esp/portals/` |

Use **qFlipper** or remove the SD card to transfer files to your computer.
