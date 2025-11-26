# GhostESP Command List


```anything you read here may or may not be entirely accurate or up to date.``` 

<details>
<summary><strong>Basic Network Scanning</strong></summary>

- <code>scanap [seconds]</code>  
  Start scanning for all WiFi networks in range (optional duration)

- <code>list -a</code>  
  Show complete list of found WiFi networks with technical details (signal strength, security type, channels)

- <code>scansta</code>  
  Find devices connected to WiFi networks around you

- <code>list -s</code>  
  Show all discovered connected devices

- <code>stopscan</code>  
  Stop any active scanning operation

- <code>select -a &lt;number[,number,...]&gt;</code>  
  Target one or more networks from the scan list (use the number(s) shown in list -a)

- <code>select -s &lt;number&gt;</code>  
  Target a specific station from the scan list

- <code>select -airtag &lt;number&gt;</code>  
  Select an AirTag from the scan list
</details>

<details>
<summary><strong>Attack Modes</strong></summary>

- <code>attack -d</code>  
  Start deauthentication (temporarily disconnects devices from selected network)

- <code>attack -e</code>  
  Start EAPOL logoff attack

- <code>attack -s &lt;password&gt;</code>  
  Start SAE flood attack (ESP32-C5/C6 only)

- <code>stopdeauth</code>  
  Stop deauth attacks
</details>

<details>
<summary><strong>Karma Attack</strong></summary>

- <code>karma start</code>  
  Start Karma attack using all discovered SSIDs (learns from probe requests)

- <code>karma start &lt;SSID1&gt; [SSID2] [SSID3] ...</code>  
  Start Karma attack with specific SSIDs  
  Example: <code>karma start FreeWiFi Starbucks McDonald's</code>

- <code>karma stop</code>  
  Stop Karma attack

**How Karma Works:**
- Listens for probe requests from nearby devices
- Automatically caches SSIDs that devices are looking for
- Creates fake access points with those SSIDs
- Starts evil portals for each fake network
- Rotates between multiple SSIDs every 5 seconds
- Broadcasts beacon frames for all cached SSIDs every 500ms
</details>

<details>
<summary><strong>Network Generation (Beacon Spam)</strong></summary>

- <code>beaconspam -r</code>  
  Create multiple random fake networks

- <code>beaconspam -rr</code>  
  Create Never Gonna Give You Up themed networks

- <code>beaconspam -l</code>  
  Clone all visible networks in the area

- <code>beaconspam &lt;name&gt;</code>  
  Create a network with your chosen name

- <code>stopspam</code>  
  Stop creating fake networks

- <code>beaconadd &lt;SSID&gt;</code>  
  Add an SSID to the beacon spam list

- <code>beaconremove &lt;SSID&gt;</code>  
  Remove an SSID from the beacon spam list

- <code>beaconclear</code>  
  Clear the beacon spam list

- <code>beaconshow</code>  
  Show the current beacon spam list

- <code>beaconspamlist</code>  
  Start beacon spamming using the beacon spam list
</details>

<details>
<summary><strong>Evil Portal Creation</strong></summary>

- <strong>Start Default Portal:</strong>  
  <code>startportal default &lt;AP_SSID&gt; [PSK]</code>  
  Example: <code>startportal default FreeWiFi</code>  
  (PSK is optional for open APs)

- <strong>Start Custom Portal (Offline HTML):</strong>  
  <code>startportal &lt;file-name.html&gt; &lt;AP_SSID&gt; [PSK]</code>  
  Example: <code>startportal myportal.html FreeWiFi</code>

- <strong>List Available Portals:</strong>  
  <code>listportals</code>  
  (Shows all available HTML portals on the SD card)

- <strong>Stop Portal:</strong>  
  <code>stopportal</code>
</details>

<details>
<summary><strong>Network Capture (Requires SD Card/Flipper)</strong></summary>

- <code>capture -probe</code>  
  Save devices searching for WiFi

- <code>capture -beacon</code>  
  Save network broadcast information

- <code>capture -deauth</code>  
  Record deauthentication packets

- <code>capture -raw</code>  
  Save all wireless traffic

- <code>capture -wps</code>  
  Capture WPS setup packets

- <code>capture -pwn</code>  
  Record Pwnagotchi activity

- <code>capture -eapol</code>  
  Record EAPOL/handshake packets

- <code>capture -stop</code>  
  Stop recording and save data
</details>

<details>
<summary><strong>Network Connection & Tools</strong></summary>

- <code>connect &lt;SSID&gt; [Password]</code>  
  Connect to a WiFi network and save credentials

- <code>dialconnect</code>  
  Find and interact with smart TVs on network

- <code>powerprinter &lt;ip&gt; &lt;text&gt; &lt;size&gt; &lt;position&gt;</code>  
  Send text to network printers  
  Positions: CM (center), TL (top-left), TR (top-right), BR (bottom-right), BL (bottom-left)
</details>

<details>
<summary><strong>Chameleon Ultra Integration</strong> <em>(13.56MHz NFC/RFID Tool)</em></summary>

- <code>chameleon connect [timeout] [pin]</code>  
  Connect to Chameleon Ultra device via Bluetooth  
  Optional timeout (default: 10s) and PIN (4-6 digits) for authentication

- <code>chameleon disconnect</code>  
  Disconnect from Chameleon Ultra device

- <code>chameleon status</code>  
  Show connection status and device information

- <code>chameleon battery</code>  
  Check Chameleon Ultra battery level

- <code>chameleon firmware</code>  
  Display firmware version information

- <code>chameleon devicemode</code>  
  Show current device mode (Reader/Emulator)

- <code>chameleon reader</code>  
  Switch Chameleon Ultra to reader mode

