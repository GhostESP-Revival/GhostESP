---
title: "Transmitting Signals"
description: "Send captured or universal IR commands"
weight: 20
---

## Send a saved command

1. Open the Infrared view and browse the *Remotes* list to see `.ir` files stored on the SD card.
2. Pick a Flipper-compatible `.ir` file. GhostESP parses the sections inside the file and lists every named button.
3. Tap a button entry to transmit. The configured LED should flash pink if not in stealth mode.

> **Tip:** If you see “No .ir files” in the Remotes or Universals lists, try reinserting your SD card and rebooting the device. Ensure the `/ghostesp/infrared/remotes` and `/ghostesp/infrared/universals` folders exist on the card.

### Universal libraries

- Universal `.ir` packs live under `/ghostesp/infrared/universals` and contain large collections of commands grouped by device.
- When you open a universal file, GhostESP scans the command list and prompts you to pick a specific button to send.
- Parsing very large libraries (for example, community dumps) can take several seconds; wait for the list to finish populating before selecting.
- You can find supported universals at [Momentum Flipper Firmware](https://github.com/Next-Flip/Momentum-Firmware/tree/dev/applications/main/infrared/resources/infrared/assets). Download and place the files under `infrared/universals`.

### Universal IR Database

GhostESP also includes a built-in Universal IR file with popular TV POWER signals. Use the `TURNHISTVOFF` universal to turn on or off many different TVs.

### Tips

- Aim the device’s IR LED directly at the target device’s receiver window and stay within a few meters.
- If nothing happens, close the popup, verify you chose the right protocol (raw vs decoded), and relearn the button or test another signal.
- Ensure no bright sunlight hits the receiver; ambient infrared noise can reduce range.
