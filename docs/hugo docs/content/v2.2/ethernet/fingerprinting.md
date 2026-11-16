---
title: "Network Fingerprinting"
description: "Scan and identify devices on your Ethernet network"
weight: 10
---

Network fingerprinting discovers and identifies devices on your Ethernet network. GhostESP scans for common protocols to build a device profile.

When you run a fingerprint scan, GhostESP listens for network traffic and service announcements to identify devices. It looks for:

- **mDNS** - Service announcements (printers, speakers, etc.)
- **NBNS** (NetBIOS Name Service) - Windows device name broadcasts
- **SSDP** - UPnP device announcements

Fingerprinting requires the Ethernet build (`CONFIG_WITH_ETHERNET`). Reach the device through the GhostLink display menu or terminal, and make sure the network has active devices.

## How to Use

### Via Terminal

Run the fingerprint scan from the terminal:

```
ethfp
```

The scan runs three sequential sub-scans (mDNS, NBNS, SSDP) at approximately 3 seconds each, totaling around 9 seconds. Discovered devices are printed as a `-` title line followed by indented fields:

```
- Living-Room-TV
    IP: 192.168.1.100
    Type: Samsung
    Service: upnp
    OS: Tizen
    Protocol: SSDP
```

Each discovered device shows:

- **IP Address** - The device's IP on the network
- **Device Name** - Hostname or friendly name if available
- **Device Type** - Detected manufacturer or device category (Samsung, Apple, Google, etc.)
- **Service Type** - Protocol used for detection (mDNS, SSDP, NBNS)
- **OS Info** - Operating system or device model if detected

`Service` and `OS` lines are omitted when that information was not discovered.

### Via GhostLink Display Menu

1. **Connect** to the GhostESP device via GhostLink
2. **Navigate** to `Ethernet` menu
3. **Select** `Fingerprint Scan`
4. **Wait** for the scan to complete (approximately 9 seconds)
5. **View** the list of discovered devices

## Troubleshooting

- **No devices found**: Ensure devices are powered on and on the same network. Some devices don't broadcast.
- **Timeout**: The scan takes about 9 seconds (three ~3-second sub-scans). If no devices appear, run the scan again.
