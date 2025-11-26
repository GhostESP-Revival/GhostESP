---
title: "Infrared Files"
description: "Organize, rename, and edit GhostESP IR files"
weight: 30
---

## Directory layout

- `/ghostesp/infrared/remotes` holds individual remotes captured on-device.
- `/ghostesp/infrared/universals` stores library files with many commands.
- The Infrared UI reads from `/mnt/ghostesp/infrared/...` when the SD card is mounted.

### Flipper IR libraries

- GhostESP reads the standard Flipper `.ir` format, so you can copy files from community packs.
- A large collection is available at [Lucaslhm/Flipper-IRDB](https://github.com/Lucaslhm/Flipper-IRDB); place downloaded files under `infrared/remote` or `infrared/universals` for universal files which you can find in [Momentum custom Flipper Zero firmware](https://github.com/Next-Flip/Momentum-Firmware/tree/dev/applications/main/infrared/resources/infrared/assets).

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
