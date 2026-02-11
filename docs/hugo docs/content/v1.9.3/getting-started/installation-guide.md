---
title: "Installation Guide"
description: "Flash GhostESP firmware to your ESP32 device"
weight: 10
toc: true
---

Flash GhostESP firmware to your ESP32 board using the web flasher, manual USB tool, or Flipper Zero app. Choose the method that works best for your setup.

## Prerequisites

- A compatible ESP32 board (see [Supported Hardware]({{< relref "supported-hardware.md" >}}))
- A USB cable (Micro USB or USB-C; must be a data cable, not charge-only)
- A modern web browser (Chrome, Brave, or Edge; Firefox doesn't support WebSerial)
- [7-Zip](https://www.7-zip.org/download.html) or similar tool to extract firmware files
- VPN/firewall disabled (some configurations interfere with the web flasher)

## Flashing Methods

Choose one:

### Web Flasher Method (Recommended)

1. **Open the flasher**
   - Go to **[flasher.ghostesp.net](https://flasher.ghostesp.net/)** in Chrome, Brave, or Edge.
   - Close any apps using the serial port.

2. **Enter bootloader mode**
   - Hold **BOOT**, plug in USB, then release **BOOT**.
   - If that doesn't work: hold **BOOT**, tap **RESET**, keep **BOOT** held for 1–2 seconds, then release.

3. **Flash the firmware**
   - Select your ESP32 variant from the dropdown.
   - Click **Connect** and follow the on-screen prompts.

4. **Restart the device**
   - Unplug and replug the USB cable.
   - If the flasher times out, try again.

> **Note**: If the flasher site glitches, clear your browser cache and reload.

### USB Connection Method

Use this method if you prefer manual control or the web flasher doesn't work.

1. **Download the firmware**
   - Go to **[GhostESP Releases](https://github.com/jaylikesbunda/Ghost_ESP/releases)**.
   - Download the `.zip` file for your board.
   - Extract it with 7-Zip or your preferred tool.

2. **Enter bootloader mode**
   - Hold **BOOT**, plug in USB, then release **BOOT**.
   - If that doesn't work: hold **BOOT**, tap **RESET**, keep **BOOT** held for 1–2 seconds, then release.

3. **Flash using ESP Huhn Tool**
   - Open **[ESP Huhn Tool](https://esp.huhn.me/)** in your browser.
   - Click **Connect** and select your device's COM port.
   - Load the three binary files with the correct offsets:

   | Chip | `bootloader.bin` | `partitions.bin` | `firmware.bin` |
   |------|------------------|------------------|----------------|
   | ESP32-S2 | `0x1000` | `0x8000` | `0x10000` |
   | ESP32-S3 / C3 / C6 | `0x0` | `0x8000` | `0x10000` |

   - Click **Flash** and wait for completion.

4. **Verify the flash**
   - Unplug and replug the USB cable.
   - Open a **[serial console](https://ghostesp.net/serial)** to see device logs.


### Flipper Zero Method

Flash GhostESP using your Flipper Zero as the programmer. No PC required.

1. **Install the GhostESP app**
   - Download from the [Flipper app store](https://lab.flipper.net/apps) or **[releases](https://github.com/jaylikesbunda/ghost_esp_app/releases/latest)**.
   - Copy the `.fap` file to your Flipper's SD card.

2. **Download the firmware**
   - Go to **[GhostESP Releases](https://github.com/jaylikesbunda/Ghost_ESP/releases)**.
   - Download the `.zip` file that matches your ESP32 chip (e.g., `esp32-generic.zip` for a generic ESP32).
   - Extract the `.zip` file.

3. **Copy firmware files to Flipper**
   - Use **qFlipper** or pull out the Flipper's SD card and insert it into your computer.
   - Copy the three `.bin` files to `SDCard/apps_data/esp_flasher/`.
   - Do not put them in `assets/`.

4. **Wire the ESP32 to Flipper**
   - Connect the ESP32 to the Flipper's GPIO pins (refer to the GhostESP app for pinout).
   - Enter bootloader mode on the ESP32 (hold **BOOT**, plug USB, release **BOOT**).

5. **Flash the firmware**
   - Open the **ESP flasher** app on your Flipper.
   - Select **Manual Flash**.
   - Choose `bootloader.bin`, `partitions.bin`, and `GhostESP.bin`.
   - Verify the device variant is correct.
   - Start the flash and wait for completion.
   - Reset the ESP32 when done.

## After Flashing

GhostESP boots automatically and creates a Wi-Fi access point called `GhostNet` (password: `GhostNet`). Choose how you want to control it.

### Control Methods

**Web Interface** (Easiest)
- Connect to the `GhostNet` Wi-Fi network.
- Open a browser and go to `ghostesp.local` or `192.168.4.1`.
- No authentication required by default. Run `webauth on` in the terminal to enable login.
- **Limitation**: Wi-Fi and BLE commands don't work here (the radio is hosting the AP). Use [GhostLink]({{< relref "dual-communication.md" >}}) to run attacks remotely.

**Serial Terminal** (Full Control)
- Connect via USB and open a serial console at 115200 baud.
- Access all CLI commands. See [Command Line Reference]({{< relref "command-line-reference.md" >}}).
- On Android, use [Serial USB Terminal](https://play.google.com/store/apps/details?id=de.kai_morich.serial_usb_terminal).

**Flipper Zero App**
- Download from the [Flipper app store](https://lab.flipper.net/apps).
- Control GhostESP directly from your Flipper.

**Qt6 Desktop App**
- [Download and install](../../scripts/control%20app/) for advanced features and customization.

**Touch Screen** (Supported Boards Only)
- Navigate menus with touch gestures.
- Use the on-screen terminal for keyboard input.

## Troubleshooting

**Device won't boot or loops**
- Verify you flashed the correct firmware for your chip.
- Try a different USB cable (some are charge-only).
- Reboot the device and wait 10 seconds.

**Flash fails or times out**
- Ensure the chip is in bootloader mode (hold **BOOT** while plugging in USB).
- Try a different USB port or hub.
- Close other apps using the serial port.

**Can't connect to GhostNet**
- Reboot the device.
- Check that you're using the correct password: `GhostNet`.
- Move closer to the device.

**Web flasher doesn't work**
- Clear your browser cache.
- Try Chrome, Brave, or Edge (Firefox doesn't support WebSerial).
- Disable VPN or firewall temporarily.

**Serial connection issues**
- Install the [USB-to-UART driver](https://www.silabs.com/software-and-tools/usb-to-uart-bridge-vcp-drivers).
- Verify the correct COM port is selected.

**Flipper Zero flashing fails**
- Double-check GPIO wiring between ESP32 and Flipper.
- Confirm all `.bin` files are in `SDCard/apps_data/esp_flasher/` (not `assets/`).
- Verify the device variant matches your ESP32 chip.
