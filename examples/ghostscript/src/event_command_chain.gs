-- Run a Wi-Fi scan, inspect results, then run a follow-up command.

ghost.ui.set_title("Scanning")

if not ghost.wifi.scan_start() then
    print("scan_start failed")
    return
end

local started = ghost.system.uptime_ms()
while not ghost.wifi.scan_done() do
    if ghost.system.uptime_ms() - started > 15000 then
        print("scan timeout")
        ghost.wifi.scan_stop()
        break
    end
    ghost.delay(500)
end

local count = ghost.wifi.scan_finish()
print("scan finished: " .. count .. " APs")

local count = ghost.results.count("wifi.ap") or 0
print("cached APs: " .. count)

if count > 0 then
    local bssid = ghost.results.field("wifi.ap", 0, "bssid")
    if bssid then
        print("strongest AP: " .. bssid)
    end
end
