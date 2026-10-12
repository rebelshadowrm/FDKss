# Console, inspector, SDK and export tools

For offline archive/table/authoring work, use the
[peer asset workflows](offline-assets.md): bounded PH3/FPAC access and packaging, seven
explicit table profiles/editing, and external model/motion/shader comparisons. These do not require
a game session and have a separate `OfflinePeerTools` install component.

Live tools use a fresh game PID and an attached, loaded Lua mod (normally
HelloFDK). They require advancing owner-thread updates. Tool-specific dependencies
are declared in [tools](../../tools); live Windows access and offline parsing
have different requirements.

`verify_framework_restoration.py` records and verifies the complete original
frame body for the pinned original and Proud Nordics core bridges. Supply a fresh JSON identity
with `pid`, `creation_filetime`, executable `path`/`sha256` and `image_base`:

```powershell
python tools/verify_framework_restoration.py identity.json frame-baseline.json
# After detach, with the same process identity:
python tools/verify_framework_restoration.py identity.json frame-baseline.json --verify
```

Record the baseline before attachment. Both operations require the full original
body to match (971 bytes for Proud Nordics, 1,358 for original Nordics); verification also binds the baseline to the process creation,
image base and independently pinned contract. The tool writes a separate
`.verified.json` result and never writes process memory. It verifies the core
frame function only; Actor lifetime, other mod patches and world simulation are
outside its scope. Sky restoration uses its separate inventory tool.

```powershell
python tools/console.py <pid> HelloFDK "return fdk.engine.scene.map_id"
python tools/inspector.py <pid>
python tools/export_battle.py <pid> --output battle.json
python tools/export_scene.py <pid> --output scene.json
python tools/export_scripts.py <pid> --output scripts.json
```

Replace PID placeholders. Console `--timeout` is seconds; the Python execute API
uses milliseconds. Timeout is not cancellation. The inspector supports properties,
watches, calls and a manually refreshed Battle tab. Expired references require
refresh; a failed battle refresh clears its rows.

`dump_sdk.py` parses a supported original executable and generates metadata,
verified C++ prefixes and Lua annotations. See its `--help` for arguments.
Verified property/signature declarations are distinguished from raw RTTI/vtable
candidates. RTTI describes classes/inheritance, not arbitrary reflected fields;
unknown signatures must not be used as callable contracts.

`discover_engine_getters.py` describes exact leaf instruction effects in an RTTI
family without assuming Sky's virtual slots or member offsets. For example:

```powershell
python tools/discover_engine_getters.py 'C:/Games/FDK/Ys X - Nordics/ysx.exe' --output getters.json
python tools/compare_engine.py --build 'sky1=C:/Games/FDK/Trails in the Sky 1st Chapter/sora_1st.exe' --build 'ysx=C:/Games/FDK/Ys X - Nordics/ysx.exe' --output comparison
```

The comparison tool writes original hashes, RTTI, per-build getter/thread-predicate
descriptions and class/build matrices. `--base-class` selects another getter RTTI
inheritance family; thread predicates use the Application family.
These offline tools do not establish field types/extents, receiver lifetime or
thread ownership, and do not enable additional runtime adapters.

`trace_engine_references.py` finds selected RIP-relative scalar/vector reads, writes,
comparisons and address calculations referring to supplied mapped-image RVAs:

```powershell
python tools/trace_engine_references.py 'C:/Games/FDK/Ys X - Nordics/ysx.exe' --target 0xa86988 --target 0xd0bb90 --target 0xd0b658 --output references.json
```

Candidates must match a decoded instruction in a complete entry-reachable unwind
family. Output preserves the complete original body, exact instruction bytes,
operand width and read/write/address effect. Targets can describe tables or data;
an instruction's width does not establish the complete stored type or pointee
extent. The selected instruction vocabulary is bounded and does not provide all
reference coverage or runtime profile approval.
Legacy SSE moves include 4/8/16-byte effects; VEX/EVEX forms remain outside the
vocabulary. A vector store can cover multiple pointer members. Exact target
matching alone does not find every overlapping store or receiver alias.
Use `--watch RVA:BYTES` to include selected-form references that overlap a byte
range, including stores starting before it. Targets and ranges may be combined;
neither option traces aliases stored in or loaded from memory.
Selected memory OR/AND immediates and XCHG produce `operation="read_write"`;
`locked` distinguishes an explicit LOCK prefix and `atomic_memory_exchange`
records implicit memory-XCHG atomicity. These describe instruction effects,
without proving any scheduling or lifetime contract.

