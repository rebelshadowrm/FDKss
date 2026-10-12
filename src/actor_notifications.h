#pragma once
namespace actor_notifications {
inline constexpr char lua[] = R"lua(
    local subscriptions, order = {}, {}
    local next_id, active, revision = 0, 0, 0
    local cursor, epoch, round, seen, seen_count = 0, nil, 1, {}, 0
    local function reset_scan()
        cursor, epoch, round, seen, seen_count = 0, nil, 1, {}, 0
        revision = revision + 1
    end
    function fdk.notify_on_actor_discovered(class_filter, callback)
        assert(class_filter == nil or type(class_filter) == 'string', 'class filter must be a string')
        assert(type(callback) == 'function', 'actor notification callback must be a function')
        assert(active < 64, 'actor notification subscription limit reached')
        if active == 0 then reset_scan() end
        if #order >= 128 then
            local compact = {}
            for _, id in ipairs(order) do if subscriptions[id] then compact[#compact+1] = id end end
            order = compact
        end
        next_id = next_id + 1
        subscriptions[next_id] = {filter=class_filter or '', callback=callback}
        order[#order+1] = next_id
        active = active + 1
        return next_id
    end
    function fdk.cancel_actor_notification(id)
        if subscriptions[id] == nil then return false end
        subscriptions[id] = nil
        active = active - 1
        if active == 0 then reset_scan() end
        return true
    end
    fdk.on('update', function()
        if active == 0 then return end
        -- One bounded page per mod/update, shared by all of its subscriptions.
        local ok, page = pcall(function()
            return table.pack(fdk.invoke('engine.list', cursor, 32, ''))
        end)
        if not ok then cursor = 0; return end -- loading or an unavailable scene
        local next_cursor = page[1]
        if page.n < 1 or page.n > 33 or math.type(next_cursor) ~= 'integer' or
            (next_cursor ~= -1 and (next_cursor < cursor + math.max(1, page.n-1) or next_cursor > 16384)) then
            cursor = 0; return
        end
        local page_epoch
        for i=2,page.n do
            local token = page[i]
            local identity = type(token) == 'string' and token:match('^@(%d+):')
            if not identity or (page_epoch and page_epoch ~= identity) then cursor=0; return end
            page_epoch = identity
        end
        if page_epoch and epoch ~= page_epoch then
            local old_cursor = cursor
            reset_scan(); epoch = page_epoch
            if old_cursor ~= 0 then return end -- restart a changed scene from its first page
        end
        local current_revision = revision
        local snapshot = {}
        for _, id in ipairs(order) do snapshot[#snapshot+1] = id end
        for i=2,page.n do
            local token = page[i]
            if seen[token] then seen[token] = round
            elseif seen_count < 32768 then
                local actor = fdk.object(token)
                local readable, class = pcall(function() return actor.class end)
                if readable and type(class) == 'string' then
                    seen[token] = round; seen_count = seen_count + 1
                    for _, id in ipairs(snapshot) do
                        local subscription = subscriptions[id]
                        if subscription and class:find(subscription.filter, 1, true) then
                            local success, failure = pcall(subscription.callback, actor)
                            if not success then
                                fdk.cancel_actor_notification(id)
                                fdk.log('actor notification callback failed: ' .. tostring(failure))
                            end
                            if revision ~= current_revision then return end
                        end
                    end
                end
            end
        end
        if next_cursor == -1 then
            for token, last_round in pairs(seen) do
                if last_round ~= round then seen[token]=nil; seen_count=seen_count-1 end
            end
            round, cursor = round + 1, 0
        else cursor = next_cursor end
    end)
)lua";
}
