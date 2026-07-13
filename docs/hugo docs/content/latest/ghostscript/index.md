---
title: "GhostScript"
description: "Write Lua scripts that chain CLI commands, react to events, and automate multi-step workflows on GhostESP."
weight: 100
toc: true
---

## What is GhostScript?

GhostScript is a tiny Lua 5.4 runtime that runs `.gsb` precompiled Lua chunks from the SD card. Scripts can call existing APIs and CLI commands, read output, subscribe to events, and react to input, letting you chain commands and automate multi-step workflows without flashing new firmware.

You author `.gs` source on your computer, compile it to `.gsb` with `ghostbt`, then launch the `.gsb` from the **Apps** menu or the CLI. The device intentionally does not load text `.gs` scripts to keep runtime RAM and firmware size lower.

## Quickstart

1. Install the helper:

   ```bash
   pip install ghostbt
   ```

2. Create a script directory with a manifest. Create `hello/manifest.json`:

   ```json
   {
     "id": "hello",
     "name": "Hello",
     "entry": "hello.gsb",
     "permissions": ["ui"]
   }
   ```

3. Author `hello.gs`:

   ```lua
   print("firmware " .. ghost.system.firmware_version())
   ghost.ui.toast("hello from lua")
   ghost.delay(1000)
   ```

4. Compile it into the script directory:

   ```bash
   python -m ghostbt script compile hello.gs --out hello/hello.gsb
   ```

5. Copy the complete `hello/` directory to `/mnt/ghostesp/scripts/`.

6. Launch it from **Main menu → Apps → GhostScript**, then select `hello`, or use the CLI commands below.

> The device accepts `.gsb` bytecode only. Keep `.gs` files as source in your repo or tooling workspace, not as deployable scripts on the device.

## Device CLI

Use the terminal or serial CLI to discover and control scripts without opening the browser:

```text
script list
script run <index>
script status
script stop
```

`script list` recursively finds standalone `.gsb` files and directories containing a `manifest.json`, then prints stable indexes. Use an index from that output with `script run <index>`. Only one script can run at a time.

On devices with a screen, `script run` opens the GhostScript runner view. On headless devices, it starts a background task and writes script output to the terminal/serial log. `script stop` requests that active script stop; use `script status` to confirm it has become idle.

## Lifecycle of a script

A script has three phases:

1. **Top-level chunk:** runs once when you launch the script. Use it for setup, `ghost.event.on(...)` subscriptions, and starting coroutines.
2. **`on_tick(ms)`:** called every ~100 ms by the runner while the script is alive. Use it for polling, time-based logic, and event-driven state machines. If `on_tick` is defined, the runtime stays alive after the top-level chunk finishes.
3. **Teardown:** when you call `ghost.exit()` or the script task is destroyed, all coroutines die, all event listeners are cleared, and any open SD files are closed.

`ghost.delay(ms)` blocks the script (not the UI) for up to 60 seconds and yields the current chunk if you are in a coroutine.

## The `ghost` API

All API calls live under the `ghost` table. Subtables are lazy-loaded on first access.

| Subtable     | What it does                                                                 |
|--------------|------------------------------------------------------------------------------|
| `print`      | Write to the runner output buffer (also global `print`).                     |
| `delay`      | Sleep up to 60 s. Yields coroutines.                                        |
| `exit`       | Request script stop.                                                         |
| `ui`         | `toast`, `set_title`, `screen_width`, `screen_height`.                       |
| `event`      | `on`, `off`, `emit`, `wait`: pub/sub between scripts and firmware.           |
| `input`      | `subscribe`, `unsubscribe`: receive joystick/touch/encoder/keyboard.         |
| `system`     | `free_heap`, `uptime_ms`, `memory_used`, `memory_limit`, `firmware_version`, `target`, `reboot`, `random`. |
| `storage`    | `read`, `write`, `append`, `delete`, `mkdir`, `list`, `stat`, `rename`, `exists`. |
| `wifi`       | `scan_start`, `scan_stop`, `ap_count`, `ap(i)`, `connect`, `disconnect`, `is_connected`, `rssi`, `ip`, `set_channel`, `get_channel`, `deauth`, `on_ap`, `station_scan_start`, `station_scan_stop`, `station_count`, `station(i)`. |
| `ble`        | `scan_start`, `scan_stop`, `device_count`, `get_device(i)`, `on_device`.     |
| `gps`        | `is_available`, `has_fix`, `latitude`, `longitude`, `altitude`, `satellites`, `on_fix`. |
| `power`      | `percent`, `voltage_mv`, `is_charging`, `get_brightness`, `set_brightness`.  |
| `nfc`        | `is_available`, `last_tag`.                                                  |
| `ir`         | `send_file`, `listen(timeout_ms)`, `stop`.                                 |
| `subghz`     | `load`, `transmit`, `receive(timeout_ms, freq)`, `read_raw`, `stop`.       |
| `badusb`     | `run`, `stop`.                                                               |
| `rgb`        | `set(r, g, b)`.                                                             |
| `net`        | `http_get`, `http_post`.                                                     |
| `time`       | `unix`, `set_unix`.                                                          |
| `settings`   | `get_u8/set_u8`, `get_string/set_string`, `save`, NVS accessors.             |
| `commands`   | `exec(line)` (one-shot), `start(line)` (streams output as `command.output` events). |
| `parser`     | `nfc_summary(path)`, `ir_summary(path)`, `subghz_summary(path)`.          |
| `results`    | Host-backed result access: `count`, `field`, `save_csv`. Providers: `wifi.ap`, `ble.device`, `ble.detect`, `command.log`, `log.serial`. |
| `oui`        | `lookup(mac)`, `prefix_match(mac, prefix)`, `prefix_set(prefix1, ...)`.     |

