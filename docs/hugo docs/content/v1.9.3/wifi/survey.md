---
title: "Scanning networks"
description: "Find and review Wi-Fi networks in your area."
weight: 10
---

Discover nearby Wi-Fi networks and gather information about them. You can scan passively without connecting, or connect to a network and explore devices on it.

## Prerequisites

- GhostESP flashed device, powered on with a wireless antenna.

## Finding nearby networks

### On-device UI
1. Open **Menu → WiFi → Scanning**.
   You should see a menu with scan options.
2. Choose **Scan Access Points**.
   The device will search for networks. Wait for the scan to finish and see a summary.
3. Select **List Access Points**.
   You should see each network listed with its name, channel, signal strength, and device manufacturer.

### On-device UI — Optional features
- **Scan APs Live**: Watch new networks appear in real time as they're discovered.
- **Channel Congestion**: See how busy the wireless channels are in your area.

### Command line
1. Open the GhostESP terminal (serial connection or on-device terminal).
2. Run `scanap` to start a scan.
   Wait for it to finish and show results.
3. Run `list -a` to see the cached list of networks.

### Command line — Optional features
- Run `scanap -live` to watch networks appear as they're discovered.

## Exploring a network

Once you connect to a network, you can discover devices and services on it.

### Connect to a network
1. Open **Menu → WiFi → Connection → Connect to WiFi**.
   Enter the network name and password when prompted.
2. Wait for the connection to complete. The terminal will show status updates.
3. To disconnect later, go to **Menu → WiFi → Connection → Disconnect**.

### Connect via command line
1. Run `connect "SSID" "password"` (use quotes if the name or password has spaces).
   The terminal will show connection progress and confirm when connected.
2. Run `connect` with no arguments to reconnect to the last network you used.
3. Run `disconnect` to leave the network.

### Find devices on the network
1. Open **Menu → WiFi → Scanning** while connected.
2. Choose **Scan LAN Devices**.
   You should see a list of devices and services on the network.

### Find devices via command line
1. Run `scanlocal` to discover devices and services.
   You should see hostnames, service types, and ports.
2. Run `scanarp` to find all active devices on the network.
   You should see IP addresses and device information.

### Check for open ports
1. From the UI, select a device with **Select LAN**, then choose **Scan Open Ports**.
   You should see which ports are responding on that device.
2. From the command line, run `scanports <ip>` to check a specific device.
   You should see open ports listed. Add `all` to check all ports, or `start-end` (like `20-1024`) for a range.
3. Run `scanssh <ip>` to specifically check if a device has SSH enabled.

## Full environment sweep

Scan all wireless activity at once and save results to your SD card.

### What it captures
- **WiFi Access Points**: Name, MAC, channel, frequency, signal strength, security type, cipher, 802.11 mode, WPS status
- **WiFi Clients**: MAC address and associated AP
- **BLE Devices**: Flippers, GATT devices, and raw BLE packets
- **802.15.4 Packets**: Zigbee/Thread traffic (ESP32-C5 and C6 only)
- **GPS Coordinates**: Location data for each entry (if GPS module connected)

### Run a sweep from the UI
1. Open **Menu → WiFi → Scanning → Sweep**.
   The device will run through each scan phase automatically.
2. Wait for all phases to complete. Progress is shown on screen.
3. Find results in `/ghostesp/sweeps/sweep_N.csv` on your SD card.

### Run a sweep from command line
1. Run `sweep` to start with default timing (10 seconds per phase).
2. Customize timing with flags:
   - `sweep -w 15` for 15-second WiFi scans
   - `sweep -b 20` for 20-second BLE scans
   - `sweep -w 15 -b 20` to set both
3. Run `sweep -h` to see all options.

### CSV output format
Results are saved in a format similar to Kismet/Wigle exports:

```
Type,Name,MAC,Associated MAC,Channel,Frequency,RSSI,Auth,Cipher,802.11,WPS,Latitude,Longitude,Altitude,First Seen
WiFi AP,MyNetwork,AA:BB:CC:DD:EE:FF,,6,2437,-45,WPA2,CCMP,ax/n/g/b,No,37.774929,-122.419416,10.5,2025-12-09 17:30:00
WiFi Client,,11:22:33:44:55:66,AA:BB:CC:DD:EE:FF,,,,,,,37.774929,-122.419416,10.5,2025-12-09 17:30:00
```

## Troubleshooting
- **No networks found**: Move closer to wireless routers and try scanning again.
- **"You Need to Scan APs First" message**: Run a scan before trying to select a network.
- **Live scan stops right away**: Stop any active Wi-Fi attacks or portals from the menu and try again.

## FAQ
- **Can I scan while connected to a network?** Yes. The device will pause the connection briefly to scan, then resume.
- **Where do the device vendor names come from?** GhostESP looks up the device's hardware address in a built-in database to identify the manufacturer.
