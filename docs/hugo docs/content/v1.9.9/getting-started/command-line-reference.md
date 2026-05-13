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

- **`help [category|all]`** — List commands by category (`wifi`, `ble`, `portal`, `comm`, `sd`, `led`, `gps`, `misc`, `printer`, `cast`, `capture`, `beacon`, `attack`, `ethernet`).
- **`chipinfo`** — Print SoC model, cores, features, and IDF version. When core dumps are enabled to flash, it also shows coredump partition status and (when available) the panic reason from the last crash.
- (for developers) **`mem [dump|trace <start|stop|dump>]`** — Print heap stats, dump allocation state, or control heap tracing.
- **`reboot`** — Soft restart the device.
- **`timezone <TZ>`** — Set timezone, e.g., `timezone EST5EDT,M3.2.0,M11.1.0`.
- **`stop`** — Stops all active attacks, scans, and background tasks. Also restarts Wi-Fi if it was suspended by BLE.

### Core dumps (flash builds only)

These commands are only present on builds that enable ESP-IDF core dumps **to flash** (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`).

- **`coredump`** — Print a quick summary (partition size and whether a coredump is present).
- **`coredump dump`** — Stream the coredump partition as base64. Save the output body to `coredump.b64` (omit the start/end marker lines), then decode on your host with `idf.py coredump-info -c coredump.b64`.
- **`coredump erase`** — Erase the coredump partition (clears the saved crash).

## WiFi

### Scanning

- **`scanap [seconds|-live|-stop]`** — Run an AP scan, optionally for a set duration, live channel hop, or stop (`-stop`).
- **`scansta`** — Hop channels and log associated stations.
- **`scanall [seconds]`** — Combined AP and STA scan with summary.
- **`sweep [-w wifi_sec] [-b ble_sec]`** — Full environment sweep: scans WiFi APs, stations, and BLE devices, then saves a CSV report to SD (`/mnt/ghostesp/sweeps/sweep_N.csv`).
- **`list [-a|-s|-airtags]`** — Show AP scan results, associated stations, or AirTags.
- **`listenprobes [channel|stop]`** — Monitor probe requests and log to PCAP if SD is present.

### Targeting

- **`select [-a|-s|-airtag] <idx[,idx]>`** — Queue APs, a station, or an AirTag by index for later actions.
- **`connect <ssid> [pass]`** — Join an infrastructure network (saves credentials); wrap SSID/password in quotes when they contain spaces, e.g., `connect "My SSID" "My Password"`.
- **`disconnect`** — Leave the current STA connection.
- **`apcred <ssid> <pass>`** or **`apcred -r`** — Change or reset GhostNet AP credentials.
- **`apenable on|off`** — Toggle AP persistence across reboots.
- **`trackap`** — Track selected AP signal strength (RSSI) in real-time.
- **`tracksta`** — Track selected station signal strength (RSSI) in real-time.

### Offense

- **`attack -d|-c|-e|-s <password>`** — Trigger deauth, channel switch (CSA), EAPOL logoff, or SAE flood (`-s` needs ESP32-C5/C6 and the target PSK).
  - `-d` — Deauthentication attack on selected AP(s).
  - `-c` — Channel Switch Announcement (CSA) attack. Sends forged 802.11 beacons with the AP's real SSID/BSSID and a Channel Switch Element (IE 37) directing clients to a different channel, causing disconnection.
  - `-e` — EAPOL logoff attack.
  - `-s` — SAE flood attack (ESP32-C5/C6 only, requires target PSK).
- **`stop`** — Stops all active attacks, scans, and background tasks.
- **`stopdeauth`** / **`stopspam`** — Halt active attacks or beacon floods.
- **`beaconspam [mode]`** — Broadcast spoof SSIDs (`-r`, `-rr`, `-l`, or custom text).
- **`karma start [ssid...]`** / **`karma stop`** — Respond to client probes with saved or provided SSIDs.
- **`pineap [-s]`** — Monitor Pineapple-style beacons; `-s` stops detection.
- **`saeflood <password>`** / **`stopsaeflood`** / **`saefloodhelp`** — Start, stop, or show help for SAE flood attacks.

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

### GATT

- **`blescan -g`** — Scan for connectable BLE devices for GATT enumeration.
- **`listgatt`** — List discovered GATT devices with tracker type detection.
- **`selectgatt <idx>`** — Select a device by index for enumeration or tracking.
- **`enumgatt`** — Connect to the selected device and enumerate its GATT services.
- **`trackgatt`** — Track the selected device using real-time RSSI signal strength.

### Aerial Detection

- **`aerialscan [seconds]`** — Scan for aerial devices (drones, UAVs, RC controllers) using WiFi and BLE in sequential phases. Default: 30 seconds. Phase 1: WiFi scan (OpenDroneID WiFi, DJI WiFi, drone networks). Phase 2: BLE scan (OpenDroneID BLE, DJI BLE) — **WiFi automatically suspended during BLE phase and restored after**.
- **`aeriallist`** — Display all detected aerial devices with full details including device ID, type, MAC address, vendor, signal strength (RSSI), GPS coordinates, altitude, speed, direction, operator location, and flight status.
- **`aerialtrack <idx|mac>`** — Track a specific aerial device by index or MAC address (e.g., `aerialtrack 0` or `aerialtrack 12:34:56:78:9a:bc`).
- **`aerialstop`** — Stop aerial device scanning and tracking.
- **`aerialspoof [device_id lat lon alt]`** — Broadcast fake drone RemoteID for testing via BLE. Without arguments, uses default test drone (GHOST-TEST at San Francisco, 100m altitude). With arguments: device ID, latitude, longitude, altitude in meters. Example: `aerialspoof DRONE-1234 40.7128 -74.0060 100`. Complies with ASTM F3411 OpenDroneID standard. **Note: WiFi automatically suspended during broadcast, restored on stop**.
- **`aerialspoofstop`** — Stop broadcasting fake drone RemoteID and restore WiFi.

## Portal

- **`startportal <path|default> <AP_SSID> [PSK]`** — Serve an Evil Portal bundle from SD or flash (`default` uses the built-in portal).
- **`stopportal`** — Shut down the active portal.
- **`listportals`** — List bundles on SD card or flash.
- **`evilportal -c <sethtmlstr|clear>`** — Manage the Evil Portal HTML buffer (`-c sethtmlstr` to capture inbound HTML, `-c clear` to revert to defaults).
- **`webauth on|off`** — Require or disable web UI login.

## GhostLink (Dual Communication)

- **`commdiscovery`** — Start discovery mode to find other GhostESP devices.
- **`commconnect <peer_name>`** — Connect to a discovered peer (after `commdiscovery`).
- **`commsetpins <tx> <rx>`** — Save preferred pins.
- **`commsend <command> [data...]`** — Issue commands to the connected peer.
- **`commstatus`** — Inspect current link state.
- **`commdisconnect`** — Close the peer link.

## Storage

### File Operations

- **`sd status`** — Show SD card mount status, type (physical/virtual), capacity, and usage percentage.
- **`sd list [path]`** — List files and directories with indices for quick reference. Default path: `/mnt/ghostesp`.
- **`sd info <index|path>`** — Display file or directory details (type, size, path).
- **`sd size <index|path>`** — Get file size in bytes (for pre-download checks).
- **`sd read <index|path> [offset] [length]`** — Read file with optional offset and length for chunked downloads. No size limit.
- **`sd write <path> <base64data>`** — Create/overwrite file with base64-decoded data.
- **`sd append <path> <base64data>`** — Append base64-decoded data to file.
- **`sd mkdir <path>`** — Create a new directory.
- **`sd rm <index|path>`** — Delete a file or empty directory.
- **`sd tree [path] [depth]`** — Recursive directory listing (default depth: 2, max: 10).

All `sd` commands return machine-parsable output with prefixes like `SD:OK:`, `SD:ERR:`, `SD:FILE:[n]`, `SD:DIR:[n]}`, `SD:READ:`, `SD:WRITE:`.

### Pin Configuration

- **`sd_config`** — Display SD mode, pins, and status.
- **`sd_pins_spi <cs> <clk> <miso> <mosi>`** — Configure SPI wiring.
- **`sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>`** — Configure SDIO wiring.
- **`sd_save_config`** — Persist SD settings to storage.

## Camera

Available on builds with **`CONFIG_HAS_CAMERA`**.

- **`motion ...`** — Motion detector controls for onboard camera builds. See the full [Motion Detector]({{< relref "../camera/motion-detector.md" >}}) guide for setup, tuning, SD snapshots, and Discord webhooks.

## RGB

- **`rgbmode <rainbow|police|strobe|off|color>`** — Run an LED effect immediately.
- **`setrgbmode <normal|rainbow|stealth>`** — Persist the LED mode across reboots.
- **`setrgbpins <r> <g> <b>`** — Override discrete RGB GPIOs; pass the same pin for all three values to switch into single-wire NeoPixel mode on that data pin.
- **`setrgbcount <1-512>`** — Persist the number of RGB LEDs connected so effects span the correct length. Reinitializes immediately if pins are already configured.
- **`setneopixelbrightness <0-100>`** / **`getneopixelbrightness`** — Control NeoPixel intensity.

## Status display (if present)

Available on boards with an onboard OLED status display or when an external status display is configured.

- **`statusidle [list|set <life|ghost|0|1>]`** — View or change the status OLED idle animation when `CONFIG_WITH_STATUS_DISPLAY` and a status display are enabled.
  - `statusidle` — Show the current idle animation and timeout.
  - `statusidle list` — List available idle animations.
  - `statusidle set <life|ghost|0|1>` — Select the idle animation mode.

## IO expander buttons (if present)

Available on boards with **CONFIG_USE_IO_EXPANDER**. Three physical buttons (P10, P11 “Right button”, P12) can each run a custom CLI command or act as a joystick button when no command is set.

- **`iobtn <1|2|3> [command]`** — View or set the command for button 1 (P10), 2 (P11), or 3 (P12). Without `command`, prints the current command (or “(none)”). With `command`, saves it and runs it on the next press. Example: `iobtn 1 nfc read`.
- **`settings get io_btn_p10_cmd`** / **`settings set io_btn_p10_cmd <value>`** — Same for P10; use `io_btn_p11_cmd` and `io_btn_p12_cmd` for P11 and P12.

On press, the device switches to the terminal view and runs the command. To use a button as a normal joystick action instead, clear its command (e.g. `iobtn 1 ""` or `settings set io_btn_p10_cmd ""`).

**On-device UI:** **Settings → IO Buttons** lets you edit each button’s command with the keyboard; the current command is pre-filled when editing.

## Infrared

- **`ir list [path]`** — List `.ir` files (default: `/mnt/ghostesp/infrared/remotes`).
- **`ir show <path|remote_index>`** — Parse and display signals from an IR file. After `ir list`, you can pass a numeric remote index.
- **`ir send <path|remote_index> [button_index]`** — Transmit a signal from a file. Use `remote_index` from `ir list` and optional `button_index` from `ir show`.
- **`ir universals list [-all]`** — List universal IR files and, with `-all`, all built‑in universal signals.
- **`ir universals send <index>`** — Transmit a built‑in universal signal by index (see `ir universals list -all`).
- **`ir universals sendall <file|TURNHISTVOFF> <button_name> [delay_ms]`** — Transmit all signals for a named button from a universal file or the built‑in TURNHISTVOFF set; can be stopped with `stop`.
- **`ir rx [timeout]`** — Wait up to `timeout` seconds (default 60) for a single IR signal, print it (decoded or RAW), then stop.
- **`ir learn [path]`** — Wait for a signal (10s). Without `path`, auto-create a new `.ir` file under `/mnt/ghostesp/infrared/remotes`; with `path`, append the learned signal to that file.
- **`ir dazzler [stop]`** — Start/stop continuous IR dazzler flood. Responses are machine-parsable: `IR_DAZZLER:STARTED`, `IR_DAZZLER:FAILED`, `IR_DAZZLER:ALREADY_RUNNING`, `IR_DAZZLER:STOPPING`, `IR_DAZZLER:NOT_RUNNING`.
- **`[IR/BEGIN]` / `[IR/CLOSE]` (UART IR envelope)**
  - **Usage:** Send `[IR/BEGIN]`, then a single IR message body, then `[IR/CLOSE]` on the same UART stream to trigger a one‑off transmit.
  - **Body format (`.ir` text block):** Same fields as a standard `.ir` file entry (for example: name, type, protocol, address, command).
  - **Body format (JSON):** Single JSON object carrying the same information as a `.ir` entry (parsed signal fields or raw timing data).
  - **Examples:**

    ```text
    [IR/BEGIN]
    name=Power
    type=parsed
    protocol=NEC
    addr=0x0000FFFF
    cmd=0x0000E718
    [IR/CLOSE]
    ```

    ```json
    [IR/BEGIN]
    {"name":"Power","type":"parsed","protocol":"NEC","addr":"0x0000FFFF","cmd":"0x0000E718"}
    [IR/CLOSE]
    ```

  - **CLI response on success:** `IR: send OK`, followed by a compact summary:
    - Parsed: `IR: signal [Name] protocol=NEC addr=0x0000FFFF cmd=0x0000E718`
    - Raw: `IR: signal raw len=N freq=38000Hz duty=0.33`

## GPS

- **`gpspin [pin]`** — View or set the GPS RX pin for external GPS modules. Without arguments, shows current pin. Setting persists to NVS; restart GPS commands to apply.
- **`gpsinfo [-s]`** — Stream current fix, satellites, and speed; pass `-s` to stop the display task.
- **`startwd [-s]`** — Start wardriving (logs Wi-Fi/GPS to CSV). Use `-s` to stop.

## Ethernet
*(Requires `CONFIG_WITH_ETHERNET`)*

### Connection Management

- **`ethup`** — Initialize and bring up Ethernet interface; waits for link establishment and DHCP assignment.
- **`ethdown`** — Deinitialize and bring down Ethernet interface.
- **`ethinfo`** — Display Ethernet connection information (status, IP address, netmask, gateway, DNS servers, DHCP server).
- **`webuiap [on|off|toggle|status]`** — Restrict the web UI to clients connected to the onboard AP subnet (AP-only mode).

### Network Scanning

- **`ethfp`** — Fingerprint network hosts using mDNS, NetBIOS, and SSDP (discovers Apple devices, Chromecasts, printers, Windows PCs, routers, smart TVs).
- **`etharp`** — Perform ARP scan on local Ethernet network subnet (1-254) to discover active hosts.
- **`ethping`** — Perform ICMP ping scan on local Ethernet network subnet (1-254) to find alive hosts.
- **`ethports [ip] [all|start-end]`** — Scan TCP ports on a target IP address.
  - Without arguments: scans common ports on gateway.
  - `all`: scan all ports (1-65535).
  - `start-end`: custom port range (e.g., `80-443`).
  - Examples: `ethports`, `ethports 192.168.1.1`, `ethports 192.168.1.1 all`, `ethports 192.168.1.1 80-443`.

### Network Tools

- **`ethdns <hostname>`** — Perform forward DNS lookup.
- **`ethdns reverse <ip_address>`** — Perform reverse DNS lookup.
- **`ethtrace <hostname_or_ip> [max_hops]`** — Perform traceroute to a target host (default: 30 hops, max: 64).
- **`ethserv [ip_address]`** — Service discovery and banner grabbing on a target IP (default: gateway). Scans common services (FTP, SSH, Telnet, SMTP, HTTP, HTTPS, etc.).
- **`ethhttp <url> [lines|all]`** — Send HTTP/HTTPS GET request to a server and display response.
  - Default: shows first 25 lines
  - `[lines]`: show first N lines (e.g., `ethhttp http://example.com 50`)
  - `all`: show full response (e.g., `ethhttp http://example.com all`)
  - Supports both HTTP and HTTPS (TLS 1.2)
  - Examples: `ethhttp http://example.com`, `ethhttp https://www.google.com 100`, `ethhttp http://192.168.1.1/index.html all`
