---
title: "Transmitting Signals"
description: "Send captured or universal IR commands"
weight: 20
---

## Send a Saved Command

1. Open **Infrared** and browse **Remotes** to see saved `.ir` files.
2. Pick a Flipper-compatible `.ir` file. GhostESP parses the sections inside the file and lists every named button.
3. Tap a button entry to transmit. The configured LED should flash purple if not in stealth mode.

> **Tip:** If you see “No .ir files” in the Remotes or Universals lists, try reinserting your SD card and rebooting the device. Ensure the `/mnt/ghostesp/infrared/remotes` and `/mnt/ghostesp/infrared/universals` folders exist on the card.

{{< flow title="What happens when you send" >}}
Parse the `.ir` file | Protocol fields, or raw timing data plus its carrier and duty cycle
Select the signal | By named button, or by index from `ir list`
Transmit over RMT | Carrier frequency and duty come from the file for raw signals
LED feedback | Flashes purple unless stealth mode is on
{{< /flow >}}

### Universal Libraries

- Universal `.ir` packs live under `/mnt/ghostesp/infrared/universals` and contain large collections of commands grouped by device.
- When you open a universal file, GhostESP scans the command list and prompts you to pick a specific button to send.
- Parsing very large libraries (for example, community dumps) can take several seconds; wait for the list to finish populating before selecting.
- You can find supported universals at [Momentum Flipper Firmware](https://github.com/Next-Flip/Momentum-Firmware/tree/dev/applications/main/infrared/resources/infrared/assets). Download and place the files under `infrared/universals`.

GhostESP also includes a built-in Universal IR file with popular TV POWER signals - see [Infrared Files]({{< relref "files.md" >}}) for details.

### Tips

- Aim the LED directly at the target's receiver window.
- If nothing happens, close the popup, verify you chose the right protocol (raw vs decoded), and relearn the button or test another signal.
- Ensure no bright sunlight hits the receiver; ambient infrared noise can reduce range.

## CLI Support

You can list, inspect and transmit signals using the CLI, which is useful for scripting or automation.

### Listing and Inspecting

```bash
# List .ir/.json files in the remote directory (or a given path)
ir list [path]

# Show the buttons in a file, or use a remote index printed by `ir list`
ir show <path|remote_index>

# List universal library files; -all also dumps the built-in signals
ir universals list [-all]
```

### Sending from File

Use `ir send` to transmit a signal from an existing `.ir` file. The first argument may be a path or a remote index from `ir list` (indices are only valid right after `ir list`):

```
# Send the first signal in the file
ir send /mnt/ghostesp/infrared/remotes/TV.ir

# Send the 3rd signal (index 2)
ir send /mnt/ghostesp/infrared/remotes/TV.ir 2

# Using a remote index from `ir list`, then a button index
ir list
ir send 0 1
```

### Universal Libraries

```
# Send a built-in universal signal by index
ir universals send <index>
```

### Receiving and Pin Override

```
# Capture incoming IR into the log for inspection (timeout in seconds, default 60)
ir rx [timeout]

# Show the current TX/RX pins and whether they are overridden
irpin

# Override a pin; -1 clears the override and uses the board default
irpin tx <pin|-1>
irpin rx <pin|-1>
```

### Inline Sending

You can send raw or parsed signals directly without a file using the `inline` mode markers. This is useful for sending commands from a script or external tool.

**Text Format:**

```text
[IR/BEGIN]
name: Power
type: parsed
protocol: NEC
address: 00 FF
command: 18 E7
[IR/CLOSE]
```

**JSON Format:**

```json
[IR/BEGIN]
{"type":"parsed","protocol":"NEC","address":255,"command":6375}
[IR/CLOSE]
```

> **Note:** In the text format, address and command bytes are listed in MSB-first order and parsed LSB-first. `00 FF` produces address `0x00FF` (255), and `18 E7` produces command `0x18E7` (6375).

See the [CLI Reference]({{< relref "../getting-started/command-line-reference.md" >}}#infrared) for full command details.
