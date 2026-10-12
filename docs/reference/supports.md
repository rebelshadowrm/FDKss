# Supports

Documented October 12, 2026. Support is per capability and exact build. This list
summarizes existing acceptance, not a new live test. Build hashes are recorded in
[installation](installation.md).

| Game | FDKss runtime | Offline asset tooling | Advanced combat logging |
| --- | --- | --- | --- |
| Trails in the Sky 1st Chapter | Pinned core runtime and bounded object/resource APIs | Explicit tables, FPAC and selected model workflows | Shared bridge planned; selected resource observers exist, full action logging unverified |
| Trails in the Sky 2nd Chapter | Pinned core runtime and bounded object/resource APIs | Explicit tables, FPAC and selected model workflows | Partial capture/replay in Details; shared service and complete coverage unverified |
| Ys X: Nordics | Pinned core Framework runtime only | Explicit tables, P3A and selected model workflows | Planned; Actor/VM/combat adapter unavailable |
| Ys X: Proud Nordics | Pinned core Framework runtime only | Explicit tables, P3A and selected model workflows | Planned; worker/receiver lifetime and combat adapter unresolved |
| Trails through Daybreak | Runtime adapter unverified | Explicit tables, P3A and selected model workflows | Research target |
| Trails through Daybreak II | Runtime adapter unverified | Explicit tables, P3A and selected model workflows | Research target |
| Kyoto Xanadu | Runtime adapter unverified | Explicit tables, FPAC and selected model workflows | Research target |
| Trails beyond the Horizon | Offline executable baseline; runtime adapter unverified | No FDKss table profile or asset workflow accepted in this list | Research target |

Offline support means the bounded copied workflows in [offline assets](offline-assets.md).
Unknown formats/sections retain raw data or are refused. It does not establish
native loading, every asset or runtime capture. Upstream Ingert compatibility
does not promote the FDKss Horizon row.

Promote logging support only with verified event meaning, identity or declared
absence, ordering, measured loss, replay checks and transition/cleanup acceptance.
List damage, healing, resources, actions, status and encounter boundaries
separately as each becomes accepted. Resource changes alone do not identify an
attacker. Action games may need boundaries different from command battles.

See [direction](direction.md) for staged delivery and [features](features.md) for
broader modding limitations. Complete shared-service combat logging has not been
accepted for any game yet.
