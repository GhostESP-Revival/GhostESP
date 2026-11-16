---
title: "Scanning networks"
description: "Discover and review nearby Wi-Fi access points"
weight: 10
---

An AP scan lists nearby Wi-Fi access points without connecting to them. GhostESP uses an **active** scan (it transmits probe requests) on normal channels and falls back to passive scanning on DFS/restricted channels. Use the on-device UI or the serial CLI; on a single-board setup the WebUI cannot stay connected while the Wi-Fi radio scans.

{{< flow title="How an active scan works" caption="Channels are visited in turn. The scan records what it hears and creates no connection." >}}
Tune the radio to the next channel | Channels hop automatically while the scan runs
Capture beacon and probe responses | Beacon (subtype 0x08) and Probe Response (0x05) frames
Record SSID, BSSID, channel, and RSSI | SSID can be hidden; RSSI comes from radio metadata
Repeat until every channel has been visited | The channel set depends on the country code
{{< /flow >}}

## Scanning

1. Open **Menu → Wi-Fi → Recon** and choose **Scan APs**, then wait for the scan to finish.
2. Select **List APs** and review each network's name, channel, signal strength, and manufacturer.

Use **Scan APs Live** to watch access points appear, or **Channel Congestion** to review activity by channel.

From the CLI, run `scanap` to start a scan, or `scanap <1-120>` to scan for a fixed number of seconds, then run `list -a` to see the cached list of networks. Run `scanap -live` to watch networks appear as they are discovered, or `scanap -stop` to stop an in-progress scan. Use `hop [auto|all|basic|custom <1,2,3>|1,2,3]` to control which channels scans and captures hop through.

A scan lists what is nearby but creates no network connection.

## Troubleshooting

- **No networks found**: Move closer to wireless routers and try scanning again.
- **"You Need to Scan APs First" message**: Run a scan before trying to select a network.
- **Live scan stops right away**: Stop any active Wi-Fi attacks or portals from the menu and try again.

## Related tasks

- [Connect GhostESP to a Wi-Fi network]({{< relref "connect.md" >}})
- [Find devices on your Wi-Fi network]({{< relref "lan-discovery.md" >}})
- [Run a full environment sweep]({{< relref "environment-sweep.md" >}})
