---
title: "Network Fingerprinting"
description: "Scan and identify devices on your Ethernet network."
weight: 10
---

Network fingerprinting discovers and identifies devices on your Ethernet network. GhostESP scans for common protocols to build a device profile.

## Overview

When you run a fingerprint scan, GhostESP listens for network traffic and service announcements to identify devices. It looks for:

- **mDNS** — Service announcements (printers, speakers, etc.)
- **NBNS** (NetBIOS Name Service) — Windows device name broadcasts
- **SSDP** — UPnP device announcements

The scan collects device names, IP addresses, detected device types (Chromecast, Roku, Apple, Samsung, etc.), and service information.

## Prerequisites

- A Banshee device with Ethernet connected
- Access to the device via GhostLink display menu or terminal
- A network with active devices

## How to Use

### Via Terminal

Run the fingerprint scan from the terminal:

```
ethfp
```

The scan will run for approximately 3 seconds and display discovered devices with their details:

```
IP Address: 192.168.1.100
Device Name: Living-Room-TV
Device Type: Samsung
Service: upnp
```

### Via GhostLink Display Menu

1. **Connect** to the Banshee device via GhostLink
2. **Navigate** to `Ethernet` menu
3. **Select** `Fingerprint Scan`
4. **Wait** for the scan to complete (approximately 3 seconds)
5. **View** the list of discovered devices

## Understanding Results

Each discovered device shows:

- **IP Address** — The device's IP on the network
- **Device Name** — Hostname or friendly name if available
- **Device Type** — Detected manufacturer or device category (Samsung, Apple, Google, etc.)
- **Service Type** — Protocol used for detection (mDNS, SSDP, NBNS)
- **OS Info** — Operating system or device model if detected

## Troubleshooting

- **No devices found**: Ensure devices are powered on and on the same network. Some devices don't broadcast.
- **Incomplete information**: Not all devices provide full details. GhostESP shows what it can detect from available network traffic.
- **Timeout**: The scan runs for 3 seconds. If you need more time, run the scan again.
