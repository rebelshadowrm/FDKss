# Direction: shared FDK combat logging

The October 12 direction is advanced combat logging across FDK games, with
Details as a visualization consumer. FDKss remains the working name until a
later rebrand. This is an expansion plan; cross-game capture is not implemented.

## Reusable architecture

FDKss supplies independently verified engine access, owner-thread scheduling,
copied state, native observation, build profiles and metadata inspection.
Details already separates capture from presentation through `ICombatCaptureSource`,
capture messages, encounter storage/replay and meter projections. These seams
can support a recorder that operates without a meter window.

The proposed layers are:

1. Engine platform: FDKss lifecycle, exact-build profiles and safe observation.
2. Game adapters: verified event meaning, actor identity and game mechanics.
3. Logging core: raw evidence, normalized events, explicit gaps, persistence,
   encounter/session state, deterministic replay and export.
4. Consumers: Details meter, timelines, death recaps and future analysis tools.

Observation, transport, storage and replay have substantial overlap. Action
attribution, teams, encounter boundaries and resource names remain game-specific.
Trails HP/EP/CP and command-battle assumptions must not become universal Ys
requirements. RTTI/table similarity does not prove common combat semantics.

This is a logical architecture, not a repository merger. Reuse the existing
Details codec/replay seams through a versioned adapter before moving code.

## Progressive delivery

| Stage | Deliverable | Acceptance |
| --- | --- | --- |
| L0 | Shared observation envelope and adapter capabilities | Game/build/session identity, sequence/time, provenance, raw keys, unknowns and explicit loss/gaps; preserve Details fixtures |
| L1 | Saved Sky2 trace replay through the shared boundary into Details | Existing expected totals/timeline preserved; unattributed writes stay unattributed; UI-independent replay and visible malformed-input/gap handling |
| L2 | One accepted Sky resource-observer family bridged to recording | Independent live comparison, bounded queue/loss telemetry and reload/detach/transition cleanup; HP deltas do not imply action identity |
| L3 | Broader Sky2 combat workflow | Player-confirmed action/effect ordering, healing, no-damage casts, interrupts, status/resource changes and encounter outcomes; actual coverage reported |
| L4 | Separately verified second-game adapter | Reuse accepted Sky1 contracts first; Ys Actor capture requires ownership proof; demonstrate differing mechanics |
| L5 | Wider per-game capability packs | Independent natural-state acceptance, packaging and compatibility; promote only accepted operations in Supports |

L0/L1 can progress offline without full UE4SS parity or new game launches. Unsafe
receiver lifetimes block affected live operations, not replay. Practical logging
can ship before full rendering, VM breadth or original source-language recovery.
The existing modding/parity requirements remain in scope; logging milestones do
not declare those requirements complete.

## Peer usage

Consult the [peer register](asset-tools.md) and `tools/peer_resources.json` before
reimplementing formats or capture research. Record consumer, exact revision,
license/notices, patches, checks and limits. Hosting a fork is not integration.

- Details: reuse capture/protocol/storage/replay seams and pinned local evidence;
  preserve MIT notices and keep Sky-specific semantics in the adapter.
- FalcomSchema/KuroTools: explicit-game metadata and independent comparisons;
  unknown records remain raw.
- ED9ModManager: candidate LoadScript/SpawnEvent and scene/script patch paths;
  independently verify native originals. Noncommercial source remains separate.
- SoraSaveSlots: per-game resolver/shared patch design reference; independently
  verify full match counts and transactional cleanup. License remains unresolved.
- sora2looseload: pinned external static preflight and signature/coexistence
  reference; debug messages are not a structured combat observation protocol.
- Ingert: external script-authoring candidate; validate copied roundtrips and
  clarify licensing before incorporation/redistribution. Its community notation
  does not establish Falcom's original language.
- PH3/FPAC/model peers: existing asset preparation and comparisons; actual game
  loading/rendering remains separate acceptance.

## Feasibility and support

There is enough demonstrated overlap for incremental shared logging. No inspected
peer closes PN post-worker Actor lifetime, arbitrary call ABIs or universal action
attribution. Every game needs independently accepted capabilities; no all-game
compatibility, parity percentage or completion date is established.

[Supports](supports.md) separates runtime, offline tooling and logging coverage.
Exact supported build hashes remain in [installation](installation.md).
