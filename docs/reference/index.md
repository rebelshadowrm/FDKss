# FDKss technical documentation

FDKss aims to provide [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS)-comparable
modding utility for Falcom's Development Kit. UE4SS's
[documentation](https://docs.ue4ss.com/) is a reference for scripting, native mods,
inspection and SDK workflows. FDKss uses independently verified FDK equivalents;
UE4SS APIs, Unreal reflection and Blueprint assets are not interchangeable here.

- [Project direction and shared combat logging](direction.md)
- [Supports: per-game capabilities and research targets](supports.md)
- [Installation and supported builds](installation.md)
- [Lua mod API](lua-api.md)
- [Objects, battle snapshots and resource observers](engine-api.md)
- [Native C++ mods](native-mods.md)
- [Console, inspector, SDK and export tools](tools.md)
- [Feature support and limitations](features.md)
- [Asset tools, upstream pins and fork decisions](asset-tools.md)
- [Runtime peers and external loader preflight](runtime-peers.md)
- [Offline archive, table and model workflows](offline-assets.md)

The guides describe implemented contracts. Supported means the specified pinned
build and bounded operation; it does not establish all game states or full parity.
For exact native declarations see [api.h](../../include/fdkss/api.h) and
[plugin.h](../../include/fdkss/plugin.h). Runnable examples are in
[examples](../../examples). Regression sources are in the repository's
[tests directory](https://github.com/rebelshadowrm/FDKss/tree/main/tests);
the [fixture policy](../../tests/fixtures/README.md) separates public and local
original-code checks.

See the [source preview publication audit](source-preview-audit.md) for build/test evidence and publication limits.
