---
title: "SubGHz"
description: "Overview of GhostESP's SubGHz radio capabilities"
keywords: ["subghz", "radio", "433MHz", "garage door", "remote control", "CC1101"]
weight: 120
aliases:
  - "/subghz/"
---

GhostESP includes SubGHz radio tools for scanning, capturing, and transmitting signals on common frequency bands. Use these features to work with garage door remotes, gate openers, wireless sensors, and other sub-1 GHz devices. SubGHz requires a CC1101-based radio module; not all GhostESP boards include this hardware (see [Hardware Support]({{< relref "hardware.md" >}})).

## Supported Frequency Bands

- 315 MHz
- 390 MHz
- 433.92 MHz
- 868.35 MHz
- 915 MHz

## Features

- **Signal scanning** - Monitor activity across 64 channels in real-time
- **Frequency analyzer** - Visualize signal strength across bands to find active frequencies
- **Waterfall spectrum analyzer** - View a stable 5-band RSSI waterfall with 320 real RF bins per sweep
- **Signal capture** - Record and decode signals from remotes and transmitters
- **Protocol decoding** - Automatic detection of 30+ common protocols
- **Signal transmission** - Replay captured signals to control devices
- **Saved signals** - Store and manage captured signals as `.sub` files
- **Flipper compatibility** - Uses Flipper SubGhz Key File format

## Technical Implementation

GhostESP uses **dedicated protocol decoders based on Flipper Zero Unleashed/xMasterX firmware**, with precise per-protocol timing constants and real-time edge-queue processing. This covers 30+ protocols including Manchester and other multi-bit encodings, and supports a wider range of static-code protocols than generic libraries such as RCSwitch (10-15 protocols).

Recently added support includes Ansonic, Bett, Clemsa, Dickert MAHS, Dooya, Elplast, Marantec24, Hollarm, Hay21, Feron, Roger, Treadmill37, KeyFinder, and Nord ICE.

## Getting Started

- [Scanning]({{< relref "scanning.md" >}}) - Monitor radio activity
- [Capturing Signals]({{< relref "capturing.md" >}}) - Record and decode signals
- [Waterfall Spectrum Analyzer]({{< relref "waterfall.md" >}}) - Visualize RSSI activity across common bands
- [Transmitting]({{< relref "transmitting.md" >}}) - Replay captured signals
- [Frequency Analyzer]({{< relref "freq-analyzer.md" >}}) - Find active frequencies
- [Supported Protocols]({{< relref "protocols.md" >}}) - Protocol reference
- [Files and Management]({{< relref "files.md" >}}) - `.sub` file format and saved signals
- [Hardware Support]({{< relref "hardware.md" >}}) - Required boards and peripherals
