-- Long-running GPS and power logger.
-- Requires gps/system, power, and storage permissions.

ghost.ui.set_title("GPS tracker")
if not ghost.storage.exists("gps.csv") then
    assert(ghost.storage.append("gps.csv", "uptime_ms,fix,lat,lon,sats,battery_pct,battery_mv\n"), "header write failed")
end

while true do
    local uptime = ghost.system.uptime_ms()
    local fix = ghost.gps.has_fix()
    local lat = fix and ghost.gps.latitude() or 0
    local lon = fix and ghost.gps.longitude() or 0
    local sats = ghost.gps.satellites()
    local pct = ghost.power.percent()
    local mv = ghost.power.voltage_mv()

    local line = string.format("%d,%s,%.6f,%.6f,%d,%d,%d\n", uptime, tostring(fix), lat, lon, sats, pct, mv)
    if not ghost.storage.append("gps.csv", line) then
        print("storage append failed")
        return
    end
    print(line)

    ghost.delay(5000)
end
