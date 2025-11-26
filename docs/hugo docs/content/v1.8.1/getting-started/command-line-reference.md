---
title: "CLI Reference"
description: "Common GhostESP CLI commands grouped by category."
weight: 20
toc: true
---

## Connecting to the CLI interface

- Use a [serial console](https://ghostesp.net/serial) (115200 baud is recommended) with a USB data cable or the built-in Terminal app on touch-enabled boards.
- From the web UI, open the Terminal panel for remote access. When you launch a Wi-Fi or BLE command, the device suspends the GhostNet AP until the radio work finishes; once you run `stop` (or the command completes), BLE deinitializes and Wi-Fi returns automatically.
- Send `help` to confirm connectivity; output appears prefixed with `>` in the console.

## Core

- **`help [category|all]`** — List commands by category (`wifi`, `ble`, `portal`, `comm`, `sd`, `led`, `gps`, `misc`, `printer`, `cast`, `capture`, `beacon`, `attack`).
- **`chipinfo`** — Print SoC model, cores, features, and IDF version.
- (for developers) **`mem [dump|trace <start|stop|dump>]`** — Print heap stats, dump allocation state, or control heap tracing.
- **`reboot`** — Soft restart the device.
- **`timezone <TZ>`** — Set timezone, e.g., `timezone EST5EDT,M3.2.0,M11.1.0`.
- **`stop`** — Global kill switch: halt Wi-Fi attacks, BLE/BLE spam, GPS logging, wardriving, PCAP/CSV captures, RGB effects, and other background timers. It also tears down the BLE stack so suspended Wi-Fi/AP services come back online.

## WiFi

### Scanning

- **`scanap [seconds|-live|-stop]`** — Run an AP scan, optionally for a set duration, live channel hop, or stop (`-stop`).
- **`scansta`** — Hop channels and log associated stations.
- **`scanall [seconds]`** — Combined AP and STA scan with summary.
- **`list [-a|-s|-airtags]`** — Show AP scan results, associated stations, or AirTags.
- **`listenprobes [channel|stop]`** — Monitor probe requests and log to PCAP if SD is present.

### Targeting

- **`select [-a|-s|-airtag] <idx[,idx]>`** — Queue APs, a station, or an AirTag by index for later actions.
- **`connect <ssid> [pass]`** — Join an infrastructure network (saves credentials); wrap SSID/password in quotes when they contain spaces, e.g., `connect "My SSID" "My Password"`.
- **`disconnect`** — Leave the current STA connection.
- **`apcred <ssid> <pass>`** or **`apcred -r`** — Change or reset GhostNet AP credentials.
- **`apenable on|off`** — Toggle AP persistence across reboots.

### Offense

- **`attack -d|-e|-s <password>`** — Trigger deauth, EAPOL logoff, or SAE flood (`-s` needs ESP32-C5/C6 and the target PSK).
- **`stopdeauth`** / **`stopspam`** — Halt active attacks or beacon floods.
- **`beaconspam [mode]`** — Broadcast spoof SSIDs (`-r`, `-rr`, `-l`, or custom text).
- **`karma start [ssid...]`** / **`karma stop`** — Respond to client probes with saved or provided SSIDs.
- **`pineap [-s]`** — Monitor Pineapple-style beacons; `-s` stops detection.
- **`saeflood <password>`** / **`stopsaeflood`** / **`saefloodhelp`** — Launch, stop, or review SAE flood attack guidance.

### Network

- **`scanports <local|ip> [all|start-end]`**, **`scanarp`**, **`scanlocal`**, **`scanssh <ip>`** — Scan the subnet, a target host, or run mDNS/SSH discovery utilities.
- **`dhcpstarve <start [threads]|stop|display>`** — Flood a DHCP server or show collected leases.
- **`capture <-probe|-deauth|-beacon>`** — Start packet captures for the specified frame type to SD.

### Output

- **`powerprinter [ip text font alignment]`** — Send formatted PCL text jobs to LAN printers; pull saved defaults when arguments are omitted.
- **`dialconnect`** — Pair with a DIAL-capable device (e.g., Chromecast/YouTube).

## BLE
*(ESP32-S2 excluded)*

### Discovery

- **`blescan [-f|-ds|-a|-r|-s]`** — Scan for BLE devices, Flippers, spam detectors, or raw advertising; `-s` stops.
- **`blewardriving [-s]`** — Log BLE beacons with GPS metadata.

### Spoofing

- **`blespam [mode|-s]`** — Emit spoofed BLE advertisements (Apple, Microsoft, Samsung, Google, random).
- **`spoofairtag`** / **`stopspoof`** — Launch or stop AirTag spoofing.

### Devices

- **`listflippers`** — Scan for nearby Flipper Zero devices.
- **`selectflipper <idx>`** — Choose a Flipper from the discovered list for interactions.
- **`listairtags`** — Discover nearby AirTags.
- **`selectairtag <idx>`** — Choose an AirTag for follow-up actions.

## Portal

- **`startportal <path|default> <AP_SSID> [PSK]`** — Serve an Evil Portal bundle from SD or flash (`default` uses the built-in portal).
- **`stopportal`** — Shut down the active portal.
- **`listportals`** — List bundles on SD card or flash.
- **`evilportal -c <sethtmlstr|clear>`** — Manage the Evil Portal HTML buffer (`-c sethtmlstr` to capture inbound HTML, `-c clear` to revert to defaults).
- **`webauth on|off`** — Require or disable web UI login.

## Dual Communication

- **`commdiscovery`** — Enter UART discovery mode, broadcasting handshake frames until peers reply (run before `commconnect`).
- **`commconnect <peer_name>`** — Connect to a discovered peer (after `commdiscovery`).
- **`commsetpins <tx> <rx>`** — Save preferred pins.
- **`commsend <command> [data...]`** — Issue commands to the connected peer.
- **`commstatus`** — Inspect current link state.
- **`commdisconnect`** — Close the peer link.

## Storage

- **`sd_config`** — Display SD mode, pins, and status.
- **`sd_pins_spi <cs> <clk> <miso> <mosi>`** — Configure SPI wiring.
- **`sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>`** — Configure SDIO wiring.
- **`sd_save_config`** — Persist SD settings to storage.

## RGB

- **`rgbmode <rainbow|police|strobe|off|color>`** — Run an LED effect immediately.
- **`setrgbmode <normal|rainbow|stealth>`** — Persist the LED mode across reboots.
- **`setrgbpins <r> <g> <b>`** — Override discrete RGB GPIOs; pass the same pin for all three values to switch into single-wire NeoPixel mode on that data pin.
- **`setneopixelbrightness <0-100>`** / **`getneopixelbrightness`** — Control NeoPixel intensity.

## GPS

- **`gpsinfo [-s]`** — Stream current fix, satellites, and speed; pass `-s` to stop the display task.
- **`startwd [-s]`** — Begin Wi-Fi wardriving with GPS logging, CSV output, and monitor mode; pass `-s` to stop and flush logs.

## Settings

- **`settings list`** — Dump available configuration keys.
- **`settings help`** — Show supported subcommands.
- **`settings get <key>`** / **`settings set <key> <value>`** — Inspect or change individual options.
- **`settings reset [key]`** — Restore all settings or a specific key to defaults.
