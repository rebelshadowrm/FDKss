local pres, posts = 0, 0
fdk.hook('script.command', 'pre', function(group, command, program, fn, raw_arguments)
    pres = pres + 1
    if pres <= 3 then
        fdk.log(string.format('VM pre %d:%d %s/%s (%d raw arguments)', group, command, program, fn, #raw_arguments // 4))
    end
end)
fdk.hook('script.command', 'post', function(group, command, program, fn, raw_arguments, result)
    posts = posts + 1
    if posts <= 3 then fdk.log(string.format('VM post %d:%d %s/%s result=%s', group, command, program, fn, tostring(result))) end
    -- Return nil to preserve the engine result.
end)
fdk.on('stop', function() fdk.log(string.format('VM observed %d pre / %d post commands', pres, posts)) end)
