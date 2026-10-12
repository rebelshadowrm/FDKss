local elapsed = 0
local reported = 0

fdk.on("start", function()
    fdk.log("Hello from " .. fdk.mod_name .. "!")
end)

fdk.on("update", function(delta_seconds)
    elapsed = elapsed + delta_seconds
    if elapsed >= reported + 1 then
        reported = elapsed
        fdk.log(string.format("Elapsed: %.3f seconds", elapsed))
    end
end)

-- A game adapter can emit this after a verified map-load hook.
fdk.on("map.loaded", function(map_name)
    fdk.log("Map loaded: " .. map_name)
end)

fdk.on("stop", function()
    fdk.log("Goodbye!")
end)
