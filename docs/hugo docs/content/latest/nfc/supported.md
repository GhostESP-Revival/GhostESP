---
title: "Supported Tags"
description: "Reference for NFC tag support in GhostESP"
weight: 40
---

## Overview

GhostESP's NFC stack focuses on ISO14443A tags. It runs on either a PN532 or an ST25R3916/ST25R3916B frontend (selectable at runtime). This page summarizes current support.

Much of GhostESP's tag decoding is ported from the [Flipper Zero](https://github.com/flipperdevices/flipperzero-firmware) / [Momentum](https://github.com/Next-Flip/Momentum-Firmware) NFC stacks, so behavior is close to a Flipper. The Flipper wiki is a good companion reference for what each card exposes — see the links throughout this page.

## NTAG / Ultralight

- **Models:** NTAG213 (45 pages), NTAG215 (135 pages), NTAG216 (231 pages), and Ultralight variants with similar memory layouts.


- **Save:** Exports to Flipper `.nfc` files including UID, header metadata, and all user pages.

- **Write:** Supported. GhostESP programs NTAG images back to blank tags, enforcing the original page count before starting.

- **Notes:** UID is read-only; the saved UID is used for filenames and verification only.

## MIFARE Classic

- **Models:** 1K, 4K, Mini (via SAK/ATQA detection).

- **Read:** Sector-by-sector brute-force with layered keys:
  - User dictionary file on the SD card (`/mnt/ghostesp/nfc/mfc_user_dict.nfc`), which you will have to edit manually.
  - Built-in common keys.
  - Flipper Zero dictionary pre-compiled in.

- **User Dictionary:** Successful keys are appended to `/mnt/ghostesp/nfc/mfc_user_dict.nfc` for future scans.

- **Save:** Unlocked sectors and recovered keys are stored in Flipper formatted `.nfc` files.

