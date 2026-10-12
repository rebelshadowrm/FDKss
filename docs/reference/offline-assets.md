# Offline peer asset workflows

These tools prepare and inspect copied assets without launching a game. Pins,
licenses and integration decisions are in `tools/peer_resources.json` and the
[resource register](asset-tools.md). Commands require new output paths/workspaces.
External tools are explicit dependencies; the install component includes no game
assets, external executable, GPL source or native attachment.

```powershell
python -m pip install -r tools/peer-tool-requirements.txt
python -m pip install -r tools/external-model-requirements.txt
cmake --install build --config Release --prefix dist/offline-tools --component OfflinePeerTools
```

## P3A access and packaging

```powershell
python tools/p3a_access.py COPIED_SCRIPT.p3a --tool 'C:/Games/FDK/Ys X - Nordics/p3a_tool.exe' --workspace .cache/new-script-access
python tools/p3a_access.py MODEL.p3a --entry 'exact/indexed/model.mdl' --output .cache/new-model/model.mdl --manifest .cache/new-model/identity.json
python tools/archive_workflow.py COPIED_ASSET_TREE --kind p3a --tool EXTERNAL_p3a_tool.exe --compression zstd --workspace .cache/new-package --benchmark-accesses 1000
```

Full access checks v1200 header/index hashes, names, ranges, budgets and original
identity, invokes PH3 verify/extract on a copy, then independently checks every
output size/XXH64. Defaults: source/total expansion 512 MiB, each file 128 MiB,
100,000 entries; `--max-expanded-mib` can raise full expansion to 4096 MiB.
Selected-entry access permits a directory-only archive/expansion inventory up to
64 GiB; the selected packed/expanded file remains at most 128 MiB. Raw, LZ4 and
Zstandard (including bounded dictionaries) are supported. Full source SHA-256,
entry hashes, commands, tool hashes and logs retain provenance.

Packaging copies an explicit asset tree, refuses links/path conflicts, limits it
to 10,000 files/256 MiB total/128 MiB each, and verifies exact names/content after
external packing. Raw, LZ4 and Zstandard packages passed copied roundtrips.
Each of the four installed PH3 tool versions also packed and verified a game-specific
table candidate. The original Ys candidate contains an edited item table.
Script extraction verified all outputs: original Ys 1,617; Proud 3,118;
Daybreak I 814; Daybreak II 1,485. Original Ys misc verified another 753.

Tool benchmarks record commands/timings and repeated copied-package extraction.
OS cache is uncontrolled. These establish an extraction baseline; game hot/cold
loading, overlay selection and contention still require runtime measurements.

## Seven explicit table profiles and authoring

```powershell
python tools/table_inspect.py COPY/t_item.tbl --game daybreak1 --output .cache/new-items.json
python tools/table_coverage.py COPIED_TABLE_TREE --game kyoto --workspace .cache/new-coverage
python tools/table_edit.py COPY/t_item.tbl --game ysx --edits edits.json --output .cache/edited/t_item.tbl --manifest .cache/edited/identity.json
```

Profiles: `ysx`, `ysxpn`, `daybreak1`, `daybreak2`, `sky1`, `sky2`, `kyoto`.
The package preserves 228 unchanged MIT FalcomSchema files and 287 unchanged MIT
KuroTools header files with per-file identities/notices. Only the explicit game's
definitions are eligible; there is no foreign-game same-width fallback.
FalcomSchema is preferred when compatible. Same-game KuroTools definitions fill
missing/size-mismatched coverage. A validated FDKss overlay corrects Daybreak II
NPCParam; neutral names preserve unknown semantics. Its 49 tables/9,190 records
passed bounded pointer/array inspection. Daybreak I's 248-byte item layout uses
the pinned Kuro1 definition. Both corrections are preserved in a reconstructible
local maintained FalcomSchema fork patch under `tools/schemas/overrides`, with
base, commit, SHA-256 and notices. No hosted fork was created.

Input must be unencrypted `#TBL`, at most 64 MiB. Pointer/string/array expansion,
common references and nested repeats are bounded. Repeated section names retain
directory index/occurrence. Unknown/mismatched/schema-refused sections remain raw,
with reasons; decoded rows retain raw bytes and trailing data. `--strict-schema`
instead refuses the input on schema interpretation errors. Names remain upstream
annotations, not proof of gameplay semantics or runtime offsets.

All extracted table collections completed container inspection without refusal:

| Profile | Tables | Decoded sections | Decoded records | Unknown sections | Size mismatches |
| --- | ---: | ---: | ---: | ---: | ---: |
| ysx | 243 | 258 | 176313 | 156 | 0 |
| ysxpn | 450 | 110 | 8060 | 665 | 15 |
| daybreak1 | 179 | 233 | 40002 | 117 | 22 |
| daybreak2 | 473 | 131 | 49818 | 648 | 1 |
| sky1 | 1185 | 525 | 60974 | 1150 | 0 |
| sky2 | 2520 | 1072 | 63048 | 2680 | 24 |
| kyoto | 3376 | 2468 | 70452 | 1800 | 0 |

