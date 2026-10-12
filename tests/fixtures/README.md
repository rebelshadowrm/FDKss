# Public and private test evidence

Public fixtures contain synthetic inputs, modeled expected outcomes or numerical
comparisons from controlled cases, not executable/assets/save/raw gameplay dumps.
The modeled actor cleanup outcomes are authored cases, not original function bytes.

Bulky original-byte research fixtures are absent from public tree/history. Their
identities are in ../private_evidence_manifest.json. Affected Python checks report
explicit skips by default; synthetic checks in the same files still run. A public
CTest pass does not establish private original-code coverage, fresh game acceptance
or full parity.

For local research, preserve evidence from the exact authorized game build.
`tools/freeze_engine_evidence.py` freezes selected complete bodies/words, not every
historical fixture format. Reuse your independently preserved fixture set instead
of guessing missing fields. Enable original-code checks with:

```powershell
$env:FDKSS_PRIVATE_EVIDENCE = '1'
ctest --test-dir build -C Release --output-on-failure
```

Every manifest-pinned private fixture must exist and match SHA-256 in that mode.
Missing/changed evidence fails, rather than becoming an accepted synthetic
substitute. Keep reconstructed files in ignored paths. A new evidence revision
requires explicit provenance and a reviewed manifest update.
