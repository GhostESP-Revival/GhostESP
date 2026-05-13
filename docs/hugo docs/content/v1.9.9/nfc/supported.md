---
title: "Supported Tags"
description: "Reference for NFC tag support in GhostESP"
weight: 40
---

## Overview

GhostESP's PN532 stack focuses on ISO14443A tags. This page summarizes current support.

## NTAG / Ultralight

- **Models:** NTAG213 (45 pages), NTAG215 (135 pages), NTAG216 (231 pages), and Ultralight variants with similar memory layouts.


- **Save:** Exports to Flipper `.nfc` files including UID, header metadata, and all user pages.

- **Write:** Supported. GhostESP programs NTAG images back to blank tags, enforcing the original page count before starting.

- **Notes:** UID is read-only; the saved UID is used for filenames and verification only.

## MIFARE Classic

- **Models:** 1K, 4K, Mini (via SAK/ATQA detection).

- **Read:** Sector-by-sector brute-force with layered keys:
  - User dictionary file on the SD card (`/mnt/ghostesp/nfc/mfc_user_keys.nfc`), which you will have to edit manually.
  - Built-in common keys.
  - Flipper Zero dictionary pre-compiled in.

- **User Dictionary:** Successful keys are appended to `/mnt/ghostesp/nfc/mfc_user_keys.nfc` for future scans.

- **Save:** Unlocked sectors and recovered keys are stored in Flipper formatted `.nfc` files.

- **Write:** Not currently supported from GhostESP.

- **Notes:** UI shows progress as “Bruteforcing keys…” and “Reading sectors…” while blocks are cached. Classic tags with the well-known magic backdoor are detected; when enabled, GhostESP can read blocks without authenticating that sector first.

## Other ISO14443A

- **Detection:** GhostESP reports UID, ATQA, SAK, and basic type info for ISO14443A tags that don’t fit the above categories.

- **Read/Write:** Not implemented beyond basic presence detection.

- **Notes:** Future firmware updates may expand support; check release notes for changes.

## Unsupported Families

- **MIFARE DESFire / Plus:** Not supported.
- **ISO14443B, ISO15693, FeliCa:** Not supported by the current PN532 integration.
- **Emulation / Peer-to-Peer:** Not supported.
