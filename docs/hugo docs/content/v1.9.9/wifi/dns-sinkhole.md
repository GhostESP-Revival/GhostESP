---
title: "DNS Sinkhole"
description: "Block ads and unwanted domains by running a DNS sinkhole on your GhostESP device."
weight: 55
---

Run a portable DNS sinkhole that blocks ad domains and other unwanted hosts. Any device on the same network that uses the GhostESP as its DNS server will receive `NXDOMAIN` for blocked domains.

> **Note**: This feature requires a device connected to a network via WiFi or Ethernet. It is mutually exclusive with Evil Portal — starting one will stop the other.

## Prerequisites

- GhostESP device connected to a network via WiFi (`connect`) or Ethernet.
- SD card inserted. PSRAM boards can start without SD in proxy-only mode; no-PSRAM boards need SD to start.
- For blocking, a blocklist file at `/mnt/ghostesp/dns_sinkhole/blocklist.txt` on the SD card.
  - The file must be a plain text file with one domain per line, sorted alphabetically.
  - A default blocklist is included with the firmware. You can add your own domains.

PSRAM-enabled boards are recommended for large blocklists and on-device downloads. No-PSRAM boards are supported, but they rely on SD-card lookups and work best with smaller, sorted lists.

PSRAM-enabled configs include:

- `TEmbedC1101`
- `crowtech7inch`
- `JC3248W535EN`
- `sunton7inch`
- `NM-CYD-C5`
- `xiao_esp32c5`
- `waveshare7inch`
- `awokmini`
- `xiao_esp32s3_sense`
- `xiao_esp32s3`
- `lolins3pro`
- `tdeck`
- `poltergeist`
- The Banshee (`somethingsomething`)

## How it works

The DNS sinkhole listens on UDP port 53 and checks each query:

1. Match the domain, parent domains, and built-in bypass domains.
2. Return `NXDOMAIN` when blocked.
3. Forward allowed queries upstream and inspect CNAME answers before relaying them.

Apple Private Relay, Firefox DoH canary, and DDR resolver-discovery domains are blocked automatically because they can bypass manual DNS settings.

### Lookup paths by hardware

**PSRAM boards**

- Load the SD blocklist into a Bloom filter and hash table in PSRAM.
- After startup, lookups are handled from PSRAM.
- If no SD blocklist exists, the sinkhole starts in proxy-only mode and does not block domains.

**No-PSRAM boards**

- Keep the sorted blocklist on SD.
- Build a fixed 8 KB Bloom filter in internal RAM.
- Most non-blocked domains are rejected from RAM; possible matches are verified with SD binary search.

Both paths use a small heap-backed cache for repeated queries.

> **Note**: Blocklist downloading is only available on PSRAM-enabled devices. On devices without PSRAM, copy a pre-sorted blocklist file to the SD card manually.

## Starting the sinkhole

### On-Device UI

1. Connect to a network (WiFi via `connect`, or Ethernet) first.
2. Open **WiFi → DNS Sinkhole → Start**.
3. The display will show "Sinkhole On" and the ESP's IP address.

The on-device flow opens a terminal view while the sinkhole is active. Backing out of that terminal stops the sinkhole and restores the normal AP services.

### Command line

1. Connect to a network: `connect` (WiFi) or connect Ethernet.
2. Start the sinkhole: `sinkhole start`
3. The ESP will log: `DNS sinkhole started on <IP>:53`

For diagnostics, start with query logging enabled:

```
sinkhole start log
```

## Configuring client devices

The sinkhole operates in **listen-only mode** — it does not perform DHCP spoofing or ARP poisoning. You must manually configure each client device to use the GhostESP's IP address as its DNS server.

### Android
1. Open **Settings → Network & Internet → WiFi**.
2. Long-press your connected network → **Modify network**.
3. Set **IP settings** to **Static**.
4. Set **DNS 1** to the GhostESP IP address (e.g., `192.168.6.135`).

### iOS
1. Open **Settings → WiFi**.
2. Tap the **i** button next to your network.
3. Scroll to **Configure DNS** → **Manual**.
4. Remove existing DNS servers and add the GhostESP IP address.

### Windows
1. Open **Settings → Network & Internet → Change adapter options**.
2. Right-click your adapter → **Properties** → **IPv4** → **Properties**.
3. Set **Preferred DNS server** to the GhostESP IP address.

### macOS
1. Open **System Preferences → Network → Advanced → DNS**.
2. Add the GhostESP IP address as a DNS server.

### Linux
```bash
# Temporary test
dig @<ESP_IP> google.com

# Set as system DNS (example using systemd-resolved)
resolvectl dns <interface> <ESP_IP>
```

## Stopping the sinkhole