Permissions for each subtable are set in the script manifest. A script without the right permission gets a runtime error when it calls the API.

## Chaining CLI commands

The killer feature: scripts can call any CLI command, capture its output as events, and decide what to do next based on what came back.

```lua
ghost.event.on("command.output", function(line)
    if line:find("BSSID") then return end  -- skip header
    print("cli> " .. line)
end)

ghost.event.on("wifi_scan_done", function(value)
    print("scan finished, count=" .. tostring(value))
end)

print("starting scanap")
ghost.commands.start("scanap")
ghost.delay(5000)
print("stopping scan")
ghost.commands.start("stopscan")
ghost.delay(500)

local count = ghost.results.count("wifi.ap") or 0
for i = 0, count - 1 do
    print("  " ..
        (ghost.results.field("wifi.ap", i, "bssid") or "?") .. "  " ..
        (ghost.results.field("wifi.ap", i, "ssid") or "?") .. "  ch=" ..
        tostring(ghost.results.field("wifi.ap", i, "channel") or 0) .. "  rssi=" ..
        tostring(ghost.results.field("wifi.ap", i, "rssi") or 0))
end
```

Things to know:

- `ghost.commands.start(line)` runs the command and emits every line of output as a `command.output` event with the line as the value.
- `ghost.commands.exec(line)` runs the command but discards output (use it for fire-and-forget commands like `reboot`).
- Commands run synchronously on the same script task; while a command is running, your script does not get ticks. Use `ghost.delay` if you need to give a long-running command time to emit.
- Prefer `ghost.results` for large scan outputs. It keeps full result sets in firmware-owned memory instead of materializing Lua tables for every row.
- The full CLI surface is available. See the [CLI reference]({{< relref "getting-started/command-line-reference.md" >}}).

## Host-Backed Results

`ghost.results` is the generic way to read scan/capture caches without copying the full result set into Lua memory.

Available providers:

| Kind | Fields |
| --- | --- |
| `wifi.ap` | `bssid`, `ssid`, `channel`, `rssi`, `auth` |
| `ble.device` | `mac`, `name`, `rssi` |
| `ble.detect` | `mac`, `name`, `subtype`, `type`, `rssi`, `tracking` |
| `command.log` | `number`, `line` |
| `log.serial` | `number`, `line` |

Usage:

```lua
local count = ghost.results.count("wifi.ap") or 0
for i = 0, count - 1 do
    print(ghost.results.field("wifi.ap", i, "ssid") or "")
end
ghost.results.save_csv("wifi.ap", "wifi.csv")
```

Adding support for station scans, IR captures, NFC dumps, SubGHz captures, or other result sources should be done by adding a provider on the firmware side. The Lua API stays the same: `count`, `field`, and optional `save_csv`.

`command.log` is not the global serial terminal buffer. It is a per-script firmware RAM ring buffer populated only while `ghost.commands.start(...)` captures command output. Lua can read one line at a time with `ghost.results.field("command.log", i, "line")`. Saving it with `save_csv` is optional.

`log.serial` reads the existing terminal/global log line storage through a read-only accessor. It does not duplicate the full terminal buffer and does not require SD; each `field(...)` call copies only the requested line into Lua.

## Events

The event bus is shared by the firmware, the runner, and your script. There are two ways to react to events: `on/off/emit` for normal pub/sub, and `wait` for coroutine-style synchronization.

