---
title: "Files and Management"
description: "SubGHz file format and signal management"
weight: 60
---

GhostESP uses the Flipper SubGhz Key File format for storing captured signals. This format is compatible with Flipper Zero devices, allowing signal interchange.

## File format

SubGHz signals are saved as `.sub` text files with the following structure:

```
Filetype: Flipper SubGhz Key File
Version: 1
Frequency: 433920000
Preset: FuriHalSubGhzPresetOok270Async
Protocol: Princeton
Bit: 24
Key: AA BB CC DD EE FF
TE: 390
Manufacture: Unknown
```

### Fields

- **Filetype**: Identifier for the file format
- **Version**: File format version (currently 1)
- **Frequency**: Transmission frequency in Hz
- **Preset**: Modulation preset (Ook270Async or Ook650Async)
- **Protocol**: Decoded protocol name (or "RAW" for raw captures)
- **Bit**: Number of bits in the decoded signal
- **Key**: Hexadecimal representation of the decoded code
- **TE**: Timing element (microseconds) for the protocol
- **Manufacture**: Manufacturer information (optional)
- **RAW_Data**: Raw timing sequence (for raw captures only)

## Raw signal format

For undecoded signals, the file uses the RAW_Data field:

```
Filetype: Flipper SubGhz Key File
Version: 1
Frequency: 433920000
Preset: FuriHalSubGhzPresetOok270Async
Protocol: RAW
RAW_Data: 100 -200 100 -200 100 -400 100 -200 ...
```

RAW_Data contains timing values in microseconds, alternating between positive (HIGH) and negative (LOW) periods.

## Storage location

All SubGHz files are stored in `/mnt/ghostesp/subghz/` on the SD card.

## Managing saved signals

### On-device UI

1. Open **SubGHz → Saved** to browse captured signals.
2. Navigate through the list using the arrow keys.
3. Select a file to view details or replay.
4. Use **Delete** to remove unwanted signals.

### Command line

```bash
# List all saved signals
subghz list

# Load a specific signal
subghz load <filename>

# Load the last captured signal
subghz load last
```

## Manual file creation

You can manually create `.sub` files if you know the protocol details:

1. Create a text file with the `.sub` extension.
2. Add the required fields in the correct format.
3. Save to `/mnt/ghostesp/subghz/`.
4. The file will appear in the Saved list.

**Example manual file:**

```
Filetype: Flipper SubGhz Key File
Version: 1
Frequency: 433920000
Preset: FuriHalSubGhzPresetOok270Async
Protocol: Princeton
Bit: 24
Key: A1 B2 C3 D4 E5 F6
TE: 390
Manufacture: Unknown
```

## Flipper Zero compatibility

GhostESP uses the same file format as Flipper Zero, enabling:

- Import signals from Flipper Zero devices
- Export signals to Flipper Zero devices
- Use community signal databases
- Share signals across platforms

Simply copy `.sub` files between devices using the SD card.

## File naming

GhostESP auto-generates file names based on:

- **Decoded signals**: Protocol name + decoded code (e.g., `Princeton_A1B2C3.sub`)
- **Raw signals**: Timestamp (e.g., `capture_12345678.sub`)
- **Custom names**: You can provide a name hint during capture

## Notes

- Files are plain text and can be edited with any text editor.
- Editing frequency or protocol fields can enable transmission on different bands.
- Be careful when manually editing files; incorrect values may cause transmission failures.
- Large RAW_Data fields may span multiple lines in the file.

## Troubleshooting

- **File not appearing in list**: Ensure the file is in `/mnt/ghostesp/subghz/` and has the `.sub` extension.
- **File fails to load**: Check that the file format is valid and all required fields are present.
- **Transmission fails after editing**: Verify the frequency, protocol, and key values are correct for your target device.
