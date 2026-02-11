# Ghost ESP: Revival

> **Note:** this is a detached fork of [Spooky's GhostESP](https://github.com/Spooks4576/Ghost_ESP) which has been archived and not in development anymore.

**⭐️ Enjoying Ghost ESP? Please give the repo a star!**

Ghost ESP turns your ESP32 into a powerful, cheap and helpful wireless testing tool. Built on ESP-IDF.

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

- **Evil Portal** – Set up a fake WiFi portal with a custom SSID and domain.
- **Deauthentication Attacks** – Disconnect clients from specific networks (supports multiple APs).
- **Beacon Spam** – Broadcast customizable SSID beacons.
- **WiFi Capture** – Log probe requests, beacon frames, deauth packets, and raw data *(requires SD card or compatible storage)*.
- **Pineapple Detection** – Detect Wi-Fi Pineapples and Evil Twin Attacks.
- **SAE Flood Attack** – Target WPA3 networks specifically.
- **EAPOL Logoff Attack** – Force disconnect authenticated clients.
- **Web-UI** – Built-in interface for configuring settings, sending commands to another connected ESP, and managing the filesystem.
- **AP Scanning** – Detect nearby WiFi networks.
- **Station Scanning** – Monitor connected WiFi clients.
- **Combined AP/Station Scan** – Perform both AP and station scans in one command (`scanall`).
- **Beacon Spam List Management** – Manage SSID lists (`beaconadd`, `beaconremove`, `beaconclear`, `beaconshow`) and spam them (`beaconspamlist`).
- **Probe Request Listening** – Passive monitoring of device probe requests.
- **DHCP Starvation** – Flood DHCP requests to exhaust network leases (`dhcpstarve`).
- **Port Scanning** – Scan your local network for open ports.
- **ARP Scanning** – Scan for devices on local network using ARP (`scanarp`).
- **SSH Scanning** – Scan for SSH services on network (`scanssh`).
- **IP Lookup** – Retrieve local network IP information (`scanlocal`).
- **Ethernet Mode** – Wired networking with fingerprint scanning and OUI vendor lookup.
- **Wardriving Enhancements** – Unique AP counting, deduped WiGLE v1.6 exports, and a sweep scan that logs WiFi/BLE/GPS/802.15.4 to CSV.
- **RSSI Tracking** – Track signal strength for selected APs and stations.
- **Drone Detection/Spoofing** – Detect and spoof detected drones.

</details>

<details>
<summary><strong>BLE Features</strong></summary>

- **BLE Spam** – Spoof Apple, Microsoft, Samsung, and Google devices *(not supported on ESP32S2)*.
- **AirTag Spoofing** – Spoof the identity of a selected AirTag device (`spoofairtag`).
- **BLE Packet Capture** – Capture and analyze BLE traffic.
- **BLE Scanning** – Detect BLE devices, including specialized modes for AirTags, Flipper Zeros, and more.
- **Flipper Zero RSSI Tracking** – Detect and monitor the signal strength (RSSI) of Flipper Zero devices (`blescan -f`).
- **AirTag RSSI Updates** – Existing tags periodically refresh RSSI so proximity changes are visible.
- **GATT/Service Discovery** – Scan services/characteristics and track RSSI per device.
- **BLE Wardriving** – Map and track BLE devices in your vicinity.

</details>

<details>
<summary><strong>IR Features</strong></summary>

- **Easy Learn Mode** – Learn IR signals from your remote with auto naming *(supported on TEmbed C1101)*.
- **FlipperZero IR File Support** – Use FlipperZero formatted IR files stored on SD card *(supported on LilyGo S3TWatch, Cardputer and TEmbed C1101)*.
- **Universal Library IR Transmit** – Send pre-programmed universal remote signals.
- **IR Transmit** – Transmit IR signals from F0 files.
- **IR Receive and Decode** – Decode IR signals received by the device *(supported on TEmbed C1101)*.
- **Multiple IR Protocols** – Support for NEC, Kaseikyo, Pioneer, RCA, Samsung, SIRC, RC5, and RC6 protocols.
- **IR Rename, Delete, Add Remotes** – Rename, delete, and add remotes *(supported on TEmbed C1101)*.
- **IR CLI Tools** – Full IR command-line control.
- **IR Dazzler** – 38 kHz high-duty pulsing for IR dazzler use cases.

</details>

<details>
<summary><strong>NFC Features</strong></summary>

#### PN532 NFC Capability

- **NTAG Support (Type 2)**
  - Read NTAG213/215/216 with NDEF parsing.
  - Write NTAG213/215/216 from `.nfc` files.
  - Save to Flipper `.nfc` format.
- **MIFARE Classic Support (Mini/1K/4K)**
  - Flipper's 1000+ key dictionary attack.
  - Parse and display NDEF TLV data.
  - Save to Flipper `.nfc` format.
- **File Management**
  - 'Saved' menu to browse `.nfc` files and rename/delete them from the UI.
  - 'User Keys' view to list `/mnt/ghostesp/nfc/mfc_user_dict.nfc`.
- **Flipper Parser Compatibility** – Built-in Flipper Zero parser layer with dozens of transit/parking/access cards (Aime, CSC, WashCity, Metromoney, Bip, CharlieCard, Disney Infinity, HI!, HID PACS, H World, Kazan, Microel, MiZIP, Plantain, Saflok, Skylanders, SmartRider, Social Moscow, Troika, Two Cities, Umarsh, Zolotaya Korona, Zolotaya Korona Online).
- **MIFARE Desfire Detection** – Basic detection to flag Desfire cards.

#### Chameleon Ultra Support

- **CLI & UI Integration**
  - Connect/disconnect and status/battery commands.
- **Card Support**
  - NTAG and MIFARE Classic NDEF parsing.
  - Flipper `.nfc` exports via `chameleon savehf/savedump/saventag` and UI.
  - Dictionary attack capability.

</details>

<details>
<summary><strong>Additional Features</strong></summary>

- **GhostLink (Dual Comm)** – Split-view terminal on-device when linked to a peer device.
- **Setup Wizard** – First-boot guided setup for display builds.
- **DIAL & Chromecast V2 Support** – Interact with DIAL-capable devices (e.g., Roku, Chromecast).
- **Rave Mode** – Extra visualizer app for boards with displays.
- **GPS Integration** – Retrieve location info via the `gpsinfo` command *(on supported hardware)*.
- **Network Printer Output** – Print custom text to a LAN printer (`powerprinter`).
- **RGB LED Modes** – Customizable LED feedback (Stealth, Normal, Rainbow).
- **Timezone Configuration** – Change system timezone string (`timezone`).

</details>
<img width="600" height="800" alt="image" src="https://github.com/user-attachments/assets/9f1ee121-23a9-481a-94cd-aeb353e0e4b7" />



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
