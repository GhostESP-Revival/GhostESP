-- Save BLE scan results through host-backed result providers.
-- Depending on firmware mode, either ble.device or ble.detect may be populated.

ghost.ui.set_title("BLE results")
print("starting BLE scan")

if not ghost.ble.scan_start() then
    print("BLE scan failed to start")
    return
end

ghost.delay(8000)
ghost.ble.scan_stop()

local device_count = ghost.results.count("ble.device") or 0
local detect_count = ghost.results.count("ble.detect") or 0
print("ble.device count: " .. device_count)
print("ble.detect count: " .. detect_count)

if device_count > 0 then
    ghost.results.save_csv("ble.device", "ble_devices.csv")
    for i = 0, device_count - 1 do
        print((ghost.results.field("ble.device", i, "mac") or "?") .. " " ..
              (ghost.results.field("ble.device", i, "name") or "") .. " rssi=" ..
              tostring(ghost.results.field("ble.device", i, "rssi") or 0))
    end
end

if detect_count > 0 then
    ghost.results.save_csv("ble.detect", "ble_detect.csv")
    print("saved ble_detect.csv")
end

ghost.ui.set_title("BLE saved")
