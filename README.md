# FDKss

**Experimental source preview (0.3.0-preview.1).** This publication is for
developers building from source, not a general-user binary release. Current work
is shared combat observation/replay design and unresolved cross-game ownership;
see the [roadmap](ROADMAP.md) and [per-game support matrix](docs/reference/supports.md).

FDKss is an experimental Lua and C++ modding runtime for Falcom's Development Kit.
It aims for [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS)-comparable modding utility:
scripting, object inspection, hooks, SDK generation and content/export tools.
FDKss implements verified FDK equivalents rather than Unreal-specific APIs.
**Full parity is not yet implemented.**

The planned expansion is advanced combat logging across FDK games, with Details
as a visualization consumer of shared recorded events. FDKss supplies verified
engine access; game adapters will supply combat semantics to a reusable capture,
storage and replay core. FDKss remains the working name until a later rebrand.
See [direction and staged delivery](docs/reference/direction.md).

Sora2-Details supplies existing capture/replay code and exact-build research.
Additional games require independently accepted adapters; shared engine lineage
does not establish combat coverage.

The design target is shared engine discovery and modding facilities with validated
build profiles and game-specific extensions. The current two Sky implementations
are the initial validation base; broader engine adaptation is still incomplete.

## Documentation

Start with the [technical documentation](docs/reference/index.md).
The [publication audit](docs/reference/source-preview-audit.md) records checks and exclusions for this preview.

- [Build, installation and supported executables](docs/reference/installation.md)
- [Lua API and mod lifecycle](docs/reference/lua-api.md)
- [Engine objects, battle snapshots and resource hooks](docs/reference/engine-api.md)
- [Native C++ mods](docs/reference/native-mods.md)
- [Developer tools and exports](docs/reference/tools.md)
- [Implemented features and remaining limitations](docs/reference/features.md)

## Implemented features

- Lua 5.4 mod states, lifecycle callbacks and explicit reload with failed-candidate retention.
- Native/hybrid C++ mods with a versioned C ABI and shadow-copy reload.
- Owned timers, foreground keybindings and requests executed on the engine update thread.
- Scene Actor search, RTTI inheritance, guarded references and 52 property schema entries per supported build.
- Selected property edits, watches, external inspector and in-game console/browser.
- Copied battle rosters with level and HP/EP/CP values; selected native resource pre/post observers.
- Application/VM hooks, selected verified native calls and restricted pure asynchronous script jobs.
- SDK prefixes, Lua annotations, class/vtable metadata and scene/script/battle exports.
- File overlays, FPAC extraction, JSON bytecode assembly and supported MDL v4 model/isolated-animation conversion.

Engine object APIs are bounded to independently pinned Sky 1st and Sky 2nd Chapter
builds. Pinned original Ys X and Proud Nordics builds additionally support the core
Framework mod platform; their Actor/property/VM/content/render APIs remain
unavailable. Daybreak and Kyoto runtime adapters remain unverified. See the
[supported executable contracts](docs/reference/installation.md).

## Supports

- **Sky 1st / Sky 2nd:** pinned runtime and bounded engine/resource APIs.
- **Ys X / Proud Nordics:** pinned core mod runtime; broader engine APIs pending.
- **Combat logging:** partial Sky2 capture/replay exists in Details; the shared
  service and additional-game capture are planned.
- **Daybreak I/II, Kyoto and Horizon:** runtime/logging research targets.

See the [per-game support matrix](docs/reference/supports.md) for offline tooling,
capability boundaries and promotion criteria. The
[peer register](docs/reference/asset-tools.md) records upstream materials,
hosted forks and actual integration status.

## Build

Windows x64, Visual Studio 2022 Build Tools with C++ support, and CMake 3.24+:

```powershell
python -m pip install -r tools/test-requirements.txt
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix dist
```

Dependencies are downloaded with pinned hashes. The install places binaries and
example mods in `dist/bin`, plus headers, tools and public reference docs.
Python ownership-trace tests require the pinned PE/Capstone dependencies above
in the interpreter selected by CMake; offline tool dependencies are separate.
See the installation guide before attaching to a game.

Public tests use synthetic inputs and generated numerical comparisons. Original-byte
research checks are explicitly skipped unless opted into with local pinned evidence;
see [fixture policy](tests/fixtures/README.md). Public success is not fresh game
acceptance. [Contributor guidance](CONTRIBUTING.md) includes an isolated environment
recipe and portable agent skills. Machine/session-specific instructions remain private.

## First Lua mod

Create `Mods/MyMod/enabled.txt` and `Mods/MyMod/Scripts/main.lua`:

```lua
fdk.on('start', function()
    print('Loaded', fdk.mod_name)
end)
fdk.on('update', function(dt)
    -- Work runs on the runtime owner thread.
end)
fdk.on('stop', function()
    print('Stopping')
end)
```

Runnable [mod examples](examples/Mods) and a [native example](examples/NativeClock)
are included. Mods are trusted local code, not a security sandbox. Engine bindings
require a supported game attachment; the standalone host tests core lifecycle only.

## Not yet implemented

General all-engine object reflection and editing, unrestricted gameplay/native
invocation, real Actor-context action/wait jobs, owned engine object creation,
the original script source-language compiler, complete native animation/render
fidelity and additional-game adapters remain incomplete. Battle snapshots do not
provide complete action logs or persistent combatant identities. Selected working
examples do not establish full parity or coverage of every game state.

## References

Project-owned code and guidance are MIT licensed; [notices](NOTICE.md) distinguish
third-party terms and interoperability evidence. No game executables, archives,
saves, raw logs or bulky original-byte research fixture dumps are published.

The [UE4SS documentation](https://docs.ue4ss.com/) supplies the comparison workflows;
its implementation targets Unreal Engine, not FDK. Lua embedding follows the
[Lua 5.4 manual](https://www.lua.org/manual/5.4/manual.html). Native interfaces are
specified in [api.h](include/fdkss/api.h) and [plugin.h](include/fdkss/plugin.h).
