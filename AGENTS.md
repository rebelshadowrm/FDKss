# Contributor agent guidance

Read README.md, CONTRIBUTING.md and the relevant public guide before changing a
capability. ROADMAP.md owns public priorities; docs/reference/supports.md and
docs/reference/features.md distinguish implementation from research and plans.

Build/test with the documented toolchain. Run affected checks while developing
and clean configure/build/CTest for publication. Public tests use synthetic inputs
and generated numerical cases. Private original-code checks are opt-in; see
tests/fixtures/README.md. A public pass is not live game acceptance.

Retain exact executable/body validation and unknown-build refusal. Shared RTTI,
pointer readability, a worker join or successful injection do not prove receiver
lifetime or all-game support. Separate copied values from retained references;
keep exceptional paths, cancellation and address reuse explicit.

Consult tools/peer_resources.json and the public register before peer integration.
Preserve pins, licenses/notices and copied-input validation. Source assessment,
external tooling and native loading are different acceptance stages.

Live work requires a fresh process/build/module identity and the authorized test.
Preserve saves and unrelated mods; never overwrite a loaded DLL. Agent completion
does not imply Lua stop, detach or game exit. Resolve timed-out mutations before
replaying them.

Portable task skills are under skills/. Local machine/session procedures can be
kept in ignored .agents/AGENTS.local.md; read them when present for local work,
but never publish that file, caches, saves, logs or private evidence dumps.
