---
title: "Installation Guide"
description: "Instructions for installing GhostESP firmware on ESP32 devices"
weight: 10
toc: true
---

## Prerequisites

Before starting, make sure you have:

- A compatible ESP32 board (see [Supported Hardware](#supported-hardware))
- A modern web browser (Google Chrome, Brave, or Microsoft Edge is recommended, as Firefox doesn't support WebSerial)
- An appropriate USB cable (Micro USB or USB-C, ensure it's a data cable, not a charge-only cable)
- **File Extraction Tool**: Install [7-Zip](https://www.7-zip.org/download.html) or a similar program for extracting .zip bundles.

> <p class="note-heading"><strong>Note</strong></p>
> <p>For best results, disable any VPN or firewall that might block the flashing process, as some network configurations may interfere with the web flasher.</p>

## Flashing Methods

Choose your preferred method:

- [Web Flasher Method](#web-flasher-method) (Recommended)
- [USB Connection Method](#usb-connection-method)
- [Flipper Zero Method](#flipper-zero-method) (Doesn't require a PC)

### Web Flasher Method

1. **Prepare**
   - Open **[flasher.ghostesp.net](https://flasher.ghostesp.net/)** in Chrome.
   - Close apps using the serial port. If the site glitches, clear cache.

2. **Enter Bootloader**
   - Hold **BOOT** → plug USB → release.  
   - If needed: hold **BOOT**, tap **RESET**, keep holding **BOOT** 1–2 s, release.

3. **Flash**
   - Pick your the ESP32 variant your board uses, click **Connect**, and follow the prompts.

4. **Verify**
   - Unplug and replug. Continue with [Post-Installation](#post-installation).

> <a id="web-flasher-timeout"></a>**Tips**: After flashing, you will need to restart the device to initialize the new firmware. Disconnect and reconnect your ESP32. If the flasher times out, retry after a fresh USB reconnection.

### USB Connection Method

Use when selecting files and offsets manually.

1. **Download**
   - Get firmware from **[GhostESP Releases](https://github.com/jaylikesbunda/Ghost_ESP/releases)**.
   - Extract the `.zip` with your preferred tool.

2. **Connect**
   - Enter bootloader
   - Hold **BOOT** → plug USB → release.  
   - If needed: hold **BOOT**, tap **RESET**, keep holding **BOOT** 1–2 s, release.


3. **Flash** via **[ESP Huhn Tool](https://esp.huhn.me/)**
   - Click **Connect**, select the COM port labeled with your chipset.
   - Load binaries with offsets:

   | Chip family          | `bootloader.bin` | `partitions.bin (Partition table)` | `firmware.bin (GhostESP_IDF)` |
   |----------------------|------------------|------------------|----------------|
   | ESP32-S2 and similar | `0x1000`         | `0x8000`         | `0x10000`      |
   | ESP32-S3 / C3 / C6   | `0x0`            | `0x8000`         | `0x10000`      |

   - Click **Flash** and wait.

4. **Verify**
   - Replug the board and connect to a **[serial console](https://ghostesp.net/serial)** to see logs from your device.


### Flipper Zero Method

1. **Download**
   - Install GhostESP app (`.fap`) from the [Flipper app store](https://lab.flipper.net/apps)  
     or **[releases](https://github.com/jaylikesbunda/ghost_esp_app/releases/latest)**.
   - Download firmware from **[GhostESP Releases](https://github.com/jaylikesbunda/Ghost_ESP/releases)**.

2. **Prepare**
   - Extract archives. Choose the firmware that matches your chip  
     (e.g., Flipper Dev Board = ESP32-S2 generic).

3. **Copy to Flipper**
   - Use **qFlipper** (either the mobile app or the desktop app) or pull out the sd card and use the sd card directly. Put the `.bin` files in `SDCard/apps_data/esp_flasher/`.  
   - Do not use `assets/`.

4. **Connect Hardware**
   - Wire ESP32 to Flipper GPIO. Enter bootloader (see Web Flasher Method).

5. **Flash on Flipper**
   - Open **ESP flasher** → **Manual Flash**.
   - Select `bootloader.bin`, `partitions.bin`, `GhostESP.bin` and make sure to check the option if your device is one of the specified variants.
   - Start flash. Reset the ESP32 when done.

## Post-Installation

As soon as the flash finishes, GhostESP boots its default access point so you can pick the control surface that fits your hardware and workflow.

### Control Options

1. **Web Interface**
   - Connect to the `GhostNet` AP with the password `GhostNet`
   - Open a browser and connect to either `ghostesp.local` (requires mDNS support) or `192.168.4.1` to access device settings.
   - Sign in with the default credentials `GhostNet` / `GhostNet`. Run `webauth off` in the serial/Web CLI if you want to disable the WebUI authentication.
   - Use the auto-updating configuration panel to manage settings, Evil Portal controls, and device info.

2. **Qt6 Control App**
   - [Download and install](../../scripts/control%20app/) the Qt6-based desktop application for more advanced control options.
   - Offers access to advanced features, customizable settings, and an improved interface for interacting with GhostESP.

3. **Serial Command Line**
   - Connect via a serial interface for direct command-line control.
   - Provides full access to GhostESP's command suite and is ideal for advanced users or troubleshooting.
   - Refer to the [Command Line Reference]({{< relref "command-line-reference.md" >}}) for available commands and examples.
   - Android devices can interface with the serial command line directly using the [Serial USB Terminal app](https://play.google.com/store/apps/details?id=de.kai_morich.serial_usb_terminal&hl=en-US)

4. **Flipper Zero App**
   - Control GhostESP directly from the Flipper Zero.
   - Now available directly from the official flipper app store!
   - Ensure you have the latest `.fap` version on your Flipper Zero for full functionality and access to features like Evil Portal, Wi-Fi control, and more.

5. **Touch Screen Interface** (for supported boards)
   - Navigate through menus using touch gestures
   - Access all major features through the graphical interface
   - Terminal App available for keyboard-based command input on boards with keyboards

### Common Installation Issues

- **Boot loops**: Usually power or board-target mismatches—verify you flashed the right image and try a known-good USB-C cable.
- **Flash errors**: Ensure the chip is in bootloader mode (Refer back to the [Web Flasher section](#web-flasher-timeout)) ; swap USB ports or hubs if it times out. 
- **Connection issues**: Try installing the [USB-to-UART driver](https://www.silabs.com/software-and-tools/usb-to-uart-bridge-vcp-drivers?tab=downloads).
- **Browser issues**: If the web flasher misbehaves, reload the page, clear cached site data or switch to a different broswer like Chrome, Brave or Microsoft Edge.
> **Troubleshooting Tip**: When flashing from the Flipper Zero app, double-check GPIO wiring and confirm every firmware blob lives under `SDCard/apps_data/esp_flasher/` before you start the transfer.
