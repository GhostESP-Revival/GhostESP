# Infrared (IR) Guide

This guide explains the Infrared features in GhostESP: how to learn (receive) signals, transmit saved signals, manage IR files, and what hardware is required.

## Quick overview

- **Learn (Receive)**: Capture signals from a remote and save them as `.ir` files.
- **Easy Learn Mode**: Helps name buttons automatically.
- **Transmit**: Send signals from Flipper-style `.ir` files or universal libraries.
- **Files**: IR files live on the SD card under `/ghostesp/infrared/remotes` or `/ghostesp/infrared/universals`.
- **Protocols**: The firmware supports many common IR protocols (NEC, Kaseikyo, Pioneer, RCA, Samsung, SIRC, RC5, RC6 and more).

## What you can do

- Learn a single button or entire remotes.
- Use Easy Learn to auto-name common buttons (Power, Volume, etc.).
- Browse `.ir` files and transmit an individual command.
- Use universal library files (many commands in one file) and pick which command to transmit.
- Rename, delete, or add remotes (on supported boards).

## How to learn a signal

1. Open the Infrared view and choose `Learn Remote` or `Learn Button`.
2. If Easy Learn is enabled, follow on-screen prompts to map buttons; otherwise the device will show a learning popup.
3. Point the original remote at the device and press the button you want to capture.
4. The UI shows a preview and attempts to decode the protocol (if possible).
5. Save the captured signal to the SD card as a `.ir` file or add it to an existing remote.

Notes:
- Learning uses the RMT peripheral; the firmware initializes an RMT RX channel and a background learning task.
- If the RMT channel is not available the UI will cancel the learning popup.

## How to transmit signals

1. In the Infrared view browse `Remotes` or `Universals` on the SD card.
2. Open a remote file to list commands, or open a universal file to pick a named command.
3. Select a command to transmit; the device sends the IR pattern via the configured IR TX pin.

Notes:
- Universal files may be large; the UI scans files to extract unique command names for selection.
- Transmission uses protocol encoders included in firmware to recreate the timing and carrier frequency.

## Supported file formats and directories

- Flipper `.ir` files are supported and expected to live under:
  - `/ghostesp/infrared/remotes` — individual remote files
  - `/ghostesp/infrared/universals` — universal library files
- The UI lists `.ir` files by extension and presents contained commands for transmit.

## Supported protocols

Supported protocols:
- NEC / NECext
- Kaseikyo
- Pioneer
- RCA
- Samsung
- SIRC (SIRC15, SIRC20)
- RC5
- RC6

If a protocol is not decoded, the device will still attempt to capture raw timings which may be replayable.

## Managing IR files (Rename, Delete, Add)

- Open a saved remote to view and manage it. You can rename or delete a remote file from the UI.
- Add a learned signal to an existing remote or create a new file when saving.
- The UI performs safe string handling when saving and will show errors if the SD card is not writable.

## Hardware / Board support

- Not every board includes IR TX/RX hardware. IR features are enabled only on boards that provide the necessary pins and hardware.
- Supported boards include:
  - LilyGo S3TWatch (IR TX and virtual storage support)
  - ESP32-S3 Cardputer (IR TX)
  - LilyGo TEmbed C1101 (IR TX and RX)

## Web UI file management

- To manage IR files via Web UI:
  1. Open the Web UI and navigate to the SD card / file manager section (requires the web server to be running on the device).
  2. Use the file browser to view `/ghostesp/infrared/remotes` or `/ghostesp/infrared/universals`.
  3. Upload new `.ir` files to those directories or download existing files for inspection.
  4. Delete or rename files from the Web UI; uploaded files appear immediately in the Infrared view when the SD is mounted.

- Notes:
  - The Web UI may require authentication depending on settings (`webauth` can be toggled); check your web settings before uploading.
  - The server treats `/mnt` as the SD mount root; paths used in the UI map to `/mnt/ghostesp/...` on the device filesystem.

## Troubleshooting

- Transmit not working: confirm TX pin wiring, try restarting the device or send a different signal.
- Check serial logs and contact the team on discord for support.
- Out of memory: large universal files may require more RAM to scan; try smaller files if the UI cannot list commands.

## Privacy and safety

- Only capture or transmit signals for devices you own or have permission to control.

