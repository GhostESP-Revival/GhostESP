---
title: "WebUI"
description: "Access and control GhostESP through the web interface."
weight: 35
---

The WebUI allows you to configure GhostESP and manage files from your browser.

> **Note**: Wi-Fi/BLE commands require **GhostLink** because the radio cannot scan and host the AP simultaneously.

## Connecting to the WebUI

### Prerequisites
- GhostESP device powered on and booted
- A computer, phone, or tablet with a web browser
- Wi-Fi capability

### Steps

1. **Find the GhostNet network**
   - On your device, scan for Wi-Fi networks and look for `GhostNet`.
   - Connect to it using the password `GhostNet`.

2. **Open the web interface**
   - Open a web browser and navigate to one of:
     - `ghostesp.local` (requires mDNS support; works on most networks)
     - `192.168.4.1` (direct IP; always works)

3. **Authentication (optional)**
   - WebUI authentication is **disabled by default** for faster access.
   - To enable it, run `webauth on` in the serial CLI or WebUI terminal.
   - When enabled, the default credentials are `GhostNet` / `GhostNet`, protected by HTTP Digest (RFC2617) with signed nonces.

## Tabs and Features

The WebUI has five main tabs:

### Settings

Configure device behavior through multiple pages:

- **Miscellaneous**: General device settings
- **WiFi**: Wi-Fi configuration options
- **Evil Portal**: Portal settings and credential management
- **Power Printer**: Power printer configuration
- **RGB**: LED/RGB lighting settings
- **Watch / RTC**: Real-time clock and watch settings
- **SD Card Config**: SD card configuration

### SD Card

Browse and manage files on the SD card:

- Navigate `/mnt/ghostesp/` directory structure
- Download PCAP captures, portal credentials, and other files
- Delete files to free space
- Upload custom portal HTML files

### Terminal

Execute commands directly from the browser:

- Type any GhostESP command (same as serial CLI)
- View command output in real time
- Useful for quick diagnostics and file transfers

> **Limitation**: Wi-Fi and BLE commands (e.g., `scanap`, `karma start`, `blescan`) cannot run here because the device's radio is hosting the access point. Running them locally temporarily suspends the AP; to keep GhostNet online, use the Dual Comm tab to send these commands to a paired device instead.

### Help

Access built-in documentation for features and commands:

- **Wi-Fi**: Scanning and attack commands
- **BLE**: Bluetooth Low Energy operations
- **Communication**: GhostLink dual communication setup
- **SD Card**: SD card operations
- **LED / RGB**: LED and RGB lighting
- **GPS**: GPS functionality
- **Misc**: Miscellaneous features
- **Evil Portal**: Portal configuration
- **Printer**: Power printer features
- **YouTube Cast**: YouTube casting
- **Capture**: Packet capture modes
- **Beacon**: Beacon spam options
- **Attack**: Attack command reference

### GhostLink

Manage a GhostLink connection between a paired ESP32 device:

- View connection status
- Send commands to the paired device
- Monitor GhostLink link health

## Important Limitations

### Wi-Fi and BLE Commands Require GhostLink

The WebUI cannot directly run Wi-Fi or BLE commands (e.g., `scanap`, `karma start`, `blescan`). These commands require the device's radio hardware, which conflicts with hosting the Wi-Fi access point.

**To run Wi-Fi or BLE commands from the WebUI:**

1. Set up [GhostLink]({{< relref "dual-communication.md" >}}) with two ESP32 devices wired together.
2. One device hosts the access point (runs WebUI).
3. The other device performs attacks/scans.
4. Send commands through the WebUI using `commsend <command>`.

**Example workflow:**
- Device A (with WebUI): Hosts GhostNet AP
- Device B (attack device): Wired to Device A via UART
- From WebUI on Device A: `commsend scanap` → scans on Device B
- From WebUI on Device A: `commsend karma start` → runs Karma on Device B

### Commands that work directly in WebUI:
- NFC commands (scanning, writing, saving)
- Infrared commands (learning, transmitting)
- File management
- Device configuration
- Help and diagnostics

## Tips and Tricks

### Faster File Transfers

- Use a card reader for faster file transfers.
- The WebUI file manager is convenient but slower over Wi-Fi.

### Toggle Authentication When Needed

- `webauth on` — enable HTTP Digest authentication for the WebUI.
- `webauth off` — return to the default open-access mode.

## Troubleshooting

### Can't connect to GhostNet

- **GhostNet not visible**: Reboot the device. It should broadcast the AP within 10 seconds of boot.
- **Connection drops**: Move closer to the device or check for interference.
- **Wrong password**: Default is `GhostNet` (case-sensitive).

### Can't reach the WebUI

- **Page won't load**: Try `192.168.4.1` instead of `ghostesp.local`.
- **"Connection refused"**: The device may still be booting. Wait 10 seconds and refresh.
- **Timeout**: Check that you're connected to the GhostNet AP, not a different network.

### Login fails

- **Authentication prompts unexpectedly**: Run `webauth off` to confirm the feature is disabled.
- **Credentials rejected**: If you enabled auth, defaults are `GhostNet` / `GhostNet` (case-sensitive).
- **Forgot credentials**: Reboot the device to reset to defaults.

### Wi-Fi commands don't work

- **"Command not available"**: You need GhostLink. See [GhostLink]({{< relref "dual-communication.md" >}}) for setup.
- **Command times out**: Verify the second device is wired correctly and powered on.

### File manager is slow

- **Downloads are slow**: Use a USB card reader instead.
- **Uploads fail**: Ensure the SD card has free space and is properly mounted.

## Next Steps

- Set up [GhostLink]({{< relref "dual-communication.md" >}}) to run Wi-Fi and BLE attacks from the WebUI.
- Explore the [Command Line Reference]({{< relref "command-line-reference.md" >}}) for available commands.
- Review [Wi-Fi Basics]({{< relref "../wifi/basics.md" >}}) to learn about attacks and scanning.
