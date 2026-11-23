---
title: "Flock Detection"
description: "Detect Flock Safety surveillance cameras and related infrastructure on 2.4 GHz Wi-Fi"
weight: 65
---

GhostESP's Flock detector monitors 2.4 GHz Wi-Fi traffic for Flock Safety cameras, extended battery units, and Penguin surveillance devices. It matches transmitter, receiver, and BSSID addresses against 38 known Flock/Penguin MAC prefixes, flags wildcard probe requests, and matches SSID keywords such as `flock`, `FS Ext Battery`, `Penguin`, or `Pigvision`, ranking each detection by confidence. It hops the active channel plan (the `hop` profile, or your country's channel list; a fresh default is US, channels 1-11) with 250 ms dwell time, and runs from any ESP32 board with Wi-Fi capability via serial, GhostLink, or the display.

Based on [bennjordan/flock-you](https://github.com/bennjordan/flock-you).

{{< flow title="How a Flock detection is made" caption="Each frame is checked against known OUIs, SSID keywords, and the wildcard-probe pattern." >}}
Hop the active channel plan | 250 ms dwell per channel
Match MAC addresses against 38 known Flock/Penguin prefixes | Checked as transmitter, receiver, and BSSID
Flag wildcard probe requests | A probe with no SSID asking for any network
Match SSID keywords | `flock`, `FS Ext Battery`, `Penguin`, `Pigvision`
Rank confidence | HIGH for a wildcard or SSID match, LOW for OUI alone; frames weaker than -95 dBm are ignored
{{< /flow >}}

## Using the detector

### Via terminal

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

### Via display

1. **Open** the on-device **Wi-Fi** menu.
2. **Select** **Flock Camera Detection**.
3. The scan starts automatically and detections appear in the terminal pane.

## What gets detected

| Category | OUI Count | Examples |
|---|---|---|
| Flock Wi-Fi cameras | 9 | `70:c9:4e`, `3c:91:80`, `d8:f3:bc` |
| FS Ext Battery units | 10 | `58:8e:81`, `cc:cc:cc`, `ec:1b:bd` |
| Penguin surveillance | 19 | `cc:09:24`, `ed:c7:63`, `e8:ce:56` |

Each detection record stores MAC, detection method, confidence level, signal strength, channel, hit count, and SSID (if available).

## Confidence levels

| Level | Trigger | Behavior |
|---|---|---|
| **HIGH** | Wildcard probe + OUI match, or SSID keyword match | Real-time log alert + white RGB pulse |
| **LOW** | OUI match only (no wildcard probe or SSID) | Recorded silently, visible in `flocklist` |

A device initially seen at LOW confidence will be upgraded to HIGH if it later exhibits a high-confidence signal (e.g., sends a wildcard probe).

## Detection methods

| Method | Description |
|---|---|
| OUI (transmitter) | Source MAC matches a known prefix |
| OUI (receiver) | Destination MAC matches - catches sleeping cameras that don't transmit |
| OUI (BSSID) | Access point BSSID matches |
| Wildcard probe | Probe request with empty SSID + OUI match |
| SSID keyword | Beacon/probe response SSID contains a known keyword |

## Troubleshooting

- **No detections**: Move closer to suspected devices. Flock cameras spend most of their duty cycle asleep and only transmit briefly during uploads.
- **Too many LOW-confidence hits**: OUI-only matches can include non-Flock devices sharing the same chip vendor. Focus on HIGH-confidence detections.
- **False positives on wildcard probes**: Rare but possible. Cross-reference signal strength and channel with physical observation.
