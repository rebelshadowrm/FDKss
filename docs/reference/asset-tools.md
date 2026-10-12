# Asset tools and upstream resources

Use this register before adding archive, table, model, animation or compression tooling. Serialized formats do not establish runtime object layouts, ABI or lifetime. Reuse decisions apply to a concrete operation and pinned game build.

## Resource register

| Resource | Inspected revision / license | Use when | Reuse and fork decision |
| --- | --- | --- | --- |
| [FalcomSchema](https://github.com/Trails-Research-Group/FalcomSchema) | `7a545e7205b2067990005d15518285456e66d1c9`; MIT | Daybreak I/II or original Ys X table inspection/editing | Start with pinned upstream schemas. Maintain a fork before relying on local corrections or validated schema extensions; preserve upstream base and notices. Local maintained fork `801315b51bbc5933e9f3974fbfffb64e99726ea2`; reconstructible patch/notices in `tools/schemas/overrides`; hosted at [rebelshadowrm/FalcomSchema](https://github.com/rebelshadowrm/FalcomSchema/tree/fdkss/offline-table-layouts). |
| [FPACker](https://github.com/coinkillerl/FPACker) | `6589cd1935f6b2c6e45999bbe653e0e1dfc07c1e`; no top-level license found in snapshot | Sky FPAC packing or interoperability | Retain FDKss extraction. Use as reference or external tool. Source incorporation/distribution is deferred until licensing is resolved; record that dependency before maintaining a fork. No fork created. |
| [kuro_mdl_tool](https://github.com/eArmada8/kuro_mdl_tool) | `e44f0990604bbd3fa5b40b85458cd15313a56a75`; GPL-3.0 | Model/animation import, collision, shader comparisons or missing format/compression variants | Use externally first. Maintain a fork if persistent automation/output-control patches or validated format extensions are needed. Record GPL and dependency notices before redistribution/source incorporation. No fork created yet. |
| [PH3 P3A specification](https://github.com/ph3at/p3a-format) | `8bd31f36a9bb4a3cfaf1dc186eae8d1e6174243b`; MIT | P3A access or packaging | Prefer installed PH3 tooling. Pin specification for missing custom parsing; fork only for maintained specification/header changes. Archivist runtime source is not included. No fork needed now. |
| [PH3 compression article](https://ph3at.github.io/posts/Asset-Compression/) | Reviewed October 9, 2026; CC BY 4.0 | Load measurement or cache design | Reference, not code dependency or fork. Measure hot/cold behavior and contention first. |

These revisions are assessment baselines, not automatic update instructions. A local clone is not a hosted fork. The scoped integrations below carry their own acceptance; this register does not imply universal compatibility.

## Hosted forks and selected usage - October 12

The user authorized hosted forks for reuse. Forks preserve upstream licenses and
do not establish native compatibility or source incorporation permission.

| Peer | Hosted fork / inspected base | Intended consumer and actual state |
| --- | --- | --- |
| ED9ModManager | [rebelshadowrm/ED9ModManager](https://github.com/rebelshadowrm/ED9ModManager), `d20ca677181a765970d8ab5720739cb54e61e32d` | Source-assessed LoadScript/SpawnEvent and scene/script patch leads. Fork unchanged; no FDKss integration. PolyForm Noncommercial 1.0.0; keep separate research/external use and assess terms before incorporation. Independently prove ABI/receiver/unload contracts. |
| Ingert | [rebelshadowrm/Ingert](https://github.com/rebelshadowrm/Ingert), `087ba207a4fa6669ce4b1bae44faf43ea298405f` | External DAT/ING authoring candidate for copied Daybreak/Horizon/original Ys scripts. Fork unchanged; no binary/roundtrip acceptance. No license found in inspected top-level tree/package metadata; clarify before incorporation/redistribution. |
| FalcomSchema | [rebelshadowrm/FalcomSchema](https://github.com/rebelshadowrm/FalcomSchema/tree/fdkss/offline-table-layouts), upstream `7a545e7205b2067990005d15518285456e66d1c9`, correction `801315b51bbc5933e9f3974fbfffb64e99726ea2` | Existing maintained correction branch is now hosted at `fdkss/offline-table-layouts`; unchanged patch/notices in `tools/schemas/overrides`. Existing bounded offline table acceptance retained; native semantics/loading remain unverified. |

For combat capture, prefer Details protocol/replay seams and pinned local traces.
Script tooling supports authored scenarios and comparison; it does not establish
complete action/effect capture. See [direction](direction.md) and [Supports](supports.md).

## Runtime research and downstream consumers

The [runtime peer assessment](runtime-peers.md) adds SoraSaveSlots and
sora2looseload and expands the existing ED9 assessment. The former is a source
reference with unresolved licensing and identified scan/rollback limits; the
latter has a checked external static preflight through
`tools/peer_runtime_preflight.py`. None closes PN Actor ownership.

| Resource | Inspected revision / license | Use when | Reuse and fork decision |
| --- | --- | --- | --- |
| [Sora2-Details](https://github.com/rebelshadowrm/Sora2-Details) | `ce03a918b0ae81824087c7a5d7cfc95d48db5844`; MIT; local working tree also contains modified/untracked research | Sky2 combat/resource/status/action/animation research, table joins, capture coverage and downstream meter validation | Prefer the user's local checkout, including preserved raw logs and negative results. Pin each selected file/trace by SHA-256 because HEAD does not identify local changes or ignored logs. Existing research has informed FDKss resource work; further integration requires a named consumer and independent native contract proof. No fork created; preserve MIT notice if incorporating source. |
| [SoraSaveSlots](https://github.com/Bestfast/SoraSaveSlots) | `7079af422ae2a86710dc93677192414f13ecc936`; no top-level license found | Per-game accessor/profile design and patch verification research | Full read-only signature counts checked on both pinned Sky files. Reference only; no installed plugin/source incorporation. Truncated scan counts and sequential partial-install failures cannot replace FDKss safety/restoration checks. No fork created. |
| [sora2looseload](https://github.com/lmaple0/sora2looseload) | `745cf903bee976b72ad84c200a71e9137d666261`; MIT | Sky2 signature uniqueness, loose-loader preflight and hook coexistence research | Unchanged external verifier via `peer_runtime_preflight.py`; exact Sky2 static check passed. Optional debug output is not a combat feed. Live proxy composition remains unverified; no DLL installed or source/binary redistributed. No fork needed for unchanged external use. |

This is a two-way research relationship: Details supplies exact-build evidence,
and verified FDKss capabilities can improve Details capture across supported FDK
games. Raw traces, partial encounter projections and player-confirmed cases have
different evidentiary roles. Historical pointers/PIDs are not live identity;
capture sites alone do not prove callable ABI, thread ownership or lifetime.
Local evidence locations and the prior research assessment are kept in private
development notes. The manifest records the source baseline, not a packaged
Details dependency or complete combat capture acceptance.

## Accepted offline integrations - October 11

See [offline peer workflows](offline-assets.md) and `tools/peer_resources.json`.
All seven explicit game table profiles were scanned (8,426 tables/468,667 decoded
records). Unknown/mismatched sections retain raw bytes. Daybreak item/NPC layouts
have pinned corrections, provenance and a reconstructible local fork patch.
KuroTools at `8b568364232d6a51b03b85b7ed7723f6a1218750` (MIT) provides unchanged
explicit-game schema metadata and independent copied table comparisons.
PH3 access, all four installed tool versions' game-specific packages and
raw/LZ4/Zstd tool-only extraction baselines passed offline content checks.
External kuro_mdl_tool covers selected real v1/v2/v4 mesh/motion, D9BA, material
switch and shader comparisons. Collision/pose/normalized-stream differences are
measured rather than assumed byte-identical. External FPACker built unmodified
under WSL passed font and edited-model package checks. No external binary or GPL
source/dataset is redistributed. No native loading/rendering/ABI acceptance is
implied by these results.

## Select existing tooling first

- **FPAC:** `tools/pac.py` already provides bounded inventory and exact extraction. Evaluate FPACker for missing packing/interoperability operations.
- **P3A:** both Daybreak and both Ys installs under `C:/Games/FDK` contained `p3a_tool.exe` at the October 9 inspection. Discover its fresh path/hash and inspect help. Original Ys help exposed `info`, `extract`, `archive`, `verify` and `benchmark`; verify options on each selected installed version. Tool availability does not establish FDKss runtime overlays.
- **Tables:** `table_inspect.py`, `table_coverage.py` and `table_edit.py` use seven explicit profiles and pinned compatible definitions. Unknown records remain raw.
- **Models/animation:** retain verified FDKss exports. Use kuro_mdl_tool externally for missing authoring and comparison outputs; check version/port restrictions. Acceptance is operation-specific; consult the measured cases in the offline guide.

Keep extraction/import/repack outputs in an isolated workspace. External tools may overwrite inputs or process every file in their working directory; inspect commands and run on copies. Preserve original archive/model hashes. The initial assessment checked source/help; the October 11 integrations additionally tested the documented real copied workflows.

## Integration and fork record

Record upstream URL/base commit; selected external tool or fork URL/branch; actual commit/tool SHA-256; purpose; patches and notice locations; dependencies; exact invocation and working directory; game/executable/asset identities; baseline/output hashes; validation; unsupported cases; next step. Use `not created` and `not integrated` until work exists.

Create a maintained fork when persistent upstream changes are required, before those changes become a workflow dependency. Unmodified external tools and read-only snapshots need no fork. Resolve a missing destination/account when a fork is actually needed. Preserve upstream remote/base and local patches. Assess updates deliberately rather than pulling latest during startup.

Validation follows the operation: extraction compares content; schema decoding checks record bounds and semantics; repacking checks content and game loading; model import checks relevant sections and native loading; caching measures hot/cold loads and contention. Parsing, valid glTF and tool exit alone do not prove runtime parity.
