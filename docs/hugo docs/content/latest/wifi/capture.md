---
title: "Capturing packets"
description: "Record Wi-Fi traffic to a PCAP for later analysis"
weight: 20
---

Record Wi-Fi network traffic and save it to the SD card, then review the captured data on your computer or Flipper Zero. You need GhostESP with an SD card mounted; a Flipper Zero with a microSD card is optional if you want to save captures to the Flipper.

{{< flow title="What a capture does" caption="Files land in `/mnt/ghostesp/pcaps/`; with no SD card the stream goes out over UART." >}}
Choose a capture mode | `-probe`, `-deauth`, `-beacon`, `-raw`, `-eapol`, `-pwn`, `-wps`, `-skimmer`, or `-802154`
Enable monitor mode | Optional channel lock with `-c <n>`, otherwise the radio hops channels
Write frames to PCAP | `/mnt/ghostesp/pcaps/<mode>_<n>.pcap`
Stop and review | `capture -stop`; convert handshakes with `capture -export`
{{< /flow >}}

## Capturing traffic

### On-device UI
1. Open **Menu → Wi-Fi → Capture**.
   You should see a list of capture types to choose from.
2. Select the type of traffic you want to record (see capture types below).
   The device will start recording. Leave it running until you have enough data.
3. Back out of the terminal view to stop recording.
   The device will save the file to the SD card and show the file path.
4. Retrieve the file from the SD card using the GhostNet WebUI file browser or by removing the card.
   Files are saved under `/mnt/ghostesp/pcaps/` with names like `probescan_3.pcap`.

### CLI
1. Open the GhostESP terminal.
2. Run `capture -<type>` where type is one of the capture types below (for example, `capture -probe`).
   The device will start recording.
3. Let it run while traffic is collected.
4. Run `capture -stop` when you're done.
   The device will save the file and show where it was saved.

### Capture modes
- **-probe**: Records probe requests so you can see devices searching for known SSIDs.
- **-deauth**: Records deauthentication frames to diagnose disconnect storms or targeted kicks.
- **-beacon**: Records beacons to review advertised SSIDs and channel metadata.
- **-raw**: Dumps every Wi-Fi frame seen on the tuned channel for full analysis.
- **-eapol**: Captures WPA/WPA2 4-way handshakes, PMKID messages, and rekeys so you can validate client authentications. For exporting captures for offline key cracking, see [Capturing handshakes]({{< relref "handshakes.md" >}}).
- **-pwn**: Records frames from `Pwnagotchi` devices.
- **-wps**: Captures Wi-Fi Protected Setup traffic to confirm whether a router exposes WPS enrolment.
- **-802154** (ESP32-C5/C6 only): Records IEEE 802.15.4 frames when you supply `capture -802154`, optionally with a channel such as `capture -802154 ch15` (11-26).
- **-ble**: Records BLE packets for offline analysis.
- **-skimmer**: Detects payment terminal Bluetooth skimmers and captures their BLE traffic.
- **-wireshark**: Streams Wi-Fi packets live to Wireshark over USB/UART instead of saving to SD.
- **-wiresharkble**: Streams BLE advertising packets live to Wireshark.
- **-list**: Lists the captures already saved on the SD card.
- **-export <pcap-file>**: Exports PMKID and M2/M3 handshakes from a saved capture; the hc22000 export workflow is covered in [Capturing handshakes]({{< relref "handshakes.md" >}}).

Most Wi-Fi capture modes also accept `-channel <n>` or `-c <n>` to lock to a single channel (1-177, validated against your country's allowed channels).

Confirm the SD card contains a `.pcap` file, and open it in the Wireshark app to make sure packets are listed.

## Notes
- Captures save to `/mnt/ghostesp/pcaps/` only when an SD card is mounted. If the folder is missing the device streams packets over UART to the Flipper Zero when writing a file.
- The firmware logs `PCAP: saving to SD as ...` when file storage is active, or `PCAP: streaming over UART` when it falls back to the terminal.
- Flipper Zero GhostESP app expects captures under `/ext/apps_data/ghost_esp/pcaps/` on its microSD. Copy the `.pcap` using QFlipper or directly from the Flipper's SD card to review in Wireshark.
- Large captures can take time to copy. Use a card reader instead of QFlipper or the WebUI for faster transfers.

## Troubleshooting
- **Capture file not saved**: Make sure the SD card is mounted and has free space. Always use `stop` or back out through the menu to properly save the file.
- **SD card not recognized**: Reboot the device with the SD card already inserted. Check that the card is properly seated and not corrupted.
- **File transfer is slow**: Use a card reader connected to your computer instead of the WebUI or other methods for faster transfers.

