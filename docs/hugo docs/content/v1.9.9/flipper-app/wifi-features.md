---
title: "Wi-Fi Features"
description: "Wi-Fi operations available in the Flipper Zero companion app."
weight: 30
keywords: ["wifi", "scanning", "capture", "attacks", "beacon spam"]
---

## Scanning & Probing

- **Scan Access Points**: Discover and list nearby Wi-Fi networks
- **Scan Stations**: Find connected devices on networks
- **Probe Requests**: Listen for probe requests with channel hopping or fixed channel
- **Pineapple Detection**: Identify rogue access points
- **Channel Congestion Analysis**: View network activity per channel
- **Port Scanning**: Scan ports on local network or specific IP addresses
- **ARP Scanning**: Discover devices on local network
- **SSH Scanning**: Scan for SSH services on target IPs

## Packet Capture

- **Multiple Capture Modes**:
  - WPS capture
  - Raw packet capture
  - Probe request capture
  - Deauthentication capture
  - Beacon capture
  - EAPOL handshake capture
  - Pwnagotchi capture
- **PCAP Export**: Save captures as PCAP files for analysis in Wireshark
- **Cycling Menu**: Easily switch between capture types

## Beacon Spam & Attacks

- **Beacon Spam Modes**:
  - List mode (spam from predefined list)
  - Random mode (random SSIDs)
  - Rickroll mode (entertainment)
  - Custom SSID mode
- **Deauthentication Attacks**: Disconnect devices from networks
- **EAPOL Logoff Attacks**: Force re-authentication
- **SAE Handshake Flood**: WPA3 attack method
- **Karma Rogue AP**: Create fake access points with custom SSIDs
- **DHCP Starvation**: Exhaust DHCP server resources
- **Beacon List Management**: Add, remove, clear, show, and spam from lists

## Evil Portal & Network

- **Captive Portal**: Create fake login pages
- **Portal HTML Management**: List, set, and clear custom portal HTML files
- **Wi-Fi Connection**: Connect to Wi-Fi networks
- **Network Disconnect**: Disconnect from current network
- **Network Scanning**: Scan local network for devices
- **Media Casting**: Cast random videos to Cast/DIAL devices
- **Printer Control**: Control network printers
- **Smart Plug Control**: Control TP-Link smart plugs
- **WebUI Credentials**: Configure WebUI authentication

> **Note**: The Aerial Detector submenu is under the **Wi-Fi** category in the Flipper UI (not BLE). On-device displays keep it under BLE.

## Custom Portal HTML

1. Create your HTML file (max 2048 bytes for app upload)
2. Navigate to **WiFi** → **Evil Portal & Network** → **Set Evil Portal HTML**
3. Select upload method
4. Enter HTML content or select file
5. Portal will use custom HTML on next start

## Beacon List Management

1. Navigate to **WiFi** → **Beacon Spam & Attacks** → **Beacon List**
2. Choose action:
   - **Add**: Add new SSID to list
   - **Remove**: Remove SSID from list
   - **Clear**: Remove all SSIDs
   - **Show**: Display current list
   - **Spam**: Start spamming from list
