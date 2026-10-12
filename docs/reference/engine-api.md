# Objects, battle copies and resource observers

The object, battle, property and native hook APIs in this guide are supported by
the pinned Sky adapters. Original and Proud Nordics currently provide the core mod platform
only; these engine APIs are unavailable there. Inspect `game.capabilities` before
selecting an engine-dependent path in a mod shared across games.

## Current roots and scene Actors

`fdk.engine.player`, `fdk.engine.scene` and other supported roots resolve the
current engine object on access. Root references follow replacements; they do not
keep the original object alive. Known properties return nil when a root is absent;
unknown roots/properties raise errors. Individual property reads are not atomic.

```lua
local player = fdk.engine.player
if player.class then
    print(player.position_x, player.position_y, player.position_z)
end
for _, actor in ipairs(fdk.find_objects()) do
    print(actor.class)
end
```

`fdk.find_objects(class_substring, name_substring)` searches the scene Actor
collection with optional literal, case-sensitive filters. `fdk.find_object` returns
the first match or nil. The optional third argument `true` enables exact decorated
RTTI inheritance matching. Scene references additionally use membership,
generation and class checks and may expire. Selected destruction hooks do not
prove every nonvirtual or same-address reuse route. Address strings are diagnostic
values, not callable pointers. Writes are limited to verified properties.

## Battle snapshots

```lua
local snapshot = fdk.battle_snapshot()
if snapshot then
    for _, row in ipairs(snapshot.records) do
        print(row.index, row.level, row.hp, row.max_hp, row.ep, row.cp)
    end
end
```

The API reads the battle manager roster, separately from scene Actors. It returns
nil when the manager is absent, or a copied table with `count`, `records` and
`atomic=false`. An existing empty roster has count zero. At most 70 records are
returned. Fields are `index`, `status_word`, `level`, `hp`, `max_hp`, `ep`, `max_ep`,
`cp`, `max_cp`. Indices are zero-based and copy-local; the status word is opaque.
Names, teams, persistent identity and battle outcome are not supplied.

Held copies survive transitions as ordinary Lua data; edits affect only the copy.
Manager/entry/status contracts and resource ranges are checked, then chains are
rechecked before emission. Requests run on the established update thread. These
checks detect observed changes, not atomicity or same-address ABA reuse. No battle
handles, receivers or writes are exposed. The typed `engine.battle_snapshot`
binding returns count and 32-byte records of eight little-endian uint32 fields,
excluding the derived index. Invalid reads discard the whole candidate.

## HP/EP/CP pre/post observation

```lua
local token = fdk.hook('status.resource_change', 'post',
    function(address, word, level, resource, operation, request,
             before, maximum, after, after_maximum, error)
        if not error then print(resource, operation, before, after) end
    end)
-- Remove with fdk.unhook(token).
```

The pre phase receives the first eight arguments shown above. Selected original
set/add paths for HP, EP and CP preserve arguments and the original return value;
callback return values do not replace them. Post values are actual copied
readback, or nil/nil/error on a failed copy. Address and word are diagnostic,
not persistent identity or action attribution. Signed addition follows original
32-bit wrap/clamp behavior.

Foreign-thread, nested, explicit native-API and unselected/invalid-contract calls
bypass observation while preserving the original call. This deliberately bounded
observer does not capture every writer or constitute a complete combat log.
Reload/unload removes owned registrations. Explicit resource edits do not deliver
these observer callbacks; not every game-side effect is automatically reversible.
