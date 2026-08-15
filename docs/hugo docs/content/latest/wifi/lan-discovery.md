---
title: "Find Devices on Your Wi-Fi Network"
description: "Discover devices and services on a network you own or are authorized to test."
weight: 12
toc: true
---

## What this does

LAN discovery lists devices and advertised services on the Wi-Fi network GhostESP is currently connected to.

## Before you start

- Connect GhostESP to the network with [Connect to a Wi-Fi Network]({{< relref "connect.md" >}}).
- Only scan networks you own or have explicit permission to test.

## On-device steps

1. Open **Menu → WiFi → Scanning** while GhostESP is connected.
2. Choose **Scan LAN Devices**.
3. Review the discovered devices and services.

## CLI steps

1. Run `scanlocal` to discover devices and services.
2. Run `scanarp` to list active devices by IP address and MAC vendor.
3. Run `snmpprobe` / `enumscan` to identify SNMP devices and SMB hosts.

LAN scans automatically reuse the most recent ARP sweep results as the host
list (with MAC vendor labels) instead of ICMP-pinging every address, which
catches Windows hosts that block ping. When no ARP results are available,
scans use the actual subnet mask up to /20 rather than assuming /24.

## Expected result

GhostESP lists available hostnames, service types, ports, or IP addresses. Results depend on the devices that respond and the network's isolation settings.

## Govee lights and Wake-on-LAN

- For a supported Govee light, enable **LAN Control** in the Govee Home app, then open **WiFi → Gadgets → Govee Lights → Scan Govee Devices**. Select a result to turn it on or off, set brightness, or set an RGB color.
- To wake a compatible PC, open **WiFi → Gadgets → Wake on LAN**, enter its MAC address or its live LAN IP, and submit. The PC must have Wake-on-LAN enabled and remain connected to power and the network.

## Troubleshooting

- **No devices appear:** Confirm GhostESP is connected to the intended network and that client isolation is not enabled.
- **Only some devices appear:** Retry after devices are active; not every device advertises services or responds to discovery requests.
- **The connection drops:** Reconnect GhostESP, then repeat the discovery scan.

## Related tasks

- [Connect to a Wi-Fi network]({{< relref "connect.md" >}})
- [Check open ports on an authorized device]({{< relref "port-scanning.md" >}})
- [Scan nearby Wi-Fi networks]({{< relref "survey.md" >}})
