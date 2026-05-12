---
title: "Frequency Analyzer"
description: "Use the NRF24 analyzer from the UI or CLI"
weight: 10
---

The NRF24 analyzer scans 2.4 GHz channels and shows relative RF activity from `2.400 GHz` to `2.525 GHz`.

It also includes a built-in **jamming detection engine** that identifies known threat signatures in real time.

## Prerequisites

- Firmware built with NRF24 support (`CONFIG_HAS_NRF24=y`)
- Supported NRF24L01/NRF24L01+ module connected to the configured SPI pins
- 2.4 GHz antenna connected to the module

If you see `built without CONFIG_HAS_NRF24`, the firmware was built without local NRF24 analyzer support.

## On-device UI

1. Open **Menu -> NRF24 -> Frequency Analyzer**.
2. Watch the graph while the scan cursor sweeps channels.
3. Use **Pause/Resume** to freeze or continue sampling.
4. Select **Back** to exit.

## CLI

Run from the GhostESP terminal:

```text
nrf24 start
nrf24 pause
nrf24 resume
nrf24 status
nrf24 stop
```

`nrf24 status` prints running state, pause state, last error, and active pin config.

## Reading the graph

- **X axis:** frequency in GHz (`2.400` to `2.525`)
- **Y axis:** activity percentage (`0-100%`)
- **Bars:** current activity estimate per channel
- **White markers:** recent peak levels
- **Gray cursor:** current scan position
- **Red alert bar:** appears across the top of the graph when jamming is detected, showing the threat classification and peak channel
- **Red vertical marker:** highlights the peak signal channel during a detection event

## Jamming Detection

The analyzer continuously monitors RF activity patterns and can identify several known threat signatures from firmware observed in the wild. Detection runs passively alongside normal spectrum scanning with no additional hardware required.

### How It Works

The detector analyses channel activity in each scan tick by checking:

1. **Consecutive channel runs** -- long unbroken stretches of high signal indicate a sequential sweep
2. **Gap channel activity** -- channels between WiFi bands (e.g. 24-25, 49-50, 74-83) are normally quiet; activity here suggests broadband jamming
3. **Known channel pattern matching** -- specific channel arrays associated with known threat actors
4. **Simultaneous carrier detection** -- multiple channels at near-100% signal at the same time indicates multi-radio hardware
5. **Confidence counter** -- detection must persist for 6 consecutive ticks (~270ms) before triggering, preventing false alarms from transient noise

### Threat Classifications

When a jamming event is confirmed, the status line below the graph changes from the normal peak readout to a threat classification:

| Display Label | Behavior |
|---|---|
| `BROADBAND NOISE` | Full-band sequential sweep across channels 1-83, often with gap channel saturation |
| `SIG: FW-A BLE ADV` | Targeted BLE advertising channel jamming on {2, 26, 80} with low total active channel count |
| `SIG: FW-A WIFI` | WiFi-band jamming using a 15-channel step-5 pattern (channels spaced 5 MHz apart) |
| `SIG: FW-A HI-BAND` | Targeted upper-band jamming on channels 76, 78, 79 |
| `SIG: FW-B BLE ADV` | Alternative BLE advertising channel sweep on {2, 26, 80} with multi-radio characteristics |
| `SIG: FW-B WIFI` | Contiguous lower-band block sweep channels 1-12, indicative of continuous carrier flooding |
| `SIG: FW-B HI-BAND` | Alternative upper-band targeting on 76, 78, 79 with simultaneous carriers |
| `SIG: FW-B TRI-RADIO` | Simultaneous jamming on 3+ disparate channels at near-max signal -- consistent with multi-radio hardware |
| `GENERIC SWEEP` | Broadband activity detected that does not match any known signature |

### Firmware A vs Firmware B Signatures

The detector distinguishes two primary threat actor profiles based on their channel behavior:

**Firmware A** patterns are characterized by:
- Sequential channel sweeping with controlled dwell times
- A distinctive 15-channel WiFi pattern with 5-channel step spacing
- Single-radio operation (one active carrier at a time)
- Targeted protocol modes (BLE advertising, upper-band NRF)

**Firmware B** patterns are characterized by:
- Contiguous block sweeps (e.g. channels 1-12 without gaps)
- Simultaneous multi-carrier injection (3 concurrent CW signals)
- Random channel selection within target arrays rather than sequential hops
- Tri-radio hardware configuration producing 3 simultaneous carriers

### Signal Strength and Direction Finding

The detection readout includes:

- **Peak channel** -- the channel with the strongest detected signal, shown as `CH:XXX`
- **Average signal strength** -- mean activity percentage across all active jammed channels
- **Red vertical marker** -- drawn on the graph at the peak channel position

To locate the source, rotate or move the device while watching the signal strength percentage. The direction where the percentage is highest is the likely direction of the jammer.

## Notes

- This is an activity analyzer with passive threat detection, not a packet decoder.
- It is useful for channel occupancy checks, interference hunting, and jamming identification.
- Scan smoothness and responsiveness depend on analyzer tuning values in Kconfig.
- Detection confidence requires sustained activity; momentary spikes will not trigger false alerts.
