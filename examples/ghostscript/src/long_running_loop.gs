-- Cooperative long-running loop. Use Stop or Back in the runner to end it.

ghost.ui.set_title("Loop running")

local iteration = 0
while true do
    iteration = iteration + 1
    print("loop iteration " .. iteration)
    ghost.storage.append("loop.log", "iteration " .. iteration .. "\n")

    -- Yield often. Tight infinite loops still hit the per-slice timeout.
    ghost.delay(1000)
end