`trace_engine_members.py` inventories encoded positive disp32 memory members
through complete original function walks. For the pinned PN phase fields:

```powershell
python tools/trace_engine_members.py 'C:/Games/FDK/Ys X - Proud Nordics/ysx_pn.exe' --member 0xfba30 --member 0xfba34 --member 0xfba38 --output member-effects.json
```

It checks actual decoded operands and displacement bytes, rejects matching
immediates/skipped instructions/incomplete walks, and reports vector widths,
read/write/address-taking, LOCK and implicit XCHG separately. A displacement
does not prove a receiver or field schema. Arithmetic aliases and other encoding
forms remain outside coverage. Dependencies are the same pinned PE/Capstone set
used by the other engine evidence tools.

`trace_engine_ownership.py` tracks bounded address alternatives through complete
original function walks, reports watched byte-range reads/writes, and records
addresses passed to callees or stored in memory. It requires the dependencies in
`tools/caller-discovery-requirements.txt`. For the pinned Proud Nordics build:

```powershell
python tools/trace_engine_ownership.py 'C:/Games/FDK/Ys X - Proud Nordics/ysx_pn.exe' --entry 0x21f980 --entry 0x224260 --entry 0x21eff0 --receiver 0x21f980:0xd2a4f0 --receiver 0x224260:0xd2a4f0 --watch 0xd2a970:8 --watch 0xd2a978:8 --output scene-aliases.json
```

`--receiver ENTRY:RVA` is an explicit conditional receiver hypothesis, requiring
independent caller/receiver proof. Register aliases join at branches; volatile
registers are discarded after calls, partial writes invalidate addresses, and
unmodeled index ranges/alternative overflow are reported. Stack reloads, pointer
chasing, exceptional paths and callee effects remain unmodeled. Missing effects
cannot establish absence of writers. Output always refuses lifetime/profile
approval; no game process is accessed and no engine pointers are exposed.

`trace_engine_calls.py` traces direct rel32 calls and tail jumps to selected RVA
candidates using entry-reachable instructions in contiguous x64 unwind families:

```powershell
python tools/trace_engine_calls.py 'C:/Games/FDK/Ys X - Proud Nordics/ysx_pn.exe' --target 0x742f40 --target 0x750350 --output calls.json
```

It records original hashes, complete-walk caller bodies, edges and rejected raw
matches. Indirect branches, separated unwind families and undecodable walks remain
rejected. Calls are assumed to return; exceptional control flow and indirect
dispatch are outside this trace. A caller edge supplies no receiver type, native
signature or thread/ownership contract.
`--entry RVA` additionally preserves a selected complete function body, decoded
entry-reachable instructions and static edges. It requires the exact primary
entry of a contiguous unwind family; middle entries and separated ranges are
reported as rejected. Incomplete walks remain explicit even for selected bodies.
With exactly one `--entry`, `--barrier BEFORE:AFTER` checks whether static paths
from a decoded instruction cross another instruction before exiting. Bypass exits
and possible cycles remain explicit. This proves instruction ordering under the
static walk's assumptions; it does not prove that a wait call succeeds or that
runtime objects are synchronized.

```powershell
python tools/trace_engine_calls.py 'C:/Games/FDK/Ys X - Proud Nordics/ysx_pn.exe' --entry 0x761520 --barrier 0x76176f:0x761899 --output frame-order.json
```

`discover_engine_threads.py` discovers exact current-thread ID comparisons through
the Application inheritance family, without fixed virtual slots. It validates
original pointer words, complete bodies, branch targets, dependency sections and
the named `GetCurrentThreadId` import. For example:

```powershell
python tools/discover_engine_threads.py 'C:/Games/FDK/TLoH - Trails through Daybreak/kuro.exe' --output threads.json
```

