---
title: "Transmitting Signals"
description: "Send captured or universal IR commands"
weight: 20
---

## Send a saved command

1. Open **Infrared** and browse **Remotes** to see saved `.ir` files.
2. Pick a Flipper-compatible `.ir` file. GhostESP parses the sections inside the file and lists every named button.
3. Tap a button entry to transmit. The configured LED should flash pink if not in stealth mode.

> **Tip:** If you see “No .ir files” in the Remotes or Universals lists, try reinserting your SD card and rebooting the device. Ensure the `/ghostesp/infrared/remotes` and `/ghostesp/infrared/universals` folders exist on the card.

### Universal libraries

- Universal `.ir` packs live under `/ghostesp/infrared/universals` and contain large collections of commands grouped by device.
- When you open a universal file, GhostESP scans the command list and prompts you to pick a specific button to send.
- Parsing very large libraries (for example, community dumps) can take several seconds; wait for the list to finish populating before selecting.
- You can find supported universals at [Momentum Flipper Firmware](https://github.com/Next-Flip/Momentum-Firmware/tree/dev/applications/main/infrared/resources/infrared/assets). Download and place the files under `infrared/universals`.

GhostESP also includes a built-in Universal IR file with popular TV POWER signals. Use the `TURNHISTVOFF` universal to turn on or off many different TVs.

### Tips

- Aim the LED directly at the target's receiver window.
- If nothing happens, close the popup, verify you chose the right protocol (raw vs decoded), and relearn the button or test another signal.
- Ensure no bright sunlight hits the receiver; ambient infrared noise can reduce range.

## CLI Support

You can transmit signals using the command line interface, which is useful for scripting or automation.

### Sending from file

Use `ir send` to transmit a signal from an existing `.ir` file:

```
# Send the first signal in the file
ir send /ghostesp/infrared/remotes/TV.ir

# Send the 3rd signal (index 2)
ir send /ghostesp/infrared/remotes/TV.ir 2
```

### Inline Sending

You can send raw or parsed signals directly without a file using the `inline` mode markers. This is useful for sending commands from a script or external tool.

**Text Format:**

```text
[IR/BEGIN]
name: Power
type: parsed
protocol: NEC
address: FF 00
command: E7 18
[IR/CLOSE]
```

**JSON Format:**

```json
[IR/BEGIN]
{"type":"parsed","protocol":"NEC","address":65280,"command":59160}
[IR/CLOSE]
```

See the [CLI Reference](/getting-started/command-line-reference/#infrared) for full command details.
