---
title: "Check Open Ports"
description: "Check a device you own or are authorized to test for responding network ports."
weight: 13
toc: true
---

## What this does

Port scanning checks which network ports on an authorized device respond to connection attempts. Use it to inventory services on your own network.

## Before you start

- Connect GhostESP to the same network as the device.
- Identify the device with [LAN discovery]({{< relref "lan-discovery.md" >}}).
- Only scan devices you own or have explicit permission to test.

## On-device steps

1. Select a discovered device with **Select LAN**.
2. Choose **Scan Open Ports**.
3. Review the responding ports.

## CLI steps

1. Run `scanports <ip>` to check a specific device.
2. Add `all` to check all ports, or a range such as `20-1024` to limit the scan.
3. Run `scanssh <ip>` to check whether SSH responds on the device.

## Expected result

GhostESP lists ports that respond. A missing result does not prove that a service is absent; firewalls and network policy can block scans.

## Troubleshooting

- **No ports respond:** Confirm the IP address, network connection, and whether the device has a firewall.
- **The wrong device is scanned:** Run LAN discovery again and verify the IP address before retrying.
- **The scan is slow:** Limit the port range to the services you need to inspect.

## Related tasks

- [Find devices on your Wi-Fi network]({{< relref "lan-discovery.md" >}})
- [Connect to a Wi-Fi network]({{< relref "connect.md" >}})
