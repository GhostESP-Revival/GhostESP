---
title: "Transmitting Signals"
description: "Replay captured SubGHz signals to control devices"
weight: 30
---

Replay captured signals to control garage doors, gates, alarms, and other SubGHz devices.

## Before you start

- **Hardware**: Board with CC1101 SubGHz support (see [Hardware Support]({{< relref "hardware.md" >}})).
- **Captured signals**: Saved `.sub` files in `/mnt/ghostesp/subghz/` (see [Capturing]({{< relref "capturing.md" >}})).
- **Frequency matching**: Ensure the transmission frequency matches the target device's expected frequency.

## Transmitting a saved signal

### On-device UI

1. Open **SubGHz → Saved** to browse captured signals.
2. Select a `.sub` file from the list.
3. Review the signal details (protocol, frequency, code).
4. Choose **Replay** to transmit the signal.
5. The device will transmit on the frequency stored in the file.

### Command line

```bash
# List all saved signals
subghz list

# Load and replay a specific signal
subghz load <name>
subghz replay <name>

# Load the last captured signal
subghz load last
subghz replay last
```

## Frequency matching

Signals are transmitted at the frequency stored in the `.sub` file. If the target device expects a different frequency:

1. Edit the `.sub` file to change the `Frequency` field (see [Files]({{< relref "files.md" >}})).
2. Or use the [Frequency Analyzer]({{< relref "freq-analyzer.md" >}}) to determine the correct frequency before capturing.

## Protocol considerations

### Decoded signals

For signals with decoded protocols (Princeton, CAME, KeeLoq, etc.):

- GhostESP reconstructs the signal from the decoded code and protocol parameters.
- Transmission should be reliable if the protocol was correctly identified.
- Some protocols use rolling codes; replay may fail if the target device expects a new code.

### Raw signals

For raw captures (unknown protocol):

- GhostESP transmits the exact timing sequence that was captured.
- Ensure the capture was clean and complete for reliable replay.
- Raw signals are more sensitive to timing and environmental factors.

## Tips for successful transmission

- **Distance**: Start within 5-10 meters of the target device.
- **Antenna orientation**: Point the antenna toward the target receiver.
- **Multiple attempts**: Some devices require multiple transmissions to respond.
- **Interference**: Avoid areas with strong RF interference.
- **Frequency verification**: Use the Frequency Analyzer to confirm the target device's frequency.

## Notes

- Transmission power is limited by regulatory requirements and hardware capabilities.
- Some devices use rolling codes or challenge-response mechanisms that prevent simple replay attacks.
- Commercial security systems may have additional protections against replay attacks.
- Always ensure you have authorization to control the target device.

## Troubleshooting

- **No response from device**: Verify the frequency matches the target device. Check antenna connection and orientation.
- **Transmission fails**: Ensure the `.sub` file is valid and not corrupted. Try recapturing the signal.
- **Intermittent success**: The device may use rolling codes or require specific timing. Try multiple transmissions.
- **Wrong frequency**: Edit the `.sub` file to correct the frequency, or recapture on the correct band.
