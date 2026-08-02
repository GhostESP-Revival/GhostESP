---
title: "Scanning networks"
description: "Passively find and review nearby Wi-Fi access points."
weight: 10
---

## What this does

This passive scan lists nearby Wi-Fi access points without connecting to them or transmitting network traffic.

## Before you start

- GhostESP flashed device, powered on with a wireless antenna.
- Use the on-device UI or serial CLI. On a single-board setup, the WebUI cannot remain connected while the Wi-Fi radio scans.

## On-device steps

1. Open **Menu → WiFi → Scanning**.
2. Choose **Scan Access Points**.
3. Wait for the scan to finish.
4. Select **List Access Points**.
5. Review each network's name, channel, signal strength, and manufacturer.

Use **Scan APs Live** to see access points as they appear, or **Channel Congestion** to review activity by channel.

## CLI steps

1. Open the GhostESP terminal (serial connection or on-device terminal).
2. Run `scanap` to start a scan.
3. Run `list -a` to see the cached list of networks.

Run `scanap -live` to watch networks appear as they are discovered.

## Expected result

GhostESP displays nearby access points and their available details. No network connection is created.

## Troubleshooting

- **No networks found**: Move closer to wireless routers and try scanning again.
- **"You Need to Scan APs First" message**: Run a scan before trying to select a network.
- **Live scan stops right away**: Stop any active Wi-Fi attacks or portals from the menu and try again.

## Related tasks

- [Connect GhostESP to a Wi-Fi network]({{< relref "connect.md" >}})
- [Find devices on your Wi-Fi network]({{< relref "lan-discovery.md" >}})
- [Run a full environment sweep]({{< relref "environment-sweep.md" >}})
