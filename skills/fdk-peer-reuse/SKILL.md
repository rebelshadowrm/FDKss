---
name: fdk-peer-reuse
description: Select and validate pinned external FDK archive, table, model or runtime research tooling for a named consumer, preserving provenance and operation-specific limits.
---

# Reuse a peer operation

Read tools/peer_resources.json and the [register](../../docs/reference/asset-tools.md).
For runtime leads also read [runtime peers](../../docs/reference/runtime-peers.md).
Compare existing FDKss coverage before writing another tool or wrapper.

Choose one consumer operation and establish the actual source/tool revision, dirty
state, license/notices and game/build/format. Verify that a path is its own checkout;
Git can otherwise resolve to a parent repository. A source snapshot or hosted fork
is not installed tooling, accepted native loading or permission to relicense it.

Prefer an unchanged external tool in an isolated workspace using copied inputs.
Inspect overwrite/bulk-processing behavior first. Record exact invocation, working
directory, input/output hashes and unsupported cases. Maintain a fork only when
persistent licensed patches become necessary, retaining upstream base and patches.

Validate by operation: compare extracted content, bounds/unknown table fields,
repacked contents or relevant model sections. Native loading/rendering/collision
and hook ownership require their own acceptance; parsing or successful exit is
insufficient. Serialized metadata does not establish runtime fields or lifetimes.

Update the manifest/public guide with actual integration and concrete next-use
triggers. Unused peers remain references rather than new active investigations.
Do not launch games or install a proxy solely because offline preparation passes.
