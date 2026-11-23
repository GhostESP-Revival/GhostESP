---
title: "Find Devices on Your Wi-Fi Network"
description: "Discover devices and services on a network you own or are authorized to test"
weight: 12
---

LAN discovery lists devices and advertised services on the Wi-Fi network GhostESP is currently connected to. [Connect to a Wi-Fi Network]({{< relref "connect.md" >}}) first. Only scan networks you own or have permission to test; see [Legal and ethical rules]({{< relref "basics.md#legal-and-ethical-rules" >}}).

{{< flow title="Discovery sequence" caption="LAN scans reuse the most recent ARP sweep as the host list." >}}
Reuse the last ARP sweep, or run a new one | Catches Windows hosts that block ICMP ping
Discover services per host | mDNS, LLMNR, SSDP, and NetBIOS
Probe specific protocols | NetBIOS, HTTP banners, SNMP, and SMB
List hosts and services | Hostnames, service types, ports, and IP addresses
{{< /flow >}}

## Discovering devices

On-device, open **Menu → Wi-Fi → Network** while connected and choose an item: **mDNS Discovery**, **ARP Sweep**, **List Hosts (ARP)**, **Scan Open Ports**, **SSH Banner Scan**, **NetBIOS Scan**, **HTTP Banner Scan**, **SNMP Probe**, **SMB Enum (enum4linux)**, or **SNMP Walk**. The **… Subnet...** variants target a specific subnet. Review the discovered devices and services.

From the CLI, run `scanlocal` to discover devices and services, or `scanarp` to list active devices by IP address and MAC vendor (`scanarp monitor [seconds]` watches for new hosts over time). Run `netbiosscan [subnet <a.b.c.>]` and `httpbannerscan [subnet <a.b.c.>]` to enumerate NetBIOS and HTTP hosts. Run `snmpprobe <IP>` (or `snmpprobe walk <IP> [OID]`, `snmpprobe communities <c1,c2,...|file>`) to probe and walk SNMP devices, and `enumscan` for SMB hosts. Run `mdnssniff <IP|all>` to watch mDNS traffic for a host or the whole subnet (stop with `mdnssniff stop`). Run `scanports local` to port-scan the local subnet, or see [Check Open Ports]({{< relref "port-scanning.md" >}}) for single-host scans.

LAN scans automatically reuse the most recent ARP sweep results as the host list (with MAC vendor labels) instead of ICMP-pinging every address, which catches Windows hosts that block ping. When no ARP results are available, scans use the actual subnet mask up to /20 rather than assuming /24.

GhostESP lists available hostnames, service types, ports, or IP addresses; results depend on which devices respond and the network's isolation settings.

## Govee lights and Wake-on-LAN

- For a supported Govee light, enable **LAN Control** in the Govee Home app, then open **Wi-Fi → Gadgets → Govee Lights → Scan Govee Devices**. Select a result to turn it on or off, set brightness, or set an RGB color.
- To wake a compatible PC, open **Wi-Fi → Gadgets → Wake on LAN**, enter its MAC address or its live LAN IP, and submit. The PC must have Wake-on-LAN enabled and remain connected to power and the network.

## Troubleshooting

- **No devices appear:** Confirm GhostESP is connected to the intended network and that client isolation is not enabled.
- **Only some devices appear:** Retry after devices are active; not every device advertises services or responds to discovery requests.
- **The connection drops:** Reconnect GhostESP, then repeat the discovery scan.

## Related tasks

- [Scan nearby Wi-Fi networks]({{< relref "survey.md" >}})
