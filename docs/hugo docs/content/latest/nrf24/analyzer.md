---
title: "Frequency Analyzer"
description: "Use the NRF24 analyzer from the UI or CLI"
weight: 10
---

The NRF24 analyzer scans 2.4 GHz channels and shows relative RF activity from `2.400 GHz` to `2.525 GHz`.

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

## Notes

- This is an activity analyzer, not a packet decoder.
- It is useful for channel occupancy checks and interference hunting.
- Scan smoothness and responsiveness depend on analyzer tuning values in Kconfig.
