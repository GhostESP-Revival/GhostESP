# Ghost ESP: Revival

> **Note:** this is a detached fork of [Spooky's GhostESP](https://github.com/Spooks4576/Ghost_ESP) which has been archived and not in development anymore.

**⭐️ Enjoying Ghost ESP? Please give the repo a star!**

Ghost ESP turns your ESP32 into a powerful, cheap and helpful wireless testing tool. Built on ESP-IDF.

---

## Getting Started

1. Flash your device at <https://flasher.ghostesp.net>

1. Join our **NEW** community on [Discord](https://discord.gg/5cyNmUMgwh) for support and feedback.

1. Visit our [Official Website](https://ghostesp.net) to stay in touch!

---

## Key Features

<details>

<summary>WiFi Features</summary>

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

</details>

<details>

<summary>BLE Features</summary>

- **BLE Spam** – Spoof Apple, Microsoft, Samsung, and Google devices *(not supported on ESP32S2)*.

- **AirTag Spoofing** – Spoof the identity of a selected AirTag device (`spoofairtag`).

- **BLE Packet Capture** – Capture and analyze BLE traffic.

- **BLE Scanning** – Detect BLE devices, including specialized modes for AirTags, Flipper Zeros, and more.

- **Flipper Zero RSSI Tracking** – Detect and monitor the signal strength (RSSI) of Flipper Zero devices (`blescan -f`).

- **BLE Wardriving** – Map and track BLE devices in your vicinity.

</details>

<details>

<summary>IR Features</summary>

- **Easy Learn Mode** – Learn IR signals from your remote with auto naming *(supported on TEmbed C1101)*.

- **FlipperZero IR File Support** – Use FlipperZero formatted IR files stored on SD card *(supported on LilyGo S3TWatch, Cardputer and TEmbed C1101)*.

- **Universal Library IR Transmit** – Send pre-programmed universal remote signals.

- **Custom Signals IR Transmit** – Transmit custom IR signals from files.

- **IR Receive and Decode** – Decode IR signals received by the device *(supported on TEmbed C1101)*.

- **Multiple IR Protocols** – Support for NEC, Kaseikyo, Pioneer, RCA, Samsung, SIRC, RC5, and RC6 protocols.

- **IR Rename, Delete, Add Remotes** – Rename, delete, and add remotes *(supported on TEmbed C1101)*.


</details>

<details>

<summary>Additional Features</summary>

- **DIAL & Chromecast V2 Support** – Interact with DIAL-capable devices (e.g., Roku, Chromecast).

- **Flappy Ghost and Rave Modes** – Extra apps for boards with displays.

- **GPS Integration** – Retrieve location info via the `gpsinfo` command *(on supported hardware)*.

- **Network Printer Output** – Print custom text to a LAN printer (`powerprinter`).

- **RGB LED Modes** – Customizable LED feedback (Stealth, Normal, Rainbow).

- **Timezone Configuration** – Change system timezone string (`timezone`).

</details>

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

- FlipperHub Rocket

- FlipperHub Pocker Marauder

- RabbitLabs Phantom

- RabbitLabs Yapper Board (GPS NOT SUPPORTED AT THIS TIME)

- Waveshare 7″ Touch

- 'CYD2 USB'

- 'CYD2 USB 2.4″'

- 'CYD2 USB 2.4″ (C Variant)'

- 'CYD Micro USB'

- 'CYD Dual USB'

- 'S3 T-Watch'

- Marauder V4

- Marauder V6

- LilyGo TEmbed C1101 

- LilyGo T-Display S3 Touch

- LilyGo T-Deck

</details>

---

## Acknowledgments

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
      <sub>Flipper BLE Spam code</sub>
    </td>
    <td align="center">
      <!-- Empty cell for symmetry -->
    </td>
  </tr>
</table>

---

## Legal Disclaimer

Ghost ESP is intended solely for educational and ethical security research. Unauthorized or malicious use is illegal. Be sure to familiarize your local laws, and always obtain proper permissions before conducting any network tests.

---

## Open Source Contributions

This project is open source and welcomes your contributions. If you've added new features or enhanced device support, please submit your changes!
