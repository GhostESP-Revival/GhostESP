---
title: "Wardriving"
description: "Capture Wi-Fi and BLE observations with GPS data for mapping and analysis"
weight: 5
toc: true
---

Wardriving in GhostESP records nearby wireless observations with GPS coordinates into WiGLE-compatible CSV files on SD.

## Before You Start

- GPS module connected and receiving NMEA data
- SD card mounted (recommended for persistent CSV files)
- For BLE wardriving: a non-ESP32-S2 build
- Optional for split-channel mode: a second GhostESP linked via GhostLink

For best results, wait for a valid 2D/3D fix before expecting CSV growth. Indoor starts and poor sky visibility usually cause heavy GPS rejection.

## Quick Start (Display)

### Wi-Fi

1. Connect GPS and insert SD card
2. Open **GPS** -> **Start Wardriving**
3. Wait for GPS lock (`2D`/`3D` preferred)
4. Move through your route and let channel hopping run
5. End with **Stop Wardriving**

### BLE

1. Connect GPS and insert SD card
2. Open **GPS** -> **BLE Wardriving**
3. Wait for GPS lock
4. Move through your route
5. Stop BLE wardriving from the same menu

## CLI Quick Start

### Wi-Fi

```bash
startwd
```

Stop:

```bash
startwd -s
```

### BLE

```bash
blewardriving
```

Stop:

```bash
blewardriving -s
```

Global stop for active scans/tasks:

```bash
stop
```

## What It Captures

### Wi-Fi Wardriving (`startwd`)

- Wi-Fi AP observations (BSSID, SSID, auth mode, channel, RSSI)
- GPS position/quality fields with each accepted observation
- Hidden SSIDs logged as `<hidden>`

Wi-Fi logging is intentionally deduped: an AP is re-logged only when the observation is materially better.

### BLE Wardriving (`blewardriving`)

- BLE MAC, name (when present), appearance/manufacturer metadata, RSSI
- GPS position/quality fields with each accepted observation

BLE logging is also deduped to reduce noise and file bloat.

## How Wi-Fi Scanning Works

During `startwd`, GhostESP:

1. Starts monitor mode and listens to management frames
2. Processes beacon/probe-response style AP observations
3. Parses SSID, BSSID, channel, and security IEs from frames
4. Hops channels on a timer (default 100 ms)
5. Sends a wildcard probe request on each hop to stimulate AP responses
6. Applies GPS validity gates (valid fix, at least 2D, enough satellites) before accepting rows

If GPS quality drops, observations are rejected (counted in `gpsrej`) rather than writing low-quality rows.

## Dedupe Rules (Exact Behavior)

### Wi-Fi dedupe (`BSSID` key)

An observation is logged when at least one condition is true:

- First time this `BSSID` is seen
- Previous SSID was hidden/empty and now SSID is known
- RSSI improves by more than `3 dBm` vs best seen value

### BLE dedupe (`MAC` key)

An observation is logged when at least one condition is true:

- First time this BLE MAC is seen
- Device name was previously empty and now available
- RSSI improves by more than `5 dBm`

### Practical implications

- Revisiting the same APs/devices with weaker or similar RSSI usually does not grow CSV fast
- Dedupe state is committed only after a successful CSV write (prevents losing entries on failed writes)
- Dedupe tables are bounded (larger with PSRAM), so very long sessions can eventually evict older entries

## Channel Hopping and Split-Channel Helper

- Default hop interval is `100 ms`
- Channel list is built from current Wi-Fi country configuration (with safe fallback channels if unavailable)
- In single-device mode, primary scans the full local channel list
- In split-channel mode with GhostLink:
  - Primary prefers 5 GHz when both 2.4 and 5 GHz are available
  - Helper prefers 2.4 GHz (or configured helper channels)
  - Helper observations are streamed back and merged into the primary CSV pipeline
- If GhostLink drops mid-session, primary automatically continues local-only scanning

Advanced helper commands:

```bash
startwd --helper
startwd --helper --channels 1,6,11
startwd -s --helper
```

## Output Files

- CSV directory: `/mnt/ghostesp/gps/`
- Wi-Fi files: `wardriving_<n>.csv`
- BLE files: `ble_wardriving_<n>.csv`
- Format: WiGLE-compatible CSV headers/rows

Use normal stop actions (`startwd -s`, `blewardriving -s`, or `stop`) so buffered rows are flushed before ending a session.

## Reading Wardriving Status Output

You may see log lines similar to:

```text
Wardrive: ap=421 logged=133/192 gpsrej=59 helper=12/20 ch=6 up=3m10s gps=3D/9 pending=0B heap=182000/120000B
```

- `ap` is total Wi-Fi observations seen by wardriving callbacks, not unique AP count
- `logged=x/y` is accepted logging calls vs attempts (CSV growth may still be slower because of dedupe and GPS/date gates)
- `gpsrej` counts observations rejected because GPS validity checks failed
- `helper=merged/received` shows peer contributions accepted on primary
- `pending` and `heap` help diagnose buffering pressure and memory headroom

In helper mode, status lines include helper transmit counters (`tx(...)`) and stream send success/fail (`send(ok/fail)`).

## Data Quality Tips

- Allow GPS to settle before route start (open sky, minimal obstructions)
- Drive/walk at steady speed so channel hops can revisit channels consistently
- Watch `gpsrej`; high values usually mean weak fix or poor satellite geometry
- Use `stop`/`-s` commands before power-off so final buffered rows are flushed
- If AP growth seems "stuck," remember dedupe suppresses repeated weak sightings

## Troubleshooting

- **No AP growth**: confirm monitor mode is active and you are not only revisiting already-logged APs at weaker RSSI
- **High `gpsrej` count**: move to a better sky view, wait for stable lock, verify GPS wiring/power
- **BLE wardriving unavailable**: BLE wardriving is not supported on ESP32-S2 builds
- **`Standalone` while expecting GhostLink**: connect peer before `startwd` and verify link stability
- **Low helper merge ratio (`helper=merged/received`)**: check GPS quality on primary and GhostLink signal quality
- **No CSV output**: verify `/mnt/ghostesp/gps/` exists, SD is writable, and sessions are stopped cleanly

## Next Step

After capture, see [WiGLE Upload](/latest/gps/wigle/) to upload CSV files.
