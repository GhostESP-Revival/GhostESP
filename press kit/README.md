# Ghost ESP Press Kit

Use these snippets and angles when covering Ghost ESP: an actively maintained ESP-IDF fork that turns cheap ESP32 boards into a polished wireless security toolkit.

## What is Ghost ESP?

Ghost ESP delivers professional-grade Wi-Fi/BLE/NFC/IR/USB tooling on accessible ESP32 hardware. It now includes GhostLink dual-ESP control, split-channel wardriving helper mode, setup wizard, wired + web screen mirroring, Wireshark USB dongle mode, Ethernet scans/fingerprinting, and expanded NFC parser support.

**Elevator pitch**: “Ghost ESP brings pro wireless testing tools to $10-$20 ESP32 boards—fast UI, broad hardware support, and a thriving community.”

## Core Capabilities

**Wi-Fi Testing**
- Deauth/disassoc, beacon spam, probe logging
- Evil Portal with keylogging, WPA3 SAE flood, DHCP starvation
- AP/STA scans, ARP/SSH/port scans, OUI lookup, sweep capture (WiFi/BLE/GPS/802.15.4)
- Handshake/PMKID capture, Wireshark USB dongle mode, GhostLink split-channel helper
- GhostLink Ethernet scans + fingerprinting

**Bluetooth (BLE)**
- AirTag spoofing, vendor-specific spam (Apple/Microsoft/Samsung/Google)
- BLE packet capture, Flipper finder/RSSI tracking, GATT/service scans
- BLE wardriving with GPS logging, BLE skimmer detection, BLE Wireshark streaming

**USB**
- USB keyboard host mode (ESP32-S3 builds)
- Remote keyboard control via GhostLink
- BadUSB script runner with per-run identity options (VID/PID/manufacturer/product/layout/randomize)

**NFC**
- NTAG read/write (213/215/216) with NDEF parsing
- MIFARE Classic 1000+ key dictionary attacks + PM3 dict attack
- Flipper Zero `.nfc` file export/import, NFC parser layer for many transit/park/access cards
- Chameleon Ultra support and Desfire detection

**Infrared**
- Learn/“Easy Learn” IR, decode, transmit universals and Flipper IR files

**Bonus Features**
- GhostLink dual-ESP control with on-device split terminal
- Wired + web screen mirroring, GPS wardriving, DIAL/Chromecast control, network printer output
- Customizable LED modes

## Key Selling Points

- Runs on dozens of affordable ESP32 boards, from tiny wearables to full-screen dev kits.
- Manage everything from on-device menus, CLI or the Flipper Zero app.
- Backed by a large Discord community and frequent firmware updates.
- Designed to be a powerful educational tool that showcases real-world attack surfaces.

## High-Engagement Story Angles

- **"$10 vs $200 Hacking Gear"**: A comparison of similar features across consumer and pro devices.
- **"I Turned My ESP32 into a Wi-Fi Pineapple Detector"**: How Ghost ESP sniffs out rogue access points and Evil Twin setups.
- **"5GHz Wi-Fi Attacks with the ESP32-C5"**: Next-gen dual-band deauth, congestion monitoring, and Pineapple/Evil Twin detection alerts.
- **"ESP32 + Chameleon Ultra: The Ultimate NFC Toolkit?"**: How Ghost ESP can be used with a Chameleon Ultra to scan parse and save flipper zero compatible NFC tags.
- **"The Pocket-Sized Pentesting Tool You Need"**: How Ghost ESP can be used for researchers on the go.
- **"Setting Up a Keylogging Evil Portal in Minutes"**: How Ghost ESP can be used to set up a keylogging evil portal.
- **"Build a Universal Remote for $10"**: How Ghost ESP can be used to capture and edit and replay Flipper Zero ir files.

## Demo-Friendly Commands

- **`beaconspam -r`** — Flood the screen with fake SSIDs in real-time
- **`startportal default FreeWiFi`** — Launch an Evil Portal and capture credentials live
- **`karma start`** — Respond to device probes with fake networks automatically
- **`pineap`** — Detect rogue access points and Evil Twin setups
- **`blewardriving`** — GPS-logged BLE capture
- **`listenprobes`** — Show devices searching for networks 
- **`attack -d`** — Live deauthentication with real-time disconnections
- **`congestion`** — Display Wi-Fi channel congestion chart

## Reference Links
- GitHub repository (source, issues, changelog): https://github.com/GhostESP-Revival/GhostESP
- Online flasher: https://flasher.ghostesp.net
- Documentation: https://docs.ghostesp.net
- Community Discord: https://discord.gg/5cyNmUMgwh
- Merch Store: https://shop.ghostesp.net

## Contact & Attribution
- **Press/creator inquiries**: Use the Discord server channel, email us at partners@ghostesp.net or @ghostesprevival on instagram.
- Please credit "GhostESP: Revival" and link to https://ghostesp.net or https://github.com/GhostESP-Revival/GhostESP in descriptions or footers.

## Legal & Responsible Use Reminder
Ghost ESP is an educational tool. Cover responsible usage, local laws, and consent-based testing in your content. Encourage viewers to practice ethical security research and respect privacy.
