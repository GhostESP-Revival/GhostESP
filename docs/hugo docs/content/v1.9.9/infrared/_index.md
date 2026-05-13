---
title: "Infrared"
description: "Overview of GhostESP's infrared capabilities"
keywords: ["infrared", "IR", "remote control", "signal learning", "NEC", "Kaseikyo"]
weight: 30
aliases:
  - "/infrared/"
---

GhostESP lets you learn IR signals, store them on the SD card, and retransmit them using the onboard LED.

## Supported protocols

- NEC / NECext
- Kaseikyo
- Pioneer
- RCA
- Samsung
- SIRC (12-bit, 15-bit, 20-bit)
- RC5
- RC6

If a remote uses an unsupported protocol, GhostESP still attempts to capture raw timings so you can replay the signal.