Total: 8,426 tables, 468,667 decoded records. Unknown/mismatched sections are an
explicit coverage boundary. Inventing their layouts is outside the accepted
decoder contract; the coverage report identifies them for a future named consumer.

An edit document requires `input_sha256` and an `edits` list, each naming exact
`directory_index`, `record_index`, `field` and `value`. Only schema-decoded fields
are editable. Scalars check serialized bounds; strings/arrays append trailing
data; nested composites require the schema's exact shape. Opaque fields cannot
be edited. Reinspection verifies every expected decoded field and retains
unselected record/header bytes. Edited Ys price/name fields were independently
compared with KuroTools: 881 records, 11 compatible fields each (9,691 checks).
Other annotations differ between peers, including signedness; they are not
silently treated as equivalent. Gameplay effects and native loading are unverified.

## External model, animation and shader workflows

Acquire kuro_mdl_tool at the manifest pin and install its declared dependencies.
The wrapper requires the pinned tracked source to be clean. External importers
overwrite their input and change working directory; the wrapper passes one
absolute copied asset path, retains logs/hashes and checks the original unchanged.

```powershell
python tools/peer_model_workflow.py ORIGINAL.mdl --peer PINNED_KURO_MDL --mode mesh --compress-after --workspace .cache/new-mesh
python tools/peer_model_workflow.py MOTION.mdl --peer PINNED_KURO_MDL --mode animation-json --workspace .cache/new-motion
python tools/peer_model_workflow.py MOTION.mdl --peer PINNED_KURO_MDL --mode animation-gltf --workspace .cache/new-gltf
python tools/peer_model_workflow.py ORIGINAL.mdl --peer PINNED_KURO_MDL --mode mesh --material-index 0 --switch-name SPECULARMODE --switch-value 1 --workspace .cache/new-material
python tools/shader_compare.py --peer PINNED_KURO_MDL --game sky1 --model ORIGINAL.mdl --output .cache/new-shader.json
```

Real v1/v2 Daybreak mesh/JSON motion roundtrips preserve sections exactly; the
pure-motion version tag is restored only after validating the supported sections.
Real v4 skinned/static Sky and Ys/Proud/Kyoto samples passed geometry/material/
hierarchy comparison. D9BA compression is bounded to 128 MiB and four layers;
F9BA/C9BA encryption is explicitly unsupported. Skinned SNORM endpoint changes
(-128 to -127) are counted separately and accepted only for equal normalized
normal/tangent values. Other byte changes are not generalized away.

The glTF route restores unknown track words by exact track identity and checks
key metadata/trailer plus local/world poses at keys/midpoints. The selected
81-pose roundtrip has maximum matrix error approximately 4.8e-7, within 1e-5.
This is sampled serialized pose agreement, without clocks/blending/GPU playback.
The JSON route preserves animation sections exactly.

Independent collision inspection confirms the tested static model's reachable
11-node tree, triangle positions/membership and leaf bounds. Ten unreachable
empty nodes were removed; derived normals/midpoints/radii changed by at most
about 1.2e-7. Collision responses and native traversal remain unverified.

Shader comparison independently reproduces the external CSV switch differences
and applies explicit game availability across all seven profiles. Material
signature hashes are independently computed. Similarity is an authoring lead;
shader interchangeability/rendering fidelity requires live acceptance. Upstream
GPL code/datasets remain external and are not incorporated or redistributed.

## FPAC access and external packing

```powershell
python tools/pac.py COPIED_LAYOUT.pac --allow-duplicates --output .cache/new-inventory.json
python tools/pac.py COPIED_LAYOUT.pac --extract 'layout/actioncard/support/card.lay' --entry-index 12 --output .cache/new-card.lay
python tools/archive_workflow.py COPIED_ASSET_TREE --kind fpac --tool EXTERNAL_FPACker --wsl-distro Ubuntu --workspace .cache/new-fpac
```

Kyoto's layout/layout_a archives each contain one duplicate path whose entries
have equal bytes. Explicit name/index extraction passed independent slice checks;
strict default refusal remains. Native first/last lookup precedence is unknown.
External FPACker packs one top-level asset directory into an isolated output.
Both a copied font and an edited Sky model passed exact name/content verification.
Its assessed source lacks a top-level license: no source/binary is distributed.

On the assessed Windows host, the unmodified pinned C source was built using
existing WSL Ubuntu and `gcc -std=gnu11 -O2 -o OUTPUT` with `main.c`, `common.c`,
`util.c`, `unpacker.c`, `packer.c`, `strnatcmp.c`. Select an absolute external
output path. Linux executable SHA-256 and distro/commands are evidence, not an
installed Windows dependency. Without WSL, supply a compatible native build and
omit `--wsl-distro`. Game installation writes/loading remain a separate phase.
