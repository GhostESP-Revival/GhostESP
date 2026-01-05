---
title: "DIAL Connect"
description: "Cast YouTube videos to smart TVs on your network."
weight: 60
---

Cast random YouTube videos to all DIAL-enabled devices (Chromecasts, smart TVs, Roku, etc.) on your network.

> **Note**: Only test this on networks and devices you own or have explicit permission to test.

## Prerequisites

- GhostESP flashed device, powered on.
- Connected to the same Wi-Fi network as the target devices (run `connect SSID PASSWORD` first).
- DIAL-enabled devices on the network (Chromecast, smart TV with YouTube app, Roku, etc.).

## How to use

### On-device UI

1. First connect to a Wi-Fi network: **Menu → WiFi → Connect**.
2. Go to **Menu → WiFi → DIAL Connect**.
3. The device will discover smart TVs and cast a random YouTube video.

### Command line

**Cast to first available device:**
```
dialconnect
```

**Cast to ALL devices on the network:**
```
dialconnect all
```
or
```
dialconnect -a
```

**Set a custom device name (shown on TV):**
```
dialconnect MyDevice
```

**Cast to all with custom name:**
```
dialconnect all GhostESP
```

## What happens

1. GhostESP discovers DIAL-enabled devices using SSDP (multicast discovery).
2. For each device found, it fetches the device description to get the app launch URL.
3. It sends a POST request to launch YouTube with a random video ID.
4. The TV should start playing the video within seconds.

## Supported devices

DIAL Connect works with any device that supports the DIAL protocol:

- **Google Chromecast** (all versions)
- **Android TV** (Sony, Philips, TCL, etc.)
- **Roku** devices
- **Samsung Smart TVs** (Tizen-based)
- **LG Smart TVs** (webOS)
- **Fire TV** devices

## Troubleshooting

- **No devices found**: Make sure GhostESP is connected to the same network as your smart TVs. Run `connect SSID PASSWORD` first.
- **404 errors**: The YouTube app may not be installed or available on that device.
- **403 errors**: The device may have DIAL restrictions enabled. Some Roku devices require enabling "Screen Mirroring" in settings.
- **Connection timeouts**: The device may be on a different subnet or have firewall rules blocking DIAL.

## How it works

DIAL (Discovery and Launch) is a protocol developed by Netflix and YouTube that allows second-screen devices to discover and launch apps on first-screen devices (TVs). GhostESP uses this protocol to:

1. Send an SSDP M-SEARCH request to discover DIAL devices.
2. Fetch device descriptions to get the Application-URL.
3. POST to the YouTube app endpoint with a video ID parameter.

The TV receives the launch request and starts playing the specified video.
