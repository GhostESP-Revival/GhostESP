
<img width="800" alt="ghostesp_white_text_logo2" src="https://github.com/user-attachments/assets/f2cb3bb4-ab79-4679-8db1-beddc306ba07" />

> **The open-source wireless research platform for ESP32.**
> Built on ESP-IDF v6.0. **v2.0** turns GhostESP into a full graphical, extensible, multi-radio environment.

[![Version](https://img.shields.io/badge/version-2.0-7c5cff?style=flat-square)](https://github.com/spookyorigin/Ghost_ESP)
[![License](https://img.shields.io/badge/license-GPL--3.0-blue?style=flat-square)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0-orange?style=flat-square)](https://docs.espressif.com/projects/esp-idf/)
[![Discord](https://img.shields.io/discord/5cyNmUMgwh?style=flat-square&label=Discord&color=5865F2)](https://discord.gg/5cyNmUMgwh)
[![Boards](https://img.shields.io/badge/board%20targets-45-2ea043?style=flat-square)](#supported-boards)

**⭐️ Enjoying GhostESP? Please give the repo a star. It helps a lot.**

---

## What's New in 2.0

v2.0 is the largest update in the project's history, building on v1.9.10 with a rebuilt user experience, a first-class app ecosystem, expanded radio and device workflows, and broad platform hardening.

- **Redesigned UI experience**: 60 FPS rendering, toast notifications, a cleaner status bar, custom **asset packs** (icons, themes, backgrounds) loaded from SD, touch-drag scrolling, polished setup wizard styling with Home WiFi setup, and a comprehensive **accessibility suite** (font size, high contrast, reduced motion, input repeat, epilepsy-safe mode).
- **Native SD Apps & App Gallery**: A new app system for loading tools from SD with permissions, scoped storage, custom icons, accent colors, and a central launcher. ESP32-C5 builds can also run supported app code directly from flash (XIP), avoiding the small internal-RAM ceiling for app code. Build your own apps with the **Ghost Build Tool (`gbt`)**, the native app SDK, and example apps.

  <img width="320" height="170" alt="app-gallery2" src="https://github.com/user-attachments/assets/f7bb96ed-db0c-4777-a721-ded2d397b167" />

- **WiFi Airspace Monitor**: Real-time packet/threat insights with adaptive channel dwell, a learned EWMA baseline that adapts to the local environment, a unified threat/insight engine, and an on-screen packets/sec sparkline.

  <img width="320" height="170" alt="airspace-monitor" src="https://github.com/user-attachments/assets/e049dfc8-3888-42ec-9fd1-6be62fcec114" />

- **Expanded Ghostchi**: The companion system now has 50 levels, 27 XP sources, passive/aggressive modes, a global mood system, level-up toasts, and a persistent status-bar badge.
- **PIN lockscreen**: A new overlay lockscreen with auto-lock support, so active captures (wardriving, sniffing) keep running while the device is locked.

  <img width="320" height="170" alt="ghost_mirror_1782715595937" src="https://github.com/user-attachments/assets/e7a691ed-24db-4a06-b9e6-f204518c6fe6" />
  
- **Expanded BadUSB**: Trackpad/cursor control, full touch support, USB HID keyboard output mode, mouse jiggler, `type_char` CLI, and a dedicated WebUI page.
- **Redesigned WebUI**: A completely refreshed browser interface with a dedicated BadUSB page and improved remote control flow.
- **More on-device workflows**: SD Browser, text previews, copy/move staging, on-device PCAP browser with hc22000 export, runtime GPS baud settings, on-device AP/STA credential editing, timezone quick-edit, and live RSSI meter views.
- **New network recon**: SSH, NetBIOS, HTTP banner, and SNMP scanners (with per-host and per-subnet variants) plus a WPA3 compliance checker.
- **GhostLink BLE bridge**: Assign a connected chip to bridge a main chip and the Android companion app, plus `wdstream` CLI streaming for companion wardriving.
- **More board support**: Added Marauder V8, Pancake C5, and LilyGo T-Dongle-S3/C5 builds, plus S3TWatch DRV2605 haptics, Cardputer ENV-III support, and fixes for NM-CYD-C5 SD app loading.
- **Under the hood**: v2.0 brings a major internal cleanup over v1.9.10: the commandline has been split into focused modules, repeated SD mount/unmount logic now goes through shared on-demand helpers, and the core is safer under memory pressure. The release also hardens the platform with checked allocations, bounded string handling, safer cleanup paths, concurrency fixes, lower memory use on non-PSRAM builds, and stability fixes across WiFi, BLE, audio, GPS, LVGL async calls, shared SPI/UART buses, NFC, DIAL, SD mounting, and app loading.

See [`CHANGELOG.md`](CHANGELOG.md) for the full v2.0 development history.

---

## Get Started

| | | |
| --- | --- | --- |
| **Flash your device** | **Community & support** | **Learn more** |
| [ghostesp.net/flasher](https://ghostesp.net/flasher) | [Discord](https://discord.gg/5cyNmUMgwh) | [Documentation](https://docs.ghostesp.net) · [Website](https://ghostesp.net) |

---

## Flagship Capabilities

A few things set GhostESP apart from every other ESP32 firmware:

- **Native SD App ecosystem**: Create, package, discover, launch, inspect, and stop SD-loaded apps with permissions and scoped storage. Build your own with `gbt`.
- **ESP-IDF-native architecture**: built directly on Espressif's SDK instead of Arduino/PlatformIO, giving GhostESP tighter control over Wi-Fi, Bluetooth, USB, memory, and low-level hardware features.
- **GhostLink**: dual-ESP32 command/display interface with remote radio, remote keyboard, BLE bridging, and split-channel wardriving.
- **Multi-interface control**: use GhostESP from the on-device UI, Flipper Zero app, serial CLI, WebUI, Android companion app, or GhostLink-connected devices.
- **Broad hardware and radio coverage**: Wi-Fi, BLE, NFC, IR, SubGHz, NRF24, Ethernet, GPS, USB HID, and 802.15.4/Zigbee across 45 board targets.
- **Research-ready capture workflows**: PCAP, hc22000, WiGLE CSV, sweep captures, Wireshark streaming, SD browsing, and on-device export tools.
- **Full graphical UI platform**: carousel, grid, and list layouts with themes, asset packs, touch/keyboard/encoder support, and accessibility options.

---

## Features

<details>
<summary><strong>WiFi Features</strong></summary>

- Evil Portal (with custom HTML from buffer via serial)
- Deauth / disassoc attacks
- Channel switch attack
- GTK abuse / client isolation testing
- EAPOL logoff attack
- Karma (with custom SSID lists and custom portal chaining)
- Beacon spam (single/list/random/Rickroll)
- AP scan / STA scan / scanall
- Multi-select APs and stations
- Probe request listening (with auto-spawned Evil Twin)
- Handshake + PMKID capture
- WiFi capture to SD (PCAP) with on-device PCAP browser + hc22000 export
- USB dongle mode for Wireshark (extcap stream)
- DHCP starvation
- ARP / port / SSH / local IP scanners
- mDNS discovery / NetBIOS scan / HTTP banner scan / SNMP probe (with per-host and per-subnet variants)
- WiFi OUI vendor lookup
- WPA3/SAE attacks (flood + compliance checker)
- Wardriving exports (WiFi/BLE/GPS) + sweep CSV (WiFi/BLE/GPS/802.15.4)
- Split-channel wardriving helper via GhostLink
- RSSI tracking (AP/station) with live RSSI meter view
- Drone detection / spoofing
- PineAP detection
- Flock / surveillance detector
- WPS detection
- Pwnagotchi-style automated capture mode (Capture PWN)
- Channel congestion analysis
- WiFi Airspace Monitor (real-time packet/threat insights, fast channel hopping, suspect device cards, packets/sec sparkline)
- DNS Sinkhole (blocklist-based NXDOMAIN blocking with built-in blocklist downloads)
- Web UI + filesystem + remote command relay
</details>

<details>
<summary><strong>BLE Features</strong></summary>

- BLE scan modes (general, AirTag, Flipper, raw)
- BLE advertisement scan with OUI prefix/vendor filtering + RGB match pulses
- BLE spam modes (Apple, Microsoft, Samsung, Google, Random)
- AirTag scan / spoof / select
- BLE packet capture
- BLE stream to Wireshark
- Flipper finder + RSSI
- GATT/service scan + per-device enumeration + device tracking (live RSSI meter)
- BLE wardriving
- BLE skimmer detection
- Drone / OpenDroneID scan / list / track / spoof
- Aerial (drone) detector with threat classification
- GhostLink BLE bridge to the Android companion app
</details>

<details>
<summary><strong>USB Features</strong></summary>

- USB keyboard host mode (ESP32-S3 builds)
- Remote keyboard control over GhostLink
- BadUSB script runner
- BadUSB identity options (VID/PID/manufacturer/product/layout/randomize)
- BadUSB trackpad (touchpad-style cursor control)
- BadUSB mouse jiggler
- BadUSB `type_char` CLI for typing individual ASCII characters
- USB HID keyboard output mode (forward on-device keystrokes over USB)
- Dedicated WebUI BadUSB page
</details>

<details>
<summary><strong>IR Features</strong></summary>

- IR TX/RX on supported boards
- IR learn mode
- IR easy learn mode
- Flipper `.ir` file support
- Universal library transmit
- IR CLI tools
- IR dazzler (38 kHz high duty)
</details>

<details>
<summary><strong>NFC Features</strong></summary>

- PN532 NTAG/MIFARE Classic support
- Flipper `.nfc` import/export
- MIFARE Classic dictionary attack (default + user dictionary + session key reuse / sector sweep)
- Full embedded MIFARE Classic dictionary
- Flipper NFC parser set (transit, parking, access, amusement, loyalty): BIP, Clipper, CharlieCard, Troika, Plantain, Zolotaya Korona, Ventra, WashCity, Social Moscow, Sonicare, Saflok, Gallagher, Disney Infinity, Skylanders, Aime, Hi, HWorld, Two Cities, Umarsh, Microel, MIZIP, MetroMoney, Kazan, SmartRider, TRT, and more
- MIFARE Desfire detection
- Chameleon Ultra support (CLI + UI + BLE control)
- Chameleon Ultra HF/LF RFID scan + reader controls
</details>

<details>
<summary><strong>SubGHz Features</strong></summary>

- Signal scanning across 64 channels
- Frequency analyzer with waterfall display
- Signal capture and decoding
- 20+ protocol decoders based on Flipper Unleashed/xMasterX
- Signal transmission and replay
- Saved signals as `.sub` files
- Flipper SubGhz Key File format compatibility
- CC1101 hardware support
- Frequency bands: 315, 390, 433.92, 868.35, 915 MHz
- Full CLI support
- SubGHz remote radio support via GhostLink
</details>

<details>
<summary><strong>Audio Features</strong></summary>

- I2S DAC audio player (MP3 playback with headphone detection and volume control)
- Audio receiver (TLV320DAC I2S)
- Music visualizer (RGB LED audio visualization)
- Microphone spectrum / MIC visualizer (microphone-driven RGB LED effects with multiple modes, color modes, sensitivity, smoothing, and contrast)
</details>

<details>
<summary><strong>Display & Sensor Features</strong></summary>

- Full LVGL graphical UI with carousel, grid, and list layouts
- Custom asset packs loaded from SD (icons, colors, backgrounds, themes)
- 17+ color themes
- On-screen splash/boot animation with progress bar
- Toast notification system
- Persistent status bar with level badge
- Touch drag scroll + tap-to-wake
- Configurable screen timeout, brightness, and orientation
- Idle animations (Game of Life, Ghost, Starfield, HUD, Matrix, Flying Ghosts, Spiral, Falling Leaves, Bouncing Text)
- On-screen Clock
- Compass screen (magnetometer)
- Accelerometer screen (G-force, tilt, orientation, shake, speed)
- ENV-III sensor screen (temperature, humidity, pressure, dew point, altitude)
- PIN Lock screen with auto-lock (overlay mode keeps captures running while locked)
- Trackpad / cursor control
- Setup wizard with Home WiFi configuration
- Accessibility settings (font size, high contrast, reduced motion, input repeat speed, epilepsy-safe mode)
- Terminal font size control
- Rave mode (display builds)
- DRV2605 haptic feedback (S3TWatch)
</details>

<details>
<summary><strong>Apps & Extensibility</strong></summary>

- Apps Gallery (central launcher for native SD apps, with categorical submenus)
- Native SD app system (load, list, inspect, launch, stop, reset apps with permissions and scoped storage)
- Ghost Build Tool (`gbt`) for scaffolding, building, and packaging apps and firmware
- Plugin/app SDK and example apps (Device Inspector, ESP32Finder)
- Ghostchi virtual pet companion (50-level XP system, 27 XP sources, passive/aggressive modes, companion lockscreen, global mood, level-up toasts, status-bar badge)
- SD Browser (file/folder browsing, rename, delete, copy/move, text file preview)
- On-device PCAP browser with hc22000 export
- On-device Info screen (device, runtime, build, credits)
- Reusable confirmation popups for dangerous UI actions
</details>

<details>
<summary><strong>Additional Features</strong></summary>

- GhostLink (dual-device command and display interface) with remote radio support and keyboard relay
- Setup wizard (display builds)
- Wired + web screen mirroring
- Ethernet mode (W5500) + full toolset: fingerprint scan, port scan, ping sweep, ARP scan, ARP poisoning, MITM, HTTP, DNS, NTP, trace route, MAC tools, statistics
- TLS SNI / HTTP / FTP credential capture over Ethernet
- DIAL / Chromecast V2 support
- GPS integration (`gpsinfo`) with WiGLE manual upload, runtime baud config, and `wdstream` companion streaming
- Network printer output (`powerprinter`, PJL)
- RGB LED modes (Normal, Rainbow, Stealth, Knight Rider, MIC Visualizer, custom colors) with neopixel brightness control
- Timezone configuration (`timezone`) + NTP time set
- Camera motion detection with SD card snapshot capture and Discord webhook alerts (XIAO S3 Sense)
- Live MJPEG camera stream (`/camera`)
- NRF24 spectrum analyzer with passive 2.4 GHz jamming detection
- Zigbee / 802.15.4 packet capture + sweep CSV (ESP32-C5/C6)
- 802.15.4 / Zigbee channel capture
- Battery monitoring / fuel gauge support
- Sensor / RTC hardware support (PCF8563)
- M5 Cardputer / Cardputer ADV keyboard support
- Android companion app
- On-device CH422G / ST7262 / AXS15231B / APX2102 display driver support
- Light-sleep idle + frequency scaling + Wi-Fi power saving
- Reduced-motion animations
- SD config backup / restore
</details>


---

## Supported ESP32 Variants

- ESP32-Wroom · ESP32-S2 · ESP32-C3 · ESP32-S3 · ESP32-C5 · ESP32-C6

> **Note:** Feature availability varies by chip. S2 lacks Bluetooth hardware; C5/C6 add 5 GHz and 802.15.4/Zigbee support.

---

## Supported Boards

45 board targets ship out of the box, grouped below.

<details>
<summary><strong>DevKit / reference boards</strong></summary>

- ESP32-Wroom DevKitC
- ESP32-S2 DevKitC (no Bluetooth)
- ESP32-C3 DevKitC
- ESP32-S3 DevKitC
- ESP32-C5 DevKitC
- ESP32-C6 DevKitC
</details>

<details>
<summary><strong>CYD family</strong></summary>

- CYD 2432S028R (2.4")
- CYD2 USB
- CYD2 USB 2.4"
- CYD2 USB 2.4" (C variant)
- CYD2 Dual USB
- CYD2 Micro USB
- NM-CYD-C5
</details>

<details>
<summary><strong>LilyGo</strong></summary>

- T-Deck
- T-Display S3 Touch
- T-Dongle-S3
- T-Dongle-C5
- T-Embed CC1101
- S3TWatch
</details>

<details>
<summary><strong>M5Stack</strong></summary>

- Cardputer
- Cardputer ADV
</details>

<details>
<summary><strong>Marauder</strong></summary>

- Marauder v4
- Marauder v6
- Marauder v8
- JCMK Devboard Pro
</details>

<details>
<summary><strong>Rabbit-Labs</strong></summary>

- GhostBoard
- Poltergeist
- Phantom
- Yapper Board
- Minion
</details>

<details>
<summary><strong>The Wired Hatter's</strong></summary>

- Rocket
- Pocket Marauder
- Banshee
</details>

<details>
<summary><strong>Other Boards</strong></summary>

- AWOK Mini
- Pancake C5
- Febris Pro
- Flipper JCMK GPS
- JC3248W535EN
- Waveshare 7" Touch
- Heltec V3 (no LoRa)
- Lolin S3 Pro
- Seeed XIAO ESP32-S3 Sense
- Seeed XIAO ESP32-S3
- Seeed XIAO ESP32-C5
</details>

---

## ESP32 Firmware Comparison

<details>
<summary><strong>View comparison table</strong></summary>

This comparison is based on GhostESP's feature set and publicly available source for the listed projects. It is not a complete feature list for every firmware. HaleHound and nyanBOX are compared against the latest public source available to us; if newer releases are closed source, this table cannot be independently updated or verified against those builds.

| Feature | GhostESP | Bruce | HaleHound | nyanBOX |
| --- | --- | --- | --- | --- |
| Current source available for audit | [x] | [x] | Limited / older public source | Limited / older public source |
| ESP-IDF-native architecture | [x] |  |  |  |
| Arduino / PlatformIO architecture |  | [x] | [x] | [x] |
| Approximate source size | ~211k LOC | ~156k LOC | ~62k LOC | ~17k LOC |
| Supported board targets | 45 | 28+ | 5 | 1 |
| Full LVGL graphical UI | [x] |  |  |  |
| Web dashboard / REST control | [x] | [x] |  |  |
| Captive portal web server | [x] | [x] | [x] | [x] |
| AP / station WiFi scanning | [x] | [x] | [x] | [x] |
| Deauth / disassoc testing | [x] | [x] | [x] | [x] |
| Beacon spam | [x] | [x] | [x] | [x] |
| Karma / probe response attack | [x] | [x] | [x] |  |
| Handshake / EAPOL capture | [x] | [x] | [x] |  |
| PMKID capture / export | [x] |  | [x] |  |
| Live Wireshark USB streaming | [x] |  |  |  |
| WPA3 / SAE-specific testing | [x] |  |  |  |
| WPA3 compliance checker | [x] |  |  |  |
| EAPOL logoff attack | [x] |  |  |  |
| Channel switch attack | [x] |  |  |  |
| GTK abuse / client isolation testing | [x] |  |  |  |
| DHCP starvation | [x] | [x] |  |  |
| ARP / port / SSH scanners | [x] | [x] |  |  |
| mDNS discovery | [x] |  |  |  |
| NetBIOS scanner | [x] |  |  |  |
| HTTP banner scanner | [x] |  |  |  |
| SNMP probe | [x] |  |  |  |
| WiFi OUI vendor lookup | [x] | [x] | [x] |  |
| PineAP / Evil Twin detection | [x] |  |  | [x] |
| WPS detection / reporting | [x] | [x] |  |  |
| Pwnagotchi-style automated capture mode | [x] | [x] |  |  |
| Pwnagotchi detector / spam |  | [x] |  | [x] |
| Channel congestion analysis | [x] |  |  |  |
| WiFi Airspace Monitor | [x] |  |  |  |
| DNS sinkhole / blocklist NXDOMAIN | [x] |  |  |  |
| GPS WiFi wardriving | [x] | [x] | [x] |  |
| BLE wardriving | [x] | [x] | [x] |  |
| WiGLE upload integration | [x] | [x] |  |  |
| 802.15.4 / Zigbee sweep export | [x] |  |  |  |
| GhostLink dual-ESP control | [x] |  |  |  |
| Split-channel wardriving helper | [x] |  |  |  |
| GhostLink remote radio support | [x] |  |  |  |
| Drone / OpenDroneID detect | [x] |  |  | [x] |
| Drone / OpenDroneID spoof | [x] |  |  |  |
| BLE scanning | [x] | [x] | [x] | [x] |
| Raw BLE scanner | [x] |  |  |  |
| BLE spam modes | [x] | [x] | [x] | [x] |
| AirTag scan / spoof | [x] | [x] | [x] | [x] |
| BLE tracker detection suite | [x] |  | [x] | [x] |
| Flipper Zero finder | [x] |  |  | [x] |
| GATT / service enumeration | [x] |  | [x] |  |
| BLE device tracking by RSSI | [x] |  |  |  |
| BLE stream to Wireshark | [x] |  |  |  |
| BLE skimmer detection | [x] |  |  | [x] |
| FastPair / pairing exploit research |  | [x] | [x] | [x] |
| BLE HID injection / DuckyScript over BLE |  | [x] |  |  |
| BLE keyboard mode |  | [x] |  |  |
| BLE GATT honeypot / cloned peripheral |  |  | [x] | [x] |
| BLE vulnerability profiling |  | [x] |  |  |
| Flock / surveillance detector | [x] |  | [x] | [x] |
| PN532 NFC support | [x] | [x] | [x] |  |
| Chameleon Ultra support | [x] | [x] |  |  |
| Chameleon Ultra BLE control | [x] | [x] |  |  |
| Flipper `.nfc` import/export | [x] |  |  |  |
| Flipper NFC parser set | [x] |  |  |  |
| MIFARE Classic default-key attack | [x] | [x] | [x] |  |
| MIFARE Classic full embedded dictionary | [x] |  |  |  |
| MIFARE Classic user dictionary file | [x] | [x] |  |  |
| MIFARE Classic session key reuse / sector sweep | [x] |  |  |  |
| EMV / payment card reader |  | [x] |  |  |
| BadUSB / DuckyScript | [x] | [x] |  |  |
| USB keyboard host mode | [x] |  |  |  |
| USB HID keyboard output mode | [x] | [x] |  |  |
| Remote keyboard over dual-device link | [x] |  |  |  |
| BadUSB VID/PID identity options | [x] | [x] |  |  |
| BadUSB mouse jiggler / trackpad | [x] |  |  |  |
| IR learn / capture / replay | [x] | [x] |  |  |
| Flipper `.ir` file support | [x] | [x] |  |  |
| Universal IR library transmit | [x] | [x] |  |  |
| CC1101 SubGHz scan / replay | [x] | [x] | [x] |  |
| CC1101 waterfall spectrum analyzer | [x] | [x] | [x] |  |
| Flipper `.sub` compatibility | [x] | [x] |  | [x] |
| SubGHz protocol decoders | [x] | [x] | [x] |  |
| NRF24 spectrum analyzer | [x] | [x] | [x] | [x] |
| NRF24 MouseJack |  |  | [x] |  |
| Passive jamming detection | [x] |  | [x] |  |
| Active RF jamming shipped | Not shipped | [x] | [x] | [x] |
| Zigbee / 802.15.4 packet capture | [x] |  |  |  |
| Ethernet W5500 support | [x] | [x] |  |  |
| Ethernet ARP poisoning / MITM tools | [x] | [x] |  |  |
| Ethernet fingerprint / port / ping tools | [x] |  |  |  |
| Ethernet DNS / NTP / HTTP / trace tools | [x] |  |  |  |
| TLS SNI / HTTP / FTP credential capture over Ethernet | [x] |  |  |  |
| Camera streaming / motion detection | [x] |  |  |  |
| Motion alerts with webhook support | [x] |  |  |  |
| Network printer / PJL output | [x] |  |  |  |
| DIAL / Chromecast testing | [x] |  |  |  |
| On-device setup wizard | [x] |  |  |  |
| Wired screen mirroring | [x] |  |  | [x] |
| Web screen mirroring |  | [x] |  |  |
| SD config backup / restore | [x] |  |  |  |
| SD file manager / browser | [x] | [x] | [x] |  |
| Native SD app/plugin system | [x] |  |  |  |
| Native app SDK / build tooling | [x] |  |  |  |
| Apps gallery / launcher | [x] |  |  |  |
| Ghostchi / virtual pet | [x] | [x] |  |  |
| Audio player | [x] | [x] |  |  |
| Microphone spectrum / visualizer | [x] | [x] |  |  |
| RGB LED visualizer modes | [x] | [x] |  |  |
| Clock / RTC screen | [x] | [x] |  |  |
| Compass screen | [x] |  |  |  |
| Accelerometer screen | [x] |  |  |  |
| ENV-III temperature / humidity / pressure | [x] |  |  |  |
| Battery monitoring / fuel gauge support | [x] | [x] | [x] |  |
| Sensor / RTC hardware support | [x] | [x] |  |  |
| M5 Cardputer keyboard support | [x] | [x] |  |  |
| Android companion app | [x] |  |  |  |
| Accessibility modes / reduced motion | [x] |  | [x] |  |
| Custom theme / UI palette system | [x] | [x] | [x] |  |
| Custom SD asset packs | [x] |  |  |  |
| LoRa support |  | [x] |  |  |
| FM radio support |  | [x] |  |  |

> GhostESP does not ship active jamming features. Distribution, promotion, sale and use of jamming devices or firmware is illegal in many jurisdictions. 

</details>




## Credits

Special thanks to:

<table>
  <tr>
    <td align="center">
      <a href="https://github.com/justcallmekoko">
        <img src="https://github.com/justcallmekoko.png" width="80" height="80" style="border-radius: 50%;" alt="JustCallMeKoKo"/><br/>
        <b>JustCallMeKoKo</b>
      </a><br/>
      <sub>ESP32Marauder foundational development</sub>
    </td>
    <td align="center">
      <a href="https://github.com/thibauts">
        <img src="https://github.com/thibauts.png" width="80" height="80" style="border-radius: 50%;" alt="thibauts"/><br/>
        <b>thibauts</b>
      </a><br/>
      <sub>CastV2 protocol insights</sub>
    </td>
    <td align="center">
      <a href="https://github.com/MarcoLucidi01">
        <img src="https://github.com/MarcoLucidi01.png" width="80" height="80" style="border-radius: 50%;" alt="MarcoLucidi01"/><br/>
        <b>MarcoLucidi01</b>
      </a><br/>
      <sub>DIAL protocol integration</sub>
    </td>
    <td align="center">
      <a href="https://github.com/SpacehuhnTech">
        <img src="https://github.com/SpacehuhnTech.png" width="80" height="80" style="border-radius: 50%;" alt="SpacehuhnTech"/><br/>
        <b>SpacehuhnTech</b>
      </a><br/>
      <sub>Reference deauthentication code</sub>
    </td>
  </tr>
  <tr>
    <td align="center">
      <a href="https://github.com/Spooks4576">
        <img src="https://github.com/Spooks4576.png" width="80" height="80" style="border-radius: 50%;" alt="Spooks4576"/><br/>
        <b>Spooks4576</b>
      </a><br/>
      <sub>Original GhostESP Developer</sub>
    </td>
    <td align="center">
      <a href="https://github.com/tototo31">
        <img src="https://github.com/tototo31.png" width="80" height="80" style="border-radius: 50%;" alt="Tototo31"/><br/>
        <b>Tototo31</b>
      </a><br/>
      <sub>Large contributions to the project</sub>
    </td>
    <td align="center">
      <a href="https://github.com/WillyJL">
        <img src="https://github.com/WillyJL.png" width="80" height="80" style="border-radius: 50%;" alt="WillyJL"/><br/>
        <b>WillyJL</b>
      </a><br/>
      <sub>Core Flipper Firmware functionality and BLE Spam code</sub>
    </td>
    <td align="center">
      <a href="https://github.com/flipperdevices/flipperzero-firmware">
        <img src="https://github.com/flipperdevices.png" width="80" height="80" style="border-radius: 50%;" alt="flipperdevices"/><br/>
        <b>Flipper Zero firmware</b>
      </a><br/>
      <sub>Core IR &amp; NFC implementation (flipperdevices/flipperzero-firmware &amp; contributors)</sub>
    </td>
  </tr>
  <tr>
    <td align="center">
      <a href="https://github.com/Garag">
        <img src="https://github.com/Garag.png" width="80" height="80" style="border-radius: 50%;" alt="Garag"/><br/>
        <b>Garag</b>
      </a><br/>
      <sub>Core NFC library</sub>
    </td>
    <td align="center">
      <a href="https://github.com/connornishijima">
        <img src="https://github.com/connornishijima.png" width="80" height="80" style="border-radius: 50%;" alt="connornishijima"/><br/>
        <b>connornishijima</b>
      </a><br/>
      <sub><a href="https://github.com/connornishijima/SensoryBridge">SensoryBridge</a> - MIC RGB visualizer algorithms &amp; inspiration</sub>
    </td>
    <td align="center">
      <a href="https://github.com/DarkFlippers">
        <img src="https://github.com/DarkFlippers.png" width="80" height="80" style="border-radius: 50%;" alt="DarkFlippers"/><br/>
        <b>DarkFlippers</b>
      </a><br/>
      <sub>Flipper Zero Unleashed firmware (SubGHz protocol decoders)</sub>
    </td>
    <td align="center">
      <a href="https://github.com/xMasterX">
        <img src="https://github.com/xMasterX.png" width="80" height="80" style="border-radius: 50%;" alt="xMasterX"/><br/>
        <b>xMasterX</b>
      </a><br/>
      <sub>Flipper Zero Unleashed SubGHz improvements</sub>
    </td>
  </tr>
</table>

> Portions of the IR, NFC, and SubGHz functionality are adapted from the open-source Flipper Zero firmware by flipperdevices, DarkFlippers, xMasterX and their community contributors.

---

## Disclaimers

Ghost ESP is intended solely for educational and ethical security research. Unauthorized or malicious use is illegal. Be sure to familiarize your local laws, and always obtain proper permissions before conducting any network tests.

> **Note:** this is a detached fork of [Spooky's GhostESP](https://github.com/Spooks4576/Ghost_ESP) which has been archived and not in development anymore.

For guidelines on using the GhostESP name and logo, please see [BRAND GUIDELINES](BRAND_GUIDELINES.md).

Interested in becoming an official partner? Email `partners@ghostesp.net`.

---

## Open Source Contributions

This project is open source and welcomes your contributions. If you've added new features or enhanced device support, please submit your changes!
