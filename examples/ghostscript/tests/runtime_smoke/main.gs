-- Exercises UI, scoped storage, custom events, and event unsubscription.
ghost.ui.set_title("Runtime smoke")
print("target=" .. ghost.system.target())
print("firmware=" .. ghost.system.firmware_version())
print("heap=" .. ghost.system.memory_used() .. "/" .. ghost.system.memory_limit())

local marker = "GhostScript runtime smoke\n"
assert(ghost.storage.write("runtime_smoke.txt", marker), "storage write failed")
assert(ghost.storage.read("runtime_smoke.txt") == marker, "storage read mismatch")

assert(ghost.capabilities.has_feature("storage_stream"), "storage streams unavailable")
assert(ghost.capabilities.has_permission("storage"), "storage permission unavailable")
local writer = assert(ghost.storage.open("stream_smoke.txt", "w"))
assert(ghost.storage.stream_write(writer, "stream ") == 7, "stream write failed")
assert(ghost.storage.stream_write(writer, "ok\n") == 3, "stream write failed")
assert(ghost.storage.close(writer), "stream close failed")
local reader = assert(ghost.storage.open("stream_smoke.txt", "r"))
assert(ghost.storage.stream_read(reader, 1024) == "stream ok\n", "stream read mismatch")
assert(ghost.storage.stream_read(reader, 1024) == nil, "stream EOF mismatch")
assert(ghost.storage.close(reader), "stream close failed")

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
        print("PASS: storage streams, capabilities, events, and off() (removed " .. removed .. ")")
        ghost.ui.toast("Runtime smoke passed")
        ghost.exit()
    end
end
