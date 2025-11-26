---
title: "Learning Signals"
description: "Capture IR buttons and remotes with GhostESP"
weight: 10
---

## Before you start

- **Hardware**: Use a board with IR RX support (see [hardware requirements]({{< relref "hardware.md" >}})).
- **SD card**: Inserted and mounted so GhostESP can save `.ir` files under `/ghostesp/infrared/`.
- **Easy Learn**: Optional assistant that suggests button names; toggle it via `infrared_easy` in the command line config or the settings menu.

## Learn a remote or button

1. Open the Infrared view from the main menu and choose *Learn Remote* for a new file or *Learn Button* inside a saved remote to append a new signal to an existing remote.

2. Align the source remote with the IR receiver window, hold it steady within a few centimeters, then press the button once.

3. Confirm the button name. Easy Learn provides a suggested label during learning, otherwise use the on-screen keyboard.

4. Watch the preview popup for carrier frequency, protocol name, and decoded address/command details. Cancel and retry if you recieve a raw decode on a known non-raw signal.

5. Confirm to save. GhostESP will create a new `.ir` file for fresh remotes or append the button to the currently open file automatically.

### Storage layout

- New remotes are written to `/ghostesp/infrared/remotes/<name>.ir`.
- Adding to an existing remote appends another named button block inside the same `.ir` file.

### Tips

- Keep the remote within a few centimeters of the receiver and avoid direct sunlight.
- If learning fails, exit and re-open the Infrared learning popup to try to reinitialize the RMT RX channel.
