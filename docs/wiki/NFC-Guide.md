# NFC Guide

- **Requires hardware**: NFC functions need a PN532-compatible NFC module on the device and NFC support enabled in the build. If your build was compiled without NFC, the features will be disabled.

## Quick overview

- **Scan**: Start an NFC scan to detect a nearby tag. The UI will show UID and type, then attempt to read full details.
- **More / Details**: After a tag is detected you can open a details view that shows NDEF records or a MIFARE Classic summary.
- **Save**: Save the scanned tag to `/mnt/ghostesp/nfc/` on the SD card as a `.nfc` file (Flipper-compatible format).
- **Write**: Choose an existing `.nfc` file from the SD card and write it to a presented tag.
- **Saved**: Browse previously saved `.nfc` files, view their contents, rename or delete them.
- **User Keys**: View user-provided MIFARE Classic keys stored on the SD card (file: `mfc_user_dict.nfc`).

## Supported / Not supported

- **Supported (typical)**:
  - NTAG / Ultralight (Type 2) tags: read NDEF, dump pages, save and write images.
  - MIFARE Classic tags: read when possible, build compact summaries, save Flipper-style files, and attempt key discovery using dictionaries and user keys.
  - Saving `.nfc` files to SD card and browsing them in `Saved`.
  - Writing `.nfc` files back to tags (when file and tag types match and NFC hardware is present).

- **Not supported / limited**:
  - NFC features on builds compiled without `CONFIG_HAS_NFC` are disabled. 
  - Tags that require proprietary authentication beyond MIFARE Classic keys or non-standard tag families (e.g., some specialized tag types) may not be readable or writable.
  - Some boards may lack SD card support — saving requires a mounted SD card path (`/mnt/ghostesp`).
  - Behavior varies if the PN532 shares the I2C bus with other hardware; initialization may fail on some boards.

## How to scan a tag

1. Open the NFC view from the main menu and press `Scan`.
2. Present the NFC tag to the reader area. The UI shows the tag UID and type when detected.
3. The firmware will attempt to read the tag:
   - For NTAG/Ultralight (Type 2) tags it reads user memory and tries to decode NDEF records.
   - For MIFARE Classic tags it will attempt to bruteforce/identify readable sectors and keys and build a compact summary.
4. While bruteforcing, a progress percentage and sector/block/key phase may be shown. You can press `Skip` (the More button becomes Skip during bruteforce) to stop dictionary attempts and perform a basic read.
5. When complete, press `Save` to store a `.nfc` file on the SD card.

Notes:
- If scanning is cancelled or the tag is removed, the UI will update and the scan task will stop.

## Saving a scanned tag

- After a successful scan, use the `Save` button to write a `.nfc` file to `/mnt/ghostesp/nfc/` (this directory is mounted on the device and corresponds to the SD card path).
- For NTAG/Ultralight the file includes metadata, signature, page dump and counters when available.

## Writing a `.nfc` file to a tag

1. In the NFC view choose `Write` to browse `.nfc` files on the SD card.
2. Select a file to open the Write popup. The UI shows a compact summary (UID/type/pages and NDEF summary if available).
3. Press `Write`. The UI will prompt you to present a tag.
4. Present the target tag to the reader. The device will attempt to write the contents and update progress.
5. You can cancel the write while it is in progress. The write will stop cooperatively and the UI will indicate success/failure.

Notes:
- Only valid, parsed `.nfc` files can be written; invalid files will disable the Write button.
- Writing waits for a tag to be present and will show progress as a percentage.

## Managing saved files

- Use `Saved` to list `.nfc` files found on the SD card path `/mnt/ghostesp/nfc/`.
- Select a file to view details. From that popup you can `Close`, `Rename`, or `Delete` the file.
- Rename operations use the keyboard view; the new name is sanitized to avoid invalid characters and will append `.nfc` if needed.

## User MIFARE Classic Keys

- The device supports loading a user keys file named `mfc_user_dict.nfc` from the SD card to provide additional keys for MIFARE Classic bruteforcing.
- Keys are normalized (hex-only, uppercase) and displayed two per line in the UI.
- The UI shows `(Out of memory)` or `(Empty)` if the file cannot be read or is empty.

## Troubleshooting

- If `Scan` does not detect tags:
  - Ensure your board includes NFC hardware (PN532) and the build enables NFC support.
  - Check the PN532 wiring/pins or module seating if applicable.
  - Some boards share I2C with other peripherals; try closing other apps or wait for the device to finish other I2C tasks.

- If `Save` fails:
  - Verify an SD card is installed and mounted (SD path `/mnt/ghostesp` present).
  - Check SD card formatting (FAT32 recommended) and free space.

- If `Write` fails or produces corrupted tags:
  - Verify the `.nfc` file is valid and parsed successfully in the Write popup.
  - Make sure the target tag type matches the file (e.g., NTAG image to NTAG tag).
  - Try a different tag or ensure the tag is properly presented to the reader.

## Privacy and safety

- Saved `.nfc` files may contain personal data or payloads (NDEF records). Handle saved files responsibly.
- Only write to tags you own or have permission to modify.

## Developer notes

- The PN532 driver is initialized on an I2C port chosen to avoid conflicts with other I2C devices. If PN532 initialization fails the firmware performs an I2C scan and logs discovered devices.


