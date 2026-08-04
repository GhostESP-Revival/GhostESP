---
title: "BLE Bridge"
description: "Use a second ESP32 as a Bluetooth bridge between a main GhostESP device and the Android companion app."
weight: 110
---

The **BLE Bridge** turns one of your GhostESP devices into a wireless proxy that exposes a Bluetooth Low Energy (BLE) GATT service to the Android companion app. Commands you send from the phone are forwarded over the existing GhostLink UART link to a second, "main" ESP32, and the response is shipped back over BLE notifications.

This lets you control a GhostESP board that doesn't have a direct BLE link to your phone by using a second, BLE-capable board as a relay.

## Prerequisites

- Two GhostESP-flashed ESP32 boards wired together as a [GhostLink]({{< relref "dual-communication.md" >}}) pair.
- The **Android companion app** installed on your phone.
- Both boards powered and able to complete the GhostLink handshake.
- The bridge board must be BLE-capable. **ESP32-S2 is not supported** (it has no Bluetooth).

## How it works

```
Phone (Android app)        Bridge ESP32            Main ESP32
   |   BLE GATT             |   UART (GhostLink)    |
   |--- write CMD --------->|-------- packet ------>|
   |                        |                       | (runs CLI command)
   |<-- notify (ACK/DATA) --|<------- response -----|
```

- The **bridge board** advertises a BLE GATT service named `GhostESP Bridge` with three characteristics:
  - **RX** (write): framed commands from the phone.
  - **TX** (notify): framed responses sent back to the phone.
  - **CTRL** (write): a raw-text escape hatch. Writing `stop` shuts the bridge down.
- The **main board** runs the command on its own CLI. From its perspective, the bridge is just another GhostLink peer.
- All communication is framed (magic bytes `0x47 0x42`, version `0x01`, 12-byte header) and command responses are correlated by a per-command ID.
- The bridge's on/off state and the last paired peer name are persisted to NVS and restored on every boot.

## Wiring

Wire the two boards the same way you would for [GhostLink]({{< relref "dual-communication.md" >}}) (TX, RX, GND):

- **TX of bridge** → **RX of main** (GPIO 6 → GPIO 7 by default, 17 → 16 on base ESP32 models)
- **RX of bridge** → **TX of main** (GPIO 7 → GPIO 6 by default, 16 → 17 on base ESP32 models)
- **GND** → **GND**

Both devices need power. If you haven't already, configure custom pins with `commsetpins <TX_PIN> <RX_PIN>` before turning the bridge on.

## Setup

### 1. Confirm GhostLink is up

On either board, run `commstatus`. You should see `CONNECTED` and the peer's auto-generated name (e.g. `ESP_A1B2C3`). If they aren't paired, see the [GhostLink]({{< relref "dual-communication.md" >}}) guide.

### 2. Choose which board is the bridge

Pick one of the two boards to act as the BLE-facing bridge. The other is the "main" board that will run all commands.

### 3. Pair the bridge with the main board

On the **bridge board**, save the peer's name so the bridge knows who to connect to when the BLE link comes up:

```bash
blebridge pair ESP_A1B2C3
```

The name is persisted to NVS under `blebridge/peer`. You only need to do this once.

### 4. Turn the bridge on

Use either the on-screen menu (**DualComm → BLE → BLE Bridge**) or the CLI:

```bash
blebridge start
```

When the bridge is active:

- The OLED briefly shows `BLE Bridge`.
- The bridge board advertises as `GhostESP Bridge`.
- The bridge's Wi-Fi (AP and STA) is suspended while the bridge is running and restored when you stop it.

## CLI reference

```bash
blebridge              # Print human-readable status
blebridge start        # Persist enabled=true, start the bridge
blebridge stop         # Persist enabled=false, stop the bridge
blebridge status       # Print machine-friendly status (BLE state, peer, UART pins, current cmd id)
blebridge pair <name>  # Save a peer name (e.g. ESP_A1B2C3) for auto-reconnect
```

`blebridge stop` is also available over BLE by writing the literal text `stop` to the **CTRL** characteristic. Simply disconnecting the GATT link also tears the bridge down.

## Connecting from the Android app

