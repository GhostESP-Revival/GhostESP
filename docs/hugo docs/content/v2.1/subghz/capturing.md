---
title: "Capturing Signals"
description: "Record and decode SubGHz signals from remotes and transmitters"
weight: 20
---

Capture signals from remote controls, garage door openers, and other SubGHz devices. GhostESP automatically decodes supported protocols and saves signals for later replay.

## Before you start

- **Hardware**: Board with CC1101 SubGHz support (see [Hardware Support]({{< relref "hardware.md" >}})).
- **SD card**: Inserted and mounted so GhostESP can save `.sub` files under `/mnt/ghostesp/subghz/`.
- **Frequency**: Know or determine the target frequency using [Scanning]({{< relref "scanning.md" >}}) or [Frequency Analyzer]({{< relref "freq-analyzer.md" >}}).

## Capture modes

GhostESP supports two capture modes:

- **Normal capture** - Attempts to decode the signal using supported protocols. Best for known remotes and standard devices.
- **Raw capture** - Records raw timing data without decoding. Use for unknown protocols or when normal capture fails.

## Capturing a signal

### On-device UI

1. Open **SubGHz → Capture** to start scanning on the current frequency.
2. Press the button on your target remote while the scanner is running.
3. When a signal is detected, GhostESP will attempt to decode it.
4. Review the decoded information (protocol, bits, frequency) in the preview.
5. Confirm to save the signal to the SD card.

### Command line

```bash
# Start the scanner
subghz start

# Enable raw capture mode (optional)
subghz capture_on

# Disable raw capture (return to normal mode)
subghz capture_off

# Capture the current signal
subghz capture [name_hint]

# Save the captured signal
subghz save [name_hint]
```

## Protocol decoding

GhostESP automatically attempts to decode captured signals using 30+ protocols including:

- Princeton
- CAME
- Nice Flo
- PT2260/PT2262
- Chamberlain
- Linear
- KeeLoq
- Dooya
- Dickert MAHS
- Nord ICE
- And many more (see [Supported Protocols]({{< relref "protocols.md" >}}))

If the protocol is recognized, GhostESP displays:
- Protocol name
- Bit count
- Decoded code in hexadecimal
- Frequency

If decoding fails or the protocol is unknown, the signal is saved as raw timing data.

## File storage

Captured signals are saved as `.sub` files in `/mnt/ghostesp/subghz/` using the Flipper SubGhz Key File format. See [Files]({{< relref "files.md" >}}) for details on the file structure.

File names are auto-generated based on:
- Decoded protocol and code (if successful)
- Timestamp (if raw capture)
- Optional name hint provided during capture

## Tips for successful captures

- **Frequency matching**: Ensure you're scanning on the correct frequency band for your target device.
- **Distance**: Start with the remote within 1-2 meters of the device for best signal quality.
- **Multiple presses**: Some remotes use rolling codes; capture multiple presses to identify patterns.
- **Interference**: Avoid areas with strong RF interference (near other transmitters, power lines, etc.).
- **Raw fallback**: If normal capture consistently fails, try raw capture mode to record the signal timings.

## Notes

- Captured signals are stored in Flipper-compatible format, allowing interchange with Flipper Zero devices.
- Rolling code protocols (like some KeeLoq implementations) may not replay successfully due to counter synchronization.
- Some devices require multiple button presses to transmit the full command sequence.

## Troubleshooting

- **No signal detected**: Verify the frequency band, check antenna connection, and ensure the target device is transmitting.
- **Decode fails**: Try raw capture mode, or move closer to the target device to improve signal quality.
- **Save fails**: Check that the SD card is mounted and the `/mnt/ghostesp/subghz/` directory exists.
- **Replay doesn't work**: Some protocols use rolling codes or require specific timing; raw capture may be necessary.
