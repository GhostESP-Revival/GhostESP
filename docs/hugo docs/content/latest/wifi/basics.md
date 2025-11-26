---
title: "Wi-Fi Basics"
description: "Learn what Wi-Fi attacks are and how they work before diving into tools."
weight: 5
---

Wi-Fi attacks sound complex, but they're built on a few simple ideas. This page explains what each attack does, why someone might use it, and what you need to know before trying the tools in GhostESP.

## What is a Wi-Fi attack?

A Wi-Fi attack is any action that disrupts, intercepts, or tricks Wi-Fi networks or devices. Most attacks fall into two categories:

- **Passive**: You listen and record without sending anything. Example: capturing packets to see what devices are on a network.
- **Active**: You send frames or signals to change how the network behaves. Example: sending deauth frames to kick devices off.

## Common attacks explained

### Packet Capture
**What it does**: Records all Wi-Fi traffic on a channel and saves it to a file (PCAP format).

**Why**: You can analyze the data later to see what devices are connecting, what SSIDs they're looking for, or extract handshakes for password testing.

**Legal note**: Capturing traffic on networks you don't own is illegal in most places. Only do this on your own network or with explicit permission.

**Learn more**: [Capturing packets]({{< relref "capture.md" >}})

### Deauthentication (Deauth)
**What it does**: Sends a fake "disconnect" message to a device, forcing it off the network.

**Why**: Tests network stability, forces devices to reconnect (so you can capture their handshake), or disrupts a specific device.

**Legal note**: Deauthing devices you don't own is illegal. Only test on your own network.

### Karma Attack
**What it does**: Pretends to be a Wi-Fi network that a device is looking for, tricking the device into connecting.

**Why**: Intercept traffic from devices, test how devices behave when connecting to fake networks, or study device behavior.

**Legal note**: Tricking devices into connecting without consent is illegal in most jurisdictions.

**Learn more**: [Karma attacks]({{< relref "karma.md" >}})

### Evil Portal
**What it does**: Creates a fake Wi-Fi network with a login page that looks real (like a coffee shop Wi-Fi).

**Why**: Capture credentials, redirect users to a phishing site, or test how users respond to fake login pages.

**Legal note**: Impersonating a real network or capturing credentials without consent is illegal.

**Learn more**: [Evil portals]({{< relref "evil-portal.md" >}})

### Handshake Capture
**What it does**: Records the WPA/WPA2 authentication handshake between a device and router.

**Why**: Export the handshake to offline password cracking tools to test weak passwords.

**Legal note**: Cracking passwords on networks you don't own is illegal.

**Learn more**: [Handshakes]({{< relref "handshakes.md" >}})

## Before you start

### Prerequisites
- A GhostESP device (flashed and working)
- An SD card (for saving captures)
- A network you own or have permission to test
- Basic understanding of Wi-Fi (SSIDs, channels, 2.4 GHz vs 5 GHz)

### Legal and ethical rules
- **Only test networks you own or have written permission to test.**
- Unauthorized access to networks or devices is illegal in most countries.
- Disrupting networks (deauth, jamming) without permission is illegal.
- Impersonating networks or capturing credentials is illegal.
- Check your local laws before testing.

### Typical workflow
1. **Scan** the area to find networks and devices (see [Survey]({{< relref "survey.md" >}})).
2. **Choose an attack** based on what you want to learn (see attack descriptions above).
3. **Capture or execute** using the GhostESP menu or CLI.
4. **Analyze** the results on your computer (e.g., open PCAP files in Wireshark).

## Next steps

Pick an attack that interests you and read the detailed guide:
- [Capturing packets]({{< relref "capture.md" >}}) — Record Wi-Fi traffic
- [Handshakes]({{< relref "handshakes.md" >}}) — Extract authentication data
- [Karma attacks]({{< relref "karma.md" >}}) — Trick devices into connecting
- [Evil portals]({{< relref "evil-portal.md" >}}) — Create fake login pages
- [Survey]({{< relref "survey.md" >}}) — Scan for networks and devices
