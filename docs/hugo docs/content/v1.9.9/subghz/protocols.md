---
title: "Supported Protocols"
description: "Reference for SubGHz protocols decoded by GhostESP"
weight: 50
---

GhostESP automatically attempts to decode captured signals using 30+ common SubGHz protocols. When a signal is successfully decoded, it can be reconstructed and transmitted reliably.

## Decoded protocols

### Princeton
- **Common use**: Garage door remotes, wireless sensors
- **Frequency**: 315 MHz, 433.92 MHz
- **Bit length**: 24 bits
- **Encoding**: Manchester-like with specific timing

### CAME
- **Common use**: Gate and garage door remotes
- **Frequency**: 433.92 MHz
- **Bit length**: 24 bits
- **Variants**: CAME, CAME Atomo

### Nice Flo
- **Common use**: Nice brand gate openers
- **Frequency**: 433.92 MHz
- **Bit length**: 24 bits

### Nice Flor S
- **Common use**: Nice brand gate systems
- **Frequency**: 433.92 MHz
- **Bit length**: Variable

### PT2260 / PT2262
- **Common use**: Generic remote controls, alarm sensors
- **Frequency**: 315 MHz, 433.92 MHz
- **Bit length**: 12-24 bits
- **Encoding**: Simple address/data format

### Chamberlain
- **Common use**: Chamberlain garage door openers
- **Frequency**: 315 MHz, 390 MHz
- **Bit length**: Variable
- **Note**: Some models use rolling codes

### Linear
- **Common use**: Linear brand gate operators
- **Frequency**: 310 MHz, 315 MHz, 390 MHz
- **Bit length**: 24 bits
- **Variants**: Linear, Linear Delta3

### KeeLoq
- **Common use**: Secure garage door and vehicle remotes
- **Frequency**: 433.92 MHz, 868 MHz
- **Bit length**: 64 bits
- **Note**: Uses rolling code encryption; replay may fail

### Holtek
- **Common use**: Holtek-based remote controls
- **Frequency**: 433.92 MHz
- **Bit length**: 12-24 bits
- **Variants**: Holtek, Holtek HT12X

### FAAC SLH
- **Common use**: FAAC gate automation systems
- **Frequency**: 433.92 MHz
- **Bit length**: Variable

### Alutech AT-4N
- **Common use**: Alutech gate systems
- **Frequency**: 433.92 MHz
- **Bit length**: Variable

### Marantec
- **Common use**: Marantec garage door openers
- **Frequency**: 868 MHz
- **Bit length**: Variable

### Unilarm
- **Common use**: Alarm systems
- **Frequency**: 433.92 MHz
- **Bit length**: 24 bits

### Gangqi
- **Common use**: Asian market remotes
- **Frequency**: 433.92 MHz
- **Bit length**: Variable

### Doitrand
- **Common use**: Various remote controls
- **Frequency**: 433.92 MHz
- **Bit length**: Variable

### Gate TX
- **Common use**: Generic gate transmitters
- **Frequency**: 433.92 MHz
- **Bit length**: 24 bits

### Ansonic
- **Bit length**: 12 or 24 bits
- **Encoding**: Fixed-code DIP-style encoding

### Bett
- **Bit length**: 18 bits
- **Encoding**: Fixed-code DIP-style encoding

### Clemsa
- **Bit length**: 18 bits
- **Encoding**: Fixed-code DIP-style encoding

### Dickert MAHS
- **Bit length**: 36 bits
- **Decoded fields**: Factory code and user code

### Dooya
- **Bit length**: 40 bits
- **Decoded fields**: Serial number, channel, and button

### Elplast
- **Bit length**: 18 bits
- **Encoding**: Fixed-code DIP-style encoding

### Marantec24
- **Bit length**: 24 bits
- **Decoded fields**: Serial number and button

### Hollarm
- **Bit length**: 42 bits
- **Decoded fields**: Serial number, button, and checksum

### Hay21
- **Bit length**: 21 bits
- **Decoded fields**: Serial number, button, and counter

### Feron
- **Bit length**: 32 bits
- **Decoded fields**: Serial number and command

### Roger
- **Bit length**: 28 bits
- **Decoded fields**: Serial number and button

### Treadmill37
- **Bit length**: 37 bits
- **Decoded fields**: Serial number and button

### KeyFinder
- **Bit length**: 24 bits
- **Decoded fields**: Serial number and device ID

### Nord ICE
- **Bit length**: 33 bits
- **Decoded fields**: Serial number and button

## Raw signal fallback

If a signal cannot be decoded with any known protocol, GhostESP saves it as raw timing data. Raw captures can still be transmitted but:

- Require cleaner captures for reliable replay
- Are more sensitive to timing and environmental factors
- Cannot be edited or modified as easily as decoded signals

## Protocol detection

GhostESP uses a stream decoder engine that tests captured signals against all supported protocols simultaneously. When a protocol matches with sufficient confidence, the signal is decoded and the protocol name, bit count, and decoded code are displayed.

## Notes

- Protocol detection is automatic; no manual selection is required.
- Some protocols share similar timing; detection may not always be 100% accurate.
- Rolling code protocols (like some KeeLoq implementations) can be decoded but may not replay successfully due to counter synchronization.
- New protocols can be added to the decoder engine by updating the firmware.

## Troubleshooting

- **Wrong protocol detected**: If the decoded protocol doesn't match your device, try raw capture mode.
- **Decode fails**: Ensure the signal capture is clean and complete. Move closer to the target device.
- **Replay fails on decoded signal**: The device may use rolling codes or additional security measures.