- **Card parsing:** Once a card is read, GhostESP runs the Flipper/Momentum supported-card parsers over the recovered sectors to decode known cards (balance, card number, history). See [Supported Cards](#supported-cards) below.

- **Write:** Not currently supported from GhostESP.

- **Notes:** UI shows progress as "Bruteforcing keys…" and "Reading sectors…" while blocks are cached. Classic tags with the well-known magic backdoor are detected; when enabled, GhostESP can read blocks without authenticating that sector first. Further reading: [Flipper wiki — MIFARE Classic](https://flipper.wiki/mifareclassic/).

- **Nested / hardnested (ST25R3916 only):** once at least one key is recovered, the software Crypto1 path captures encrypted nested nonces + parity and filters dictionaries locally, then RF-verifies the candidate. Hard-PRNG cards that resist local recovery are exported to `/mnt/ghostesp/nfc/.nested.log` in Momentum-compatible format for PC-side solving.

## MIFARE DESFire

- **Detection:** Reads PICC version and models capacity (DESFire 2K/4K/8K).

- **Read:** Walks the full application/file tree over ISO7816 APDUs — lists applications (`GetApplicationIDs`), enumerates files (`GetFileIDs`), reads each file's settings (`GetFileSettings`), and reads plaintext Standard/Backup/Record file data (`ReadData`/`ReadRecords`). Files that require authenticated or enciphered access are skipped.

- **Save:** Exports a complete Flipper-compatible `.nfc` file (PICC version, application IDs, key settings, file IDs, file settings, and file data). Reopening a saved DESFire tag reconstructs the tree and re-runs the card parsers, so a saved card shows the same details as a live scan.

- **Card parsing:** DESFire-backed transit cards (Opal, myki, ITSO, …) are decoded by the supported-card parsers. See [Supported Cards](#supported-cards).

- **Write:** Not supported.

- **Notes:** Adapted from the [Momentum](https://github.com/Next-Flip/Momentum-Firmware) MIFARE DESFire poller. To check whether a specific transit card exposes unlocked applications, the [Flipper wiki — Public transport](https://flipper.wiki/publictransport/) and MetroDroid are useful references.

## EMV Payment Cards

- **Read:** Contactless EMV cards (Visa, Mastercard, eftpos, etc.). GhostESP mirrors Momentum's `emv_poller`: SELECT PPSE → SELECT AID → GET PROCESSING OPTIONS (with a populated PDOL) → READ RECORD across the AFL → GET DATA. Extracts application label/AID, PAN, expiry/effective dates, cardholder name (when present), issuer country and currency (shown as ISO names, e.g. `Australia` / `AUD`), AIP, ATC, PIN-try counter, and the on-card transaction log where the card exposes it.

- **Save:** Exports a Flipper-format `EMV_<UID>.nfc` file; reopening it re-parses and shows the same details as a live scan.

- **Write:** Not supported (payment cards cannot be cloned or written).

- **Notes:** Read-only, and only reads data the card returns in the clear — GhostESP never derives a CVV/CVC, PIN, or any offline cryptogram, and cannot make a payment. Ported from the [Momentum](https://github.com/Next-Flip/Momentum-Firmware) EMV poller and the payment-card parser by [@leptopt1los](https://github.com/leptopt1los).

## Supported Cards

Beyond raw tag reading, GhostESP ports Flipper Zero / Momentum's `supported_cards` parsers. After the underlying tag is read — MIFARE Classic sectors unlocked, DESFire files read, or an EMV select — a matching parser decodes card-specific details such as balance, card number, and trip history. Parsing is best-effort and read-only; a card that isn't recognized still shows its raw tag data.

**Transit & stored-value**

- DESFire-based: Opal (Sydney), myki (Melbourne), ITSO (UK), Clipper (San Francisco).
- MIFARE Classic-based: SmartRider (Perth), CharlieCard (Boston), Bip! (Santiago), MetroMoney (Tbilisi), Troika, Plantain, Social Moscow, Two Cities, Umarsh, Kazan, Zolotaya Korona (Russia & CIS), microel (Italy), H World, WashCity, MiZIP, CSC, and Hi.

**Access control**

- Gallagher (Cardax), Saflok, HID iCLASS / Prox. Further reading: [Flipper wiki — HID iCLASS](https://flipper.wiki/hidiclass/).

**Amusement & toys**

- Sega/Bandai Amusement IC (`aime`), Skylanders, Disney Infinity. Further reading: [Flipper wiki — Arcade cards](https://flipper.wiki/arcadecards/).

**Payment**

- EMV contactless (see [EMV Payment Cards](#emv-payment-cards) above).

## PicoPass / HID iCLASS (ST25R3916 only)

- **Read:** Supported on the ST25R3916 backend, which reuses the ISO15693 PHY for the PicoPass/iCLASS air interface. Readable blocks are decoded and saved to `.picopass` files. Support is based on `bettse/picopass`, carried via Momentum, and includes loclass ([Proxmark3](https://github.com/RfidResearchGroup/proxmark3)) work.

- **Notes:** PN532 cannot read PicoPass. Further reading: [Flipper wiki — HID iCLASS](https://flipper.wiki/hidiclass/).

## Other ISO14443A

- **Detection:** GhostESP reports UID, ATQA, SAK, and basic type info for ISO14443A tags that don't fit the above categories.

- **Read/Write:** Not implemented beyond basic presence detection.

- **Notes:** Future firmware updates may expand support; check release notes for changes.

## Unsupported Families

- **MIFARE Plus:** Not supported.
- **ISO14443B, FeliCa:** Not supported by the current integration.
- **ISO15693:** Generic ISO15693 tags are not supported; the PHY is used only for PicoPass/iCLASS (above).

## Emulation (ST25R3916 only)

- **Type 2 / NTAG:** memory image emulation supported (SPI transport works reliably; I2C transport may be timing-limited with strict readers like the Flipper Zero).
- **MIFARE Classic:** sector image emulation supported via software Crypto1.
- **PN532:** cannot emulate in this build; emulation always routes to the ST25R3916 backend regardless of the selected backend.
