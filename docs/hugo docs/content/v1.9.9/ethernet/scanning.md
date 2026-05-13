---
title: "Network Scanning"
description: "Discover and scan devices on your Ethernet network."
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

Scan specific ports on a target:

```
ethports <ip> <port>
```

**Examples**:
```
ethports 192.168.1.100 80
ethports 192.168.1.100 22,80,443
```

### Scan Gateway

Scan the gateway (DHCP server):

```
ethports local 80
```

### Scan All Ports

Scan all common ports on a target:

```
ethports 192.168.1.100 all
```

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

### DNS Lookup

Resolve a domain name to an IP address:

```
ethdns <domain>
```

**Example**:
```
ethdns google.com
```

## Statistics

### Network Statistics

Display Ethernet interface statistics:

```
ethstats
```

Shows packet counts, errors, and other interface metrics.
