# Feature support and limitations

The comparison follows the workflow categories in the official
[UE4SS feature list](https://github.com/UE4SS-RE/RE-UE4SS). It describes FDKss
implementation boundaries, not an upstream compatibility claim. Full parity is
not yet implemented. Shared engine discovery/adaptation is a core requirement;
the current pinned Sky implementations do not yet demonstrate that generality.

| Workflow | Implemented FDKss capability | Remaining limitation |
| --- | --- | --- |
| Lua scripting | Lua 5.4, isolated mod states, lifecycle, explicit reload, timers and keybindings | General gameplay script execution and unrestricted native calls are not implemented |
| C++ mods | Versioned C ABI, native/hybrid mods and shadow-copy reload | Engine calls remain limited to registered, verified operations |
| Object search | Scene Actor enumeration, class/name filters, RTTI inheritance and current roots | All-engine object discovery and full lifetime/reuse coverage are not implemented |
| Property inspection/editing | 52 schema entries per pinned build; selected coordinates, resources and state edits; inspector and watches | Arbitrary reflected fields and all-object editing are not implemented |
| Native hooks/calls | Application/VM hooks, selected getters/placement/scalars and HP/EP/CP pre/post observation | General gameplay/animation call and observer coverage is not implemented |
| Battle data | Bounded copied roster, level and HP/EP/CP with maxima in both Sky builds | Names, teams, persistent combatant identity, action attribution and outcomes are not provided by this API |
| SDK generation | Verified layout prefixes, Lua annotations and selected signatures, plus RTTI/vtable catalogs | Unknown signatures stay candidates; RTTI does not recover arbitrary field reflection |
| Content/script authoring | File overlays, FPAC extraction, JSON bytecode assembly and restricted loaded-function calls | Engine-owned object creation, general Actor-context action/wait jobs and the original source-language compiler are not implemented |
| Scene/model export | Scene transforms, MDL v4 geometry, supported skinning/isolated clips and packed colors | Complete scene reconstruction, native playback/blending, GPU deformation and shader/material fidelity remain incomplete |
| Offline peer assets | Verified copied P3A/FPAC packages, seven table profiles/editing and external v1/v2/v4 model/motion/material workflows | Native loading/rendering/collision and unknown table semantics remain unverified; explicit raw coverage retained |
| Game adaptation | Two independently pinned Sky engine adapters; both Ys builds have separately accepted core-only Framework adapters | Ys Actor/property/VM/content/render support remains unverified; Daybreak, Kyoto and Horizon runtime adapters are unavailable |
| Compatibility | Selected gameplay, transition, reload and cleanup checks | Complete controller/IME/device-loss and all-required-state acceptance remains incomplete |

Pure asynchronous calls and private timer waits exist. They do not supply a
verified Actor-bound gameplay job. Constructor notifications do not create an
engine-owned object. Diagnostic addresses and catalog signatures do not approve
invocation. Non-atomic copied data does not establish persistent object identity.
