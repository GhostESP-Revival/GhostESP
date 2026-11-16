---
title: "Files and Management"
description: "SubGHz file format and signal management"
weight: 60
---

The **on-device UI** stores captured signals as Flipper SubGhz Key File `.sub` files, compatible with Flipper Zero devices. The **`subghz` CLI** writes a different file with the same `.sub` extension: a `ghostesp_subghz_snapshot` RSSI spectrum snapshot, which is not a Flipper file and cannot be transmitted. Both live in `/mnt/ghostesp/subghz/` on the SD card.

## File Format

On-device signals are saved as Flipper `.sub` text files with the following structure:

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

## Raw Signal Format

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

## CLI Snapshot Format

`subghz save` writes the scanner's RSSI level buffer as a spectrum snapshot under the same `.sub` extension:

```
ghostesp_subghz_snapshot=1
name=snapshot_1A2B3C4D
base_mhz=43392
step_khz=200
cursor=37
levels=12,14,11,9,...
```

- **name**: snapshot name (sanitized from the name hint, or `snapshot_%08X`)
- **base_mhz / step_khz**: scan band and channel spacing
- **cursor**: scanner position when captured
- **levels**: 64 RSSI display levels (0-100), comma-separated

A snapshot is a picture of RF activity; it cannot be replayed or transmitted. Signals you can transmit are Flipper `.sub` files written by the on-device UI.

## Managing Saved Signals

### On-Device UI

1. Open **SubGHz → Saved** to browse captured signals.
2. Navigate through the list using the arrow keys.
3. Select a file to view details or replay.
4. Use **Delete** to remove unwanted signals.

### CLI

The CLI works on **spectrum snapshots**, not decodable signals:

```bash
# List saved spectrum snapshots
subghz list

# Restore a snapshot into the scanner display (does not transmit)
subghz load <name>

# Restore the last captured/loaded snapshot
subghz load last
```

To view or replay an on-device `.sub` signal, use the on-device **Saved** list instead.

## Manual File Creation

You can manually create Flipper `.sub` files for the on-device **Saved** list if you know the protocol details:

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

## Flipper Zero Compatibility

On-device signal files use the same format as Flipper Zero, enabling:

- Import signals from Flipper Zero devices
- Export signals to Flipper Zero devices
- Use community signal databases
- Share signals across platforms

Simply copy `.sub` files between devices using the SD card.

## File Naming

- **CLI snapshots**: auto-generated as `snapshot_%08X` (tick-based), or the sanitized name hint you pass to `subghz save`.
- **On-device signals**: the UI names the file from the decoded protocol/code, or a raw timestamp, when you save.

## Notes

- Files are plain text and can be edited with any text editor.
- Editing frequency or protocol fields can enable transmission on different bands.
- Be careful when manually editing files; incorrect values may cause transmission failures.
- Large RAW_Data fields may span multiple lines in the file.

## Troubleshooting

- **File not appearing in list**: Ensure the file is in `/mnt/ghostesp/subghz/` and has the `.sub` extension.
- **File fails to load**: Check that the file format is valid and all required fields are present.
- **Transmission fails after editing**: Verify the frequency, protocol, and key values are correct for your target device.
