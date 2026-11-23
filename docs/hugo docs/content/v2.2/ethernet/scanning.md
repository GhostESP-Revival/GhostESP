---
title: "Network Scanning"
description: "Discover and scan devices on your Ethernet network"
weight: 15
---

Use Ethernet scanning tools to discover devices, services, and open ports on your network.

## Device Discovery

### Network Fingerprinting

Identify devices using service announcements (mDNS, SSDP, NBNS):

```
ethfp
```

See [Network Fingerprinting]({{< relref "fingerprinting.md" >}}) for detailed information.

### ARP Scanning

Discover active hosts on the network using ARP:

```
etharp
```

Returns a list of IP addresses and MAC addresses of devices responding to ARP requests.

### Ping Scanning

Scan for active hosts using ICMP ping:

```
ethping
```

Sends ping requests to discover which hosts are online.

## Service Discovery

### Banner Grabbing and Service Detection

Probe a host for running services and grab banners:

```
ethserv <ip>
```

**Example**:
```
ethserv 192.168.1.1
```

Attempts to connect to common ports and retrieve service information (HTTP, SSH, FTP, etc.).

## Port Scanning

### TCP Port Scanning

Scan ports on a target using range format:

```
ethports <ip> <start-end>
```

**Examples**:
```
ethports 192.168.1.100 80-80
ethports 192.168.1.100 22-443
```

> **Note:** Port scanning uses range format (`start-end`), not comma-separated lists. To scan a single port, use the same number for start and end (e.g., `80-80`). To scan common ports on the gateway, omit the port argument.

### Scan Gateway

Scan the gateway (DHCP server) for common ports:

```
ethports local
```

To sweep every port on the gateway (1-65535), add `all`:

```
ethports local all
```

### Scan All Ports

Scan the full 1-65535 range on a target:

```
ethports 192.168.1.100 all
```

> **Note:** `all` scans every port from 1 to 65535, so it takes much longer than the common-port scan. To scan only the common ports, omit the port argument entirely (e.g. `ethports 192.168.1.100`).

## Traceroute

Trace the network path to a host:

```
ethtrace <ip>
```

**Example**:
```
ethtrace 8.8.8.8
```

Shows each hop (router) between your device and the target.

## DNS Resolution

Resolve a domain name to an IP address:

```
ethdns <domain>
```

**Example**:
```
ethdns google.com
```

Reverse (PTR) lookups are **not implemented**. The command only prints a notice that PTR records may not be fully supported:

```
ethdns reverse <ip_address>
```

> **Note:** To resolve a hostname, use a forward lookup instead: `ethdns <domain>`.

## Statistics

Display Ethernet interface statistics:

```
ethstats
```

Shows packet counts, errors, and other interface metrics.
