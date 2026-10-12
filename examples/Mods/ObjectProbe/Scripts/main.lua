-- Enable explicitly. Inspect actual objects in the current scene without writing properties.
local sampled = false
fdk.on('update', function()
    if sampled or not fdk.engine.scene.class then return end
    local actors = fdk.find_objects()
    local characters = fdk.find_objects('Actor@chara@sora')
    local controlled = fdk.engine.player.address
    local found = false
    for _, actor in ipairs(characters) do
        assert(type(actor.position_x) == 'number')
        assert(type(actor.position_y) == 'number')
        assert(type(actor.position_z) == 'number')
        if actor.address == controlled then found = true end
    end
    assert(not controlled or found, 'controlled actor missing from scene collection')
    assert(#characters <= #actors)
    assert(not pcall(fdk.invoke, 'engine.get', '@0:1:1', 'class'))
    assert(not pcall(fdk.invoke, 'engine.list', 0, 64, ''))
    print('scene objects passed', fdk.engine.scene.map_id, #actors, #characters, found)
    sampled = true
end)
