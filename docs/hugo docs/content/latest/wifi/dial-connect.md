---
title: "DIAL Connect"
description: "Cast YouTube videos to smart TVs on your network"
weight: 61
---

Cast random YouTube videos to all DIAL-enabled devices (Chromecasts, smart TVs, Roku, etc.) on your network. Connect GhostESP to the same Wi-Fi network as the target devices first.

> **Note:** Only test on networks and devices you own or have permission to test. See [Legal and ethical rules]({{< relref "basics.md#legal-and-ethical-rules" >}}).

## How to use

### On-device UI

1. First connect to a Wi-Fi network: **Menu → Wi-Fi → Connection → Connect to Wi-Fi**.
2. Go to **Menu → Wi-Fi → Gadgets → TV Cast (Dial Connect)**.
3. The device will discover smart TVs and cast a random YouTube video.

### CLI

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

## How it works

DIAL (Discovery and Launch) is a protocol developed by Netflix and YouTube that allows second-screen devices to discover and launch apps on first-screen devices (TVs). GhostESP:

1. Sends an SSDP M-SEARCH request to discover DIAL devices.
2. Fetches the device description to get the Application-URL (the app launch URL).
3. POSTs to the YouTube app endpoint with a random video ID.

The TV receives the launch request and starts playing the specified video within seconds.

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
