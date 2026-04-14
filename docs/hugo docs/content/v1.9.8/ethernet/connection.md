---
title: "Connection Management"
description: "Connect and configure your Ethernet interface."
weight: 5
---

Manage your Ethernet connection with configuration and status commands.

## Connection Status

Check if Ethernet is connected and view link information:

```
ethup
```

Displays connection status, IP address, and link speed.

## Disconnecting

Bring down the Ethernet interface:

```
ethdown
```

## Configuration

### View Current Configuration

```
ethconfig show
```

Displays current IP settings (DHCP or static).

### DHCP Configuration

Enable DHCP to automatically obtain an IP address:

```
ethconfig dhcp
```

### Static IP Configuration

Set a static IP address:

```
ethconfig static <ip> <netmask> <gateway>
```

**Example**:
```
ethconfig static 192.168.1.100 255.255.255.0 192.168.1.1
```

## MAC Address Management

View or modify the MAC address:

```
ethmac
```

Shows current MAC address. Use with caution—changing MAC addresses may affect network access.

## System Time

### Get Current Time

```
time
```

Displays the current system time in Unix timestamp format.

### Set System Time

```
settime <unix_timestamp>
```

**Example**:
```
settime 1703001600
```

### Sync Time via NTP

Synchronize system time with an NTP server:

```
ethntp <time_server>
```

If no server is specified, uses `pool.ntp.org` by default:

```
ethntp
```

**Example with custom server**:
```
ethntp time.nist.gov
```
