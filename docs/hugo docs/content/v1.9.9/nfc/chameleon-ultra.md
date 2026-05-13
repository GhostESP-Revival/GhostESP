---
title: "Chameleon Ultra"
description: "Connect GhostESP to a Chameleon Ultra reader"
weight: 5
---

## Overview

GhostESP can control a Chameleon Ultra over BLE. Once paired, use the same NFC scan/save flows as with a PN532.

## Prerequisites

- **Firmware:** GhostESP build with BLE and Chameleon Ultra features enabled.
- **Hardware:** A powered Chameleon Ultra advertising over BLE.

## Supported Devices

Chameleon Ultra support is available on the following GhostESP builds:

- esp32-generic.zip
- esp32s3-generic.zip
- esp32c3-generic.zip
- esp32c5-generic-v01.zip
- esp32c6-generic.zip
- MarauderV4_FlipperHub.zip
- MarauderV6_AwokDual.zip
- ghostboard.zip
- ESP32-S3-Cardputer.zip
- CYD2USB.zip
- CYDMicroUSB.zip
- CYDDualUSB.zip
- CYD2USB2.4Inch.zip
- CYD2USB2.4Inch_C.zip
- CYD2432S028R.zip
- LilyGo-T-Deck.zip
- LilyGo-TEmbedC1101.zip
- LilyGo-S3TWatch-2020.zip
- LilyGo-TDisplayS3-Touch.zip
- JCMK_DevBoardPro.zip
- RabbitLabs_Minion.zip
- Lolin_S3_Pro.zip
- CardputerADV.zip

## Connecting to Chameleon Ultra

> **Note:** GhostESP suspends the access point and Wi-Fi services during BLE sessions (including Chameleon connections) to free memory. The radio comes back automatically after you disconnect or run `stop`.

### On-Device UI

1. Open **Main Menu → NFC → Chameleon Ultra → Connect**.
   The device scans for nearby Chameleon Ultra devices.
2. Wait for the connection popup to show "Connected".
   Battery voltage and percentage appear below the status.
3. Use **Disconnect** from the same menu when finished.
   Wi-Fi services resume automatically after disconnecting.

### Command Line

1. Run `chameleon connect` to start pairing.
   Optional arguments:
   - `timeout` in seconds (default 10) if you need more time for discovery.
   - `pin` if your Chameleon Ultra requires a 4–6 digit security code.
   Example: `chameleon connect 15 1234`
2. Wait for pairing confirmation in the terminal.
   The CLI announces when the link succeeds.
3. Check status with `chameleon status` or battery with `chameleon battery`.
   Voltage and percentage display before starting long dumps.
4. Run `chameleon disconnect` when finished.
   BLE releases and the Wi-Fi services will restore automatically.

### Saving Dumps

- **Save HF scans:** After `chameleon scanhf` completes, run `chameleon savehf <name>` to write the dump to `/mnt/ghostesp/chameleon/`.
- **Name files clearly:** Use short descriptive names without spaces, for example `office_door`.


## Troubleshooting

- **Connection timeouts:** Keep the Chameleon Ultra awake (press any button) and re-run `chameleon connect 20` for a longer window.
- **PIN failures:** Verify the configured code on the Chameleon Ultra; three failed attempts may force a power cycle.
- **No BLE advertisements:** Check the Chameleon Ultra’s BLE settings or reboot it; GhostESP only connects to active broadcasts.

## Next Steps

- **Scanning:** Continue with [PN532-based scanning guide]({{< relref "scanning.md" >}}).
- **Saving:** Store remote dumps using [Saving Tags]({{< relref "saving.md" >}}).
- **Compatibility:** Review what tag families work via [Supported Tags]({{< relref "supported.md" >}}).
