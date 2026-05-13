---
title: "Infrared Files"
description: "Organize, rename, and edit GhostESP IR files"
weight: 30
---

## Directory layout

- `/ghostesp/infrared/remotes` — Individual remotes captured on-device
- `/ghostesp/infrared/universals` — Library files with many commands
- The Infrared UI reads from `/mnt/ghostesp/infrared/...` when the SD card is mounted.

### Flipper IR libraries

GhostESP reads standard Flipper `.ir` format. Download packs from [Lucaslhm/Flipper-IRDB](https://github.com/Lucaslhm/Flipper-IRDB).

Universal IR files are available at [Momentum Flipper Firmware](https://github.com/Next-Flip/Momentum-Firmware/tree/dev/applications/main/infrared/resources/infrared/assets). Download and place them under `infrared/universals`.

### Built-in Universal IR file

In addition to SD card files, GhostESP includes a built-in Universal IR file with popular TV POWER signals. This file doesn't require an SD card.


### Append new signals

- Choose *Add Signal* while a remote is open to append a newly learned button.
- Easy Learn suggests button names; otherwise you will be prompted via the on-screen keyboard.

## Web UI management

- Connect to the GhostNet AP and open the web UI.
- Browse to the file manager tab and navigate to `/ghostesp/infrared/`.
- Upload `.ir` files to the appropriate folder or download existing ones for backups.

### Tips

- Keep file names short; the UI truncates long names in lists.
- After mass uploads, back out of the infrared file view on the on-board display UI to refresh the list.
- Back up your IR folder before flashing new firmware or reformatting the SD card.
