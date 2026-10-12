# Contributing

FDKss is an experimental source preview. Finish bounded usable workflows while
keeping unsupported builds and operations refused. Start with the [roadmap](ROADMAP.md),
[support matrix](docs/reference/supports.md) and [feature limits](docs/reference/features.md).

## Build and verification

Use Windows x64, Visual Studio 2022 with the C++ workload, CMake 3.24+ and Python
3.13. Install pinned public-test dependencies before configuring:

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r tools/test-requirements.txt
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DPython3_EXECUTABLE="$PWD/.venv/Scripts/python.exe"
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix dist
```

Linux builds the standalone platform and applicable tests, not Windows game
adapters. External model authoring has separate dependencies and is not required
for the public suite. Read the [fixture policy](tests/fixtures/README.md) before
running original-code checks. Public test success is not fresh game acceptance.

## Evidence and review

Describe the trigger, public behavior, exact build/format, independent evidence,
tests and remaining limits. A signature match does not approve an ABI; calling
the same API twice is not independent validation. Keep shared engine mechanisms,
build profiles and game semantics distinct. Update relevant API/support docs when
acceptance changes; do not turn counts or one example into a parity claim.

Submit source, relevant tests and public technical explanations. Keep executable
or asset dumps, saves, raw logs, local paths and credentials out of commits.
Describe original research with hashes/RVAs and reconstruction procedures rather
than publishing bulky original-byte dumps. Preserve third-party notices.

## Optional agent skills

The [native-evidence](skills/fdk-native-evidence/SKILL.md),
[peer-reuse](skills/fdk-peer-reuse/SKILL.md) and
[capability-audit](skills/fdk-capability-audit/SKILL.md) skills contain portable
FDK-specific workflows. Read them directly or copy them into your agent's skill
directory. They do not authorize game launches, save changes, publication or
messages. Local navigation/session skills are deliberately absent.

Project-owned code and guidance use the root MIT license; third-party materials
retain their terms. See [NOTICE.md](NOTICE.md).