```lua
-- subscribe to an event
ghost.event.on("wifi_scan_done", function(value)
    print("scan done, count=" .. tostring(value))
end)

-- unsubscribe
ghost.event.on("wifi_scan_done", my_handler)   -- returns listener id
ghost.event.off("wifi_scan_done", my_handler)  -- pass the same function

-- emit a custom event
ghost.event.emit("my_event", "payload")

-- wait in a coroutine
local co = coroutine.create(function()
    local result = ghost.event.wait("ble_device", 5000)  -- 5s timeout
    if result then
        print("got ble device: " .. tostring(result.value))
    end
end)
coroutine.resume(co)
```

Built-in event topics:

| Topic              | Value                                                      | When                                        |
|--------------------|-------------------------------------------------------------|---------------------------------------------|
| `input`            | event type string                                           | any input event                             |
| `input.touch`      | `"touch"`                                                   | touch press                                 |
| `input.joy.<i>`    | `"joystick"`                                                | joystick direction `i`                      |
| `input.encoder`    | `"encoder"`                                                 | encoder turned                              |
| `input.encoder.click` | `"encoder"`                                              | encoder button pressed                      |
| `input.back`       | `"back"`                                                    | exit/back button                            |
| `input.key.<k>`    | `"keyboard"`                                                | key `k`                                     |
| `command.output`   | one line of CLI output                                      | any line from `commands.start`              |
| `wifi_scan_done`   | AP count (string)                                           | the Wi-Fi scan finished                     |
| `wifi_ap_found`    | `bssid\|channel\|rssi`                                       | a new AP was discovered during live scan    |
| `ble_device`       | `mac\|rssi`                                                 | a BLE advertisement was received            |
| `ble_scan_done`    | empty                                                       | the BLE scan finished                       |
| `gps_fix`          | `yes\|lat\|lon\|alt\|sats` / `no\|lat\|lon\|alt\|sats`       | a GPS fix was lost or regained              |
| `gps_update`       | same as above                                               | a new fix arrived                            |
| `handshake_captured` | `bssid\|m1/m2`                                           | a WPA 4-way handshake was captured          |
| `pmkid_exported`   | `pmkid_count\|handshake_count`                              | an hc22000 export finished                  |
| `capture_started`  | `name\|type`                                                | a pcap capture was opened                   |
| `capture_stopped`  | full path of the saved pcap                                 | a pcap capture was closed                   |
| `attack_started`   | attack name (`deauth`, `beacon`, …)                          | an attack was launched                      |
| `attack_stopped`   | attack name                                                 | an attack finished                          |
| `subghz_captured`  | sample count                                                | a SubGHz capture finished                   |
| `ir_signal`        | `protocol\|addr\|cmd` or `raw\|samples`                      | an IR signal was received                   |
| `nfc_tag`          | `card_label\|uid_hex`                                       | an NFC tag was read                         |
| `dns_request`      | `client_ip\|qname`                                          | the DNS sinkhole received a request         |
| `printer_job`      | `ok` / `failed`                                             | a printer send finished                     |
| `comm_command`     | `command\|data`                                             | a remote command was received over comm     |

Auto-subscribe helpers are exposed for the common cases. They are thin wrappers around `event.on`:

- `ghost.wifi.on_ap(fn, filter)`: `fn(value)` per scanned AP
- `ghost.ble.on_device(fn, filter)`: `fn(value)` per BLE device
- `ghost.gps.on_fix(fn, filter)`: `fn(value)` on fix change

All three accept an optional `filter` table. Filters reject events at the source so the handler is never called. Common keys:

| Filter key      | Applies to                 | Effect                                              |
|-----------------|----------------------------|-----------------------------------------------------|
| `min_rssi`      | `wifi_ap_found`, `ble_device` | drop events with `rssi < min_rssi`               |
| `bssid`         | `wifi_ap_found`             | only events whose value starts with this BSSID      |
| `mac`           | `ble_device`                | only events whose value starts with this MAC        |
| `channel`       | `wifi_ap_found`             | only events from this channel                       |
| `want_fix`      | `gps_update` / `gps_fix`    | only events whose first field is `yes` or `no`     |
| `match`         | any topic                   | value `string` substring (for `command.output`)    |
| `match_fn`      | any topic                   | function `(value) -> bool` for arbitrary filtering  |

```lua
ghost.wifi.on_ap(function(v) print("strong ap:", v) end,
                 { min_rssi = -60 })

ghost.event.on("command.output", function(line)
    print("hsk:", line)
end, { match = "Handshake found" })
```

