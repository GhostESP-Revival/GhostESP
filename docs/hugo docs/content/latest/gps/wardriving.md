---
title: "Wardriving"
description: "Capture Wi-Fi and BLE observations with GPS data for mapping and analysis"
weight: 5
---

Wardriving in GhostESP records nearby wireless observations with GPS coordinates into WiGLE-compatible CSV files on SD.

## Before You Start

- GPS module connected and receiving NMEA data
- SD card mounted (recommended for persistent CSV files)
- For BLE wardriving: a non-ESP32-S2 build
- Optional for split-channel mode: a second GhostESP linked via GhostLink

For best results, wait for a valid 2D/3D fix before expecting CSV growth. Indoor starts and poor sky visibility usually cause heavy GPS rejection.

{{< flow title="What a wardrive does" caption="Rows are only written once there is a usable GPS fix." >}}
Scan Wi-Fi (and optionally BLE) while hopping channels | Each sighting carries an RSSI
Tag sightings with the current GPS fix | A 2D/3D fix with 3+ satellites is required
Deduplicate by BSSID/MAC | Wi-Fi: first sight, hidden→known, or a >3 dB RSSI change;; BLE: a >5 dB change
Append rows to the CSV | Written to `/mnt/ghostesp/gps/wardriving_<n>.csv`
{{< /flow >}}

## Quick Start (Display)

### Wi-Fi

1. Connect GPS and insert SD card
2. Open **GPS** → **Start Wardriving**
3. Wait for GPS lock (`2D`/`3D` preferred)
4. Move through your route and let channel hopping run
5. End with **Stop Wardriving**

### BLE

1. Connect GPS and insert SD card
2. Open **GPS** → **BLE Wardriving**
3. Wait for GPS lock
4. Move through your route
5. Stop BLE wardriving from the same menu

## CLI Quick Start

### Wi-Fi (CLI)

```bash
startwd [--hop <ms>] [--weighted] [--active|--monitor] [--primary-channels <list>]
```

Note: `--hop` and `--weighted` are parsed but only take effect in the `--helper` branch - a primary `startwd` ignores them.

Stop:

```bash
startwd -s
```

### BLE (CLI)

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

### Other Wardriving Commands

- `wdstream start|stop|status` - `start` accepts `-wifi`, `-ble`, `-i <ms>`, and `-ch auto|1|1,6,11`.
- `dualwd` - dual wardriving mode.
- `gpspin <0-48>` - set the GPS UART pin.
- `gpsbaud <auto|0|4800|9600|19200|38400|57600|115200>` - set the GPS baud rate.
- `gpsinfo [-s]` - show GPS information; `-s` adds detail.

## What It Captures

### Wi-Fi Wardriving (`startwd`)

- Wi-Fi AP observations (BSSID, SSID, auth mode, channel, RSSI)
- GPS position/quality fields with each accepted observation
- Hidden SSIDs logged as `<hidden>`

### BLE Wardriving (`blewardriving`)

- BLE MAC, name (when present), appearance/manufacturer metadata, RSSI
- GPS position/quality fields with each accepted observation

## How Wi-Fi Scanning Works

During `startwd`, GhostESP:

1. Starts monitor mode and listens to management frames
2. Processes beacon/probe-response style AP observations
3. Parses SSID, BSSID, channel, and security IEs from frames
4. Hops channels on a timer (default 125 ms)
5. Sends a wildcard probe request on each hop to stimulate AP responses
6. Applies GPS validity gates (valid fix, at least 2D, enough satellites) before accepting rows

If GPS quality drops, observations are rejected (counted in `gpsrej`) rather than writing low-quality rows.

## Dedupe Rules (Exact Behavior)

### Wi-Fi Dedupe (`BSSID` Key)

An observation is logged when at least one condition is true:

- First time this `BSSID` is seen
- Previous SSID was hidden/empty and now SSID is known
- RSSI differs by more than `3 dBm` vs best seen value (triggers on both improvement and degradation)

### BLE Dedupe (`MAC` Key)

An observation is logged when at least one condition is true:

- First time this BLE MAC is seen
- Device name was previously empty and now available
- RSSI differs by more than `5 dBm` (either improvement or degradation)

### Practical Implications

- Revisiting the same APs/devices with weaker or similar RSSI usually does not grow CSV fast
- Dedupe state is committed before the asynchronous SD flush, so a later write failure does not roll it back
- Dedupe tables are bounded (larger with PSRAM), so very long sessions can eventually evict older entries

## Channel Hopping and Split-Channel Helper

- Default hop interval is `125 ms` for both primary and helper; it is configurable over 40-1000 ms
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
Wardrive: ap=421 logged=133/192 gpsrej=59 helper=12/20 peergps(rx/fix tx_ok/fail)=40/38 38/2 ch=6 up=3m10s gps=3D/9 q=0/64 hi=5 drop=0/0/0 pending=0B heap=182000/120000B
```

- `ap` is total Wi-Fi observations seen by wardriving callbacks, not unique AP count
- `logged=x/y` is accepted logging calls vs attempts (CSV growth may still be slower because of dedupe and GPS/date gates)
- `gpsrej` counts observations rejected because GPS validity checks failed
- `helper=merged/received` shows peer contributions accepted on primary
- `peergps(rx/fix tx_ok/fail)` is peer GPS stream packets received / with a fix, plus peer GPS stream send successes/failures
- `q=depth/capacity` is observation queue depth vs capacity; `hi` is the queue high-water mark
- `drop=local/helper/sink` counts observations dropped by the local queue, helper, or sink
- `pending` and `heap` help diagnose buffering pressure and memory headroom

In helper mode, status lines include helper transmit counters (`tx(...)`) and stream send success/fail (`send(ok/fail)`).

## Data Quality Tips

- Allow GPS to settle before route start (open sky, minimal obstructions)
- Drive/walk at steady speed so channel hops can revisit channels consistently
- Watch `gpsrej`; high values usually mean weak fix or poor satellite geometry
- Use `stop`/`-s` commands before power-off so final buffered rows are flushed

## Troubleshooting

- **No AP growth**: confirm monitor mode is active and you are not only revisiting already-logged APs at weaker RSSI
- **High `gpsrej` count**: move to a better sky view, wait for stable lock, verify GPS wiring/power
- **BLE wardriving unavailable**: BLE wardriving is not supported on ESP32-S2 builds
- **`Standalone` while expecting GhostLink**: connect peer before `startwd` and verify link stability
- **Low helper merge ratio (`helper=merged/received`)**: check GPS quality on primary and GhostLink signal quality
- **No CSV output**: verify `/mnt/ghostesp/gps/` exists, SD is writable, and sessions are stopped cleanly

## Related tasks

After capture, see [WiGLE Upload]({{< relref "wigle.md" >}}) to upload CSV files.

For wardriving without a GPS module on your GhostESP device, see [Companion Phone GPS Wardriving]({{< relref "companion-phone-wardriving.md" >}}).
