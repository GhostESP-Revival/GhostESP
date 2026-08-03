-- Starts a scan, prints up to ten APs, and stores the firmware result cache as CSV.
ghost.ui.set_title("Wi-Fi smoke")
print("starting Wi-Fi scan")
if not ghost.wifi.scan_start() then
    print("FAIL: Wi-Fi scan could not start")
    return
end

local started = ghost.system.uptime_ms()
while not ghost.wifi.scan_done() and ghost.system.uptime_ms() - started < 20000 do
    ghost.delay(250)
end

if not ghost.wifi.scan_done() then
    ghost.wifi.scan_stop()
    print("FAIL: Wi-Fi scan timed out")
    return
end

local count = ghost.wifi.scan_finish()
print("APs found: " .. count)
for i = 0, math.min(count, 10) - 1 do
    local ap = ghost.wifi.ap(i)
    if ap then print(ap.bssid .. " " .. ap.ssid .. " ch=" .. ap.channel .. " rssi=" .. ap.rssi) end
end
print(ghost.results.save_csv("wifi.ap", "wifi_smoke.csv") and "PASS: saved wifi_smoke.csv" or "FAIL: CSV save failed")
