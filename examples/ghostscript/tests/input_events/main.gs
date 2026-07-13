-- Press joystick, encoder, touch, or keyboard controls five times to finish.
ghost.ui.set_title("Input smoke")
print("send five input events to this script")

local count = 0
ghost.input.subscribe("input", function(value)
    count = count + 1
    print("input " .. count .. ": " .. value)
    if count >= 5 then
        ghost.ui.toast("Input smoke passed")
        ghost.exit()
    end
end)
