# Native C++ mods

Native Windows x64 mods use `Mods/<Name>/Native/main.dll` and the same `enabled.txt`
marker as Lua mods. Lua is optional; in a hybrid mod, native startup registers
bindings before `Scripts/main.lua` executes.

Implement [plugin.h](../../include/fdkss/plugin.h), export `FDKssPluginQuery`, and
return a version-1 exports table. Start and stop are required; update/event/filter
are optional. [NativeClock](../../examples/NativeClock/main.cpp) is a complete
example. Plugin callbacks use zero for success and provide an error buffer on
failure. Query negotiation rejects unsupported ABI/capacity.

Use the supplied host table for logging, registering bindings and invoking engine
APIs rather than importing a particular loaded `fdkss.dll`. Registration is
startup-only and bindings belong to this mod. Host calls are synchronous on the
runtime owner thread; borrowed arguments, result sinks, host and context must
not outlive their documented callbacks/state. Never start engine work in DllMain.
Stop must release state, join owned threads and tolerate partial startup.

Native reload uses shadow copies so rebuilt source DLLs can be replaced. A failed
candidate retains the previous mod; successful replacement shuts down the old
state. Startup side effects are not automatically rolled back. An update/event
failure disables the affected native component. C++ access does not expand the
verified engine API: arbitrary receivers, lifetimes and signatures remain
unsupported until independently established.

For embedding/runtime adapters use [api.h](../../include/fdkss/api.h). Create,
start, tick, emit and destroy on the owning thread. Cross-thread and reentrant
operations are refused. Engine adapters require independently pinned executable
and dependency contracts; supplying a new hash alone does not enable a game.
