# GhostScript Examples

GhostScript on-device execution is `.gsb` bytecode only. Keep `.gs` files as source, compile them with `ghostbt`, then copy generated `.gsb` files to `/mnt/ghostesp/scripts/` on the SD card.

Recommended workflow:

```bash
python -m ghostbt script compile examples/ghostscript/src/hello.gs --out examples/ghostscript/dist/hello.gsb
```

Deploy the compiled file:

```text
/mnt/ghostesp/scripts/hello.gsb
```

`src/` contains source examples. `dist/` is intentionally empty in the repo and is the suggested output folder for compiled bytecode.

Core examples:

| Source | Purpose |
| --- | --- |
| `hello.gs` | Smallest UI/print/storage smoke test. |
| `wifi_scan_save.gs` | Scan APs, print results, save CSV from host-backed results. |
| `ble_results_save.gs` | Save BLE result caches through `ghost.results`. |
| `serial_log_tail.gs` | Read the global serial/terminal log line-by-line. |
| `long_running_loop.gs` | Cooperative infinite loop using `ghost.delay()`. |
| `event_command_chain.gs` | Capture command output and chain a follow-up action. |
| `deauth_on_ssid.gs` | Scan, parse APs, and deauth matching SSIDs. |
| `parser_summary.gs` | Parser helper examples for NFC, IR, and SubGHz. |
| `gps_tracker.gs` | Long-running GPS/power logger with storage. |

Hardware-dependent examples should fail cleanly on devices without the required peripheral or permission.

Use `ghost.results` for scan/capture/log result sets. Providers such as `wifi.ap`, `ble.device`, `ble.detect`, `command.log`, and `log.serial` keep cached data outside the Lua heap and expose one field at a time.
