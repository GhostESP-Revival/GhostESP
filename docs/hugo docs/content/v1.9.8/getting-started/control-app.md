---
title: "Control App"
description: "Use the Ghost ESP Commander desktop application to control your device via serial connection."
weight: 40
---

The **Ghost ESP Commander** is a desktop GUI application that provides comprehensive control over your GhostESP device through a serial connection. It offers WiFi/BLE scanning, packet capture, custom commands, firmware flashing, and more—all from an intuitive graphical interface.

## Overview

The Control App is a Python-based application built with PyQt6 that connects to your GhostESP device via USB serial. It provides a user-friendly alternative to the command-line interface, with organized tabs for different operations and real-time feedback.

## Prerequisites

- **Python 3.8 or newer**: Required to run the application
- **USB cable**: To connect your GhostESP device to your computer
- **GhostESP firmware**: Your device must be running compatible GhostESP firmware
- **System dependencies** (Linux only, if needed): Some Linux distributions may require `libxcb-cursor0` for PyQt6 to function properly. Install it if you encounter cursor-related errors:
  ```bash
  sudo apt update
  sudo apt install libxcb-cursor0
  ```
- **ESP-IDF** (optional, for custom builds): Required only if you plan to build custom firmware. See [ESP-IDF installation instructions](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html#manual-installation)

## Installation

The Control App is located in the `scripts/control app/` directory of the GhostESP repository.

### First-Time Setup

1. **Navigate to the control app directory**:
   ```bash
   cd scripts/control\ app/
   ```

2. **Run the application**:
   ```bash
   python main.py
   ```

The application will automatically:
- Create a Python virtual environment (`ghost-control-venv`)
- Install all required dependencies from `requirements.txt`
- Launch the GUI

**Note**: On first run, the setup process may take a few minutes as dependencies are downloaded and installed.

## Connecting to Your Device

### Initial Connection

1. **Connect your GhostESP device** via USB to your computer
2. **Select the serial port** from the dropdown in the **Serial Connection** section
3. **Click Refresh Ports** if your device doesn't appear in the list
4. **Click Connect** to establish the serial connection

Once connected:
- The UI overlay will disappear, enabling all controls
- Connection status will appear in the log area
- You can now use all features of the application

### Connection Status

- **Connected**: All controls are enabled and the overlay is hidden
- **Disconnected**: A visual overlay appears with "No serial connection. Connect to enable controls."
- **Auto-reconnect**: Enable this option in settings to automatically reconnect if the connection drops

## Main Features

### WiFi Operations

The **WiFi** tab provides access to wireless network operations:

- **Scan Access Points**: Discover nearby WiFi networks
- **List APs/Stations**: View currently stored access points and stations
- **Deauth Attacks**: Start or stop deauthentication attacks on selected networks
- **Beacon Spam**: Send beacon frames with options for:
  - Random SSIDs
  - Rickroll SSIDs
  - AP list (uses stored access points)

### BLE Operations

The **BLE** tab handles Bluetooth Low Energy scanning:

- **Find Flippers**: Scan specifically for Flipper Zero devices
- **AirTag Scanner**: Detect Apple AirTags
- **Raw BLE Scan**: Perform a general BLE device scan
- **Stop Scan**: Cancel any active BLE scan

### Packet Capture

The **Capture** tab enables WiFi packet monitoring:

- **Capture Probes**: Monitor WiFi probe requests
- **Capture Deauth**: Track deauthentication packets
- **Capture WPS**: Log WPS (Wi-Fi Protected Setup) packets

### Portal Management

The **Portal** tab handles evil portal operations:

- **Upload Custom HTML**: Send a local HTML file to use as a custom portal
- **Progress Indicator**: Visual feedback during file upload
- **Portal Selection**: Choose from built-in portals or your uploaded HTML

### Custom Commands

The **Custom Command** field allows you to send any command directly to the device:

1. Type your command in the text field
2. Press **Enter** or click **Send**
3. View the response in the log and display areas

This is useful for commands not covered by the GUI tabs or for advanced operations.

## Firmware Flashing

The Control App includes three methods for flashing firmware to your device.

### Flash Firmware Panel

Flash firmware using individual binary files:

1. **Select chip type**: Choose the correct ESP32 variant (ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C5, ESP32-C6)
2. **Select files**: Browse and select:
   - `bootloader.bin`
   - `partition-table.bin`
   - `firmware.bin`
3. **Select serial port**: Choose the port your device is connected to
4. **Click Flash Board**: The flashing process will begin
5. **Exit Flash Mode**: Return to the main UI when done

### Release Bundle Flashing

Download and flash official release bundles directly from GitHub:

1. **Select release version**: Choose from available releases or select **Custom local .zip** for your own bundle
2. **Select asset**: If multiple assets are available, choose the appropriate one for your hardware
3. **Download or browse**: Either download the asset automatically or browse for a local `.zip` file
4. **Select chip type and port**: Choose your device's chip type and serial port
5. **Click Flash Bundle**: The application will extract and flash the bundle
6. **Exit Flash Mode**: Return to the main UI when complete

### Custom Build Panel

Build and flash your own firmware using ESP-IDF:

1. **SDKConfig management** (optional):
   - Copy an SDKConfig template using the **Copy SDKConfig** button
   - Edit your existing SDKConfig with the **Edit SDKConfig** button
   - Delete SDKConfig templates as needed

2. **Set target chip**: Select your device's chip type and click **Set Target**

3. **Build firmware**:
   - Click **Run Build** to compile your firmware (requires ESP-IDF in PATH)
   - Use **Run idf.py fullclean** to clean the build folder if needed

4. **Flash custom build**: Click **Flash Custom Build** to flash the compiled firmware

5. **Status indicators**: Visual indicators show the presence of:
   - ESP-IDF installation
   - SDKConfig file
   - Build folder
   - Bootloader
   - Partition table
   - Firmware binary

6. **Exit Flash Mode**: Return to the main UI when done

**Note**: Custom builds are unsupported. Use at your own risk.

**Panel Instructions**: The Flasher Output window displays step-by-step instructions for each flashing panel when you switch between them.

## Settings

The **Settings** tab allows you to configure application behavior:

- **Auto-reconnect**: Automatically attempt to reconnect if the serial connection is lost
- **Theme**: Switch between light and dark themes
- **Baud rate**: Adjust serial communication speed (default: 115200)
- **Other preferences**: Various application-specific settings

## Logging and Display

The application provides two main output areas:

### Log Area

Shows real-time command feedback with timestamps:
- Connection status messages
- Command execution results
- Error messages and warnings
- Serial communication details

### Display Area

Displays structured output for:
- WiFi scan results
- BLE scan results
- Device status information
- Formatted command responses

Both areas support ANSI color codes for better readability of terminal output.

## UI Features

### Resizable Panes

The command and display areas can be resized by dragging the splitter between them, allowing you to customize the layout to your preference.

### Visual Feedback

- **Connection overlay**: When disconnected, a semi-transparent overlay covers the main UI with a message
- **Status indicators**: Color-coded indicators show connection status and flashing panel status
- **Progress indicators**: Visual feedback for file uploads and long-running operations

### Keyboard Shortcuts

- **Enter**: Send custom command
- Standard text editing shortcuts work in all text fields

## Troubleshooting

### Cannot Connect to ESP32

- **Check the serial port**: Ensure the correct port is selected
- **Verify firmware**: Make sure your device is running compatible GhostESP firmware
- **Check power**: Ensure the device is properly powered
- **Try different USB cable**: Some cables are charge-only and don't support data
- **Check permissions** (Linux): You may need to add your user to the `dialout` group:
  ```bash
  sudo usermod -a -G dialout $USER
  ```
  Then log out and back in.

### Unexpected Disconnections

- **Check USB cable**: Try a different cable or port
- **Lower baud rate**: Try reducing the baud rate in settings
- **Enable auto-reconnect**: This will automatically attempt to reconnect
- **Check for interference**: Move away from sources of electrical interference

### Command Errors

- **Verify firmware version**: Ensure your device firmware supports the commands you're trying to use
- **Check command syntax**: Refer to the [Command Line Reference]({{< relref "command-line-reference.md" >}}) for correct syntax
- **View error messages**: Check the log area for detailed error information

### UI Overlay Covers Controls

- **This is normal**: The overlay only appears when disconnected
- **Serial controls remain accessible**: The connection controls are always available, even when the overlay is shown
- **Connect to remove overlay**: Once connected, the overlay automatically disappears

### Portal Upload Hangs

- **Verify connection**: Ensure you're connected to the device
- **Check device status**: Make sure the ESP32 is ready and not performing other operations
- **Try again**: Cancel and retry the upload
- **Check file size**: Very large HTML files may take longer to upload

### ESP-IDF Not Found (Custom Build)

- **Install ESP-IDF**: Follow the [official installation guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html#manual-installation)
- **Add to PATH**: Ensure `idf.py` is accessible from your system PATH
- **Verify installation**: Run `idf.py --version` in a terminal to confirm it's working

### Missing SDKConfig or Build Files

- **Use status indicators**: The Custom Build panel shows which files are missing
- **Copy SDKConfig template**: Use the **Copy SDKConfig** button to create a template
- **Run Set Target**: Ensure you've set the target chip type
- **Check build folder**: Verify that the build completed successfully

### Application Won't Start

- **Check Python version**: Ensure you have Python 3.8 or newer (`python --version`)
- **Install system dependencies** (Linux): Install `libxcb-cursor0` if missing
- **Check virtual environment**: The app should create it automatically, but you can delete `ghost-control-venv` and restart to recreate it
- **View error messages**: Check the terminal output for specific error messages

## Tips and Best Practices

### Efficient Workflow

- **Keep connected**: Maintain the serial connection for best performance
- **Use tabs**: Organize your work using the different feature tabs
- **Monitor logs**: Keep an eye on the log area for important messages
- **Custom commands**: Use the custom command field for quick operations not covered by tabs

### Firmware Flashing

- **Use release bundles**: For official firmware, the Release Bundle panel is the easiest method
- **Verify chip type**: Always select the correct chip type before flashing
- **Read instructions**: The Flasher Output window provides helpful guidance for each panel
- **Backup first**: If possible, backup your current firmware before flashing

### Performance

- **Close other serial apps**: Don't run multiple applications that access the same serial port
- **Adjust baud rate**: If experiencing issues, try a lower baud rate
- **Resize panes**: Adjust the display/log splitter to optimize your workflow

## Next Steps

- Explore the [Command Line Reference]({{< relref "command-line-reference.md" >}}) to learn about all available commands
- Set up [GhostLink]({{< relref "dual-communication.md" >}}) for dual-device operations
- Review [Wi-Fi Basics]({{< relref "../wifi/basics.md" >}}) to understand wireless attacks
- Check out [BLE Scanning]({{< relref "../ble/scanning.md" >}}) for Bluetooth operations

## Notes

- **Development tool**: This application is designed for development and diagnostics
- **Responsible use**: Always comply with local regulations when using network diagnostic tools
- **Cross-platform**: The application works on Windows, macOS, and Linux
- **Automatic setup**: Dependencies are installed automatically on first run
- **Virtual environment**: The app uses its own Python virtual environment to avoid conflicts with system packages

