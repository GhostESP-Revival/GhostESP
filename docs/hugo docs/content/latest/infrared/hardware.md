---
title: "Hardware Support"
description: "Boards and peripherals required for GhostESP infrared"
weight: 40
---

## Supported boards

GhostESP exposes Infrared features only when the target board is built with Infrared RX or TX enabled

- LilyGo S3TWatch — IR transmit
- ESP32-S3 Cardputer / Cardputer ADV — IR transmit
- LilyGo TEmbed C1101 — IR transmit and receive

Other boards may ship with TX-only support or no IR hardware at all. Look for infrared support listed in the release notes or above before flashing.

## Wiring and Developer notes

- **IR LED**: Connected to the configured TX pin (`CONFIG_INFRARED_LED_PIN`). Ensure correct polarity and a current-limiting resistor if you add an external LED.
- **IR Receiver**: Required for learning. Boards with `CONFIG_HAS_INFRARED_RX` set expose the pin via `CONFIG_INFRARED_RX_PIN`.

## Power and positioning

- Keep the LED unobstructed; align it with the target device for reliable transmission.
- For learning, place the source remote close to the receiver window and shield it from direct sunlight or fluorescent flicker.
