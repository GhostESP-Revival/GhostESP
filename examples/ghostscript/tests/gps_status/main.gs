-- Prints one GPS and power snapshot without waiting for a fix.
ghost.ui.set_title("GPS status")
print("GPS available: " .. tostring(ghost.gps.is_available()))
print("GPS fix: " .. tostring(ghost.gps.has_fix()))
print("satellites: " .. ghost.gps.satellites())
if ghost.gps.has_fix() then
    print(string.format("position: %.6f, %.6f", ghost.gps.latitude(), ghost.gps.longitude()))
end
print("battery: " .. ghost.power.percent() .. "% (" .. ghost.power.voltage_mv() .. " mV)")
ghost.ui.toast("GPS status complete")
