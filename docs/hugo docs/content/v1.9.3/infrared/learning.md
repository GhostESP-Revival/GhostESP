---
title: "Learning Signals"
description: "Capture IR buttons and remotes with GhostESP"
weight: 10
---

## Before you start

- **Hardware**: Board with IR RX support (see [hardware requirements]({{< relref "hardware.md" >}})).
- **SD card**: Inserted and mounted so GhostESP can save `.ir` files under `/mnt/ghostesp/infrared/`.
- **Easy Learn**: Optional assistant that suggests button names. Toggle via settings.

## Learn a remote or button

1. Open **Infrared** and choose **Learn Remote** or **Learn Button** to append to an existing remote.

2. Align the source remote with the IR receiver window, hold it steady within a few centimeters, then press the button once.

3. Confirm the button name. Easy Learn provides a suggested label during learning, otherwise use the on-screen keyboard.

4. Watch the preview popup for carrier frequency, protocol name, and decoded address/command details. Cancel and retry if you recieve a raw decode on a known non-raw signal.

5. Confirm to save. GhostESP will create a new `.ir` file for fresh remotes or append the button to the currently open file automatically.

### Storage layout

- New remotes are written to `/mnt/ghostesp/infrared/remotes/<name>.ir`.
- Adding to an existing remote appends another named button block inside the same `.ir` file.

### Tips

- Keep the remote within a few centimeters of the receiver and avoid direct sunlight.
- If learning fails, exit and re-open the Infrared learning popup to try to reinitialize the RMT RX channel.

## CLI Support

You can learn signals via the command line using `ir learn`.

```bash
# Learn a signal and auto-save to a new file under /mnt/ghostesp/infrared/remotes
ir learn

# Learn and append to a specific file
ir learn /mnt/ghostesp/infrared/remotes/my_remote.ir
```

- Without a path, GhostESP generates a new `.ir` file name based on the decoded protocol/address/command (or a RAW timestamp) in `/mnt/ghostesp/infrared/remotes/`.
- With a path, the learned signal is appended to the given `.ir` file.

### Manual File Creation

You can manually create `.ir` files if you know the protocol details. Save them as text files with the `.ir` extension in `/mnt/ghostesp/infrared/remotes/`.

**Example `.ir` file content:**

```text
Filetype: IR signals file
Version: 1

name: Power
type: parsed
protocol: NEC
address: 00 FF
command: 00 FF
```
