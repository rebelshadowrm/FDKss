---
name: fdk-native-evidence
description: Recover and validate a bounded FDK native field, call, hook or ownership contract from pinned original code. Use for uncertain native semantics before public API promotion.
---

# Prove a native contract

Define the exact build, receiver, representation, execution boundary and observable
effect. Name a falsifying case before adding a profile. Reuse the tools documented
in [the tool guide](../../docs/reference/tools.md); diagnostic candidates do not
approve a callable ABI or engine lifetime.

Pin executable hash and original bytes/RVAs. Complete entry-reachable bodies and
chained unwind families matter; a snippet or next jump-table entry is not a function
boundary. Retain rejected/indirect/exceptional routes. Live detours are not originals.

Recover wrappers/callers, receiver extent/retention, argument/result widths and
dependencies. For asynchronous work, distinguish every producer, completion,
cancellation and teardown; one worker join does not cover other queues. Pointer
readability or a vtable match does not exclude same-address reuse.

Express a scoped contract with meaningful original/synthetic refusal cases.
Compare expected results independently rather than through the same adapter twice.
Keep field semantics, pure instruction effects, private execution, public API and
natural game acceptance distinct. SDK/inspector/consumer declarations must agree.

Use freeze_engine_evidence.py for selected originals when appropriate. Keep bulky
original dumps local under the [fixture policy](../../tests/fixtures/README.md);
publish the reproducer, identities and limitations. Live changes require fresh
process/build/module identity, original-state baseline and reversible cleanup.
Never overwrite a loaded DLL or treat injection return as owner-thread readiness.