The descriptions record a direct global comparison or a global/indirect-member
comparison. They establish no safe Actor owner, scheduling or callable profile.

`discover_actor_types.py` also reports `cleanup_helpers`: complete resource-release
loop bodies with bounded member operands. This recognizes the observed instruction
algorithm across Sky, both Ys builds and both Daybreak builds while preserving
their different layouts. These effect-only matches do not establish receiver
types, function entry boundaries or lifetime coverage. Its existing RTTI-backed
Sky destructor dependency keeps the stricter, exact Sky helper guard.

Consult [asset tools and upstream resources](asset-tools.md) before adding archive,
table, model or compression tooling. Installed P3A tools and external authoring
resources complement the tools below; their presence does not imply FDKss runtime
support for those asset operations.

`freeze_engine_evidence.py` preserves selected complete primary-entry AMD64
function families from an explicitly SHA-256-pinned executable, optional original
pointer words and import names. It cross-checks entry walks against a separate
linear Capstone pass; both use the same decoder library. Interior/unregistered,
incomplete or conflicting decodes are refused. Existing outputs require a backup
and `--replace`. Pointer words remain raw, with no inferred type/table schema or
runtime ownership acceptance. Use `--help` for entry/word/output arguments.

Optional `--thunk RVA` preserves an explicitly pointer/capture-anchored forwarding
sequence through its first direct tail jump, within 32 executable bytes. It handles
selected leaf callbacks lacking unwind records without inventing a registered
function boundary. Conditional/indirect jumps, calls, returns, interrupts,
non-executable targets and unterminated sequences are refused. The original entry
anchor and receiver/ABI/lifetime proof remain separate requirements.

`inspect_job_manager.py IDENTITY.json --output NEW_REPORT.json` performs a
fingerprinted read-only global-manager diagnostic for the two pinned Ys builds.
Identity includes PID, creation FILETIME, executable path/hash and image base;
the tool rechecks the process and complete original worker bytes. It copies only
scalar manager members, rechecks root/table identity and reports non-atomic,
unverified lifetime/ABA/phase status. It never locks/calls the manager or follows
job/Actor pointers. Use `--fixture` with the corresponding durable
`tests/fixtures/engine_global_jobs_{pn,ysx}.json` when running installed tools.
Output must remain outside the game installation. Unreadable/replaced managers,
unknown builds and altered worker code are refused. This does not enable Actor
support, certify a frame boundary or widen the existing Sky process reader.

Original-byte research fixtures are local and excluded from this source preview.
The manager reader needs explicit local fixture evidence; the freezing tool can
reconstruct selected original bodies, with the documented proof boundaries.
Public tests report original-evidence skips separately from synthetic checks.

`peer_runtime_preflight.py` runs the pinned external sora2looseload verifier on
the assessed Sky2 original file. It checks the checkout/revision/source hashes,
executes in an isolated workspace and records static results without loading a
proxy or claiming native coexistence. See [runtime peers](runtime-peers.md).

`check_checkpoint.py` performs read-only exact plan/index next-step consistency.
Optional version-1 manifests contain `next_step`, `artifacts` (path/SHA-256) and
`test_runs` (kind `ctest`, log path/SHA-256, passed/total counts). It validates
recorded files, including UTF-8/BOM-marked UTF-16 CTest logs, without rewriting
workflow state or replaying tests. `all_recorded_test_runs_passed` distinguishes
a consistent failed-test log from a passing run. Historical hashes/counts never
prove freshness or live/native acceptance.

`pac.py` handles read-only FPAC extraction. `script_sdk.py` inspects compiled script
metadata; `compile_script.py` assembles the supported JSON bytecode format.
Assets overlays affect new file opens, not necessarily already cached resources.
There is no complete original source-language compiler or engine-owned factory API.

`mdl.py` exports supported MDL v4 geometry; `mdl_animation.py` and related tools
support bounded animation analysis/conversion. Static/skinned transforms, isolated
clips and supported packed colors are distinct from native clocks/blending, GPU
skin deformation or exact shader/material reproduction. Invalid/unsupported
assets are refused. See each tool's `--help` and tests for supported options.
