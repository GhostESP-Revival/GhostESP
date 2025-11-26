# Chameleon Ultra Integration Guide

The Ghost ESP includes support for the Chameleon Ultra, a 13.56MHz NFC/RFID research tool. This integration lets you control a Chameleon Ultra over Bluetooth and collect scan results and dump files to the local storage on the Ghost ESP.

## Quick start

### Prerequisites
- Chameleon Ultra device with Bluetooth enabled
- SD card or internal virtual storage present and mounted by Ghost ESP
- Both devices powered on and within wireless range

### Basic connection
```bash
chameleon connect      # scan and connect to Chameleon Ultra
chameleon status       # verify connection and device information
chameleon battery      # check Chameleon battery level
```

## Core operations

### Device management
```bash
chameleon connect         # connect to Chameleon Ultra
chameleon disconnect      # disconnect
chameleon status          # show connection and device info
chameleon battery         # display battery level
chameleon firmware        # show firmware/firmware version
chameleon devicemode      # display current mode (reader/emulator)
```

### Mode switching
```bash
chameleon reader          # switch device to reader mode
chameleon emulator        # switch device to emulator mode
```

## HF (13.56 MHz) operations

### Scanning and saving
```bash
chameleon scanhf          # scan for HF tags (MIFARE, NTAG, ISO14443)
chameleon savehf          # save most recent HF scan (auto filename)
chameleon savehf mycard   # save with custom filename
```

Supported HF card types include MIFARE Classic (1K/4K), MIFARE Ultralight, NTAG213/215/216 and other ISO14443 Type A/B tags.

### MIFARE Classic analysis
```bash
chameleon readhf          # automated analysis: default key testing and data collection
chameleon savedump        # save detailed analysis results
```

The `readhf` command performs basic card detection, attempts default-key authentication where applicable, and collects card metadata and (when available) a sector/block dump for analysis.

## NTAG operations

```bash
chameleon ntagdetect      # detect NTAG family/version
chameleon ntagdump        # read accessible memory and gather metadata
chameleon saventalag      # save NTAG analysis results
```

The integration attempts GET_VERSION and memory checks to identify NTAG types and whether the tag is password-protected.

## LF (125 kHz) operations

```bash
chameleon scanlf          # scan EM410x tags
chameleon scanhidprox     # scan HID Prox
chameleon savelf          # save LF scan results
chameleon readlf          # read LF data (when supported)
```

Supported LF formats include EM410x and common proprietary 125 kHz formats where applicable.

## File management and formats

Files created by Chameleon-related commands are saved under the Ghost ESP storage at the following location:

- Primary save directory: `/mnt/ghostesp/nfc/`


Filename and format details:
- HF scans and simple reports: text files with a `CU_hf_scan_<YYYYMMDD>_<HHMMSS>.nfc` or `.txt` name, containing a short human-readable summary (timestamp, UID, tag type, ATQA/SAK).
- Card dumps and Flipper-compatible files: when possible the tool emits Flipper NFC file headers (``Filetype: Flipper NFC device``) and then appends tag-specific content (UID, ATQA/SAK, pages/blocks) so these files can be opened in other NFC tools that support Flipper formats.
- MIFARE Classic full dumps: saved as sector/block listings and, where available, include recovered keys in the dump (if permitted by the analysis results).

Notes on storage usage
- On devices where a virtual internal storage partition is used (some watch builds), files are saved to the same `/mnt` mount point.
- The SD and display hardware may share SPI lines on some boards. When that is the case, the firmware will mount the SD just-in-time and suspend display SPI activity during SD I/O to avoid bus contention. If you see SD-related errors, check whether the display and SD pins are shared for your board.

## Troubleshooting

### Common issues

"Not connected to Chameleon Ultra"
- Ensure the Chameleon is powered on and in Bluetooth range.
- Run `chameleon disconnect` then `chameleon connect` to re-establish.

"Insufficient memory"
- Reboot the Ghost ESP (`reboot`) and close other tasks.
- The firmware prefers >20 KB free heap for basic operations and recommends 40 KB+ for complex scans.

"SD read/write failures or UI freezes"
- If the board uses an SPI display that shares MOSI/MISO/SCLK with the SD card, the firmware suspends display SPI during SD operations. If the UI appears to freeze after a save operation, the JIT mount/unmount path may not have completed; reboot or check logs for `sdmmc_read_sectors` errors.
- Avoid removing the SD card while writes are in progress.

"Authentication required (Status 0x60)"
- The tag requires authentication (normal for protected NTAGs). Use the NTAG analysis commands for more detail.

## Status codes returned by Chameleon
- `0x00` — success (tag found)
- `0x68` — command success / OK
- `0x60` — authentication required
- `0x66` — wrong device mode
- `0x41` — no LF tag found

## Best practices
- Connect and set reader mode before scanning: `chameleon connect` → `chameleon reader` → `chameleon scanhf`.
- Save scan results promptly; files are timestamped and include UID where available to make organization easier.
- When using shared SPI boards, prefer saving when the UI is idle to reduce I/O contention.

## References
- Chameleon Ultra project: `https://github.com/RfidResearchGroup/ChameleonUltra`
- NTAG documentation: NXP AN11495 and related datasheets