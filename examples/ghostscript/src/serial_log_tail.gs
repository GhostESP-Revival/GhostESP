-- Read the global serial/terminal log without copying the full log into Lua.
-- Requires commands permission because logs can contain command output.

ghost.ui.set_title("Serial log tail")

local count = ghost.results.count("log.serial") or 0
local start = count - 10
if start < 0 then start = 0 end

print("serial log lines: " .. count)
for i = start, count - 1 do
    local line = ghost.results.field("log.serial", i, "line")
    if line then print(line) end
end
