---
title: "Connect to a Wi-Fi Network"
description: "Connect GhostESP to a network you own or are authorized to use."
weight: 11
toc: true
---

## What this does

Connect GhostESP to a Wi-Fi network so it can use network-dependent features. Connecting changes the board's Wi-Fi mode and disconnects devices using the `GhostNet` access point.

## Before you start

- The network name and password.
- Permission to connect GhostESP to that network.
- A serial or on-device control method. The WebUI connection drops when the board changes Wi-Fi mode.

## On-device steps

1. Open **Menu → WiFi → Connection → Connect to WiFi**.
2. Enter the network name and password.
3. Wait for the connection status to confirm success.
4. To disconnect later, open **Menu → WiFi → Connection → Disconnect**.

## CLI steps

1. Run `connect "SSID" "password"`. Keep the quotes when either value contains spaces.
2. Wait for the connection status.
3. Run `connect` without arguments to reconnect to the saved network.
4. Run `disconnect` to leave the network.

## Expected result

GhostESP reports that it is connected. The `GhostNet` access point is unavailable while the board uses its Wi-Fi radio as a client.

## Troubleshooting

- **Connection fails:** Recheck the network name and password, then move closer to the access point.
- **The WebUI disconnects:** This is expected. Use serial or the on-device UI while GhostESP is connected to another network.
- **The board reconnects to the wrong network:** Run `disconnect`, then use `connect "SSID" "password"` with the intended network.

## Related tasks

- [Scan nearby Wi-Fi networks]({{< relref "survey.md" >}})
- [Find devices on your Wi-Fi network]({{< relref "lan-discovery.md" >}})
- [Connect to GhostESP]({{< relref "../getting-started/control-methods.md" >}})