- <code>chameleon emulator</code>  
  Switch Chameleon Ultra to emulator mode

**HF (13.56MHz) Operations:**
- <code>chameleon scanhf</code>  
  Scan for HF cards (ISO14443 Type A/B, MIFARE, NTAG)

- <code>chameleon savehf [filename]</code>  
  Save last HF scan results to SD card (/mnt/ghostesp/chameleon/)

- <code>chameleon readhf</code>  
  Basic MIFARE Classic card detection and information collection

- <code>chameleon savedump [filename]</code>  
  Save complete card dump data to SD card

**LF (125KHz) Operations:**
- <code>chameleon scanlf</code>  
  Scan for LF EM410X tags

- <code>chameleon scanhidprox</code>  
  Scan for HID Proximity cards

- <code>chameleon scanlfall</code>  
  Try both EM410X and HID Prox scanning

- <code>chameleon savelf [filename]</code>  
  Save last LF scan results to SD card

**MIFARE Classic Operations:**
- <code>chameleon mfdetect</code>  
  Detect MIFARE Classic support and card type

- <code>chameleon mfprng</code>  
  Test MIFARE Classic PRNG weakness


**Slot Management:**
- <code>chameleon activeslot</code>  
  Show currently active slot (1-8)

- <code>chameleon setslot &lt;1-8&gt;</code>  
  Change active slot

- <code>chameleon slotinfo &lt;1-8&gt;</code>  
  Display information about specific slot

**📋 Notes:**
- All save commands support optional custom filenames
- Files are saved to `/mnt/ghostesp/chameleon/` directory on SD card
- Auto-generated filenames include UID and timestamp
- Basic card detection and information collection only
- For advanced analysis, use specialized tools

</details>

<details>
<summary><strong>Bluetooth Operations</strong> <em>(Not available on ESP32-S2)</em></summary>

- <code>blescan -f</code>  
  Find Flipper Zero devices

- <code>blescan -ds</code>  
  Detect Bluetooth spam

- <code>blescan -a</code>  
  Scan for AirTags

- <code>blescan -r</code>  
  View all Bluetooth traffic

- <code>blescan -s</code>  
  Stop Bluetooth scanning

- <code>blewardriving</code>  
  Start BLE wardriving with GPS logging

- <code>blewardriving -s</code>  
  Stop BLE wardriving

- <code>blespam -apple|-ms|-samsung|-google|-random|-s</code>  
  BLE spam attacks
</details>

<details>
<summary><strong>GPS Features</strong></summary>

- <code>startwd</code>  
  Begin recording networks with GPS location

- <code>startwd -s</code>  
  Stop GPS recording

- <code>gpsinfo</code>  
  Show live GPS info
</details>

<details>
<summary><strong>System Commands</strong></summary>

- <code>help</code>  
  Show complete command list

- <code>stop</code>  
  Stop all running operations

- <code>reboot</code>  
  Restart device

- <code>setcountry &lt;CC&gt;</code>  
  Set the Wi-Fi country code (ESP32-C5 only)

- <code>timezone &lt;TZ_STRING&gt;</code>  
  Set the display timezone for the clock view

- <code>apcred &lt;ssid&gt; &lt;password&gt;</code>  
  Change GhostNet AP credentials

- <code>apcred -r</code>  
  Reset AP credentials to default

- <code>apenable &lt;on|off&gt;</code>  
  Enable or disable the Access Point across reboots

- <code>chipinfo</code>  
  Show chip and memory info

- <code>rgbmode &lt;rainbow|police|strobe|off|color&gt;</code>  
  Control LED effects

- <code>setrgbpins &lt;red&gt; &lt;green&gt; &lt;blue&gt;</code>  
  Change RGB LED pins

- <code>setneopixelbrightness &lt;0-100&gt;</code>
  Set the maximum neopixel brightness (percent). Example: `setneopixelbrightness 50`

- <code>getneopixelbrightness</code>
  Show the current saved neopixel max brightness (percent).

- <code>sd_config</code>  
  Show current SD GPIO pin configuration

- <code>sd_pins_mmc &lt;clk&gt; &lt;cmd&gt; &lt;d0&gt; &lt;d1&gt; &lt;d2&gt; &lt;d3&gt;</code>  
  Set SDMMC pins

- <code>sd_pins_spi &lt;cs&gt; &lt;clk&gt; &lt;miso&gt; &lt;mosi&gt;</code>  
  Set SPI pins

- <code>sd_save_config</code>  
  Save SD pin config to SD card
</details>

<details>
<summary><strong>Dual ESP32 Communication</strong></summary>

- <code>commstatus</code>  
  Check connection status between two ESP32 devices

- <code>commsend &lt;command&gt;</code>  
  Send any command to the other ESP32 device

- <code>commdisconnect</code>  
  Disconnect from the other ESP32 device

- <code>commdiscovery</code>  
  Check discovery status

- <code>commconnect &lt;device_name&gt;</code>  
  Connect to specific device

- <code>commsetpins &lt;tx&gt; &lt;rx&gt;</code>  
  Change UART pins for communication
</details>

<details>
<summary><strong>Utilities</strong></summary>

- <code>scanports local</code>  
  Scan ports on local subnet

- <code>scanports [IP] [start_port-end_port (OPTIONAL)]</code>  
  Scan ports on a specific IP

- <code>scanarp</code>  
  Perform ARP scan on local network to discover active hosts

- <code>scanssh [IP]</code>  
  Perform SSH scan on a specific IP

- <code>congestion</code>  
  Display Wi-Fi channel congestion chart

- <code>listenprobes [channel] [stop]</code>  
  Listen for and log probe requests
</details>

<sub><em>Remember to check your hardware compatibility before using commands.</em></sub>
