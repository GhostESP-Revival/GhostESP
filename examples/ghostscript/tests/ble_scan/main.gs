-- Starts a short BLE scan and confirms host-backed results are readable.
ghost.ui.set_title("BLE smoke")
print("starting BLE scan")
if not ghost.ble.scan_start() then
    print("FAIL: BLE scan could not start")
    return
end

ghost.delay(8000)
ghost.ble.scan_stop()

local count = ghost.results.count("ble.device") or 0
print("BLE devices: " .. count)
for i = 0, math.min(count, 10) - 1 do
    local mac = ghost.results.field("ble.device", i, "mac") or "?"
    local name = ghost.results.field("ble.device", i, "name") or ""
    print(mac .. " " .. name)
end
if ghost.results.save_csv("ble.device", "ble_smoke.csv") then
    print(count > 0 and "PASS: saved ble_smoke.csv" or "PASS: no BLE devices found; saved header-only CSV")
else
    print("FAIL: CSV save failed")
end
