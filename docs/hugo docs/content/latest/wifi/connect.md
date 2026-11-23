---
title: "Connect to a Wi-Fi Network"
description: "Connect GhostESP to a network you own or are authorized to use"
weight: 11
---

Connect GhostESP to a Wi-Fi network so it can use network-dependent features. You need the network name and password and permission to connect. Connecting changes the board's Wi-Fi mode and disconnects devices using the `GhostNet` access point - and drops the WebUI - so keep serial or on-device control available.

## Connecting

On-device, open **Menu → Wi-Fi → Connection → Connect to Wi-Fi** (or **Connect to saved Wi-Fi** to rejoin a previously saved network), enter the network name and password, and wait for the connection status to confirm success.

From the CLI, run `connect "SSID" "password"` and keep the quotes when either value contains spaces. Wait for the connection status. Run `connect` without arguments to reconnect to the saved network. Run `disconnect` to leave the network, or `autoreconnect <on|off>` to control whether the board automatically rejoins a dropped network.

GhostESP reports that it is connected. The `GhostNet` access point is unavailable while the board uses its Wi-Fi radio as a client.

## Troubleshooting

- **Connection fails:** Recheck the network name and password, then move closer to the access point.
- **The WebUI disconnects:** This is expected. Use serial or the on-device UI while GhostESP is connected to another network.
- **The board reconnects to the wrong network:** Run `disconnect`, then use `connect "SSID" "password"` with the intended network.

## Related tasks

- [Scan nearby Wi-Fi networks]({{< relref "survey.md" >}})
- [Find devices on your Wi-Fi network]({{< relref "lan-discovery.md" >}})
- [Connect to GhostESP]({{< relref "../getting-started/control-methods.md" >}})
