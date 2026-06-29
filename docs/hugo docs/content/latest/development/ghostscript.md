---
title: "GhostScript"
description: "Run low-memory Lua scripts on GhostESP, including devices without PSRAM."
weight: 31
toc: true
---

GhostScript is GhostESP's embedded Lua scripting system. It is designed for devices without PSRAM and does not use the native SD app ELF loader.

Scripts live under:

```text
/mnt/ghostesp/scripts/
```

Script data is scoped under:

```text
/mnt/ghostesp/scriptdata/<script_id>/
```

## Script Format

GhostScript runs precompiled Lua chunks only:

- `.gsb` binary Lua chunk

Author `.gs` source on your computer, compile it to `.gsb` with `ghostbt`, then save the `.gsb` file under `/mnt/ghostesp/scripts/`. The device intentionally does not load text `.gs` scripts to keep runtime RAM and firmware size lower.

Source example:

```lua
ghost.ui.toast("Hello from GhostScript")
ghost.print("heap: ", ghost.system.free_heap())
```

## Folder Script

Folder scripts use `manifest.json`:

```json
{
  "id": "wifi_scan",
  "name": "WiFi Scan",
  "version": "1.0.0",
  "entry": "main.gsb",
  "permissions": ["ui", "wifi", "storage", "system"],
  "memory_limit": 24576,
  "timeout_ms": 30000,
  "instruction_budget": 10000
}
```

## Sandbox

GhostScript uses real Lua with a stripped standard library and a strict allocator. `io`, `os`, `debug`, and dynamic module loading are not opened. GhostESP APIs are exposed through the `ghost` table and are permission checked.

Available modules include `ghost.ui`, `ghost.event`, `ghost.input`, `ghost.system`, `ghost.storage`, `ghost.wifi`, `ghost.ble`, `ghost.gps`, `ghost.oui`, `ghost.power`, `ghost.nfc`, `ghost.time`, `ghost.rgb`, `ghost.badusb`, `ghost.ir`, `ghost.subghz`, `ghost.net`, `ghost.settings`, `ghost.commands`, `ghost.parser`, `ghost.results`, and `ghost.tasks`.

Native SD apps and GhostScript share the same permission names, but GhostScript receives sandboxed Lua wrappers instead of the raw native C SDK struct.

## Long Running Scripts

Long-running scripts are supported when they yield periodically. `timeout_ms` is enforced per Lua execution slice, not across the whole script lifetime. This protects against tight infinite loops while allowing cooperative loops.

Good:

```lua
while true do
  ghost.wifi.scan_start()
  while not ghost.wifi.scan_done() do
    ghost.delay(250)
  end
  local count = ghost.wifi.scan_finish()
  ghost.print("APs: ", count)
  ghost.delay(5000)
end
```

Bad, this will time out:

```lua
while true do
end
```

`on_tick(elapsed_ms)`, `on_input(event)`, and event handlers are also protected by the same per-slice timeout. Keep callbacks short and return quickly.

## Storage

Scripts with the `storage` permission can save files under their scoped data directory:

```text
/mnt/ghostesp/scriptdata/<script_id>/
```

Example:

```lua
ghost.storage.write("scan.txt", "hello\n")
ghost.storage.append("scan.txt", "more\n")
local data = ghost.storage.read("scan.txt")
```

Storage paths must be relative. Absolute paths and `..` path traversal are rejected.

## Chaining Scans And Actions

Scripts can run scans, inspect results, save data, and run follow-up actions. For example, a Wi-Fi script can call `ghost.wifi.scan_start()`, wait for `ghost.wifi.scan_done()`, call `ghost.wifi.scan_finish()`, inspect AP fields with `ghost.results.field("wifi.ap", i, "ssid")`, then save interesting results or call `ghost.wifi.deauth(...)` when permitted.

For large result sets, prefer `ghost.results` instead of table-returning APIs. `ghost.results.count(kind)`, `ghost.results.field(kind, i, field)`, and `ghost.results.save_csv(kind, path)` keep full result caches in firmware-owned memory and only marshal one field at a time into Lua. Current providers include `wifi.ap`, `ble.device`, and `ble.detect`; more scan/capture types can be added as firmware-side providers without changing the Lua API shape.

Command output is available as `command.log`. It is a per-script RAM ring buffer filled during `ghost.commands.start(...)`, not a read of the global serial terminal log. Scripts can inspect it line-by-line without saving it to SD.

The global terminal/serial log is available as `log.serial`. It is read-only and backed by the existing terminal log storage; scripts read one line at a time and do not duplicate the whole log buffer.

Parser helpers are exposed through:

```lua
ghost.parser.nfc_summary(path)
ghost.parser.ir_summary(path)
ghost.parser.subghz_summary(path)
```

These use app-relative paths and require the matching `nfc`, `ir`, or `subghz` permission.

## Failure State

GhostScript records the last failure in:

```text
/mnt/ghostesp/scriptdata/<script_id>/.state.json
```

Failures are informational only. Scripts are not quarantined or blocked from launching.
