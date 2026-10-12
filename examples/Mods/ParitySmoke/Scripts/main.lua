-- Read-only smoke pack. Enable explicitly; see docs/parity-smoke.md.
local state = {
    pre = 0, post = 0, ticks = 0, scenes = 0, destructions = 0,
    discovered = 0, errors_attempted = 0, after_error = 0,
    map = '', held = {}, capture_pending = true, watch_requested = false,
}
parity_smoke = {}

fdk.hook('application.update', 'pre', function()
    state.pre = state.pre + 1
end)
fdk.hook('application.update', 'post', function()
    state.post = state.post + 1
end)
fdk.on('engine.scene_changed', function()
    state.scenes = state.scenes + 1
end)
fdk.on('engine.actor_destroyed', function()
    state.destructions = state.destructions + 1
end)
fdk.notify_on_actor_discovered(nil, function()
    state.discovered = state.discovered + 1
end)

-- Retain one bounded page of generation-checked references. Never replace them
-- automatically: transition checks must examine the original handles.
function parity_smoke.capture()
    local page = table.pack(fdk.invoke('engine.list', 0, 32, ''))
    local held = {}
    for i = 2, page.n do
        local actor = fdk.object(page[i])
        held[#held + 1] = {actor = actor, name = actor.name, id = actor.entity_id}
    end
    state.held = held
    state.capture_pending = #held == 0
    state.map = fdk.engine.scene.map_id
    return #held, state.map
end

function parity_smoke.status()
    local alive, expired = 0, 0
    for _, item in ipairs(state.held) do
        local ok, value = pcall(function() return item.actor.name end)
        if ok and value ~= nil then alive = alive + 1 else expired = expired + 1 end
    end
    local samples, paused, value, watch_error = fdk.invoke('game.browser_watch', 'player', 'position_x')
    return state.pre, state.post, state.ticks, state.scenes, state.destructions,
        state.discovered, #state.held, alive, expired, state.map,
        fdk.engine.scene.map_id, samples, paused, value, watch_error,
        state.errors_attempted, state.after_error
end

-- Explicit diagnostic: one timer fails, while another must still execute.
function parity_smoke.exercise_error()
    fdk.delay(0, function()
        state.errors_attempted = state.errors_attempted + 1
        error('ParitySmoke intentional timer error')
    end)
    fdk.delay(0, function() state.after_error = state.after_error + 1 end)
end

fdk.every(0.25, function()
    state.ticks = state.ticks + 1
    if state.capture_pending then parity_smoke.capture() end
    if not state.watch_requested then
        state.watch_requested = fdk.invoke('game.browser_watch', 'player', 'position_x', true)
    end
end)
fdk.on('start', function() fdk.log('ParitySmoke started; read-only checks enabled') end)
fdk.on('stop', function()
    if state.watch_requested then
        -- The browser can stop before mod callbacks during adapter detach.
        -- Adapter cleanup clears all watches even when this request is refused.
        pcall(fdk.invoke, 'game.browser_watch', 'player', 'position_x', false)
    end
    fdk.log('ParitySmoke stopped; timers/hooks/subscriptions released by mod lifecycle')
end)
