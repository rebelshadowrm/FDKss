# Source preview publication audit

Audited October 12, 2026 for **0.3.0-preview.1**. This is an experimental
source publication, with no binary release or new live-game acceptance.

## Reproducible checks

- Isolated Windows x64 source build with Visual Studio 2022 and Python 3.13:
  configuration, Release compilation and **93/93 CTest groups passed**.
- Dependencies installed from [test requirements](../../tools/test-requirements.txt),
  including Pillow; the texture conversion check ran successfully.
- **87 original-evidence Python checks were explicitly skipped** in the public
  suite. Their pinned game-code fixtures are private. Skips are not evidence of
  native contracts. The opt-in gate rejects absent or changed fixture hashes.
- Local opt-in regression: **133 checks across 18 affected Python test files
  passed**, using the preserved private fixtures. Those inputs are not distributed.
- Isolated CMake installation inspected for public documentation, examples,
  portable skills, dependency notices and resolved local Markdown links.
- Three portable skills passed the skill metadata validator. Their procedures
  are guidance; following one does not itself establish runtime acceptance.

## Publication boundary

The public tree and initial history contain authored source, examples, regression
checks, reference documentation, capability limits, roadmap, portable agent guidance
and reusable skills. Machine paths, private plans, session checkpoints, logs,
local saves, game executables/assets and bulk original-byte research fixtures are
excluded. The public repository starts with a new root commit, so excluded fixtures
are not recoverable from earlier public commits.

The private fixture manifest contains names and hashes, not fixture bytes.
Required bounded compatibility signatures and restoration fingerprints remain in
source; [ownership and dependency notices](../../NOTICE.md) describe their scope.
The [fixture policy](../../tests/fixtures/README.md) explains the public/private
checks. Installation content was checked separately from Git ignore rules.

## What this establishes

The published source builds and its public regression suite passes in the audited
Windows configuration. It does **not** establish full UE4SS parity, comprehensive
combat logging, a runtime adapter for every FDK game, or current acceptance in a
running game. Existing acceptance is documented per capability and exact build in
[supports](supports.md). Current work and unresolved ownership are described in the
[roadmap](../../ROADMAP.md).

The source-only prerelease is intended for developer review and reproducible
experimentation. Any later binary release or expanded support needs its own
build, process-identity and live acceptance evidence.
