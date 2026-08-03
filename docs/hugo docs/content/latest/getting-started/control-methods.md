---
title: "Connect to GhostESP"
description: "Choose the best way to control your GhostESP device."
weight: 30
toc: true
---

## What this does

GhostESP can be controlled from the device itself, a browser, a serial terminal, a Flipper Zero, or the desktop Control App. Choose the interface that matches the task you want to perform.

## Before you start

- GhostESP is flashed and has completed its first boot.
- You know which interface you want to use, or can start with the on-device UI or serial CLI.

## Choose a control method

| Method | Best for | What you need | Important limitation |
|---|---|---|---|
| On-device UI | Quick local use on supported display boards | A board with a display and supported input | Available menus depend on the board hardware. |
| WebUI | Settings, files, and terminal access from a phone or computer | Connect to `GhostNet`, then open `ghostesp.local` or `192.168.4.1` | The device cannot host its access point while using Wi-Fi or BLE locally. |
| Serial CLI | Full command access and diagnostics | USB data cable and a serial terminal at 115200 baud | Requires a wired connection. |
| Flipper Zero App | Portable control through a compatible Flipper setup | GhostESP Flipper app and wiring | Feature availability depends on the connected board. |
| Desktop Control App | Serial workflows and advanced customization | USB data cable and the desktop app | Requires a computer. |

## On-device steps

1. Open the main menu on a supported display board.
2. Choose the feature you want to use, such as **WiFi → Scanning**.
3. Follow the on-screen prompts and use the board's supported touch, keys, or encoder controls.

## CLI steps

1. Connect the board with a USB data cable.
2. Open a serial console at 115200 baud.
3. Send `help` and confirm that GhostESP prints a command list.

## Expected result

You can access a GhostESP menu or receive output from the `help` command. If you connect through the WebUI, `ghostesp.local` or `192.168.4.1` opens the control interface.

## Troubleshooting

- **`GhostNet` is missing:** Restart the board and confirm that flashing completed successfully.
- **The WebUI does not open:** Use `192.168.4.1` if `ghostesp.local` does not resolve.
- **The serial terminal is blank:** Send `help`; GhostESP can be quiet until a command runs. Confirm the port and 115200 baud rate.
- **A Wi-Fi or BLE command disconnects the WebUI:** This is expected on a single board. Use the on-device UI, serial CLI, or [GhostLink]({{< relref "dual-communication.md" >}}) for those radio tasks.

## Related tasks

- [WebUI guide]({{< relref "webui-guide.md" >}})
- [CLI reference]({{< relref "command-line-reference.md" >}})
- [Control App]({{< relref "../companion-tools/control-app.md" >}})
- [Try your first scan]({{< relref "first-scan.md" >}})
