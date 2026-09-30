---
title: "Check Open Ports"
description: "Check a device you own or are authorized to test for responding network ports"
weight: 13
---

Port scanning checks which network ports on an authorized device respond to connection attempts, so you can inventory services on your own network. [Connect GhostESP]({{< relref "connect.md" >}}) to the same network and identify the device with [LAN discovery]({{< relref "lan-discovery.md" >}}). Only scan devices you own or have permission to test; see [Legal and ethical rules]({{< relref "basics.md#legal-and-ethical-rules" >}}).

## Scanning ports

1. Open **Menu → Wi-Fi → Network** while GhostESP is connected and choose **Scan Open Ports**, then review the responding ports.

From the CLI, run `scanports <ip>` to check a specific device's TCP and common UDP ports. Add `all` to check all ports, or a range such as `20-1024` to limit the scan. Run `scanports local` to scan every host on the local subnet (takes no arguments; uses the async sweep). Run `scanssh <ip>` to check whether SSH responds on a device, or `scanssh` with no argument to scan the local subnet. Run `scanarp monitor [seconds]` to watch the network for new hosts while you scan.

{{< flow title="What a port scan does" caption="A missing result does not prove a service is absent; firewalls and network policy can block scans." >}}
Resolve the target, or reuse a prior scan | `scanports <ip>`, `local` for the subnet, or an ARP sweep first
Send TCP connection attempts | Common ports by default, `all`, or a `start-end` range
Send UDP probes to common services | `scanports <ip>` also checks common UDP ports
List the ports that respond | `scanssh <ip>` checks SSH specifically
{{< /flow >}}

GhostESP lists the ports that respond. A missing result does not prove that a service is absent; firewalls and network policy can block scans.

## Troubleshooting

- **No ports respond:** Confirm the IP address, network connection, and whether the device has a firewall.
- **The wrong device is scanned:** Run LAN discovery again and verify the IP address before retrying.
- **The scan is slow:** Limit the port range to the services you need to inspect.
