# Lua mod API

## Mod layout and lifecycle

```text
Mods/MyMod/enabled.txt
Mods/MyMod/Scripts/main.lua
Mods/MyMod/Scripts/helper.lua
```

Mod names use letters, digits, underscores and hyphens. The marker enables initial
loading; deleting it does not unload an already running mod. Each Lua mod has its
own state. Initial loading is alphabetical; shutdown is in reverse order.

```lua
local elapsed = 0
fdk.on('start', function()
    print('Loaded', fdk.mod_name, fdk.version)
end)
fdk.on('update', function(dt)
    elapsed = elapsed + dt
end)
fdk.on('stop', function()
    print('Stopping after', elapsed)
end)
```

Register lifecycle callbacks during initialization. An error is logged and disables
the failing callback. `require('helper')` searches this mod's Scripts directory.
Standard Lua libraries are available; mods are trusted local code, not sandboxed.

Reload starts a replacement state before stopping the old one. A failed script or
startup retains the previous state. Globals reset on successful reload; candidate
startup side effects cannot be rolled back automatically. Handle overlap in mod
startup/shutdown. Reload is explicit rather than filesystem watching.

## Calls and values

`fdk.call(name, input_string)` invokes a registered string binding.
`fdk.invoke(name, ...)` invokes a typed binding with nil, booleans, signed 64-bit
integers, finite numbers and binary strings, including multiple results. There
are at most 64 arguments/results and 16 KiB of result string bytes. Invalid types,
outputs and native errors raise Lua errors. A failure does not roll back writes.
Engine bindings require a supported game attachment.

`fdk.call('game.capabilities')` returns JSON describing the adapter's registered
API scope: `update_source`, `engine_properties`, `actor_api` and `vm_api`.
These flags describe support, not current object availability. Sky updates run
after the Application update; original and Proud Nordics core updates run after Framework
frame execution on the verified main thread. `game.update_info` reports the
source, owner thread, callback count and observed delta. A frame tick may occur
while gameplay is paused. Check capabilities before using engine APIs in a mod
that targets several games.

```lua
print(fdk.call('game.name'))
print(fdk.engine.scene.map_id)
```

See [engine APIs](engine-api.md) for object access and hooks. Examples for timers,
keybindings, hooks and native bindings are in [examples/Mods](../../examples/Mods).

## Restricted script execution

`fdk.find_scripts(filter)` and `fdk.script(name)` inspect loaded compiled programs.
Selected constant/pure calls and owned pure asynchronous jobs are implemented:

```lua
local id = fdk.script('common'):call_pure_async('Abs', 8, -7)
-- Poll on a later update:
local status, result, slices, failure = fdk.invoke('engine.vm_job_result', id)
```

Availability depends on the loaded program and verified instruction subset.
Jobs copy private execution state and belong to the submitting mod. Cancellation,
reload/unload and detach cleanup apply. `fdk.script_wait(seconds, quantum)` runs
an isolated private-timer wait with duration 0..30 seconds; it does not perform
Actor-bound gameplay. Arbitrary script invocation, real Actor-context action/wait
jobs and the original source-language compiler are not implemented.
