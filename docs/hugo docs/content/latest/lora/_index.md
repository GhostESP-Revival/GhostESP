---
title: "LoRa (Meshtastic & MeshCore)"
description: "Turn GhostESP into a LoRa mesh node and talk to Meshtastic or MeshCore"
keywords: ["LoRa", "Meshtastic", "MeshCore", "SX1262", "Heltec V3", "mesh"]
weight: 130
aliases:
  - "/lora/"
---

GhostESP turns an SX1262 radio into a mesh node. Only one of two protocols runs at a time:

| Protocol | Talks to | Commands |
| --- | --- | --- |
| **Meshtastic** (default) | stock Meshtastic nodes and the official phone app | `lora` |
| **MeshCore** | MeshCore nodes and its companion app | `meshcore` |

Interop is the point: GhostESP uses the same sync word, channels and wire encryption as Meshtastic firmware, so it joins an existing mesh without anything being reflashed.

## Where to go next

- **[First run]({{< relref "getting-started.md" >}})**: set a region, start the radio, send your first message
- **[MeshCore mode]({{< relref "meshcore.md" >}})**: use MeshCore instead of Meshtastic
- **[Command reference]({{< relref "commands.md" >}})**: every `lora`, `mesh` and `meshcore` command
- **[Phone app]({{< relref "ble-app.md" >}})**: pair the official Meshtastic app
- **[Hardware]({{< relref "hardware.md" >}})**: supported boards and wiring

## How you'll use it

| Where | What you do |
| --- | --- |
| **On the device** | Read and write messages, start/stop the radio, change settings |
| **Phone app** | The usual Meshtastic/MeshCore app experience over BLE |
| **CLI or WebUI** | First-time setup, boot auto-start, diagnostics, scripting |

## On the device

Open **LoRa** from the main menu. It opens on a status screen without starting the radio; the first row starts the selected protocol when the radio is off.

| Action | Touch | Encoder / buttons |
| --- | --- | --- |
| Select a row | Tap | Press |
| Move | Drag | Turn |
| Back | Back row | Esc or left |

- Conversations are grouped per channel and per contact, so channels stay separate.
- Outgoing messages show **Sending**, **Delivered**, **Sent** or **Failed**. Tap a failed one to reopen it in the composer and retry.
- **Encoder boards:** double-press on the status screen toggles the radio.
- **Heltec OLED:** press **PRG** to cycle pages. On the LoRa page a double-press toggles the radio, and with the radio off the page shows `PRESS 2X TO START`.

## Keep it running

Auto-start is opt in. The protocol is remembered, so you set it once:

```text
lora autostart meshtastic     # or: lora autostart meshcore
```

The radio starts a few seconds after the interface, once boot has finished. Meshtastic needs a saved region before it can transmit; MeshCore does not.

See [First run]({{< relref "getting-started.md" >}}) for the step-by-step version and [Command reference]({{< relref "commands.md" >}}) for the full syntax.
