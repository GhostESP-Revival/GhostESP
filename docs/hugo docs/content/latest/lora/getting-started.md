---
title: "LoRa First Run"
description: "Set up GhostESP LoRa and send your first message"
keywords: ["LoRa", "Meshtastic", "SX1262", "getting started", "Heltec"]
weight: 10
---

The short path: one region, one radio start, one message. Full syntax lives in the [command reference]({{< relref "commands.md" >}}).

## Before you start

- A LoRa board: Heltec V3/V3.2, a CrowPanel Advance, or a wired SX1262 ([hardware]({{< relref "hardware.md" >}})).
- Something to talk to: a second node, or the phone app.
- A way to type commands: serial console or the WebUI.

## 1. Set your region

Meshtastic will not transmit on an unknown band plan, so pick yours first:

```text
lora set region us915
```

All 24 region codes:

`us915`, `eu868`, `eu433`, `cn`, `jp`, `anz`, `kr`, `tw`, `ru`, `in`, `nz865`, `th`,
`ua433`, `my433`, `my919`, `sg923`, `ph433`, `ph868`, `ph915`, `anz433`, `kz433`,
`kz863`, `np865`, `br902`

The [command reference]({{< relref "commands.md" >}}) lists the frequency band and duty limit for each. An unknown name makes `lora set region` print the list again.

MeshCore skips this step: it uses the frequency you configure instead.

## 2. Start the radio

```text
lora start
```

You should see `SX1262 ready` and `chash 0x08`. A `SX1262 not detected` line means the radio is not wired, or the wrong board profile is flashed. See [Hardware]({{< relref "hardware.md" >}}).

You can start it from the device too: open **LoRa** and pick the first row, or double-press on the Heltec LoRa page.

## 3. See who is around

```text
lora nodes
```

Neighbouring nodes appear within 30-60 seconds with name, signal and hop count. A node with no name, or showing **Key unavailable**, has not sent its NodeInfo yet. Run `lora nodeinfo <node>` and wait for the reply.

## 4. Send a message

From the device: **LoRa → Messages → Public chat → Write**.

From the command line:

```text
lora chat hello mesh
```

## 5. Message one person

Direct messages are encrypted to the peer's public key, which arrives with NodeInfo:

```text
lora dm !e026f431 hello
```

If that is refused, request the key first and confirm it arrived:

```text
lora nodeinfo !e026f431
lora pkinfo !e026f431
```

## 6. Keep it on between reboots

```text
lora autostart meshtastic     # or: lora autostart meshcore
```

The radio starts a few seconds after the interface. Check what came up after a reboot with `mesh`.

## Use the phone app

Pair the official Meshtastic app over BLE so the mesh shows up like any other node. See [BLE App Link]({{< relref "ble-app.md" >}}).

## Prefer MeshCore?

Switch protocols and follow [MeshCore mode]({{< relref "meshcore.md" >}}):

```text
mesh switch meshcore
```

Only one protocol runs at a time.

## If something is off

| Symptom | Fix |
| --- | --- |
| `region is not set` | `lora set region <code>` |
| `SX1262 not detected` | Check the board profile and wiring ([hardware]({{< relref "hardware.md" >}})) |
| No nodes after a minute | Both ends need the same region, antennas fitted, and range |
| A node shows **Key unavailable** | `lora nodeinfo <node>`, wait, then `lora pkinfo <node>` |
| Messages stay on **Sending** | The peer is not acknowledging. Check it is powered and in range |

Next: [Command reference]({{< relref "commands.md" >}}) · [MeshCore mode]({{< relref "meshcore.md" >}}) · [Phone app]({{< relref "ble-app.md" >}})
