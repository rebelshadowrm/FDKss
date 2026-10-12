-- Enable with enabled.txt. F7 prints the current map immediately and player
-- coordinates after one second of engine updates. Ctrl+F7 cancels that timer.
local pending
fdk.bind_key(0x76, function()
    if pending then fdk.cancel_timer(pending) end
    print('Map:', fdk.engine.scene.map_id)
    pending = fdk.delay(1, function()
        pending = nil
        local player = fdk.engine.player
        print('Player:', player.position_x, player.position_y, player.position_z)
    end)
end)
fdk.bind_key(0x76, function()
    if pending then fdk.cancel_timer(pending); pending = nil end
    print('Position timer cancelled')
end, 2)
