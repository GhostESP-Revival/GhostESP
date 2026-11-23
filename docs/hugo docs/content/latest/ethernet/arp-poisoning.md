---
title: "ARP Poisoning"
description: "Man-in-the-middle attack via ARP spoofing with DNS, SNI, HTTP, and FTP interception"
weight: 25
---

Perform ARP poisoning attacks to intercept network traffic between hosts and the gateway. The attack spoofs ARP in both directions and inspects the traffic it relays for DNS, SNI, HTTP, and FTP data.

## Commands

```
ethpoison start    # begin the attack
ethpoison stop     # stop the attack and restore ARP tables
ethpoison list     # list captured domains/URLs
ethpoison cookies  # list captured HTTP cookies
ethpoison creds    # list captured credentials
ethpoison status   # show running state and captured counts
```

Start the attack with `ethpoison start`. `ethpoison stop` halts all poisoning tasks, restores the ARP tables to correct values to prevent network disruption, and displays captured counts.

`ethpoison status` reports the running state, the number of poisoned hosts, and the number of captured domains (DNS + SNI + HTTP URLs), cookies, and credentials.

## How It Works

1. **Host Discovery**: ICMP echo requests wake up hosts on the /24 subnet, followed by ARP scanning
2. **ARP Spoofing**: Sends forged ARP replies claiming to be the gateway (to victims) and each victim (to the gateway)
3. **DNS Interception**: Receives DNS queries on port 53, logs them, forwards to the real DNS server
4. **SNI Extraction**: Parses TLS ClientHello packets to extract server names from HTTPS connections
5. **HTTP Inspection**: Extracts request URLs, Host headers, Cookies, and Authorization from HTTP traffic
6. **FTP Capture**: Monitors port 21 for USER and PASS commands
7. **Packet Forwarding**: Relays non-local traffic to maintain connectivity
8. **Passive Discovery**: Monitors network traffic to discover new hosts without rescanning

## Prerequisites

- Ethernet connection must be active (Ethernet itself is available only on the Banshee S3 build)
- IP forwarding must be enabled in firmware (`CONFIG_LWIP_IP_FORWARD=y`); the Banshee S3 build is the only shipped profile that enables it
- Target hosts must use the gateway as their DNS server (common default)

## Notes

- The attack automatically uses the network's DNS server from DHCP
- New hosts discovered passively are automatically added to the poison list
- SNI extraction works even with HTTPS (encrypted content, but server name is visible)
- Cookies and Authorization are captured from unencrypted HTTP traffic only
- FTP credentials are captured in plaintext on port 21
