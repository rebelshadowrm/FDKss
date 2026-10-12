# Runtime peer assessment and reuse

Assessment pins and selected-file hashes are in `tools/peer_resources.json`.
The October 12 review retained the existing ED9 fork and added two runtime research
peers. No peer loader/plugin was installed, no game/save was modified and no
additional native ABI, logging capability or runtime adapter was accepted.

| Peer | Useful application and implemented addition | Ownership bottleneck disposition |
| --- | --- | --- |
| [ED9ModManager](https://github.com/lom2333/ED9ModManager/tree/d20ca677181a765970d8ab5720739cb54e61e32d) | Existing registered fork retained; assessment expanded for Ed9Api hooks, RTTI/vtable/instance lookup, versioned symbol tables and script/event examples | Can shorten Sky call/discovery recovery. Inspected source does not prove PN producer completion, callback receiver retention, cancellation or ABA safety. |
| [SoraSaveSlots](https://github.com/Bestfast/SoraSaveSlots/tree/7079af422ae2a86710dc93677192414f13ecc936) | Registered reference for shared patch mechanisms, per-game accessor differences and pre-write byte checks; full read-only signature counts checked on both pinned Sky files | Indirect design reference. Save accessor/backing replacement does not establish Actor lifetime or asynchronous ownership. |
| [sora2looseload](https://github.com/lmaple0/sora2looseload/tree/745cf903bee976b72ad84c200a71e9137d666261) | Registered MIT external verifier; FDKss preflight wrapper validates checkout/source/build identity and records unique signatures/imports | Indirect signature/coexistence reference. No combat-feed, callback lifetime or PN support proof. |

## Important reuse boundaries

ED9's `find_instance` returns a candidate pointer; it does not supply a leased,
generation-safe object handle. Symbol entries without signatures and example
hardcoded RVAs remain leads. Its plugin API is distinct from FDKss's native ABI.
PolyForm Noncommercial source stays separate under the existing reuse decision;
this review adds no code dependency or permission assumption.

SoraSaveSlots separates game-specific resolution from a common patch engine, but
its `scanText` stops after the requested maximum. An expected count therefore
cannot detect excess matches. Its sequential install can return failure after
earlier hooks have been written; no rollback was found in that path. FDKss must
retain complete uniqueness, original-code, restoration and ownership checks.
The independent full-text inventory matched the selected resolver counts on the
current Sky1/Sky2 files. This is static discovery, not patch/save compatibility.
No top-level license was found; source/binary incorporation remains deferred.

sora2looseload's required signatures and XInput imports pass against FDKss's
pinned Sky2 build. Its optional DebugLogger emits formatted diagnostics and does
not forward the saved original logger; it is not a verified combat event source.
FDKss already has file overlays. Do not add a second XInput proxy or assume hook
coexistence merely because the original file passes a static scan.

## Reusable external preflight

Acquire an unmodified checkout at the manifest's exact sora2looseload revision.
Install the pinned PE dependency using the existing tool requirements. Then run:

```powershell
python tools/peer_runtime_preflight.py --peer-root PINNED_SORA2LOOSELOAD --executable 'C:/Games/FDK/Trails in the Sky 2nd Chapter/sora_2nd.exe' --output NEW_REPORT.json
```

The wrapper refuses a parent repository mistaken for a checkout, changed tracked
files or selected source hashes, unassessed executable hashes and output paths
inside the checkout/game installation. It runs the unchanged verifier externally
in an isolated temporary workspace. Reports preserve invocation, identities,
match/import results, exit status and `runtime_accepted: false`. There is no DLL
load, memory access or file override installation. A signature/import rejection
returns a failing result rather than enabling fallback hooks.

The `RuntimePeerTools` install component contains this wrapper, the peer manifest
and the public register/guide. External code and binaries are not packaged.
Verification covers the current Sky2 file and rejection fixtures; broader builds,
patched processes and live loader/FDKss coexistence remain separate work.

For the shared logging direction, resume [L0/L1](direction.md) without treating
these peers as an alternative combat protocol. For native work, select one
original-code call/receiver or loader-coexistence consumer before deeper reuse.
