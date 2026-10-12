local path = fdk.call("game.mod_directory") .. "/" .. fdk.mod_name .. "/math.scp"
local file = assert(io.open(path, "rb"), "Compile examples/scripts/math.fdk-asm.json to this mod's math.scp first")
local bytes = file:read("*a")
file:close()
local job = fdk.call_compiled_async(bytes, "Sum", 1, 5)
bytes = nil

fdk.on("update", function()
    if not job then return end
    local state, result, slices, failure = fdk.invoke("engine.vm_job_result", job)
    if state == "complete" then
        fdk.log(string.format("Compiled sum = %s after %d slices", tostring(result), slices))
        job = nil
    elseif state == "failed" or state == "cancelled" or state == nil then
        fdk.log("Compiled job stopped: " .. tostring(failure or state))
        job = nil
    end
end)
