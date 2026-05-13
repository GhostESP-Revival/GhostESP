---
title: "Flock Detection"
description: "Detect Flock Safety surveillance cameras and related infrastructure on 2.4 GHz Wi-Fi."
weight: 65
---

GhostESP's Flock detector monitors 2.4 GHz Wi-Fi traffic for Flock Safety cameras, extended battery units, and Penguin surveillance devices. It uses OUI prefix matching, wildcard probe detection, and SSID keyword matching to identify devices with tiered confidence levels.

Based on [bennjordan/flock-you](https://github.com/bennjordan/flock-you).

## Overview

- **OUI matching** — Checks transmitter, receiver, and BSSID addresses against 38 known Flock/Penguin MAC prefixes.
- **Wildcard probe detection** — Flock cameras channel-hop and broadcast probe requests with an empty SSID; combining this with an OUI match yields a high-confidence signature.
- **SSID keyword matching** — Beacons and probe responses carrying keywords like `flock`, `FS Ext Battery`, `Penguin`, or `Pigvision` are flagged regardless of OUI.
- **Confidence tiers** — Only high-confidence detections (wildcard probe or SSID keyword) trigger real-time alerts and RGB pulse. Low-confidence OUI-only matches are recorded silently and visible in the device list.
- **Channel hopping** — Scans all 14 Wi-Fi channels with 250 ms dwell time to catch devices that hop between channels.

## Prerequisites

- Any ESP32 board with Wi-Fi capability.
- Serial, GhostLink or Display access.

## How to Use

### Via Terminal

1. **Start** scanning for surveillance devices:
   ```
   flockscan
   ```
2. **List** detected devices:
   ```
   flocklist
   ```
3. **Stop** scanning:
   ```
   flockstop
   ```

### Via Display

1. **Open** the on-device **Wi-Fi** menu.
2. **Select** **Flock Detection**.
3. The scan starts automatically and detections appear in the terminal pane.

## What Gets Detected

| Category | OUI Count | Examples |
|---|---|---|
| Flock WiFi cameras | 9 | `70:c9:4e`, `3c:91:80`, `d8:f3:bc` |
| FS Ext Battery units | 10 | `58:8e:81`, `cc:cc:cc`, `ec:1b:bd` |
| Penguin surveillance | 19 | `cc:09:24`, `ed:c7:63`, `e8:ce:56` |

Each detection record stores MAC, detection method, confidence level, signal strength, channel, hit count, and SSID (if available).

## Confidence Levels

| Level | Trigger | Behavior |
|---|---|---|
| **HIGH** | Wildcard probe + OUI match, or SSID keyword match | Real-time log alert + white RGB pulse |
| **LOW** | OUI match only (no wildcard probe or SSID) | Recorded silently, visible in `flocklist` |

A device initially seen at LOW confidence will be upgraded to HIGH if it later exhibits a high-confidence signal (e.g., sends a wildcard probe).

## Detection Methods

| Method | Description |
|---|---|
| OUI (transmitter) | Source MAC matches a known prefix |
| OUI (receiver) | Destination MAC matches — catches sleeping cameras that don't transmit |
| OUI (BSSID) | Access point BSSID matches |
| Wildcard probe | Probe request with empty SSID + OUI match |
| SSID keyword | Beacon/probe response SSID contains a known keyword |

## Troubleshooting

- **No detections**: Move closer to suspected devices. Flock cameras spend most of their duty cycle asleep and only transmit briefly during uploads.
- **Too many LOW-confidence hits**: OUI-only matches can include non-Flock devices sharing the same chip vendor. Focus on HIGH-confidence detections.
- **False positives on wildcard probes**: Rare but possible. Cross-reference signal strength and channel with physical observation.
