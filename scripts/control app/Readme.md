# Ghost ESP Control Panel - README

## Overview

The **Ghost ESP Commander** is a GUI application for controlling and communicating with an ESP32 microcontroller over a serial connection. Built with Python and PyQt6, it provides WiFi/BLE scanning, packet capture, custom commands, firmware flashing, and more.

## Features

- **Serial Connection Management**: Connect/disconnect to ESP32 devices via serial port.
- **WiFi Operations**: Scan networks, list APs/stations, de-auth, beacon spam, and more.
- **BLE Operations**: Scan for BLE devices, find Flippers/AirTags, stop scans.
- **Packet Capture**: Capture various WiFi packet types.
- **Custom Command Support**: Send any command directly.
- **Logging and Display**: Real-time logs and structured scan/status display.
- **Auto-Reconnect**: Optionally reconnect if the serial connection drops.
- **UI Lock/Overlay**: The UI disables and shows a visual overlay when not connected.
- **Resizable Panes**: Command and display areas can be resized.
- **Portal File Upload**: Upload custom HTML portals with progress indication.
- **Automatic Virtual Environment & Dependency Install**: The app will create a Python venv and install dependencies on first run.
- **Color Terminal Support**: Terminal output supports ANSI color codes for better readability.
- **Integrated Firmware Flasher**: Flash official or custom firmware images to your ESP32.
- **Release Bundle Download & Flash**: Download and flash official release bundles directly from GitHub.
- **Custom Build Panel**: Build, clean, and flash your own firmware using ESP-IDF, with SDKConfig management.
- **Status Indicators**: Visual indicators for ESP-IDF, sdkconfig, build folder, bootloader, partition table, and firmware presence.
- **Panel-Specific Instructions**: The flasher output window displays usage instructions for each panel.
- **Auto-Detect Chip Type**: Chip type is auto-selected based on SDKConfig or asset name.
- **One-Click SDKConfig Management**: Copy, delete, and edit SDKConfig templates with dedicated buttons.
- **Cross-Platform Terminal Launch**: `idf.py menuconfig` opens in a new terminal and closes automatically when done.

## Table of Contents

- [Installation](#installation)
- [Usage](#usage)
  - [Starting the Application](#starting-the-application)
  - [Connecting to ESP32](#connecting-to-esp32)
  - [Firmware Flashing](#firmware-flashing)
  - [Release Bundle Flashing](#release-bundle-flashing)
  - [Custom Build Panel](#custom-build-panel)
  - [Available Operations](#available-operations)
  - [Logging and Display](#logging-and-display)
- [Code Structure](#code-structure)
- [UI](#ui)
- [Troubleshooting](#troubleshooting)

## Installation

### Prerequisites

1. **Python 3.8+**: Install Python 3.8 or later.
2. **System Dependencies**:
   ```bash
   sudo apt update
   sudo apt install libxcb-cursor0
   ```
3. idf.py must be installed and in your path. [See their Manual install instructions](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html#manual-installation)
4. **Ghost ESP Firmware**: Flash your ESP32 with compatible firmware.

## Usage

### Starting the Application

```bash
python main.py
```

### Connecting to ESP32

1. Select a serial port in the **Serial Connection** section.
2. Click **Refresh Ports** if needed.
3. Click **Connect**.
   - The UI will unlock and overlay will disappear when connected.
   - Status and errors are shown in the log area.

---

### Firmware Flashing

- **Flash Firmware Panel**:  
  1. Select the correct chip type for your board.
  2. Browse and select the `bootloader.bin`, `partition-table.bin`, and `firmware.bin` files.
  3. Select the serial port.
  4. Click **Flash Board** to flash your ESP32.
  5. Use **Exit Flash Mode** to return to the main UI.
  - **Instructions for each panel are shown in the Flasher Output window when you switch panels.**

### Release Bundle Flashing

- **Flash Release Bundle Panel**:  
  1. Select a release version or choose **Custom local .zip** to use your own bundle.
  2. Select the desired asset if multiple are available.
  3. Download the asset or browse for a local `.zip` file.
  4. Select the chip type and serial port.
  5. Click **Flash Bundle** to flash your ESP32.
  6. Use **Exit Flash Mode** to return to the main UI.

### Custom Build Panel

- **Custom Build Panel**:  
  1. (Optional) Copy an SDKConfig template or edit your existing one with the edit SDKConfig button.
  2. Set the target chip and run **Set Target**.
  3. Use **Run Build** to compile your firmware (requires ESP-IDF in PATH).
  4. Use **Run idf.py fullclean** to clean the build folder if needed.
  5. Click **Flash Custom Build** to flash the built firmware from the build folder.
  6. Use **Exit Flash Mode** to return to the main UI.
  - **Status indicators** show if ESP-IDF, sdkconfig, build folder, bootloader, partition table, and firmware are present.
  - **Note:** Use at your own risk. Support will not be provided for unofficial images.

---

### Available Operations

#### WiFi Operations

- **Scan Access Points**: Find nearby WiFi APs.
- **Start/Stop Deauth**: Deauthenticate selected APs.
- **Beacon Spam**: Send random, Rickroll, or AP list beacons.

#### BLE Operations

- **Find Flippers**: Scan for Flipper BLE devices.
- **AirTag Scanner**: Detect AirTags.
- **Raw BLE Scan**: Low-level BLE scan.

#### Packet Capture

- **Capture Probes**: Detect WiFi probe requests.
- **Capture Deauth**: Track deauth packets.
- **Capture WPS**: Log WPS packets.

#### Portal File Upload

- **Send Local HTML as Portal**: Upload a custom HTML file as an evil portal.
- Progress is shown with an indicator/spinner.
- After upload, the portal dropdown updates to "uploaded html".

#### Custom Commands

- Type a command in the **Custom Command** field.
- Press **Enter** or click **Send**.

### Logging and Display

- **Log Area**: Shows timestamps and command feedback.
- **Display Area**: Shows scan results, status, and structured responses.

## Code Structure

- **`SerialMonitorThread`**: Reads serial data in a thread, emits via `data_received`.
- **`PortalFileSenderThread`**: Uploads portal files in a thread, emits progress and completion.
- **`ESP32ControlGUI`**: Main GUI class, sets up UI, handles events, manages commands and flashing.
  - **UI Components**: Tabs for WiFi, BLE, capture, portal, settings, and flashing.
  - **Overlay**: Visual indicator when not connected.
  - **Resizable Panes**: Uses splitters for flexible layout.
  - **Status Indicators**: Shows ESP-IDF, sdkconfig, build, and firmware status in the Custom Build panel.
  - **Panel Instructions**: Flasher Output window gives step-by-step instructions for each flashing panel.

## UI

![ui](01.png)

## Troubleshooting

- **Cannot Connect to ESP32**: Check port, firmware, and power.
- **Unexpected Disconnects**: Check cable, try lower baud rate, enable auto-reconnect.
- **Command Errors**: Ensure commands match firmware.
- **UI Overlay Covers Controls**: Overlay only covers main UI; serial controls always accessible.
- **Portal Upload Hangs**: Make sure you are connected and the ESP32 is ready.
- **ESP-IDF Not Found**: Ensure `idf.py` is in your PATH for custom build features.
- **Missing sdkconfig or Build Files**: Use the status indicators in the Custom Build panel to diagnose missing files.

---

**Note**: This application is for development and diagnostics. Use responsibly and comply with local regulations when using network diagnostic tools.
