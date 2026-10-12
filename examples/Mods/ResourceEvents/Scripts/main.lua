-- Observer-only example. Enable on an independently accepted resource-hook stage.
assert(fdk.invoke('engine.hook_supported', 'status.resource_change'))
resourceEvents = {pre=0, post=0, errors=0, mismatches=0, samples={}}
local pending
fdk.hook('status.resource_change', 'pre', function(...)
    resourceEvents.pre = resourceEvents.pre + 1
    if pending then resourceEvents.mismatches = resourceEvents.mismatches + 1 end
    pending = table.pack(...)
end)
fdk.hook('status.resource_change', 'post', function(address, word, level,
        resource, operation, request, before, maximum, after, after_maximum, error)
    resourceEvents.post = resourceEvents.post + 1
    local values = {address, word, level, resource, operation, request, before, maximum}
    if not pending or pending.n ~= 8 then
        resourceEvents.mismatches = resourceEvents.mismatches + 1
    else
        for index=1,8 do
            if values[index] ~= pending[index] then
                resourceEvents.mismatches = resourceEvents.mismatches + 1
                break
            end
        end
    end
    pending = nil
    if error then
        resourceEvents.errors = resourceEvents.errors + 1
        fdk.log(error)
        return
    end
    local expected = request
    if operation == 'add' then
        expected = (before + request) & 0xffffffff
        if expected >= 0x80000000 then expected = expected - 0x100000000 end
    end
    expected = math.max(0, math.min(maximum, expected))
    if after ~= expected or after_maximum ~= maximum then
        resourceEvents.mismatches = resourceEvents.mismatches + 1
    end
    if #resourceEvents.samples < 16 then
        resourceEvents.samples[#resourceEvents.samples+1] = table.concat({
            word, level, resource, operation, request, before, maximum, after, after_maximum}, ':')
        fdk.log(resource .. ' ' .. operation .. ': ' .. before .. ' -> ' .. after)
    end
end)
fdk.on('stop', function()
    fdk.log('Resource events: ' .. resourceEvents.pre .. '/' .. resourceEvents.post ..
        ', mismatches=' .. resourceEvents.mismatches .. ', errors=' .. resourceEvents.errors)
end)
