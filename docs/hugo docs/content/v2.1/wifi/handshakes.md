---
title: "Capturing handshakes"
description: "Record Wi-Fi authentication data for analysis."
weight: 30
---

Capture Wi-Fi authentication handshakes from nearby networks.

> **Legal note**: Only capture traffic from networks you own or have explicit permission to test. Unauthorized network testing is illegal in most jurisdictions.

## Prerequisites
- GhostESP with SD card mounted (for saving captures).
- A device to connect to the target network (so it can authenticate and create a handshake).

## Capturing a handshake

### On-device UI
1. Open **Menu → WiFi → Scanning** and find your target network.
2. Select it with **Select AP** to lock onto that channel.
3. Open **Menu → WiFi → Capture → Capture Eapol**.
   The device will start listening for authentication activity.
4. Wait for a device to connect or reconnect to the network.
   You should see `Handshake found!` when the capture succeeds.
5. Back out to stop capturing.
6. The capture is saved to the SD card under `/mnt/ghostesp/pcaps/`.

### Command line
1. Run `list -a` to see nearby networks.
2. Run `select -a <number>` to lock onto your target network.
3. Run `capture -eapol` to start listening.
4. Wait for a device to authenticate to the network.
   You should see `Handshake found!` when successful.
5. Run `stop` to finish capturing.
   The file location will be shown in the log.

## Next steps
- Copy the `.pcap` file from the device to your computer for further analysis.
- For Flipper Zero saved files, copy the file from `/ext/apps_data/ghost_esp/pcaps/` on the Flipper's SD card.

## Troubleshooting
- **No handshake found**: Make sure a device is actually connecting to the network. Try toggling Wi-Fi off and on on a connected device to trigger a new authentication.
- **Capture file missing**: Verify the SD card is mounted and has free space. Check that you stopped the capture. 