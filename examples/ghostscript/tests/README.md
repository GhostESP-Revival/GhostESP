# GhostScript On-Device Smoke Tests

Each folder is a deployable script package. Compile its `main.gs` in place, then copy the complete folder to `/mnt/ghostesp/scripts/`.

```bash
python -m ghostbt script compile examples/ghostscript/tests/runtime_smoke
python -m ghostbt script compile examples/ghostscript/tests/wifi_scan
python -m ghostbt script compile examples/ghostscript/tests/ble_scan
python -m ghostbt script compile examples/ghostscript/tests/gps_status
python -m ghostbt script compile examples/ghostscript/tests/input_events
```

Run `runtime_smoke` first. The hardware tests report unavailable hardware or no results without modifying radio configuration beyond starting a scan. `input_events` exits after it receives five input events; use the runner controls only after it has started.
