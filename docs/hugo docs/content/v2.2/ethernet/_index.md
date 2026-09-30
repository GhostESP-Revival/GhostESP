---
title: "Ethernet"
description: "Overview of GhostESP's Ethernet capabilities"
weight: 510
aliases:
  - "/ethernet/"
---

GhostESP provides Ethernet connectivity with network scanning, device fingerprinting, and service discovery tools.

> **Note:** Ethernet is available **only on the Banshee S3 build** (`CONFIG_WITH_ETHERNET`). It is not present on other boards. If `ethup` reports an initialization failure, your build does not include Ethernet.

## Quick Links

- [Connection Management]({{< relref "connection.md" >}}) - bring the interface up, configure IP and MAC
- [Scanning]({{< relref "scanning.md" >}}) - ping sweeps, port scans, and traceroute
- [Fingerprinting]({{< relref "fingerprinting.md" >}}) - identify devices on the network
- [HTTP]({{< relref "http.md" >}}) - web requests and service checks
- [ARP Poisoning]({{< relref "arp-poisoning.md" >}}) - man-in-the-middle testing
