-- Enable explicitly. Observer hooks preserve all engine arguments and results.
local pre_count, post_count = 0, 0
fdk.hook('application.update', 'pre', function(application, dt)
    assert(application.class == '.?AVApplication@sora@@' and type(dt) == 'number')
    pre_count = pre_count + 1
end)
local post_token
post_token = fdk.hook('application.update', 'post', function(application, dt, result)
    assert(application.class == '.?AVApplication@sora@@' and type(dt) == 'number')
    assert(math.type(result) == 'integer')
    post_count = post_count + 1
    if post_count == 60 then
        local player = fdk.engine.player
        if player.class then
            print('character methods', player:call('type_name'), player:call('type_id'), player:call('type_flags'))
        end
        print('hook phases passed', pre_count, post_count, result)
        assert(fdk.unhook(post_token))
    end
end)