1. Open the **GhostESP Companion** app.
2. Tap **Connect** on the dashboard (or open the **BLE** tab and tap the banner's **Connect** button).
3. The picker dialog opens on the **Wireless** tab. Grant the requested `BLUETOOTH_SCAN` / `BLUETOOTH_CONNECT` permissions (or `ACCESS_FINE_LOCATION` on Android 11 and below).
4. Pick `GhostESP Bridge` from the list. Devices are filtered by service UUID and sorted by signal strength.
5. The app subscribes to TX notifications, requests an MTU of 128, and is ready to use. The dashboard's status card shows "Connected" with a Bluetooth icon.

On subsequent launches, the app auto-reconnects to the last saved BLE device silently.

From the app, every screen works the same as if you were wired in over USB — WiFi scans, BLE scans, GPS, SD downloads, raw terminal commands, all of it. The bridge forwards the commands and streams the responses back as text.

## Auto-restore on reboot

If you enabled the bridge with `blebridge start` (or the menu toggle), the state is persisted to NVS (`blebridge/enabled`). On every boot the firmware reapplies it. If the main board isn't yet connected over GhostLink when the bridge starts, the firmware schedules a one-shot task that keeps retrying for about 30 seconds and fires `blebridge start` on the peer as soon as the UART link comes up.

## On-screen UI

- **DualComm → BLE → BLE Bridge**: tap to toggle the bridge on/off. The toggle reflects the current state. On ESP32-S2, the option shows a "Device does not support Bluetooth" message instead of starting.
- **Status display**: when the bridge starts, the OLED briefly shows `BLE Bridge On` (or `Off` when stopping).

## Limitations

- **ESP32-S2 is not supported** — it has no Bluetooth radio. The CLI registration, the menu item, and the bridge code itself are all gated to exclude S2.
- **Wi-Fi is suspended** while the bridge is running. The GhostNet AP and any STA connection are torn down when `blebridge start` runs and restored when the bridge stops. This is an inherent ESP32 BLE/Wi-Fi coexistence cost.
- **One command in flight at a time.** The bridge serializes each command/response round-trip; sending a new command waits for the previous one to go idle (200 ms of no DATA/END frames, or a 120 s safety cap).
- **The phone app does not write to the CTRL characteristic.** Stopping is done either by sending `blebridge stop` as a framed command or by simply disconnecting the GATT link, which the firmware treats as "stop the bridge".
- **The app cannot set the peer name.** You must run `blebridge pair <name>` on the bridge board itself; there's no UI in the app for that.

## Troubleshooting

- **Bridge doesn't appear in the phone's scan list**
  - Confirm the bridge is actually running: run `blebridge status` on the bridge board. The BLE line should show `ON` (or the equivalent state).
  - Make sure your phone's Bluetooth is on and that you've granted the `BLUETOOTH_SCAN` permission.
  - Move closer to the bridge board; the picker sorts by signal strength.
- **App shows "Connection Error" / "Tap to retry"**
  - The bridge board requires the **CTRL** characteristic to be present alongside RX/TX. If the bridge firmware on your board is out of date it may be exposing a stripped-down GATT service. Reflash the latest GhostESP on both boards.
  - Reboot both ESP32s and let GhostLink re-pair, then try again.
- **Commands succeed in the terminal but the structured buttons (Scan WiFi, Deauth, etc.) do nothing**
  - The app waits up to 5 seconds for the bridge's `ACK` after sending a command. If GhostLink is broken or slow, commands silently time out. Verify with `commstatus` on both boards.
  - Open the **Terminal** screen in the app to see the raw command stream and any timeout/error messages.
- **App auto-reconnect on launch fails silently**
  - The saved MAC may no longer be reachable (board powered off, out of range, MAC changed). The app quietly returns to "Disconnected"; re-open the connection dialog and pick the bridge again.
- **Want to forget the saved device**
  - The Android app does not currently expose a "forget device" button. Clear the app's data (Android Settings → Apps → GhostESP Companion → Storage → Clear data), or pick a different bridge to overwrite the saved entry.
- **Bridge stops unexpectedly**
  - Check whether GhostLink dropped (`commstatus`). The bridge relies on the UART link to reach the main board, so if GhostLink is down, commands can't be forwarded and the bridge effectively goes idle.
  - The bridge's Wi-Fi suspension is normal — it does not mean the bridge failed.

## Use cases

- **Wireless control of a hidden device**: tuck the main ESP32 out of sight (in a target area) and keep the bridge board on your desk connected to your phone.
- **Phones that can't reach a far board directly**: place the bridge closer to your phone and let it relay to a more distant main board over a wired UART link.
- **Mobile demos**: drive a multi-board setup from a phone with no extra cables to your device.

## Notes

- The bridge runs on top of the existing GhostLink transport. It is **not** a replacement for GhostLink — both must be working.
- The advertised name is fixed at `GhostESP Bridge` and is not user-configurable.
- Auto-discovery on the GhostLink side happens every 3 seconds; the bridge will reconnect to the saved peer automatically once it's back in range.
- All data crossing the BLE link is framed with the `GB` (0x47 0x42) magic bytes, version 0x01, and a 12-byte header — this is the same protocol the Android app implements. There is no encryption on the link.
- Persisted NVS keys: `blebridge/enabled` (auto-start on boot) and `blebridge/peer` (last paired peer name).

## See also

- [GhostLink (Dual Communication)]({{< relref "dual-communication.md" >}}) — the underlying UART peer link the bridge rides on.
- [Command Line Reference]({{< relref "command-line-reference.md" >}}) — full CLI documentation.
- [Supported Hardware]({{< relref "supported-hardware.md" >}}) — board compatibility list (note the ESP32-S2 exclusion).