### On-device UI
Open **WiFi → DNS Sinkhole → Stop**.

### Command line
```
sinkhole stop
```

The internal access point (GhostNet) is automatically stopped while the sinkhole is running and restarted when it stops.

## Commands

- `sinkhole start [upstream_dns] [log]` starts the sinkhole. Add `log` to enable query logging immediately.
- `sinkhole stop` stops the sinkhole and restores AP services.
- `sinkhole status` shows current query and block counts.
- `sinkhole download [n]` lists or downloads built-in blocklists. PSRAM only.
- `sinkhole load <filename>` copies a downloaded list to `blocklist.txt` and restarts.
- `sinkhole add <domain>` inserts a domain into the sorted active blocklist.
- `sinkhole remove <domain>` is a placeholder; stop and edit/reload the list manually for now.
- `sinkhole reload` restarts the sinkhole to apply blocklist changes.
- `sinkhole log <on|off>` toggles query logging.

## Query logging

To log all DNS queries (blocked and forwarded) to the SD card:

```
sinkhole log on
```

You can also enable logging at startup with `sinkhole start log`. Query logging writes to the SD card on every DNS query, so leave it off for normal long-running use on SPI SD-card devices.

Logs are written to `/mnt/ghostesp/dns_sinkhole/queries.log` in CSV format:

```
<timestamp_ms>,<client_ip>,<domain>,<qtype>,BLOCKED|FORWARDED|BLOCKED_CNAME,<matched_domain>
```

For forwarded queries, `<matched_domain>` is `-`. For blocked wildcard matches, it shows the parent blocklist entry that caused the block. `BLOCKED_CNAME` means the original query was forwarded, but an upstream CNAME target matched the blocklist. Built-in privacy-relay blocks are logged as `builtin:<domain>`.

## Custom blocklists

The blocklist file is located at `/mnt/ghostesp/dns_sinkhole/blocklist.txt` on the SD card. Requirements:

- One domain per line.
- Must be sorted alphabetically (required for binary search on non-PSRAM devices).
- Parent-domain matches are blocked too, so `example.com` also blocks `www.example.com` and deeper subdomains.
- Lines starting with `#` are treated as comments.
- Case-insensitive matching.

To sort a blocklist on your PC:
```bash
sort -f -u blocklist.txt > blocklist_sorted.txt
```

### Downloading Blocklists

Blocklist downloading requires WiFi, SD card storage, and a PSRAM-enabled device. The download command stores named source files in `/mnt/ghostesp/dns_sinkhole/`; it does not directly replace the active `blocklist.txt`.

List available sources:

```
sinkhole download
```

Current built-in sources:

- `1` Peter Lowe, saved as `peter_lowe.txt`
- `2` OISD Basic, saved as `oisd.txt`
- `3` StevenBlack, saved as `stevenblack.txt`

Download a source, then load it as the active blocklist:

```
sinkhole download 1
sinkhole load peter_lowe.txt
```

`sinkhole load` copies the selected file to `/mnt/ghostesp/dns_sinkhole/blocklist.txt` and restarts the sinkhole.

### Adding domains

```
sinkhole add ad.example.com
```

This inserts the domain into the sorted blocklist file. Restart the sinkhole for it to take effect.

## Troubleshooting

- **"No connected network interface"**: Connect via WiFi (`connect`) or Ethernet first.
- **"SD card required for DNS sinkhole (no PSRAM)"**: Insert and mount an SD card before starting.
- **Blocklist download requires a PSRAM-enabled device and network connectivity**: Blocklist downloading is only supported on PSRAM-enabled devices. On devices without PSRAM, copy a pre-sorted blocklist file to `/mnt/ghostesp/dns_sinkhole/blocklist.txt` on the SD card manually.
- **Queries not being blocked**: Verify the blocklist file exists at `/mnt/ghostesp/dns_sinkhole/blocklist.txt` and is sorted alphabetically. Check `sinkhole status` to see if blocked count is increasing.
- **Pages not loading (forwarding broken)**: Ensure the GhostESP can reach the internet. The upstream DNS is auto-detected from your WiFi network. If it falls back to `8.8.8.8`, verify that UDP port 53 is not blocked on your network.
- **"DNS sinkhole: no blocklist, running in proxy mode"**: No blocklist file was found. The sinkhole will forward all queries without blocking. Add a blocklist and restart.
- **Sinkhole stops when network disconnects**: If the primary interface (WiFi STA) disconnects, the sinkhole will stop. If connected via Ethernet only, it will continue running. Reconnect the network and restart if needed.
- **Cannot start with Evil Portal running**: The sinkhole and Evil Portal are mutually exclusive. Stop one before starting the other.
