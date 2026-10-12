-- Enable explicitly. Halve only the application update argument for 60 measured calls.
-- Removing the pre hook restores ordinary arguments; no engine property is written.
local pre_token, post_token
local original_delta, samples, total_ratio = nil, 0, 0
pre_token = fdk.hook('application.update', 'pre', function(application, dt)
    original_delta = dt
    return dt * 0.5
end)
post_token = fdk.hook('application.update', 'post', function(application, dt, result)
    if original_delta and original_delta > 0 then
        local ratio = dt / original_delta
        assert(math.abs(ratio - 0.5) < 0.00001)
        total_ratio = total_ratio + ratio
        samples = samples + 1
    end
    original_delta = nil
    if samples == 60 then
        print('hook replacement passed', samples, total_ratio / samples, result)
        assert(fdk.unhook(pre_token) and fdk.unhook(post_token))
    end
    return result -- Explicitly preserve the integer return register.
end)
