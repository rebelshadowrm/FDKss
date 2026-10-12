local references, destroyed, verified, updates = {}, 0, 0, 0
fdk.on('start', function()
    for _, actor in ipairs(fdk.find_objects()) do references[actor.address] = actor end
    print('Watching scene actor lifetimes', #fdk.find_objects())
end)
fdk.on('engine.actor_destroyed', function(address, class)
    destroyed = destroyed + 1
    local reference = references[address]
    if reference then
        local readable = pcall(function() return reference.class end)
        assert(not readable, 'Destroyed actor reference remained readable')
        references[address] = nil
        verified = verified + 1
    end
    if destroyed <= 5 then print('Destroyed', address, class) end
end)
fdk.on('engine.scene_changed', function(previous, current)
    print('Scene changed', previous, current)
end)
fdk.on('engine.lifecycle_overflow', function(count)
    error('Lifetime event queue overflow: ' .. count)
end)
fdk.on('update', function()
    updates = updates + 1
    if updates % 300 == 0 then print('Lifetime counts', destroyed, verified) end
end)
