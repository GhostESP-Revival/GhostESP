---
title: "WebUI"
description: "Access and control GhostESP through the web interface"
weight: 70
---

The WebUI allows you to configure GhostESP and manage files from your browser.

> **Note:** Wi-Fi/BLE commands require **GhostLink** because the radio cannot scan and host the AP simultaneously.

## Connect

1. On your computer, phone, or tablet, scan for Wi-Fi networks, connect to `GhostNet`, and enter the password `GhostNet`.
2. Open a web browser and navigate to `ghostesp.local` (requires mDNS support; works on most networks) or `192.168.4.1` (direct IP; always works).
3. Authentication is **disabled by default** for faster access. To enable it, run `webauth on` in the serial CLI or WebUI terminal. When enabled, the login username is your **AP SSID** (default `GhostNet`) and the password is your AP password (default `GhostNet`), protected by HTTP Digest (RFC2617) with signed nonces.

## Tabs and features

The WebUI has eight main tabs: **Dashboard**, **Wi-Fi**, **BLE**, **BadUSB**, **Files**, **GhostLink**, **Settings**, and **Terminal**.

- **Dashboard** - Device status overview and quick actions.
- **Wi-Fi** - Wi-Fi scanning, connection, and attack controls.
- **BLE** - Bluetooth Low Energy scans, spoofing, and GATT operations.
- **BadUSB** - BadUSB script listing and execution.

### Files

Browse and manage files on the SD card:

- Navigate `/mnt/ghostesp/` directory structure
- Download PCAP captures, portal credentials, and other files
- Delete files to free space
- Upload custom portal HTML files

### Settings

Configure device behavior through multiple pages. The available categories are:

- **Wi-Fi**
- **Portal**
- **Printer**
- **Display**
- **RGB**
- **System**
- **Date & Time**
- **GhostLink**
- **Personalisation**
- **IO Button**

### Terminal

Execute commands directly from the browser:

- Type any GhostESP command (same as serial CLI)
- View command output in real time
- Useful for quick diagnostics and file transfers

### GhostLink

Manage a GhostLink connection between a paired ESP32 device:

- View connection status
- Send commands to the paired device
- Monitor GhostLink link health

## Important limitations

### WebUI is AP-only by default

The WebUI is restricted to clients connected to the onboard AP subnet by default (`webui_restrict_to_ap=true`). Run `webuiap [on|off|toggle|status]` to inspect or change this. With it off, the WebUI can be reached from other interfaces such as Ethernet or a joined STA network.

### Wi-Fi and BLE commands require GhostLink

To run Wi-Fi or BLE commands from the WebUI, [set up GhostLink]({{< relref "dual-communication.md" >}}) with two ESP32 devices wired together. One device hosts the access point (and runs the WebUI) while the other performs attacks and scans; send commands with `commsend <command>`.

For example, with Device A hosting `GhostNet` and Device B wired to it over UART, `commsend scanap` scans on Device B and `commsend karma start` runs Karma there.

Commands that work directly in the WebUI without GhostLink include NFC (scanning, writing, saving), infrared (learning, transmitting), file management, device configuration, and help/diagnostics.

## Tips

- For large file transfers, use a USB card reader; the WebUI file manager is convenient but slower over Wi-Fi.
- `webauth on` enables HTTP Digest authentication for the WebUI; `webauth off` returns to the default open-access mode.

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

## Related tasks

- Set up [GhostLink]({{< relref "dual-communication.md" >}}) to run Wi-Fi and BLE attacks from the WebUI.
- Explore the [CLI Reference]({{< relref "command-line-reference.md" >}}) for available commands.
- Review [Wi-Fi Basics]({{< relref "../wifi/basics.md" >}}) to learn about attacks and scanning.
