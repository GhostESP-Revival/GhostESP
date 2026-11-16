---
title: "Karma Attack"
description: "Automatically respond to device probes with fake networks"
weight: 50
---

Create fake Wi-Fi networks based on SSIDs that nearby devices are searching for. When a device searches for a network, Karma broadcasts it back; when the device connects, a captive portal starts automatically to capture credentials. In **automatic** mode Karma learns SSIDs from probe requests, while **custom** mode broadcasts SSIDs you specify. You need a flashed, powered device with a wireless antenna; an SD card is optional for Evil Portal integration.

> **Note:** Only test on networks you own or have permission to test. See [Legal and ethical rules]({{< relref "basics.md#legal-and-ethical-rules" >}}).

{{< seq title="How Karma catches a client" caption="Karma answers the networks a device is already asking for." >}}
participant Client
participant Attacker
Client -> Attacker : Probe Request (broadcast) | The client names an SSID it is looking for
Attacker -> Client : Probe Response (spoofed SSID) | GhostESP answers as that network
Client -> Attacker : association + DHCP | The client joins the fake open network
Attacker -> Client : captive portal | Optional portal page served on connect
{{< /seq >}}

## Starting Karma

### On-device UI

1. Open **Menu → Wi-Fi → Attacks → Karma Attack**.
   The device will begin learning SSIDs from probe requests.
2. To use specific SSIDs instead, choose **Karma Attack (Custom SSIDs)**.
   Enter the SSIDs you want to broadcast (separated by commas), up to 32.
3. To serve a custom captive portal on the fake networks, choose **Karma Attack (Custom Portal)**.
4. The device will start broadcasting fake networks. Leave it running to catch devices.
5. To stop, go back to the menu or select **Stop Karma Attack**.

### CLI

1. Run `karma start` to begin automatic SSID learning.
   The device will cache SSIDs from probe requests.
2. Or run `karma start SSID1 SSID2 SSID3` to use specific SSIDs (up to 32).
   Example: `karma start FreeWiFi Starbucks McDonald's`
3. Run `karma stop` when you're done.

The learned-SSID cache holds at most 32 SSIDs.

## Troubleshooting

- **Fake networks not appearing**: Try restarting with `karma stop` then `karma start`.
- **No devices connecting**: Ensure devices are actually searching for networks. Try moving closer to the GhostESP device.
