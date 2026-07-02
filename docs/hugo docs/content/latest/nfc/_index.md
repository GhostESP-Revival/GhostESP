---
title: "NFC"
description: "Information about GhostESP's NFC capabilities"
keywords: ["NFC", "RFID", "PN532", "ST25R3916", "ISO14443A", "tag reading", "tag writing", "emulation"]
weight: 310
aliases:
  - "/nfc/"
toc: false
---

GhostESP supports two NFC frontends, selectable at runtime:

- **PN532** - the default reader. Fast hardware MIFARE Classic authentication, great for dictionary brute-force.
- **ST25R3916/ST25R3916B** - software-driven reader/emulator over SPI or I2C. Adds card emulation (Type 2 and MIFARE Classic) and software Crypto1 for nested/hardnested nonce capture.

Both share the same high-level scan/cache/save/write flow, so most of this section applies regardless of which module is fitted. Use the NFC menu **Backend** row (or `nfc backend auto|pn532|st25r` in the CLI) to switch.

- **auto** tries PN532 first, then ST25R3916.
- **pn532** forces the PN532 path.
- **st25r** forces the ST25R3916 path. Emulation always forces ST25R3916 since the PN532 cannot emulate here.