- **`ethntp [ntp_server]`** — Query NTP server and synchronize system time. Default server: `pool.ntp.org`. Examples: `ethntp`, `ethntp pool.ntp.org`, `ethntp time.google.com`.

### Configuration

- **`ethconfig dhcp`** — Use DHCP for automatic IP assignment.
- **`ethconfig static <ip> <netmask> <gateway>`** — Set static IP configuration.
  - Example: `ethconfig static 192.168.1.100 255.255.255.0 192.168.1.1`
- **`ethconfig show`** — Show current IP configuration.
- **`ethmac`** — Display current MAC address.
- **`ethmac set <xx:xx:xx:xx:xx:xx>`** — Set Ethernet MAC address (may require reinitialization).
  - Example: `ethmac set 02:00:00:00:00:01`

### Statistics

- **`ethstats`** — Display Ethernet network statistics (link status, IP info, MAC address, packet statistics, ARP statistics).

## Settings

- **`settings list`** — Dump available configuration keys.
- **`settings help`** — Show supported subcommands.
- **`settings get <key>`** / **`settings set <key> <value>`** — Inspect or change individual options.
- **`settings reset [key]`** — Restore all settings or a specific key to defaults.

## BadUSB

- **`badusb list`** — List scripts in `/mnt/ghostesp/badusb/`.
- **`badusb run <filename>`** — Run a script from `/mnt/ghostesp/badusb/`.
- **`badusb stop`** — Stop the current BadUSB run.
- **`badusb exec <size>`** — Prepare for a streamed script.
- **`badusb set_vid <hex>`** — Set USB VID for the next run.
- **`badusb set_pid <hex>`** — Set USB PID for the next run.
- **`badusb set_mfr <text>`** — Set USB manufacturer for the next run.
- **`badusb set_prod <text>`** — Set USB product for the next run.
- **`badusb set_rand <0|1>`** — Toggle per-run USB detail randomization.
- **`badusb set_layout <n>`** — Set keyboard layout for the next run (`0` US, `1` DE, `2` FR, `3` UK, `4` ES).
