-- Optional, disabled by default. A short reversible engine-property test for Sky 1st Chapter.
local previous
local phase = "waiting"
local elapsed = 0
local samples = 0
local smallest, largest = math.huge, 0

fdk.on("start", function()
    local framework = fdk.call("game.framework")
    previous = tonumber(framework:match('"time_scale":([%d%.eE%+%-]+)'))
    assert(previous and previous >= 0.25 and previous <= 2, "unsupported starting time scale")
    print("Time-scale test starts with", previous)
end)

fdk.on("update", function(dt)
    if phase == "done" then return end
    elapsed = elapsed + dt
    if phase == "waiting" and elapsed >= 0.25 then
        print("Set time scale", fdk.call("game.set_time_scale", "0.5"))
        phase, elapsed = "slowed", 0
    elseif phase == "slowed" then
        samples = samples + 1
        smallest, largest = math.min(smallest, dt), math.max(largest, dt)
        if elapsed >= 0.5 then
            print("Slowed update samples", samples, "delta range", smallest, largest)
            print("Restore time scale", fdk.call("game.set_time_scale", tostring(previous)))
            phase, elapsed = "restored", 0
        end
    elseif phase == "restored" and elapsed >= 0.25 then
        print("Restored update delta", dt, "Framework", fdk.call("game.framework"))
        print("Time-scale test complete")
        phase = "done"
    end
end)

fdk.on("stop", function()
    if previous then print("Stop restores time scale", fdk.call("game.set_time_scale", tostring(previous))) end
end)
