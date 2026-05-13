---
title: "GTK Abuse Test"
description: "Test if Wi-Fi client isolation can be bypassed using the Group Temporal Key."
weight: 45
---

The GTK Abuse Test checks whether a Wi-Fi network's client isolation can be bypassed by exploiting the shared Group Temporal Key (GTK). This is based on the AirSnitch research (Vanhoef, NDSS 2026).

> **Note**: Only use this on networks you own or have explicit permission to test.

## How it works

On WPA2 networks, all connected clients share a Group Temporal Key (GTK) used to encrypt broadcast and multicast traffic. Client isolation is meant to prevent clients from communicating with each other, but many implementations only check at the MAC layer.

The test exploits this gap by:

1. **Connecting** to the target network as a normal client
2. **Reading** the installed GTK from the ESP-IDF supplicant's internal WPA state
3. **Optionally validating** that GTK against the installed STA group key when the driver exposes it
4. **Crafting** a broadcast Wi-Fi frame (encrypted with the GTK) that contains a unicast IP packet inside
5. **Injecting** the frame onto the network and waiting briefly for an ICMP echo reply

If client isolation is properly implemented, the frame should be blocked. If the target responds, isolation is broken.

```
WiFi dst: FF:FF:FF:FF:FF:FF (broadcast) → encrypted with shared GTK
  → IP dst: target's IP (unicast, only target processes it)
```

Client isolation sees "broadcast" and may let it through, but only the target IP actually processes the payload.

## Requirements

- The target network must use **WPA2-PSK** (not WPA3-SAE or open)
- You must know the **network password**
- The network must have at least one other connected client (the gateway/router counts)

## Usage

### Display (Touchscreen)

1. Navigate to **Wi-Fi → Attacks → Start GTK Abuse**
2. Enter the **network SSID** when prompted
3. Enter the **network password** when prompted
4. The test will connect, extract the GTK, inject a test frame, and check for a reply
5. Results are shown in a detail view when complete

### CLI

```
attack -g "SSID" "PASSWORD"
```

For networks with spaces in the name:

```
attack -g "My Home Network" "mypassword"
```

To stop the test:

```
stop
```

## Results

The result screen shows:

| Field | Meaning |
|-------|---------|
| **SSID** | The network that was tested |
| **Target** | The gateway IP used for the current probe |
| **Valid** | `YES`, `NO`, or `N/A` for the optional driver-side GTK cross-check |
| **Verdict** | Final outcome of the test |
| **Status** | Short explanation of the outcome |

### Interpreting results

- **Verdict: Broken**: An ICMP reply was observed after GTK-encrypted injection. This is strong evidence that client isolation is bypassed.
- **Verdict: Unconfirmed**: The frame was injected but no reply was observed. This does **not** prove the network is safe; it only means GhostESP did not observe a confirming response.
- **Verdict: Failed**: Connection or GTK extraction failed before a valid test could be sent.
- **Valid: N/A**: The driver-side GTK validation API was not available on this build, so only the internal supplicant read was used.

## Technical details

- **RAM usage**: low runtime overhead; no handshake capture buffer is used in the current implementation
- **Crypto**: CCMP encryption for the injected test frame
- **Packet injection**: Uses `esp_wifi_80211_tx()` on the STA interface
- **GTK source**: ESP-IDF internal supplicant WPA state (`gWpaSm`)
- **Validation**: Optional driver-side installed-key cross-check when available