If you pass both a builtin filter and a `match_fn`, the builtin runs first and the function only sees what passed.

### Rate limiting

To keep handlers from drowning the runner, the dispatcher throttles `command.output` to one event per 10 ms per topic. All other topics pass through; add your own throttle inside the handler if you need finer control.

### Coroutines

GhostScript runs one Lua state per script. Use standard Lua coroutines with
`ghost.event.wait()` or `ghost.delay()` for cooperative background work; do not
create FreeRTOS tasks from a script.

## Input

`ghost.input.subscribe(fn)` is the same as `ghost.event.on("input", fn)`. The event table includes:

```lua
{
    type = "joystick",  -- or "touch", "keyboard", "encoder", "back"
    index = 2,         -- for joystick, 0..4 (center, up, down, left, right)
    pressed = true,    -- for joystick
    direction = 1,     -- for encoder, -1 or 1
    button = false,    -- for encoder
    x = 0, y = 0,      -- for touch
    key = 27           -- for keyboard (LV key code)
}
```

## Manifests

Each script can be packaged in a directory containing `manifest.json` and its
entry `.gsb` file. A standalone `.gsb` has no permissions by default and uses
the target-clamped default Lua heap.

```json
{
  "id": "wifi-monitor",
  "name": "wifi-monitor",
  "entry": "wifi_monitor.gsb",
  "memory_limit": 32768,
  "instruction_budget": 50000,
  "timeout_ms": 60000,
  "permissions": ["wifi", "commands", "storage", "ui"],
  "storage_scope": "app"
}
```

Available permissions include `wifi`, `wifi_control`, `ble`, `nfc`, `ir`, `subghz`, `badusb`, `rgb`, `storage`, `ui`, `commands`, `settings`, `network`, `power`, `display`, `time`, `random`, and `system` (required for GPS access).

## Examples

The repo ships source examples in `examples/ghostscript/src/`. Each needs a folder manifest that declares its required permissions; standalone `.gsb` files intentionally have no permissions. Compile the source to the folder named in the manifest, then copy the complete folder to the SD card.

| File                       | What it shows                                                    |
|----------------------------|------------------------------------------------------------------|
| `hello.gs`                 | Small UI, print, system, and storage smoke test.                 |
| `wifi_scan_save.gs`        | Scan APs, print results, save CSV to scoped storage.             |
| `ble_results_save.gs`      | Save BLE result caches through `ghost.results`.                  |
| `serial_log_tail.gs`       | Read the global serial/terminal log without copying all of it.   |
| `long_running_loop.gs`     | Cooperative infinite loop with `ghost.delay()`.                   |
| `event_command_chain.gs`   | Run CLI commands, parse output, and trigger follow-up actions.   |
| `deauth_on_ssid.gs`        | Scan APs and deauth SSIDs matching a pattern.                    |
| `parser_summary.gs`        | Use parser helpers for NFC, IR, and SubGHz summaries.             |
| `gps_tracker.gs`           | Long-running GPS/power logger with scoped storage.               |

## Recipes

These are full workflows you can copy and adapt. Compile them with `gbt script compile` and drop the generated `.gsb` on the SD card.

### Deauth APs by SSID pattern

Live-scan for APs and deauth any whose SSID contains a target string. Useful for knocking down rogue APs or running a quick deauth loop against a specific tenant.

```lua
local TARGET = "CorpWifi"
local COOLDOWN_MS = 2000
local BURSTS = 5
local last = {}

ghost.event.on("wifi_ap_found", function(payload)
    local bssid = payload:match("([^|]+)")
    if not bssid then return end

    local now = ghost.system.uptime_ms()
    if (now - (last[bssid] or 0)) < COOLDOWN_MS then return end

    local count = ghost.results.count("wifi.ap") or 0
    for i = 0, count - 1 do
        local cached_bssid = ghost.results.field("wifi.ap", i, "bssid")
        local ssid = ghost.results.field("wifi.ap", i, "ssid") or ""
        if cached_bssid == bssid and ssid:lower():find(TARGET:lower(), 1, true) then
            last[bssid] = now
            print("deauth " .. bssid .. " ssid=" .. ssid)
            for _ = 1, BURSTS do ghost.wifi.deauth(bssid, 1) end
            break
        end
    end
end)

ghost.wifi.scan_start()
print("scanning for SSIDs matching " .. TARGET)
```

Tune `COOLDOWN_MS` and `BURSTS` to avoid hammering the radio; the per-BSSID cooldown is essential or you'll get a frame storm.

