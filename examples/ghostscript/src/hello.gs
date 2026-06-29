ghost.ui.set_title("Hello")
print("firmware " .. ghost.system.firmware_version())
print("target " .. ghost.system.target())
print("lua heap " .. ghost.system.memory_used() .. "/" .. ghost.system.memory_limit())

ghost.storage.write("hello.txt", "hello from GhostScript\n")
local saved = ghost.storage.read("hello.txt") or ""
print("storage: " .. saved)

ghost.ui.toast("Hello from GhostScript")
ghost.delay(1000)
ghost.ui.set_title("Done")
