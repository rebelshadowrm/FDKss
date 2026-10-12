# Public roadmap

This preview provides a working mod platform and bounded verified engine
operations. Full modding parity and shared combat logging remain incomplete.
The [support matrix](docs/reference/supports.md) owns exact capability scope.
Plans are not release commitments or completion dates.

## Current work

- Specify a shared observation envelope and adapter capabilities, preserving
  game/build/session identity, provenance, sequence/time, unknowns and explicit gaps.
- Replay one saved Sky2 trace into the existing Details consumer through that
  boundary, retaining expected totals and unattributed observations.
- Resolve PN MeshBatch/scene producer and receiver ownership. Idle external
  samples do not prove a safe post-worker Actor boundary; affected APIs remain
  unavailable pending lifetime, cancellation, exceptional and reuse contracts.

## Following increments

- Bridge one accepted Sky resource-observer family to recording, with independent
  live comparison, loss telemetry and transition/reload cleanup.
- Extend verified action/effect, healing, interruption, status/resource and
  encounter coverage. A resource delta does not establish action attribution.
- Accept a second game adapter independently, reusing engine mechanisms while
  preserving differing gameplay semantics.

## Retained modding goals

Broader object/field/call discovery and SDK coverage; safe observation/invocation;
Actor-context action/wait jobs; engine-owned create/use/remove content; authored
script loading/execution/unload and original source-language recovery; native
animation clocks/blending, deformation/material/render fidelity; broader build
adaptation and compatibility/state/device acceptance.

Existing external archive/table/model workflows satisfy bounded prerequisites,
not native loading or ownership. Private session queues are not public promises.
See [direction](docs/reference/direction.md) and [features](docs/reference/features.md).
