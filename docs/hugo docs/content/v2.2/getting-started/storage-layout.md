---
title: "Storage Layout"
description: "Where GhostESP saves captures, reports, applications, and settings on the SD card"
weight: 91
---

GhostESP keeps all user-visible SD-card data below `/mnt/ghostesp/` in
firmware, which is the `ghostesp` folder at the root of the card in a card
reader. This layout is stable so results are easy to locate and scripts can
rely on it.

## Find Your Results

| What you saved | Folder on the card | File type |
| --- | --- | --- |
| Packet capture | `ghostesp/pcaps/` | Standard `.pcap` |
| Scan report | `ghostesp/scans/` | Text or CSV |
| Radio sweep | `ghostesp/sweeps/` | `sweep_N.csv` |
| Wardriving/GPS data | `ghostesp/gps/` | WiGLE-compatible CSV |
| Motion snapshot | `ghostesp/captures/` | `motion_N.jpg` |
| Ghostchi capture/history | `ghostesp/ghostchi/pcaps/`, `ghostesp/ghostchi/sessions/` | `.pcap`, `.log` |
| NFC, IR, or SubGHz save | `ghostesp/nfc/`, `ghostesp/infrared/` (`remotes/`, `universals/`), `ghostesp/subghz/` | `.nfc`, `.ir`, `.sub` |
| Settings backup | `ghostesp/settings_backup.json` | Versioned JSON |

The folders for NFC, IR, SubGHz, audio, and BadUSB are created only when the
installed build supports the matching feature.

## Files You Can Add

| Add this | Put it here |
| --- | --- |
| Evil Portal HTML and assets | `ghostesp/evil_portal/portals/` |
| Native SD apps | `ghostesp/apps/` |
| GhostScript packages | `ghostesp/scripts/` |
| Theme packages | `ghostesp/themes/` |
| SNMP community strings (one per line, `#` comments) | `ghostesp/snmp_communities.txt` |

## Mounting

Most boards keep the SD card mounted for the whole session. On boards where the
SD card shares an SPI bus with the display (like Wired Hatters Banshee),
GhostESP uses **Just-In-Time (JIT) mounting**: it suspends display SPI, mounts
the card, performs the read or write, unmounts the card, then resumes display
SPI. This prevents display freezes during SD access. Boards with dedicated SPI
buses leave the card mounted during the operation.

## Internal Folders

Do not rely on `app_cache/` or `downloads/` for backups; firmware can replace
their contents. `appdata/<app_id>/` and `scriptdata/` contain persistent data
owned by an app or script. Firmware also manages `debug/` (debug dumps),
`dns_sinkhole/` (sinkhole blocklist data), and `logs/` (diagnostics and
coredumps), and keeps the selected theme under `themes/active/`. The `games/`
and `comics/` folders hold content managed by the Games and Reader apps.
Ghostchi's `learn.bin` and `state.bin` are also firmware-private state rather
than portable exports.

## Privacy

`evil_portal/portal_creds_N.txt`, `evil_portal/portal_keystrokes_N.txt`, DNS
logs, packet captures, and radio/NFC saves can contain sensitive information.
The SD card is not encrypted. Review these files before sharing the card or
the device.

For the complete developer and compatibility contract, see
[`STORAGE_CONTRACT.md`](https://github.com/GhostESP-Revival/GhostESP/blob/main/STORAGE_CONTRACT.md).
