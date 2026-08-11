
<img width="800" alt="ghostesp_white_text_logo2" src="https://github.com/user-attachments/assets/f2cb3bb4-ab79-4679-8db1-beddc306ba07" />

> **The open-source wireless research platform for ESP32.**
> Built on ESP-IDF v6.0. **v2.0** turns GhostESP into a full graphical, extensible, multi-radio environment.

[![Version](https://img.shields.io/badge/version-2.0-7c5cff?style=flat-square)](https://github.com/spookyorigin/Ghost_ESP)
[![License](https://img.shields.io/badge/license-GPL--3.0-blue?style=flat-square)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0-orange?style=flat-square)](https://docs.espressif.com/projects/esp-idf/)
[![Discord](https://img.shields.io/discord/5cyNmUMgwh?style=flat-square&label=Discord&color=5865F2)](https://discord.gg/5cyNmUMgwh)
[![Boards](https://img.shields.io/badge/board%20targets-46-2ea043?style=flat-square)](#supported-boards)

**⭐️ Enjoying GhostESP? Please give the repo a star. It helps a lot.**

---

## What's New in 2.0

The biggest update yet: a rebuilt UI, a native app ecosystem, and expanded radio workflows.

<img width="320" height="170" alt="app-gallery2" src="https://github.com/user-attachments/assets/f7bb96ed-db0c-4777-a721-ded2d397b167" /> <img width="320" height="170" alt="airspace-monitor" src="https://github.com/user-attachments/assets/e049dfc8-3888-42ec-9fd1-6be62fcec114" />

- **Redesigned UI**: 60 FPS rendering, toasts, SD-loaded asset packs (icons/themes/backgrounds), touch-drag scrolling, and a full accessibility suite.
- **Native SD Apps**: Load, build, and package apps with the new App Gallery, scoped permissions, and the Ghost Build Tool (`gbt`). C5 builds can run app code from flash (XIP).
- **WiFi Airspace Monitor**: Real-time threat insights with adaptive channel dwell, a learned baseline, and a packets/sec sparkline.
- **Expanded Ghostchi**: 50 levels, 27 XP sources, mood system, and a status-bar badge.
- **PIN lockscreen**: Auto-lock overlay — captures keep running while locked.
- **Expanded BadUSB**: Trackpad/mouse jiggler, USB HID output mode, `type_char` CLI, dedicated WebUI page.
- **Redesigned WebUI**: Refreshed interface with better remote control.
- **New network recon**: SSH, NetBIOS, HTTP banner, and SNMP scanners, plus a WPA3 compliance checker.
- **GhostLink BLE bridge**: Bridge a chip to the Android companion app, with `wdstream` wardriving.
- **More boards**: Marauder V8, Pancake C5, LilyGo T-Dongle-S3/C5, S3TWatch haptics, Cardputer ENV-III, and NM-CYD-C5 fixes.
- **Under the hood**: Modularized commandline, shared SD mount helpers, checked allocations, and stability fixes across WiFi/BLE/audio/GPS/NFC.

Full history in [`CHANGELOG.md`](CHANGELOG.md).

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
- **Broad hardware and radio coverage**: Wi-Fi, BLE, NFC, IR, SubGHz, NRF24, Ethernet, GPS, USB HID, and 802.15.4/Zigbee across 46 board targets.
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

> **Note:** Feature availability varies by chip. S2 lacks Bluetooth hardware; C5 has 5 GHz and 802.15.4/Zigbee support.

---

## Supported Boards

46 board targets build in CI ([`.github/workflows/compile_all.yml`](.github/workflows/compile_all.yml)) from 45 configs in [`configs/`](configs/); Awok V5 shares the generic ESP32-S2 config. Feature support below is derived from those configs.

<details>
<summary><strong>Board feature matrix (click to expand)</strong></summary>

| Board | Bluetooth | NFC (PN532) | NFC (Chameleon) | IR TX | IR RX | GPS Default | Keyboard | Display | SD | OTA | Native SD Apps |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| ESP32-Wroom DevKitC | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| ESP32-S2 DevKitC | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| ESP32-S3 DevKitC | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| ESP32-C3 DevKitC | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| ESP32-C5 DevKitC | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| ESP32-C6 DevKitC | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| Awok V5 | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| GhostBoard | ✓ | ✗ | ✓ | ✗ | ✗ | ✓ | ✗ | — | ✓ | ✗ | ✗ |
| Marauder v4 | ✓ | ✗ | ✓ | ✗ | ✗ | ✓ | ✗ | Full | ✓ | ✓ | ✗ |
| Marauder v6 | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✗ | ✗ | ✗ |
| AWOK Mini | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ | ✗ | Full | ✗ | ✗ | ✓ |
| Cardputer | ✓ | ✗ | ✓ | ✓ | ✗ | ✓ | ✓ | Full | ✓ | ✓ | ✗ |
| Heltec V3 | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Status | ✓ | ✗ | ✗ |
| CYD2 USB | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✗ | ✗ |
| CYD2 Micro USB | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✗ | ✗ |
| CYD2 Dual USB | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✗ | ✗ |
| CYD2 USB 2.4" | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✗ | ✗ |
| CYD2 USB 2.4" (C variant) | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✗ | ✗ |
| CYD 2432S028R | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✗ | ✗ |
| Waveshare 7" Touch | ✓ | ✗ | ✓* | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✓ | ✓ |
| Crowtech 7" | ✓ | ✗ | ✓* | ✗ | ✗ | ✗ | ✗ | Full | ✗ | ✗ | ✓ |
| Sunton 7" | ✓ | ✗ | ✓* | ✗ | ✗ | ✗ | ✗ | Full | ✗ | ✓ | ✓ |
| JC3248W535EN | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✓ | ✓ |
| Flipper JCMK GPS | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ | ✗ | — | ✓ | ✗ | ✗ |
| T-Deck | ✓ | ✗ | ✓ | ✗ | ✗ | ✓ | ✓ | Full | ✓ | ✗ | ✓ |
| T-Embed CC1101 | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | Full | ✓ | ✓ | ✓ |
| T-Dongle-S3 | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✓ | ✗ |
| T-Dongle-C5 | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✓ | ✓ |
| S3TWatch | ✓ | ✗ | ✓ | ✓ | ✗ | ✗ | ✗ | Full | ✗ | ✓ | ✗ |
| T-Display S3 Touch | ✓ | ✗ | ✓ | ✗ | ✗ | ✓ | ✗ | Full | ✓ | ✗ | ✗ |
| JCMK Devboard Pro | ✓ | ✗ | ✓ | ✗ | ✗ | ✓ | ✗ | — | ✓ | ✗ | ✗ |
| Minion | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| Lolin S3 Pro | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| Cardputer ADV | ✓ | ✗ | ✓ | ✓ | ✗ | ✓ | ✓ | Full | ✓ | ✓ | ✗ |
| Poltergeist | ✓ | ✗ | ✓ | ✓ | ✓ | ✗ | ✗ | Status | ✓ | ✗ | ✗ |
| Banshee (C5 display MCU) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | Full + Status | ✓ | ✓ | ✓ |
| Banshee (S3 main) | ✓ | ✗ | ✓ | ✗ | ✗ | ✓ | ✗ | — | ✗ | ✓ | ✗ |
| Febris Pro | ✓ | ✗ | ✓ | ✗ | ✗ | ✓ | ✗ | — | ✗ | ✗ | ✗ |
| ACE C5 | ✓ | ✗ | ✓ | ✗ | ✗ | ✓ | ✗ | — | ✓ | ✗ | ✗ |
| NM-CYD-C5 | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✓ | ✓ |
| ACE S3 | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| Seeed XIAO ESP32-S3 Sense | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✓ | ✗ |
| Seeed XIAO ESP32-S3 | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✓ | ✗ |
| Seeed XIAO ESP32-C5 | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| Marauder v8 | ✓ | ✗ | ✗ | ✗ | ✗ | ✓ | ✗ | Full | ✓ | ✗ | ✓ |
| Pancake C5 | ✓ | ✗ | ✗ | ✗ | ✗ | ✓ | ✗ | Full | ✓ | ✗ | ✓ |

`*` — the checked-in config for this board predates a Kconfig option (`NFC_CHAMELEON`) that defaults on for BLE-capable boards; no board-specific override is present, so this reflects the Kconfig default rather than an explicit setting in the file. Most unstarred BLE-capable boards set the symbol explicitly, but some generic configs may also rely on the Kconfig default.

**Display:** `Full` = LVGL graphical UI. `Status` = secondary small status display only (shares the IO-expander I2C bus), no full UI. `—` = headless, no display.

**SD:** most boards use SPI-mode SD. JC3248W535EN and T-Dongle-S3 use SDMMC 1-bit mode instead; the SDMMC bus option exists in Kconfig for other boards.

**NFC (Chameleon):** Chameleon Ultra support rides over BLE, so it's on by default for any BLE-capable board and off where BLE is unavailable (ESP32-S2 boards) or explicitly disabled (Marauder v8, Pancake C5).

**Native SD Apps:** at compile time the feature depends only on `CONFIG_SPIRAM` (`main/Kconfig.projbuild:1410`). At runtime the app gallery checks `MALLOC_CAP_SPIRAM` and renders into the full LVGL screen, so a display is required for the UI to be usable. That leaves it enabled on: AWOK Mini, Waveshare/Crowtech/Sunton 7″, JC3248W535EN, T-Deck, T-Embed CC1101, T-Dongle-C5, NM-CYD-C5, Banshee (C5), and Marauder v8/Pancake C5. Boards with a screen but no PSRAM (Cardputer, Cardputer ADV, the CYD2 family, S3TWatch, T-Dongle-S3, etc.) don't get it.

**Banshee** ships as two configs: the S3 main board (headless) and the C5 module that drives its display and status LED, paired over GhostLink.

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
| Supported board targets | 46 CI targets | 42+ | 4 | 1 |
| Full LVGL graphical UI | [x] |  |  |  |
| Web dashboard / REST control | [x] | [x] |  |  |
| Captive portal web server | [x] | [x] | [x] | [x] |
| AP / station WiFi scanning | [x] | [x] | [x] | [x] |
| Deauth / disassoc testing | [x] | [x] | [x] | [x] |
| Beacon spam | [x] | [x] | [x] | [x] |
| Karma / probe response attack | [x] | [x] | [x] |  |
| Handshake / EAPOL capture | [x] | [x] | [x] |  |
| On-device PCAP browser / hc22000 export | [x] |  |  |  |
| PMKID capture / export | [x] |  | [x] |  |
| Live Wireshark USB streaming | [x] |  |  |  |
| SAE flood / WPA3-specific testing | [x] |  |  |  |
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
| Channel congestion analysis | [x] |  |  | [x] |
| Live WiFi packet monitor / visualizer | [x] |  | [x] | [x] |
| WiFi Airspace Monitor | [x] |  |  |  |
| DNS sinkhole / blocklist NXDOMAIN | [x] |  |  |  |
| GPS WiFi wardriving | [x] | [x] | [x] |  |
| BLE wardriving | [x] | [x] | [x] |  |
| WiGLE upload integration | [x] | [x] |  |  |
| 802.15.4 capture and PCAP export | [x] |  |  |  |
| GhostLink dual-ESP control | [x] |  |  |  |
| GhostLink BLE bridge to Android | [x] |  |  |  |
| Split-channel wardriving helper | [x] |  |  |  |
| GhostLink remote radio support | [x] |  |  |  |
| Drone / OpenDroneID detect | [x] |  |  | [x] |
| Drone / OpenDroneID spoof | [x] |  |  | [x] |
| BLE scanning | [x] | [x] | [x] | [x] |
| Raw BLE scanner | [x] |  |  |  |
| BLE spam modes | [x] | [x] | [x] | [x] |
| AirTag scan / spoof | [x] | [x] | [x] | [x] |
| BLE tracker detection tools | [x] |  | [x] | [x] |
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
| ST25R3916 NFC support | [x] | [x] |  |  |
| Chameleon Ultra support | [x] | [x] |  |  |
| Chameleon Ultra BLE control | [x] | [x] |  |  |
| Flipper `.nfc` import/export | [x] |  |  |  |
| Flipper NFC parser collection | [x] |  |  |  |
| MIFARE Classic default-key attack | [x] | [x] | [x] |  |
| MIFARE Classic embedded dictionary | [x] |  |  |  |
| MIFARE Classic user dictionary file | [x] | [x] |  |  |
| MIFARE Classic session key reuse / sector sweep | [x] |  |  |  |
| MIFARE Classic hardnested recovery | [x] |  |  |  |
| PicoPass / iCLASS reading | [x] |  |  |  |
| MIFARE DESFire application / file tree reads | [x] |  |  |  |
| EMV / payment card reader | [x] | [x] |  |  |
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
| Flipper `.sub` read/write support | [x] | [x] | [x] | [x] |
| SubGHz protocol decoders | [x] | [x] | [x] |  |
| NRF24 spectrum analyzer | [x] | [x] | [x] | [x] |
| NRF24 MouseJack |  | [x] | [x] |  |
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
| PIN / password lock | [x] |  | [x] | [x] |
| On-device OTA / SD firmware update | [x] |  | [x] |  |
| Firmware verification / rollback protection | [x] |  |  |  |
| GhostLink peer firmware update | [x] |  |  |  |
| Wired screen mirroring | [x] |  |  | [x] |
| Web screen mirroring | [x] | [x] |  |  |
| SD config backup / restore | [x] |  |  |  |
| SD file manager / browser | [x] | [x] | [x] |  |
| Native SD app/plugin system | [x] |  |  |  |
| Native app SDK / build tooling | [x] |  |  |  |
| Sandboxed on-device scripting runtime | Lua 5.4 | JavaScript |  |  |
| Cloud app / script / asset store | [x] |  |  |  |
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
    <td align="center">
      <a href="https://github.com/DecentLabs">
        <img src="https://github.com/DecentLabs.png" width="80" height="80" style="border-radius: 50%;" alt="DecentLabs"/><br/>
        <b>DecentLabs</b>
      </a><br/>
      <sub><a href="https://github.com/DecentLabs/officeAir">officeAir</a> — multi-pass ARP scanning &amp; lwIP thread-safety techniques</sub>
    </td>
  </tr>
</table>

> Portions of the IR, NFC, and SubGHz functionality are adapted from the open-source Flipper Zero firmware by flipperdevices, DarkFlippers, xMasterX and their community contributors.

---

## Disclaimers

Ghost ESP is intended solely for educational and ethical security research. Unauthorized or malicious use is illegal. Be sure to familiarize your local laws, and always obtain proper permissions before conducting any network tests.

> **Note:** this is a detached fork of [Spooky's GhostESP](https://github.com/Spooks4576/Ghost_ESP) which has been archived and not in development anymore.

For guidelines on using the GhostESP name and logo, please see the [Brand Guidelines](https://ghostesp.net/brand-guidelines). Brand assets are available at [ghostesp.net/brand-assets](https://ghostesp.net/brand-assets).

Interested in becoming an official partner? Email `partners@ghostesp.net`.

---

## Open Source Contributions

This project is open source and welcomes your contributions. If you've added new features or enhanced device support, please submit your changes!
