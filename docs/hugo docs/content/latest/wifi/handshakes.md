---
title: "Capturing handshakes"
description: "Record Wi-Fi authentication data for analysis"
weight: 30
---

Capture Wi-Fi authentication handshakes from nearby networks for analysis. You need GhostESP with an SD card mounted to save captures, and a device that will connect to the target network so it can authenticate and create a handshake.

> **Legal note**: Only capture traffic from networks you own or have permission to test. See [Legal and ethical rules]({{< relref "basics.md#legal-and-ethical-rules" >}}).

{{< seq title="The four EAPOL frames" caption="GhostESP forces a reconnect, then records all four frames so the passphrase can be tested offline (or the PMKID from message 1)." >}}
participant Client
participant AP
Client -> AP : association after a forced reconnect | A short deauth makes the client rejoin
AP -> Client : EAPOL 1 - ANonce | Key Info: pairwise, ACK requested
Client -> AP : EAPOL 2 - SNonce + MIC | Client nonce and MIC
AP -> Client : EAPOL 3 - GTK + MIC | Install flag set; encrypted GTK delivered
Client -> AP : EAPOL 4 - ACK | Handshake complete
{{< /seq >}}

## Capturing a handshake

### On-device UI
1. Open **Menu → Wi-Fi → Scanning** and find your target network.
2. Select it with **Select AP** to lock onto that channel.
3. Open **Menu → Wi-Fi → Capture → Capture Eapol**.
   The device will start listening for authentication activity.
4. Wait for a device to connect or reconnect to the network.
   You should see `Handshake found!` when the capture succeeds.
5. Back out to stop capturing.
6. The capture is saved to the SD card under `/mnt/ghostesp/pcaps/`.
7. To convert it for offline cracking, choose **Menu → Wi-Fi → Capture → Export Handshakes (hc22000)**.

### CLI
1. Run `list -a` to see nearby networks.
2. Run `select -a <number>` to lock onto your target network.
3. Run `capture -eapol` to start listening.
4. Wait for a device to authenticate to the network.
   You should see `Handshake found!` when successful.
5. Run `capture -stop` to finish capturing.
   The file location will be shown in the log.
6. Run `capture -export <pcap-file>` to write an hc22000 file for offline cracking.

## Copying the capture

- Copy the `.pcap` file from the device to your computer for further analysis.
- For Flipper Zero saved files, copy the file from `/ext/apps_data/ghost_esp/pcaps/` on the Flipper's SD card.

## Troubleshooting
- **No handshake found**: Make sure a device is actually connecting to the network. Try toggling Wi-Fi off and on on a connected device to trigger a new authentication.
- **Capture file missing**: Verify the SD card is mounted and has free space. Check that you stopped the capture.