### Stop capture on first handshake

Useful for unattended captures where you only need one 4-way. The script starts `capture -handshake`, waits for `handshake_captured`, and stops capture as soon as one is seen.

```lua
local TARGET_BSSID = nil  -- nil = any BSSID; or pin to a specific MAC

ghost.commands.start("capture -handshake")

ghost.event.on("handshake_captured", function(payload)
    local bssid, pair = payload:match("([^|]+)|(.+)")
    if not bssid then return end
    if TARGET_BSSID and bssid:lower() ~= TARGET_BSSID:lower() then
        print("ignoring " .. bssid .. ", want " .. TARGET_BSSID)
        return
    end
    print("handshake: " .. bssid .. " pair=" .. pair)
    ghost.commands.start("capture -stop")
    ghost.delay(500)
    ghost.ui.toast("handshake saved")
    ghost.exit()
end)

-- safety net
ghost.delay(60000)
ghost.commands.start("capture -stop")
ghost.exit()
```

If you want to filter by AP, set `TARGET_BSSID` to the BSSID string. To capture every handshake on the channel, leave it nil and `ghost.exit()` on the first event.

## Limits and gotchas

- **Single task, multiple coroutines.** The script runs on one FreeRTOS task. `coroutine.create` works and `ghost.delay` / `ghost.event.wait` yield, but long-running commands still block the script task while they execute. A few coroutines is fine; thousands is not.
- **No `require()` of other `.gsb` files.** Each script is a single chunk. Copy helpers between scripts for now.
- **Event rate is unbounded.** Bridges like `command.output` and `ble_device` can fire thousands of times per second. If your handler is slow, the runner buffer grows and you may run out of Lua heap. Keep handlers short; defer work to a coroutine via `coroutine.create`.
- **`ble_device` is deduped per scan.** You get one event per MAC per `ghost.wifi.ble_scan_start`. Use a set in your handler if you need to track across scans.
- **Event values can contain `|`.** Topics that include user-controlled strings (DNS qname, NFC card label, IR protocol name) are escaped as `\p`, `\n`, `\r`, `\b`, or `\xNN`. Split on raw `|` and call `unescape()` on each field if you write a parser:

  ```lua
  local function unescape(s)
       return (s:gsub("\\p", "|"):gsub("\\b", "\\"):gsub("\\n", "\n"):gsub("\\r", "\r"))
  end
  local fields = {}
  for f in (value .. "|"):gmatch("(.-)|") do
      table.insert(fields, unescape(f))
  end
  ```
- **Output buffer is 4 KB**; older text is trimmed. If you need a full log, `commands.start` to a CLI command that already writes to a file (e.g. `capture -handshake`) instead of capturing in Lua.
- **No permission prompt at runtime.** If a script doesn't have a permission, the API call errors with `permission denied`. Check the manifest before shipping.
- **Wi-Fi and BLE share one radio on most ESP32s.** Starting a BLE scan mid-Wi-Fi-scan will tear down Wi-Fi. Drive them sequentially from the same script.

## Building and deploying with `ghostbt`

```bash
# compile a single file
python -m ghostbt script compile hello.gs

# compile all bundled source examples
python -m ghostbt script compile examples/ghostscript/src --out examples/ghostscript/dist

# compile and copy an unprivileged standalone script to the default mount
python -m ghostbt script compile hello.gs --deploy

# choose the scripts directory when the SD card has another mount point
python -m ghostbt script deploy hello.gs --deploy-dir E:\ghostesp\scripts
```

`ghostbt` bundles Lua 5.4.7 plus a `luac` that matches the firmware ABI. On Windows, macOS, and Linux the binary is included; if it is missing on macOS/Linux, `ghostbt` builds `luac` from the bundled Lua 5.4.7 source using the system C compiler.

## Limits and gotchas

- Scripts share one task per runner. Long-running CLI commands block the script task; use short `ghost.delay` calls if you need to interleave.
- Output buffer is 4 KB; older text is trimmed.
- The Lua heap is capped by `memory_limit` (default 16 KB on no-PSRAM, up to 192 KB on PSRAM). The allocator prefers PSRAM, falls back to internal RAM.
- BLE and Wi-Fi cannot run at the same time on most ESP32 variants. The runner does not start Wi-Fi or BLE itself. You call `scan_start` / `scan_stop` like you would from the CLI.
- `ghost.deauth(bssid)` and other destructive commands require the `wifi` permission. A script without the permission gets a clear runtime error, not a silent failure.
