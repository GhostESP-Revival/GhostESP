---
title: "SubGHz"
description: "Overview of GhostESP's SubGHz radio capabilities"
keywords: ["subghz", "radio", "433MHz", "garage door", "remote control", "CC1101"]
weight: 120
aliases:
  - "/subghz/"
---

GhostESP includes SubGHz radio tools for scanning, capturing, and transmitting signals on common frequency bands. Use these features to work with garage door remotes, gate openers, wireless sensors, and other sub-1 GHz devices.

## Supported frequency bands

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

## Technical implementation

GhostESP uses **dedicated protocol decoders based on Flipper Zero Unleashed/xMasterX firmware**, providing professional-grade signal analysis capabilities:

- **30+ protocol decoders** with precise timing constants for each protocol
- **Real-time decoding engine** with edge queue processing
- **Proven implementation** - same decoders used by industry-standard Flipper Zero
- **Supports complex encodings** including Manchester and multi-bit protocols

Recently added support includes Ansonic, Bett, Clemsa, Dickert MAHS, Dooya, Elplast, Marantec24, Hollarm, Hay21, Feron, Roger, Treadmill37, KeyFinder, and Nord ICE.

Unlike other ESP32 firmwares that rely on generic libraries like RCSwitch (10-15 protocols), GhostESP's dedicated decoders offer superior accuracy and support for a wider range of static code protocols.

## Hardware requirements

SubGHz requires a CC1101-based radio module. Not all GhostESP boards include this hardware. See [Hardware Support]({{< relref "hardware.md" >}}) for details.

## Getting started

- [Scanning]({{< relref "scanning.md" >}}) - Monitor radio activity
- [Capturing Signals]({{< relref "capturing.md" >}}) - Record and decode signals
- [Transmitting]({{< relref "transmitting.md" >}}) - Replay captured signals
- [Frequency Analyzer]({{< relref "freq-analyzer.md" >}}) - Find active frequencies
- [Waterfall Spectrum Analyzer]({{< relref "waterfall.md" >}}) - Visualize RSSI activity across common bands
- [Supported Protocols]({{< relref "protocols.md" >}}) - Protocol reference
