---
title: "USB Dongle Mode (Wireshark)"
description: "Use GhostESP as a wireless capture dongle for Wireshark."
weight: 25
---

Use your GhostESP as a **USB wireless capture dongle** for Wireshark. Instead of saving packets to an SD card, the device streams Wi-Fi and BLE traffic directly to your computer in real-time—just like a commercial Wi-Fi adapter in monitor mode.

This is perfect for live analysis, debugging networks, or learning how wireless protocols work without needing expensive hardware.

## Prerequisites

- GhostESP device connected via USB to your computer
- Wireshark installed on your computer

## Setup

### Option 1: GUI Installer (Recommended)

The easiest way to install is using the GUI installer, which automatically handles dependencies and installation:

1. Navigate to `scripts/wireshark_extcap_installer/` in your GhostESP repository
2. Run the installer:
   ```
   python installer_gui.py
   ```
3. Click **Install** in the GUI
4. Restart Wireshark

The installer will automatically:
- Detect your Wireshark installation
- Install the extcap files to the correct location
- Set up all required dependencies in an isolated environment

### Option 2: Manual Installation

If you prefer manual installation:

**Prerequisites for manual installation:**
- Python 3.7+ installed
- `pyserial` Python package (`pip install pyserial`)

**Installation steps:**

1. Copy the extcap files to your Wireshark extcap folder:
   - **Windows**: `%APPDATA%\Wireshark\extcap\`
   - **macOS**: `~/.local/lib/wireshark/extcap/`
   - **Linux**: `~/.local/lib/wireshark/extcap/`

2. Files to copy from `scripts/wireshark_extcap_installer/`:
   - `ghostesp_extcap.py`
   - `ghostesp_extcap.bat` (Windows only)

3. Make the script executable (macOS/Linux only):
   ```
   chmod +x ~/.local/lib/wireshark/extcap/ghostesp_extcap.py
   ```

4. Restart Wireshark.

### Verify installation

1. Open Wireshark.
2. Go to **Capture → Options** or click the gear icon.
3. Look for **GhostESP WiFi/BLE Capture** in the interface list.
   - If you don't see it, check that the files are in the correct extcap folder and Python is in your PATH.

## Capturing Wi-Fi traffic

### Start capture in Wireshark

1. In Wireshark, find **GhostESP WiFi/BLE Capture** in the interface list.
2. Click the **gear icon** next to it to configure:
   - **Serial Port**: Select your GhostESP COM port
   - **Baud Rate**: Leave at 115200 (default)
   - **Capture Type**: Select **WiFi**
   - **Channel Lock**: Choose between:
     - **Auto (channel hopping)**: Continuously hop through all channels (default)
     - **Channel 1-13**: Lock to a specific WiFi channel for focused capture
3. Click **Start** to begin capturing.

The GhostESP will automatically:
- Enter monitor mode
- Start capture based on your Channel Lock setting:
  - **Auto mode**: Channel hop through all legal channels (150ms per channel)
  - **Fixed channel**: Monitor only the selected channel continuously
- Stream packets in real-time to Wireshark

### What you'll see

Wireshark will display:
- **Beacon frames**: Access points advertising their SSIDs
- **Probe requests**: Devices searching for known networks
- **Data frames**: Encrypted Wi-Fi traffic
- **Management frames**: Association, authentication, deauth
- **Control frames**: ACKs, RTS/CTS

### Channel hopping vs. Channel lock

**Auto (Channel Hopping)**
- Continuously hops through all legal channels for your country
- **2.4 GHz**: Channels 1-13 (or 1-14 for Japan)
- **5 GHz** (ESP32-C5/C6 only): Country-specific channels
  - **US/CA**: All UNII bands (36-165)
  - **EU**: UNII-1, 2a, 2c (36-140)
  - **JP**: UNII-1, 2a, 2c (36-140, no 165)
  - **CN**: UNII-1, 2a, 3 (36-64, 149-165)
- Best for general surveillance and discovering networks
- Set your country with the `setcountry` command before starting capture

**Fixed Channel Lock**
- Monitors only the selected channel continuously
- Higher packet capture rate on the target channel
- Ideal for:
  - Analyzing traffic on a known network
  - Capturing handshakes from a specific AP
  - Debugging connectivity issues
  - Following a specific conversation

Set your country with the `setcountry` command before starting capture.

### Stop capture

Click the red **Stop** button in Wireshark or run `capture -stop` on the GhostESP terminal.

## Capturing BLE traffic

### Start BLE capture

1. In Wireshark, configure **GhostESP WiFi/BLE Capture**:
   - **Serial Port**: Select your GhostESP COM port
   - **Capture Type**: Select **Bluetooth LE**
2. Click **Start**.

The GhostESP will stream BLE advertising packets to Wireshark.

### What you'll see

Wireshark will decode:
- **LE Advertising Reports**: Device advertisements with names, UUIDs, manufacturer data
- **RSSI values**: Signal strength for each packet
- **MAC addresses**: Device identifiers (public or random)
- **Advertisement data**: Service UUIDs, local names, flags

### Stop capture

Click **Stop** in Wireshark or run `capture -stop` on the device.

## Command line alternative

You can also start Wireshark mode from the GhostESP terminal without using the Wireshark interface:

### Wi-Fi capture
```
capture -wireshark [-c <channel>]
```
- `-c <channel>`: Optional channel lock (1-13)
- Without `-c`: Auto channel hopping
- With `-c`: Lock to specific channel

### BLE capture
```
capture -wiresharkble
```

### Stop capture
```
capture -stop
```

When using command line mode, you'll need to manually configure Wireshark to capture from the serial port.

## Troubleshooting

### Interface not showing in Wireshark
- Verify the extcap files are in the correct folder
- Check that Python is installed and in your system PATH
- Restart Wireshark after copying the files
- Run `python ghostesp_extcap.py --extcap-interfaces` in the extcap folder to test

### No packets appearing
- Confirm the correct COM port is selected
- Check that the GhostESP is connected and powered on
- Verify the device responds to serial commands
- Try increasing the baud rate if packets are being dropped

### Malformed packets in Wi-Fi capture
- This is normal for promiscuous mode capture
- The ESP32 captures everything including corrupted frames and radio noise
- Valid packets will appear alongside malformed ones
- Use Wireshark filters to focus on valid frames

### Slow packet rate
- USB serial has limited throughput (~11 KB/s at 115200 baud)
- High-traffic environments may drop packets
- Channel hopping reduces time on each channel
- This is expected behavior for live capture

### Country setting not applied
- Run `setcountry <code>` before starting capture (e.g., `setcountry US`)
- Restart the device after changing country
- Verify with `info` command that country is set correctly

## Notes

- Wireshark mode streams packets over USB/UART without saving to SD card
- The device automatically handles PCAP formatting and headers
- **Auto mode**: Channel hopping covers the full spectrum but reduces dwell time per channel
- **Fixed channel**: Higher packet capture rate on the selected channel
- For focused capture on a single channel, use either Channel Lock setting or file-based capture modes
- BLE capture does not hop channels (BLE uses 3 advertising channels: 37, 38, 39)
- Channel lock is only available for WiFi capture, not BLE

## Next steps

- [Capturing packets to file]({{< relref "capture.md" >}}) — Save captures to SD card for later analysis
- [Handshakes]({{< relref "handshakes.md" >}}) — Extract WPA/WPA2 authentication data
- [Survey]({{< relref "survey.md" >}}) — Scan for networks and devices
