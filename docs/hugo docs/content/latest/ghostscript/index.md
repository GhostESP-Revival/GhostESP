---
title: "GhostScript"
description: "Write Lua scripts that chain CLI commands, react to events, and automate multi-step workflows on GhostESP."
weight: 620
toc: true
aliases:
  - "/development/ghostscript/"
---

## What is GhostScript?

GhostScript is a tiny Lua 5.4 runtime that runs `.gsb` precompiled Lua chunks from the SD card. Scripts can call existing APIs and CLI commands, read output, subscribe to events, and react to input, letting you chain commands and automate multi-step workflows without flashing new firmware.

You author `.gs` source on your computer, compile it to `.gsb` with `ghostbt`, then launch the `.gsb` from the **Apps** menu or the CLI. The device intentionally does not load text `.gs` scripts to keep runtime RAM and firmware size lower.

Choose GhostScript for portable automation and event-driven workflows. If you need custom native widgets, lower-level hardware APIs, or C performance, compare it with [Native SD Apps]({{< relref "development/native-sd-apps" >}}).

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
   gbt script compile hello.gs --out hello/hello.gsb
   ```

5. Copy the complete `hello/` directory to `/mnt/ghostesp/scripts/`.

6. Launch it from **Main menu → Apps → GhostScript**, then select `hello`, or use the CLI commands below.

> The device accepts `.gsb` bytecode only. Keep `.gs` files as source in your repo or tooling workspace, not as deployable scripts on the device.

> Copy the complete folder, including `manifest.json`. A standalone `.gsb` receives no permissions and cannot call permission-gated APIs.

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
| `input`      | `subscribe(topic, fn)`, `unsubscribe(topic, fn)`: aliases for event subscription; use topic `"input"` for all input. |
| `system`     | `free_heap`, `free_internal_heap`, `uptime_ms`, `memory_used`, `memory_limit`, `firmware_version`, `target`, `reboot`, `random` (requires `random` permission). |
| `storage`    | `read`, `write`, `append`, `delete`, `mkdir`, `list`, `stat`, `rename`, `exists`, bounded logical streams. |
| `capabilities` | `has_permission(name)`, `has_feature(name)`: inspect manifest authorization and host support. |
| `wifi`       | `scan_start`, `scan_stop`, `scan_done`, `scan_finish`, `ap_count`, `ap(i)`, `connect`, `disconnect`, `is_connected`, `rssi`, `ip`, `set_channel`, `get_channel`, `deauth`, `on_ap`, `station_scan_start`, `station_scan_stop`, `station_count`, `station(i)`. |
| `ble`        | `scan_start`, `scan_stop`, `device_count`, `get_device(i)`, `on_device`.     |
| `gps`        | `is_available`, `has_fix`, `latitude`, `longitude`, `altitude`, `satellites`, `on_fix`. |
| `power`      | `percent`, `voltage_mv`, `is_charging`, `get_brightness`, `set_brightness`.  |
| `nfc`        | `is_available`, `scan_start`, `stop`, `scan_active`, `read(max_bytes)`, `write_ndef(message)`, `last_tag`. Type-2 NDEF only. |
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

Permissions for each subtable are set in the script manifest. A script without the right permission gets a runtime error when it calls the API. Use `wifi` for scanning and results; operations that change radio state or transmit frames, including `ghost.wifi.deauth`, require `wifi_control` as well.

### Storage Streams

Scripts with `storage` permission can use bounded logical streams for files that do not fit in the normal `storage.read` buffer. A stream never keeps the SD card mounted or a file descriptor open between calls, so it is safe on JIT-SD targets.

```lua
local out = assert(ghost.storage.open("capture.bin", "w"))
assert(ghost.storage.stream_write(out, chunk_a) == #chunk_a)
assert(ghost.storage.stream_write(out, chunk_b) == #chunk_b)
ghost.storage.close(out)

local input = assert(ghost.storage.open("capture.bin", "r"))
while true do
    local chunk, err = ghost.storage.stream_read(input, 1024)
    assert(not err, err)
    if not chunk then break end
    -- Process chunk.
end
ghost.storage.close(input)
```

`open` accepts `"r"`, `"w"`, or `"a"`; `"w"` truncates and `"a"` starts at the existing end. Reads and writes are limited to 1024 bytes per call and writes are flushed before they return. Paths remain app-scoped, reject absolute paths, backslashes, traversal, and hidden files. `close` is idempotent, and runtime teardown invalidates every stream handle. Use `ghost.capabilities.has_feature("storage_stream")` to feature-detect the API and `has_permission("storage")` to check authorization.

### Type-2 NFC

With the `nfc` permission and a local PN532 or ST25R3916 reader, `ghost.nfc.scan_start()` begins an isolated Type-2 NDEF scan. Poll `scan_active()` and use `read(max_bytes)` after it finishes; the result includes the UID, model, user capacity, NDEF bytes, and read-only, password, and lock state. `write_ndef(message)` writes a bounded NDEF message only to the same tag that was scanned, rechecks its UID immediately before writing, and refuses read-only, password-protected, or locked tags.

This facade intentionally excludes MIFARE authentication, key recovery, nonce collection, raw page access, emulation, and non-Type-2 protocols. Call `stop()` when finished to release the reader. A successful scan emits the compact `nfc_tag` event (`model|uid`); read the tag object for complete metadata and NDEF bytes.

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

-- wait for an event in the current coroutine (must be inside ghost.delay or on_tick)
local result = ghost.event.wait("ble_device", 5000)  -- 5s timeout
if result then
    print("got ble device: " .. tostring(result.value))
end
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

GhostScript runs one Lua state per script. Use `ghost.event.wait()` or
`ghost.delay()` for cooperative background work; both yield the current
execution context without blocking the UI. The `coroutine` standard library
is not available, but `ghost.delay` and `ghost.event.wait` use Lua's yield
mechanism internally. Do not create FreeRTOS tasks from a script.

## Input

`ghost.input.subscribe("input", fn)` is the same as `ghost.event.on("input", fn)`. Pass the same topic and function to `ghost.input.unsubscribe("input", fn)`. The event table includes:

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

Available permissions include `wifi`, `wifi_control`, `ble`, `nfc`, `ir`, `subghz`, `badusb`, `rgb`, `storage`, `ui`, `commands`, `settings`, `network`, `power`, `display`, `time`, `random`, `system`, `tasks`, `gpio` / `raw_gpio`, `lvgl`, `uart` / `serial`, `i2c`, `spi`, `adc`, `pwm`, `input` / `buttons`, `camera`, `usb`, `ethernet` / `eth`, `audio` / `mic`, and `zigbee`. Unknown permission names grant nothing, so a typo normally appears later as `permission denied`.

## Examples

The repo ships source examples in `examples/ghostscript/src/`. Each needs a folder manifest that declares its required permissions; standalone `.gsb` files intentionally have no permissions. The packages under `examples/ghostscript/tests/` are complete, deployable examples with manifests. Compile the source in place, then copy the complete folder to the SD card.

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

Useful for unattended captures where you only need one 4-way. The script starts `capture -eapol`, waits for `handshake_captured`, and stops capture as soon as one is seen.

```lua
local TARGET_BSSID = nil  -- nil = any BSSID; or pin to a specific MAC

ghost.commands.start("capture -eapol")

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

## Runtime environment

GhostScript uses Lua 5.4 in 32-bit mode (`LUA_32BITS=1`): integers are 32-bit and floats are 32-bit. This halves memory usage per Lua value compared to 64-bit mode. For hardware scripting (GPIO, radio channels, RSSI, timestamps) this is fully adequate. GPS coordinates retain ~7 digits of precision.

Available standard libraries:

| Library | Status |
|---------|--------|
| `base` | Loaded (`print`, `tostring`, `type`, `pcall`, etc.) |
| `string` | Loaded (`string.format`, `string.sub`, `string.byte`, `string.rep`, `string.find`, `string.match`, `string.gsub`, etc.) |
| `table` | Loaded (`table.insert`, `table.remove`, `table.sort`, `table.concat`, etc.) |
| `math` | Lite subset: `math.abs`, `math.floor`, `math.ceil`, `math.max`, `math.min`, `math.random`, `math.randomseed` |
| `coroutine` | **Not available.** Use `ghost.delay()` and `ghost.event.wait()` for cooperative yielding. |
| `io`, `os`, `debug`, `package`, `utf8` | **Not available.** Use `ghost.storage` for file I/O. |

## Limits and gotchas

- **Bytecode is limited to 8 KiB per `.gsb` file.** Split the workflow or reduce generated code if the compiled chunk exceeds the limit.
- **Single task, cooperative yielding.** The script runs on one FreeRTOS task. `ghost.delay` and `ghost.event.wait` yield execution without blocking the UI, but long-running CLI commands still block the script task while they execute. The `coroutine` standard library is not loaded; use `ghost.delay` / `ghost.event.wait` for cooperative work.
- **No `require()` of other `.gsb` files.** Each script is a single chunk. Copy helpers between scripts for now.
- **Most event rates are unbounded.** `command.output` is throttled to one event per 10 ms, but topics such as `ble_device` can still arrive quickly. Keep handlers short and add filtering or throttling where needed.
- **`ble_device` is deduped per scan.** You get one event per MAC per `ghost.ble.scan_start()`. Use a set in your handler if you need to track across scans.
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
- **Command log buffer is 640 bytes** (8 lines × 80 characters); older lines are trimmed. If you need a full log, pipe a CLI command that already writes to a file (e.g. `capture -eapol`) instead of capturing output in Lua.
- **No permission prompt at runtime.** If a script doesn't have a permission, the API call errors with `permission denied`. Check the manifest before shipping.
- **Wi-Fi and BLE share one radio on most ESP32s.** Starting a BLE scan mid-Wi-Fi-scan will tear down Wi-Fi. Drive them sequentially from the same script.

## Compiling and copying with `ghostbt`

```bash
# compile a single file
gbt script compile hello.gs

# compile all bundled source examples
gbt script compile examples/ghostscript/src --out examples/ghostscript/dist

# compile and copy an unprivileged standalone script to the default mount
gbt script compile hello.gs --deploy

# choose the scripts directory when the SD card has another mount point
gbt script deploy hello.gs --deploy-dir E:\ghostesp\scripts
```

`ghostbt` bundles Lua 5.4.7 plus a `luac` that matches the firmware ABI. On Windows, macOS, and Linux the binary is included; if it is missing on macOS/Linux, `ghostbt` builds `luac` from the bundled Lua 5.4.7 source using the system C compiler.

The current `--deploy` and `script deploy` commands copy compiled `.gsb` files only. They do not copy a package manifest. Use them only for unprivileged standalone scripts. For a script that needs permissions, compile with `--out <package>/<entry>.gsb`, then copy the complete package directory containing both the manifest and bytecode.
