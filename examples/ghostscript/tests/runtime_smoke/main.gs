-- Exercises UI, scoped storage, custom events, and event unsubscription.
ghost.ui.set_title("Runtime smoke")
print("target=" .. ghost.system.target())
print("firmware=" .. ghost.system.firmware_version())
print("heap=" .. ghost.system.memory_used() .. "/" .. ghost.system.memory_limit())

local marker = "GhostScript runtime smoke\n"
assert(ghost.storage.write("runtime_smoke.txt", marker), "storage write failed")
assert(ghost.storage.read("runtime_smoke.txt") == marker, "storage read mismatch")

local received = false
local handler = function(value)
    print("event received: " .. value)
    received = value == "ok"
end

ghost.event.on("runtime_smoke", handler)
ghost.event.emit("runtime_smoke", "ok")

function on_tick()
    if received then
        local removed = ghost.event.off("runtime_smoke", handler)
        print("PASS: storage, events, and off() (removed " .. removed .. ")")
        ghost.ui.toast("Runtime smoke passed")
        ghost.exit()
    end
end
