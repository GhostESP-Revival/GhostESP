---
title: "M5Stack AtomS3R"
description: "Using the M5Stack AtomS3R with GhostESP, including its button navigation and Grove port."
weight: 20
toc: true
keywords: ["M5Stack AtomS3R", "AtomS3R", "Grove", "ENV-III", "GPS", "ST25R3916"]
---

## Overview

The **M5Stack AtomS3R** is a compact ESP32-S3 board with a small display and a Grove connector. The display is **not** touch-enabled — the entire display panel is a single button, so navigation is driven by pressing the screen itself. Use the `M5Stack_AtomS3R.zip` build from the [supported hardware]({{< relref "../getting-started/supported-hardware.md" >}}) list.

## Navigation

The screen is a push button, not a touch panel. Press down on the display to activate it.

| Action | How |
|---|---|
| **Move down** | Quick single press |
| **Move up** | Two quick presses |
| **Select** | Hold for about 0.5 seconds, then release |
| **Go back** | Triple press, or hold for about 2 seconds |

The display wakes when you press the button. Menu actions are processed when you release it.

## Grove port

Connect a Grove module to the AtomS3R's HY2.0-4P Grove port. The port is connected to I2C port 1:

| Signal | GPIO |
|---|---:|
| SDA | G2 |
| SCL | G1 |

The port can be used with:

- **M5Stack ENV-III:** open the **ENV-III** app after connecting it. The sensor uses SHT30 (`0x44`) and QMP6988 (`0x70`).
- **ST25R3916:** connect an I2C module and open the NFC app. This Grove setup is currently **untested**. The configured I2C address is `0x50`.
- **GPS:** connect a compatible Grove GPS module and configure its GPS RX pin and baud rate if the module does not match the current settings. The default GPS RX pin is GPIO2 at 9600 baud.

The ST25R3916 and ENV-III both use I2C port 1 and can share the Grove bus when their addresses do not conflict. Do not connect or disconnect modules while the device is powered.

## Firmware

Flash the `M5Stack_AtomS3R.zip` release build over USB. See the [installation guide]({{< relref "../getting-started/installation-guide.md" >}}) for the flashing process.
