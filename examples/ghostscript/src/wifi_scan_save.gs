-- Scan APs, print results, and save a CSV in script-scoped storage.

ghost.ui.set_title("Wi-Fi scan")
print("starting Wi-Fi scan")

if not ghost.wifi.scan_start() then
    print("scan_start failed")
    ghost.ui.set_title("Scan failed")
    return
end

local started = ghost.system.uptime_ms()
while not ghost.wifi.scan_done() do
    local elapsed = ghost.system.uptime_ms() - started
    ghost.ui.set_title("Scanning " .. (elapsed // 1000) .. "s")
    if elapsed > 20000 then
        print("scan timeout")
        ghost.wifi.scan_stop()
        break
    end
    ghost.delay(500)
end

local count = ghost.wifi.scan_finish()
print("scan finished: " .. count .. " APs")

-- Results stay in firmware-owned memory. save_csv writes the current AP cache
-- directly to storage without building a Lua table of all APs.
local saved = ghost.results.save_csv("wifi.ap", "wifi_scan.csv")

for i = 0, count - 1 do
    local bssid = ghost.results.field("wifi.ap", i, "bssid") or "?"
    local ssid = ghost.results.field("wifi.ap", i, "ssid") or "(hidden)"
    local ch = ghost.results.field("wifi.ap", i, "channel") or 0
    local rssi = ghost.results.field("wifi.ap", i, "rssi") or 0
    if ssid == "" then ssid = "(hidden)" end
    print(tostring(i + 1) .. " " .. bssid .. " " .. ssid .. " ch=" .. ch .. " rssi=" .. rssi)
    if i % 4 == 3 then collectgarbage("collect") end
end

print(saved and "saved wifi_scan.csv" or "failed to save wifi_scan.csv")

ghost.ui.set_title("Saved " .. count .. " APs")
