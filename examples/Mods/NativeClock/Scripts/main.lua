-- The companion Native/main.dll registers this binding for this mod only.
local sampled = false
fdk.on('update', function()
    local elapsed, frames, pre, post, thread = fdk.invoke('native.clock')
    if frames >= 60 and not sampled then
        print('native clock', elapsed, frames, pre, post, thread)
        sampled = true
    end
end)
