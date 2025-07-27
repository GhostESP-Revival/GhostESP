# Installation Guide

This guide provides detailed instructions for installing GhostESP firmware on your ESP32 device.

## Prerequisites

Before starting, ensure you have:

- A compatible ESP32 board (see [Supported Hardware](About.md#supported-hardware))
- A modern web browser (Google Chrome recommended, as Firefox doesn't support WebSerial)
- Appropriate USB cable (Micro USB or USB-C, ensure it's a data cable, not a charge-only cable)
- **File Extraction Tool**: Install [7-Zip](https://www.7-zip.org/download.html) or a similar program for extracting firmware files.
- Internet connection for downloading firmware
- If using a Flipper Zero, ensure it has the latest firmware for compatibility

> **Note**: For best results, disable any VPN or firewall that might block the flashing process, as some network configurations may interfere with the web flasher.

## Flashing Methods

Choose your preferred method:

- [Web Flasher Method](#web-flasher-method) (Recommended)
- [USB Connection Method](#usb-connection-method)
- [Flipper Zero Method](#flipper-zero-method)

### Web Flasher Method

1. **Prepare for Flashing**
   - Visit [Ghost ESP Web Flasher](https://flasher.ghostesp.net/)
   - If the site doesn't load correctly, clear your browser cache and try again.
   - Use **Google Chrome** as Firefox does not support WebSerial.
   - Ensure your ESP32 is disconnected from other software or tools to avoid port conflicts.

1. **Enter Bootloader Mode**

   - **Method 1: Boot Button and USB Connection** (most boards):
     - **Step 1**: Hold down the **BOOT** button on your ESP32 board.
     - **Step 2**: While holding the **BOOT** button, plug in the USB cable to connect the ESP32 to your computer.
     - **Step 3**: Keep holding the **BOOT** button for a few seconds, then release it. Your board should now be in bootloader mode.

   - **Method 2: Boot and Reset Button Sequence**:
     - **Step 1**: Press and hold the **BOOT** button on your ESP32.
     - **Step 2**: While holding the **BOOT** button, press and release the **RESET** button.
     - **Step 3**: Continue holding the **BOOT** button for an additional second or two, then release it. This sequence will also put the ESP32 into bootloader mode.

1. **Flash Process**
   - Select your board type from the dropdown menu on the flasher site. If you're unsure of your specific ESP model, check the engraving on the chip.
   - Follow the on-screen instructions to initiate flashing.
   - Wait until the process is complete.

1. **Verify the Flash**
   - After flashing, disconnect and reconnect your board to ensure the firmware update was successful.
   - Verify that the firmware boots correctly by accessing the control options described in the [Post-Installation](#post-installation) section.

> **Tips**: After flashing, you may need to restart the device to initialize the new firmware. To ensure a clean start, disconnect and reconnect your ESP32.

### USB Connection Method

This method is suitable for boards equipped with either a Micro USB or USB-C port and involves using the ESP flashing tool.

1. **Download and Extract the Firmware**
   - Visit the [GhostESP Releases](https://github.com/jaylikesbunda/Ghost_ESP/releases) page to download the latest firmware version for your board.
   - Extract the downloaded `.tar.gz` or `.zip` file using [7-Zip](https://www.7-zip.org/download.html) or another extraction tool.

1. **Connect Your Board**
   - Identify the boot button on your board. For Flipper Dev Boards, it's the button closest to the USB port. On other boards, this button should be labeled "Boot" or "Select," but it can vary, especially if your board has a case.
   - Enter the board's bootloader mode by holding the identified boot button while connecting the board to your PC via a Micro USB or USB-C cable.

1. **Flash the Firmware**
   - Go to [ESP Huhn Flashing Tool](https://esp.huhn.me/) and click on "Connect".
   - Select the COM port that your board is connected to. It should be labeled with your board's chipset, like "ESP32-S2".
   - **Important Offsets:**
     - For **ESP32-S2** and similar boards, use the following offsets:
       - `bootloader.bin` at `0x1000`
       - `partitions.bin` at `0x8000`
       - `firmware.bin` at `0x10000`
     - For **ESP32-S3, C3 or C6** boards, the bootloader offset changes to:
       - `bootloader.bin` at `0x0`
       - Continue using `0x8000` for `partitions.bin` and `0x10000` for `firmware.bin`.
   - Click "Flash" and wait until the process completes.

1. **Verify the Flash**
   - After flashing, disconnect and reconnect your board to ensure the firmware update was successful.

## Flipper Zero Method

1. **Download Required Files**
   - Download the latest GhostESP `.fap` file from the [flipper app store](https://lab.flipper.net/apps)!
      - or Obtain the latest GhostESP `.fap` file from [this link](https://github.com/jaylikesbunda/ghost_esp_app/releases/latest) and manually upload it to your flipper.
   - Download the latest firmware files from the [GhostESP Releases](https://github.com/jaylikesbunda/Ghost_ESP/releases) page for the GhostESP project.

1. **Prepare the Firmware Files**
   - Extract the downloaded firmware files using [7-Zip](https://www.7-zip.org/download.html) or a similar tool.
   - Use the firmware file that aligns with the ESP chip your board uses! ex. Flipper zero dev board is esp32s2 generic.

1. **Transfer Files to Flipper Zero**
   - Ensure you have [qFlipper](https://flipperzero.one/update) installed on your PC.
   - Connect the Flipper Zero to your PC and use qFlipper to transfer the firmware files.
   - Place the firmware files into the following directory on the Flipper Zero `SDCard/apps_data/esp_flasher`
     **Important**: Do not place the files in the `assets` or other folders to ensure correct functionality.

1. **Connect the Board to Flipper Zero**
   - Connect your ESP32 to the Flipper Zero using the GPIO header.
   - Enter **bootloader mode** on your ESP32 as described in the [Web Flasher Method](#web-flasher-method).

1. **Flashing Process**
   - Open the ESP flasher app on your Flipper Zero.
   - Select **Manual Flash** to flash each firmware component:
      - Choose the appropriate `bootloader.bin`, `partitions.bin`, and `GhostESP.bin` files.
      - **Important Offsets:**
         - For **ESP32-S2** and similar boards, use the following offsets:
            - `bootloader.bin` at `0x1000`
            - `partitions.bin` at `0x8000`
            - `firmware.bin` at `0x10000`
         - For **ESP32-S3, C3 or C6** boards, the bootloader offset changes to:
            - `bootloader.bin` at `0x0`
            - Continue using `0x8000` for `partitions.bin` and `0x10000` for `firmware.bin`.
   - Initiate the flash process and wait until it completes.
   - Reset your ESP32 after completion to finalize the installation.

## Post-Installation

After flashing, you have several control options to configure and interact with GhostESP.

### Control Options

1. **Web Interface**
   - Connect to the `GhostNet` AP with the password `GhostNet`
   - Open a browser and connect to either `ghostesp.local` (requires mDNS support) or `192.168.4.1` to access device settings.
   - Use the auto-updating configuration panel to manage settings, Evil Portal controls, and device info.

1. **Qt6 Control App**
   - [Download and install](../../scripts/control%20app/) the Qt6-based desktop application for more advanced control options.
   - Offers access to advanced features, customizable settings, and an improved interface for interacting with GhostESP.

1. **Serial Command Line**
   - Connect via a serial interface for direct command-line control.
   - Provides full access to GhostESP's command suite and is ideal for advanced users or troubleshooting.
   - Refer to the [Command Line Interface Documentation](https://github.com/jaylikesbunda/Ghost_ESP/blob/main/main/core/commandline.c) for available commands and usage examples.
   - Android devices can interface with the serial command line directly using the [Serial USB Terminal app](https://play.google.com/store/apps/details?id=de.kai_morich.serial_usb_terminal&hl=en-US)

1. **Flipper Zero App**
   - Control GhostESP directly from the Flipper Zero.
   - Now available directly from the official flipper app store!
   - Ensure you have the latest `.fap` version on your Flipper Zero for full functionality and access to features like Evil Portal, Wi-Fi control, and more.

1. **Touch Screen Interface** (for supported boards)
   - Navigate through menus using touch gestures
   - Access all major features through the graphical interface
   - Terminal App available for keyboard-based command input on boards with keyboards

## Common Installation Issues

Refer to the [Troubleshooting](Troubleshooting.md) page for solutions to common issues:

- **Boot loops**: Check your board model and USB cable for compatibility.
- **Flash errors**: Ensure you are in bootloader mode, and try a different USB port if flashing fails.
- **Connection issues**: Ensure proper drivers are installed, especially on Windows.
- **Power supply problems**: Use a stable USB power source to avoid disconnection during flashing.
- **Cache-related issues with the web flasher**: Clear your browser cache or try a different browser if problems persist.

> **Troubleshooting Tip**: For the Flipper Zero method, ensure the GPIO connections are secure, and the `.bin` file is correctly placed in `SDCard/apps_data/esp_flasher`.

## Additional Recommendations

To ensure a smooth installation experience, consider the following:

- **File Extraction**: Always verify that firmware files are correctly extracted before flashing.
- **Memory Offsets**: Pay attention to the specific memory offsets required for your ESP32 variant to avoid firmware corruption.
- **Firmware Versions**: Use the latest firmware versions for both GhostESP and Flipper Zero to ensure compatibility and access to the latest features.
- **Backup**: If possible, backup your existing firmware before flashing new firmware to recover in case of any issues.

## Additional Resources

- [GhostESP GitHub Repository](https://github.com/jaylikesbunda/Ghost_ESP)
- [Flipper Zero Official Documentation](https://docs.flipperzero.one/)
- [ESP32 Bootloader Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/bootloader.html)
- [Command Line Interface Documentation](https://github.com/jaylikesbunda/Ghost_ESP/blob/main/main/core/commandline.c)
