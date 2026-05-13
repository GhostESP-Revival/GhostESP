
<img width="800" alt="ghostesp_white_text_logo2" src="https://github.com/user-attachments/assets/f2cb3bb4-ab79-4679-8db1-beddc306ba07" />

GhostESP turns your ESP32 into a powerful, cheap and helpful wireless testing tool. Built on ESP-IDF.

**⭐️ Enjoying GhostESP? Please give the repo a star!**

---

## Get Started

1. **Flash your device:** [ghostesp.net/flasher](https://ghostesp.net/flasher)

1. **Community & support:** [Discord](https://discord.gg/5cyNmUMgwh)

1. **Learn more:** [Documentation](https://docs.ghostesp.net) • [Official Website](https://ghostesp.net)

> **Making content about GhostESP?** Check out the [Press Kit](https://github.com/jaylikesbunda/Ghost_ESP/blob/Development-deki/presskit.zip) for resources.

---

## Features

<details>
<summary><strong>WiFi Features</strong></summary>

- Evil Portal
- Deauth / disassoc attacks
- Karma
- Beacon spam (single/list/random)
- AP scan / STA scan / scanall
- Probe request listening
- Handshake + PMKID capture
- WiFi capture to SD (PCAP)
- USB dongle mode for Wireshark (extcap stream)
- DHCP starvation
- ARP / port / SSH / local IP scanners
- WiFi OUI vendor lookup
- WPA3/SAE attacks
- EAPOL logoff attack
- Wardriving exports (WiFi/BLE/GPS) + sweep CSV (WiFi/BLE/GPS/802.15.4)
- Split-channel wardriving helper via GhostLink
- RSSI tracking (AP/station)
- Drone detection / spoofing
- Web UI + filesystem + remote command relay

</details>

<details>
<summary><strong>BLE Features</strong></summary>

- BLE scan modes (general, AirTag, Flipper)
- BLE spam modes
- AirTag scan / spoof
- BLE packet capture
- BLE stream to Wireshark
- Flipper finder + RSSI
- GATT/service scan + per-device RSSI
- BLE wardriving
- BLE skimmer detection

</details>

<details>
<summary><strong>USB Features</strong></summary>

- USB keyboard host mode (ESP32-S3 builds)
- Remote keyboard control over GhostLink
- BadUSB script runner
- BadUSB identity options (VID/PID/manufacturer/product/layout/randomize)

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
- MIFARE Classic dictionary attack
- Flipper NFC parser set (transit/parking/access)
- MIFARE Desfire detection
- Chameleon Ultra support (CLI + UI)

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

</details>

<details>
<summary><strong>Additional Features</strong></summary>

- GhostLink (dual-device command and display interface)
- Setup wizard (display builds)
- Wired + web screen mirroring
- Ethernet mode + fingerprint scan
- DIAL / Chromecast V2 support
- GPS integration (`gpsinfo`)
- Network printer output (`powerprinter`)
- RGB LED modes
- Timezone configuration (`timezone`)
- Camera motion detection with SD card snapshot capture and Discord webhook alerts (XIAO S3 Sense)
- Rave mode (display builds)

</details>


---

## Supported ESP32 Variants

- **ESP32 Wroom**

- **ESP32 S2**

- **ESP32 C3**

- **ESP32 S3**

- **ESP32 C5**

- **ESP32 C6**

> **Note:** Feature availability may vary by model.

---

## Supported Boards

<details>

<summary>Supported Boards</summary>

- DevKitC-ESP32

- DevKitC-ESP32-S2 (lacks bluetooth hardware)

- DevKitC-ESP32-C3

- DevKitC-ESP32-S3

- DevKitC-ESP32-C5

- DevKitC-ESP32-C6

- RabbitLabs GhostBoard

- AWOK Mini

- M5 Cardputer

- M5 Cardputer ADV

- FlipperHub Rocket

- FlipperHub Pocker Marauder

- RabbitLabs Phantom

- RabbitLabs Yapper Board 

- RabbitLabs Poltergeist

- CYD2432S028R

- Waveshare 7″ Touch

- 'CYD2 USB'

- 'CYD2 USB 2.4″'

- LilyGo T-Display S3 Touch

- LilyGo T-Deck

- JCMK Devboard Pro

- Flipper JCMK GPS

- CrowTech 7″

- JC3248W535EN

- Heltec V3

- Lolin S3 Pro

- Minion

- Sunton 7″

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
| Supported board targets | 40+ | 28+ | 5 | 1 |
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
| EAPOL logoff attack | [x] |  |  |  |
| Channel switch attack | [x] |  |  |  |
| GTK abuse / client isolation testing | [x] |  |  |  |
| DHCP starvation | [x] | [x] |  |  |
| ARP / port / SSH scanners | [x] | [x] |  |  |
| WiFi OUI vendor lookup | [x] | [x] | [x] |  |
| PineAP detection | [x] |  |  | [x] |
| WPS detection / reporting | [x] | [x] |  |  |
| Pwnagotchi-style automated capture mode | [x] | [x] |  |  |
| Pwnagotchi detector / spam |  | [x] |  | [x] |
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
| BLE spam modes | [x] | [x] | [x] | [x] |
| AirTag scan / spoof | [x] | [x] | [x] | [x] |
| Flipper Zero finder | [x] |  |  | [x] |
| GATT / service enumeration | [x] |  | [x] |  |
| BLE stream to Wireshark | [x] |  |  |  |
| BLE skimmer detection | [x] |  |  | [x] |
| FastPair / pairing exploit research |  | [x] | [x] | [x] |
| BLE HID injection / DuckyScript over BLE |  | [x] |  |  |
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
| BadUSB VID/PID identity options | [x] | [x] |  |  |
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
| TLS SNI / HTTP / FTP credential capture over Ethernet | [x] |  |  |  |
| Camera streaming / motion detection | [x] |  |  |  |
| Motion alerts with webhook support | [x] |  |  |  |
| Network printer / PJL output | [x] |  |  |  |
| DIAL / Chromecast testing | [x] |  |  |  |
| On-device setup wizard | [x] |  |  |  |
| Wired screen mirroring | [x] |  |  | [x] |
| Web screen mirroring |  | [x] |  |  |
| SD config backup / restore | [x] |  |  |  |
| Battery monitoring / fuel gauge support | [x] | [x] | [x] |  |
| Sensor / RTC hardware support | [x] | [x] |  |  |
| M5 Cardputer keyboard support | [x] | [x] |  |  |
| Android companion app | [x] |  |  |  |
| JavaScript app engine |  | [x] |  |  |
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
