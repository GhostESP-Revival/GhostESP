# GhostESP: Revival

> **Note:** this is a detached fork of [Spooky's GhostESP](https://github.com/Spooks4576/Ghost_ESP) which has been archived and not in development anymore.

**⭐️ Enjoying GhostESP? Please give the repo a star!**

GhostESP turns your ESP32 into a powerful, cheap and helpful wireless testing tool. Built on ESP-IDF.

---

## Getting Started

1. **Flash your device:** [flasher.ghostesp.net](https://flasher.ghostesp.net)

1. **Community & support:** [Discord](https://discord.gg/5cyNmUMgwh)

1. **Learn more:** [Documentation](https://docs.ghostesp.net) • [Official Website](https://ghostesp.net)

> **Making content about GhostESP?** Check out the [Press Kit](https://github.com/jaylikesbunda/Ghost_ESP/blob/Development-deki/presskit.zip) for resources.

---

## Key Features

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
- Rave mode (display builds)

</details>

## ESP32 Firmware Comparison

<img width="800" height="2400" alt="image" src="https://github.com/user-attachments/assets/005c96f0-4d0a-4f97-926c-99059dfb9e21" />




---

## Supported ESP32 Models

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
  
</details>

---

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
      <!-- Empty cell for symmetry -->
    </td>
    <td align="center">
      <!-- Empty cell for symmetry -->
    </td>
    <td align="center">
      <!-- Empty cell for symmetry -->
    </td>
  </tr>
</table>

> Portions of the IR and NFC functionality are adapted from the open-source Flipper Zero firmware by flipperdevices and its community contributors.

---

## Disclaimers

Ghost ESP is intended solely for educational and ethical security research. Unauthorized or malicious use is illegal. Be sure to familiarize your local laws, and always obtain proper permissions before conducting any network tests.

For guidelines on using the GhostESP name and logo, please see [BRAND GUIDELINES](BRAND_GUIDELINES.md).

Interested in becoming an official partner? Email `partners@ghostesp.net`.

---

## Open Source Contributions

This project is open source and welcomes your contributions. If you've added new features or enhanced device support, please submit your changes!
