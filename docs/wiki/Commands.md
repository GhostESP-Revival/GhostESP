# Ghost ESP Commands Guide

## 🔍 Basic Network Scanning

- `scanap [seconds]` - Start scanning for all WiFi networks in range (optional duration)
- `list -a` - Show complete list of found WiFi networks with technical details (signal strength, security type, channels)
- `scansta` - Find devices connected to WiFi networks around you
- `list -s` - Show all discovered connected devices
- `stopscan` - Stop any active scanning operation
- `select -a <number[,number,...]>` - Target one or more networks from the scan list (use the number(s) shown in list -a)
- `select -s <number>` - Target a specific station from the scan list
- `select -airtag <number>` - Select an AirTag from the scan list

## ⚡ Attack Modes

- `attack -d` - Start deauthentication (temporarily disconnects devices from selected network)
- `attack -e` - Start EAPOL logoff attack
- `attack -s` - Start SAE flood attack (ESP32-C5/C6 only)
- `stopdeauth` - Stop deauth attacks

## 📡 Network Generation

- `beaconspam -r` - Create multiple random fake networks
- `beaconspam -rr` - Create Never Gonna Give You Up themed networks
- `beaconspam -l` - Clone all visible networks in the area
- `beaconspam <name>` - Create a network with your chosen name
- `stopspam` - Stop creating fake networks
- `beaconadd <SSID>` - Add an SSID to the beacon spam list
- `beaconremove <SSID>` - Remove an SSID from the beacon spam list
- `beaconclear` - Clear the beacon spam list
- `beaconshow` - Show the current beacon spam list
- `beaconspamlist` - Start beacon spamming using the beacon spam list

## 🕸️ Evil Portal Creation

- **Start Default Portal:**  
  `startportal default <AP_SSID> [PSK]`  
  Example:  
  `startportal default FreeWiFi`  
  (PSK is optional for open APs)

- **Start Custom Portal (Offline HTML):**  
  `startportal <file-name.html> <AP_SSID> [PSK]`  
  Example:  
  `startportal myportal.html FreeWiFi`  

- **List Available Portals:**  
  `listportals`  
  (Shows all available HTML portals on the SD card)

- **Stop Portal:**  
  `stopportal`

## 💾 Network Capture (Requires SD Card/Flipper)

- `capture -probe` - Save devices searching for WiFi
- `capture -beacon` - Save network broadcast information
- `capture -deauth` - Record deauthentication packets
- `capture -raw` - Save all wireless traffic
- `capture -wps` - Capture WPS setup packets
- `capture -pwn` - Record Pwnagotchi activity
- `capture -eapol` - Record EAPOL/handshake packets
- `capture -stop` - Stop recording and save data

## 🌐 Network Connection & Tools

- `connect <SSID> [Password]` - Connect to a WiFi network and save credentials
- `dialconnect` - Find and interact with smart TVs on network
- `powerprinter <ip> <text> <size> <position>` - Send text to network printers  
  Positions: CM (center), TL (top-left), TR (top-right), BR (bottom-right), BL (bottom-left)

## 📱 Bluetooth Operations

Not available on ESP32-S2:

- `blescan -f` - Find Flipper Zero devices
- `blescan -ds` - Detect Bluetooth spam
- `blescan -a` - Scan for AirTags
- `blescan -r` - View all Bluetooth traffic
- `blescan -s` - Stop Bluetooth scanning
- `blewardriving` - Start BLE wardriving with GPS logging
- `blewardriving -s` - Stop BLE wardriving
- `blespam -apple|-ms|-samsung|-google|-random|-s` - BLE spam attacks


## 📍 GPS Features

- `startwd` - Begin recording networks with GPS location
- `startwd -s` - Stop GPS recording
- `gpsinfo` - Show live GPS info

## 🔧 System Commands

- `help` - Show complete command list
- `stop` - Stop all running operations
- `reboot` - Restart device
- `setcountry <CC>` - Set the Wi-Fi country code (ESP32-C5 only)
- `timezone <TZ_STRING>` - Set the display timezone for the clock view
- `apcred <ssid> <password>` - Change GhostNet AP credentials
- `apcred -r` - Reset AP credentials to default
- `apenable <on|off>` - Enable or disable the Access Point across reboots
- `chipinfo` - Show chip and memory info
- `rgbmode <rainbow|police|strobe|off|color>` - Control LED effects
- `setrgbpins <red> <green> <blue>` - Change RGB LED pins
- `sd_config` - Show current SD GPIO pin configuration
- `sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>` - Set SDMMC pins
- `sd_pins_spi <cs> <clk> <miso> <mosi>` - Set SPI pins
- `sd_save_config` - Save SD pin config to SD card

## 🔗 Dual ESP32 Communication

- `commstatus` - Check connection status between two ESP32 devices
- `commsend <command>` - Send any command to the other ESP32 device
- `commdisconnect` - Disconnect from the other ESP32 device
- `commdiscovery` - Check discovery status
- `commconnect <device_name>` - Connect to specific device
- `commsetpins <tx> <rx>` - Change UART pins for communication

## 🛠️ Utilities

- `scanports local [-C/-A/start_port-end_port]` - Scan ports on local subnet
- `scanports [IP] [-C/-A/start_port-end_port]` - Scan ports on a specific IP
- `congestion` - Display Wi-Fi channel congestion chart
- `listenprobes [channel] [stop]` - Listen for and log probe requests

> Remember to check your hardware compatibility before using commands
