# Niagara support matrix and corpus results (N1–N5)

Status: 2026-10-07. Design: [asset-document-niagara-design.md](asset-document-niagara-design.md).
Rules: [design rules](asset-document-design-rules.md) §3 (rollback = functional equivalence), §4
(editor-equivalent code + behaviour test), §5 (support matrix). Reference editors: the Niagara System / Emitter
editor (stack view models) and the Niagara script editor (`UEdGraphSchema_NiagaraSystem`, the graph action menu,
`FNiagaraScriptToolkit` Apply). The agent-facing description is `MCP/schemas/assetdocument/Niagara.md`.

| Class | Profile | Phase | State |
|---|---|---|---|
| `NiagaraSystem` | `profile.niagara_system.v1` | N1–N4 | Extract / Inspect / Apply / Diff |
| `NiagaraEmitter` | `profile.niagara_emitter.v1` | N1, N5 | Extract / Inspect / Apply / Diff |
| `NiagaraScript` (Module, Function, DynamicInput) | `profile.niagara_script.v1` | N4 | Extract / Inspect / Apply / Diff |
| `NiagaraParameterCollection` | `profile.niagara_parameter_collection.v1` | N1, N5 | Extract / Inspect / Apply / Diff |
| `NiagaraParameterCollectionInstance` | — | — | not authored (no profile) |

Classification: **Supported**, **Read only**, **Refused** (an error code; the editor cannot do it either, or the
asset uses something the editor no longer offers), **Later** (phase named).

## Editor surfaces

| Editor surface | Body location | Class | Mechanism |
|---|---|---|---|
| System Details, user parameters, declared parameters | `Properties`, `UserParameters`, `Parameters` | Supported (N2) | `fx.niagara.edit.v1` through the system view model; values through the Niagara value codec |
| Emitters: add (empty / minimal / `From`), delete, rename, enable, parent removal and version | `Emitters` | Supported (N2) | `FNiagaraSystemViewModel` add / remove / rename; emitter Details |
| Renderers: add, delete, order, Details, bindings | `Emitters.<e>.Renderers` | Supported (N2) | stack renderer items |
| Module stacks: add, delete, move, enable, version, display name, note | `Stacks` | Supported (N3) | `FNiagaraStackGraphUtilities` as the stack's menu |
| Module inputs: every value mode, nested dynamic inputs, data interfaces | module `Inputs` | Supported (N3) | stack function input view model |
| Set Parameters modules, event handlers, simulation stages | `Stacks` | Supported (N3) | stack items |
| Lightweight (stateless) emitters | `Mode: Stateless` | Supported (N3) | the stateless module list by reflection |
| Script asset: New (Module / Function / Dynamic Input) | `Action: Create`, `Usage` | Supported (N4) | the New-asset factory of that usage; `Usage` immutable (`Niagara.UsageImmutable`) |
| Script Details | `Settings`, top-level `Properties` | Supported (N4) | script Details |
| Graph: place node from the action menu | `Graph.Nodes` | Supported (N4) | `graph.*` primitives + Niagara host adapter; actions from the graph's own menu (`graph.actions.enumerate.v1`) |
| Graph: Add Parameter / Promote to Parameter | `Niagara:Input:<name>` | Supported (N4) | parameter input node as `FNiagaraEditorUtilities::InitializeParameterInputNode` (not exported; same result) |
| Graph: Map Get / Set / custom HLSL / op Add pin, pin rename and type | node `Pins` | Supported (N4) | the node's Add pin + rename; numeric pins: Convert Numeric To (`UNiagaraNode::ConvertNumericPinToType`) |
| Graph: links, pin defaults, Map Get default pins | `Links`, `PinDefaults` | Supported (N4) | schema `TryCreateConnection`; Map Get default pins after their parameter's `DefaultMode: Custom` |
| Graph: function call version switch | node `Version` | Supported (N4) | as `FNiagaraFunctionCallNodeDetails::SwitchToVersion` (`ChangeScriptVersion`, then refresh) |
| Graph: data interface function specifiers | `Properties.FunctionSpecifiers` | Supported (N4) | the node's specifier widget (`Specifiers` map) |
| Graph: If node, Usage Selector, VM fast-path calls | `Niagara:If`, `Niagara:UsageSelector`, `Niagara:Function:Signature:*` | Read only | the menu does not offer them; `Graph.ActionNotFound` |
| Graph: deprecated / hidden data interface functions | `Niagara:DataInterfaceFunction:*` | Refused | `Niagara.DeprecatedFunction` (existing nodes keep them; the menu no longer offers them) |
| Parameters panel Details (default mode, default, static switch default, metadata) | `Parameters` | Supported (N4) | `UNiagaraScriptVariable` Details; unused parameters leave the list as a reload does |
| Version menu | `Versions`, `EditExposedVersion` | Supported (N4) | add, expose, delete; editing the exposed version needs `EditExposedVersion` (`Niagara.ExposedVersionEdit`) |
| Compile / Apply | — | Supported (N4) | new compile errors fail at the node path (`Niagara.CompileFailed`) and roll back; dependents refresh (`Niagara.DependentsUpdated`); unsaved dependents refuse (`Niagara.DependentHasUnsavedChanges`) |
| Scratch pads in systems: add, rename, delete, edit | system `ScratchPads` | Supported (N4) | the Scratch Pad panel's view model; the same graph component |
| Scratch pads in emitters (from a system) | emitter `ScratchPads` | Supported (edit in place); add / rename / delete Refused (`Niagara.ScratchPadScope`) | the emitter's pads come with the emitter asset |
| Emitter editor: every emitter surface above (Details, stacks, renderers, scratch pads add / rename / delete / edit) | emitter asset `Body` | Supported (N5) | the emitter editor's working copy (`MakeEmitterPlaceholder`, a placeholder system in EmitterAsset mode) and its Apply (`UpdateOriginalEmitter`: `StaticDuplicateObject` over the asset, `ClearResolvedData`, `PostEditChange`); not exported, same result |
| New Niagara Emitter from an existing emitter (copy / inherit) | `From`, `Parent` on `Action: Create` | Supported (N5) | `UNiagaraEmitterFactoryNew` with `EmitterToCopy`: `CreateWithParentAndOwner` or a copy with versioning disabled; not inheritable = `Niagara.NotInheritable` |
| Emitter asset settings | top-level `Properties` (`bIsInheritable`, `TemplateAssetDescription`, ...) | Supported (N5) | generic property admission outside the version data |
| Emitter version menu | `Versions`, `EditExposedVersion` | Supported (N5) | as for scripts; the Body describes the exposed version |
| Emitter Compile / Apply and dependents | — | Supported (N5) | new compile errors fail (`Niagara.CompileFailed`); loaded children and systems merge and recompile (`UpdateExistingEmitters`, `Niagara.DependentsUpdated`); unsaved dependents refuse (`Niagara.DependentHasUnsavedChanges`); dependents open in an editor follow as after the emitter editor's Apply (the editors' working copies merge, system editors `RefreshAll`): `UEAssetDoc.Niagara.OpenEditorApply.Dependents` |
| Lightweight emitter asset | `Stateless` | Refused (`Niagara.StatelessEmitter`) | lightweight emitters exist only inside systems |
| Parameter collection editor: add, delete, rename, retype, value | `Parameters` | Supported (N5) | the collection's Details customization (`UNiagaraParameterCollection` Parameters / DefaultInstance); values through the Niagara value codec |
| Collection Namespace, Source Material Collection | top-level `Properties` | Supported (N5) | a namespace in use is refused (`Niagara.NamespaceInUse`; the editor would rename it); material collection parameters are owned (`Niagara.CollectionParameterOwned`) |
| Asset open in its editor | — | Supported | system, emitter asset, script: close / apply / reopen (`Niagara.EditorReopened`): the system editor keeps its own view models and scratch pad copies, the emitter and script editors edit a working copy they write back on Apply; edits there not applied yet (Apply / Apply Scratch enabled) refuse before any change (`Niagara.OpenInEditorUnsavedEdits`). Parameter collection: in place, the editor's view model rebuilds on the collection's change event. An apply is not an undo step. `UEAssetDoc.Niagara.OpenEditorApply.System` / `.EmitterAsset` / `.Script` / `.Collection` |

## Corpus results

Timings: Windows CI PC, Development editor, commit `cd369978`, suite `-j 3` (other agents' suites ran
at the same time), in milliseconds. `extract` = first Extract, `inspect`, `reload` = Extract after a package reload,
`size` = Body JSON bytes. Round trip = Extract → the document re-targeted to a new `/Game` asset with `Action: Create`
→ Extract equal to the source (`create`), then the same document applied again is a no-op (`reapply`). Every
asset of every tier also re-extracts equal (repeat, reload, Content Browser duplicate). Refused round trips must fail
with the named code and leave no asset.

### Read smoke (N1)

Every system and emitter under the engine and enabled plugin mounts (52 systems + 81 emitters, 17,410 rows):
0 failures, 34.5 s total, median 63 ms. Slowest: `Grid2D_Gas_Control_Emitter` 2387, `Grid2D_SW_River` 2139,
`Grid3D_Gas_Master_Emitter` 1758, `Grid3D_Gas_ParticleAdvection` 1711.

### Emitters (N1 read, N5 round trip as emitter assets)

All 22 extract, re-extract equal, reload equal and duplicate equal. N5 round trip (Mac, sandbox niagara5, Development
editor, milliseconds): the extract as a `Create` document of a new emitter asset gives the same document (version
guids aside: a copy's versions get new ones), and re-applying it is a no-op.

| Asset | extract | size | create | reapply | Result |
|---|---|---|---|---|---|
| CompletelyEmpty | 10 | 604 | 636 | 32 | round trips |
| Minimal | 17 | 1,287 | 767 | 51 | round trips |
| SingleLoopingParticle | 19 | 1,655 | 807 | 58 | round trips |
| SubUVAnimation | 24 | 1,963 | 947 | 59 | round trips |
| LocationBasedRibbon | 21 | 2,406 | 955 | 63 | round trips |
| SimpleSpriteBurst | 20 | 2,535 | 830 | 62 | round trips |
| RecycleParticlesInView | 42 | 2,729 | 1702 | 116 | round trips |
| RibbonID | 35 | 2,951 | 1647 | 89 | round trips |
| Fountain | 39 | 3,305 | 1549 | 118 | round trips |
| KillParticles | 37 | 3,455 | 1637 | 93 | round trips |
| SpawnGroupsFromMultipleBones | 32 | 3,749 | 1520 | 83 | round trips |
| RendererBindingOverrides | 34 | 4,167 | 1176 | 96 | round trips |
| DirectionalBurst | 27 | 4,500 | 1446 | 79 | round trips |
| OmnidirectionalBurst | 39 | 4,511 | 1261 | 116 | round trips |
| DynamicBeam | 25 | 4,740 | 1144 | 73 | round trips |
| ConfettiBurst | 42 | 4,804 | 2121 | 130 | round trips |
| BlowingParticles | 66 | 5,172 | 4947 | 188 | round trips |
| HangingParticulates | 45 | 6,497 | 2103 | 136 | round trips |
| UpwardMeshBurst | 35 | 7,340 | 1809 | 103 | round trips |
| StaticBeam | 24 | 4,777 | — | — | Refused (`Niagara.DynamicInputType`): `Beam Start` uses SimulationPosition, whose output type the input no longer takes (its dynamic input menu does not offer it) |
| GroomRodsEmitter | 27 | 10,625 | — | — | Refused (`Niagara.CompileFailed`): the emitter does not compile on its own (`Emitter.*` values its system sets); a new asset has no errors before the apply |
| WorkingWithAttributes | 45 | 20,056 | — | — | Refused (`Graph.PinNotFound`): a scratch pad's Map Get default pin keeps an older engine's name (`<output>_Input`; the editor now names it after its output) |

### Stress emitters (N5, NiagaraFluids; refusals recorded)

Three of them show the Summary View in their editor data; until the summary view was read their extracts had only what that view lists (see
the design, section 1), which is why the earlier sizes were smaller.

| Asset | extract | inspect | reload | size | Round trip |
|---|---|---|---|---|---|
| SourceParticles (2 versions; summary view) | 146 | 171 | 135 | 6,999 | round trips (create 13,816, reapply 459; PC) |
| Grid3D_Gas_Emitter (summary view) | 760 | 1,221 | 1,079 | 57,634 | Refused (`Graph.ConnectNeedsConversion`): a pad links a Position output to a Vector pin (PC) |
| ParticleSurfacing_Emitter | 142 | 234 | 177 | 58,724 | Refused (`Graph.PinNotFound`): a pad's custom HLSL node has a `NiagaraParameterMap` output the current node does not make |
| SandEmitter (summary view) | 91 | 173 | 109 | 11,407 | Refused (`Niagara.ModuleUsage`): `Grid3D_CreateUnitToWorldTransform` sits in EmitterSpawn, which the module's usage no longer offers (PC). Before that the summary view hid its stacks (empty module lists, so the copy missed `Emitter.NeighborGrid`) |

### Emitter asset scenarios (N5)

`EmitterAsset.Apply` (details, stacks, renderer, scratch pad add / delete, no-op, reload, no Undo history left on the
asset, compile error rollback, refusals): apply 685. `EmitterAsset.Inheritance`: a child (`Parent`), a copy (`From`) and a system using the
child; a dirty dependent refuses; the parent's edit merges into the child and the system and not into the copy
(apply with dependents 831); versions 1.0 / 1.1 exposed, `Niagara.ExposedVersionEdit` and the opt-in. `EmitterAsset.SummaryView`: an
emitter asset and a system emitter with the Summary View on read, edit and re-apply as with it off; the toggle stays on.
`EmitterAsset.FreeEdits`: the same edits (determinism, random seed, a renderer's sort order) through the editor's API on
one duplicate and through a document on another read the same (apply 846).

### Parameter collections (N5)

`NPC_Built`: extract 7, 413 bytes. `Collection.Apply`: add / rename / retype / delete, a curve data interface value,
refusals (material collection parameters: Python planner tests only). `Collection.VRTemplate`: the template's `NPC_VRTemplate` round trips
(create 67), its namespace in use is refused, and a system's Color module links `NPC.NPC_VRTemplate.TeleportColor`
and compiles.

### Systems (N2–N4)

| Asset | extract | inspect | create | reapply | Result |
|---|---|---|---|---|---|
| DefaultSystem | 131 | 421 | 2730 | 535 | round trips |
| DirectionalBurst | 132 | 481 | 3580 | 453 | round trips |
| RadialBurst | 266 | 930 | 6528 | 691 | round trips |
| SimpleExplosion | 243 | 1174 | 6231 | 717 | round trips |
| AttributeReaderTrails | 256 | 765 | 4310 | 658 | round trips |
| RenderTargetTexturePainter (5 scratch pads, custom HLSL) | 162 | 732 | 13266 | 661 | round trips |
| NiagaraShore_System (simulation stages) | 97 | 353 | 1732 | 354 | round trips |
| NDCCollision_FountainDemo (data channels) | 238 | 1012 | 11473 | 941 | round trips |
| VectorFieldVisualizationSystem | 157 | 366 | — | — | Refused (`Niagara.InvalidModuleName`): module `UpdateAge` of script ParticleState, a name the editor never gives a new module |
| MinimalLightweight (stateless) | 99 | 194 | 978 | 374 | round trips |
| DirectionalBurstLightweight | 82 | 170 | 968 | 493 | round trips |
| FountainLightweight | 116 | 198 | 848 | 391 | round trips |

### Stress systems (N4)

| Asset | pads | extract | inspect | reload | size | no-op reapply | pad edit | Round trip |
|---|---|---|---|---|---|---|---|---|
| Grid2D_Gas_Explosion | 11 | 549 | 1522 | 841 | 79,765 | 1729 | 6054 | Refused (`Niagara.DeprecatedFunction`): a pad calls `Grid2DCollection.GetWorldBBoxSize`, deprecated and hidden from the menu |
| Grid2D_SW_River | 43 | 2484 | 5736 | 2894 | 361,341 | 7581 | 15891 | Refused: module `EmitterLifeCycle` (`Niagara.InvalidModuleName`); a pad also links a Position output to a Vector input (`Graph.ConnectNeedsConversion`) |
| Grid3D_Gas_Fire | 4 | 920 | 1336 | 1268 | 65,550 | 2111 | 11019 | Refused (`Graph.ConnectNeedsConversion`): a pad links a Vector output to a Position pin |

`no-op reapply` = the extracted document applied to the asset itself (nothing changes, nothing is dirtied).
`pad edit` = one pad's graph changed and applied: the pad's Apply, its users refresh, the system compiles once.
The refusals are content the current editor cannot make again; editing these assets in place works (the pad edit
column).

### Scripts (N4)

| Asset | Kind | extract | size | create | reapply | Result |
|---|---|---|---|---|---|---|
| DefaultModule | small | 36 | 1,180 | 551 | 101 | round trips |
| DefaultFunction | small | 28 | 774 | 497 | 95 | round trips |
| DefaultDynamicInputs | small | 32 | 1,117 | 215 | 117 | round trips |
| CameraOffset | small | 36 | 1,836 | 629 | 108 | round trips |
| Abs_Float | small | 26 | 1,429 | 602 | 104 | round trips |
| MakeScaleMatrix | custom HLSL | 33 | 1,680 | 940 | 77 | round trips |
| SimulationPosition | static switches | 31 | 2,009 | 641 | 75 | round trips |
| AerodynamicDrag | custom HLSL, enum switches, propagated switches | 112 | 80,173 | 22958 | 354 | round trips |
| MatrixToQuaternion | small | 54 | 1,196 | — | — | Read only: VM fast-path calls (`Graph.ActionNotFound`) |
| MakeQuaternion | static switches | 50 | 33,250 | — | — | Read only: a pin named `Vector` that the menu's node now names `Vector3f` |
| ShapeLocation | static switches | 551 | 313,208 | — | — | Read only: If nodes (`Graph.ActionNotFound`) |
| Collision | large | 170 | 206,075 | — | — | Read only: If nodes |
| SkeletalMeshLocation | large | 219 | 217,426 | — | — | Read only: Usage Selector |

Free edits on duplicates (Apply only): ShapeLocation 3660, CameraOffset 533, DefaultModule 539. AI from scratch:
the `OrbitAroundPoint` module written from a document and used from a system's stack, 655.

## Engine notes

- **Scratch pad parameter panel leak.** Each scratch pad view model initialised by a headless
  `FNiagaraSystemViewModel` registers a `FNiagaraScriptToolkitParameterPanelViewModel` in the static
  `TNiagaraViewModelManager` map and never unregisters it (`Cleanup` is not exported). When a later view model lands at
  the same address, `check(FoundItem->Contains(ViewModel) == false)` crashes the editor. Reads and edits set
  `System.ScratchPadScripts` (and an emitter's pad list) aside while the view model initialises
  (`FScratchPadsSetAside`) and create pad view models only when a pad is edited (`EnsureScratchPadViewModels`).
- **Older assets' pin defaults.** Pins saved before autogenerated defaults were recorded hold the type default with an
  empty `AutogeneratedDefaultValue`; they read as generated (extras `type_default_pins`), not as authored defaults.
- **Binding-only parameters.** A parameter only a `DefaultBinding` uses joins the parameter list once the binding is
  set: the apply sets parameters in two rounds.
- **Map Get default pins** take links and values only after the parameter's `DefaultMode` is `Custom`: the graph
  applies without them, then the parameters, then the rest of the graph.
- **New-node Details.** Node `Properties` are read as differences from the node the menu makes: a new comment takes
  the Graph Editor settings' colour and bubble (`PostPlacedNewNode`), a new static switch has auto refresh on.
- **Numeric pins.** Older assets hold typed pins where the op's or function's signature is Numeric; those nodes list
  `Pins`, applied with Convert Numeric To before the links.
- **Emitter Apply over a held asset.** `StaticDuplicateObject` destroys the asset object and constructs the copy in
  its memory, which it cannot do while the object is reference counted (an apply that created the asset holds it to
  undo the creation: `ObjectItem->GetRefCount() == 0` assertion). Such an asset is reset to its class defaults and the
  copy is serialized into it (`DuplicationSeed`), then `PostDuplicate` and `PostLoad` run on it as on a duplicated root.
  The write-back runs outside the transaction (the editor's Apply is not one): recording the destroyed nodes' link
  breaks serializes pins of nodes already destroyed. A write-back names the emitter after its asset
  (`UNiagaraEmitter::PostDuplicate`), so a new asset's working copy is renamed after its first edit.
- **Emitter compile.** A placeholder made from the written-back asset compiles only scripts whose compile id changed:
  `RequestCompile(true)` (as the toolbar's Compile) recomputes them, so new errors are seen.
- **Presave warning.** Saving an emitter after Apply logs "Data interface count mismatch during script presave"
  (`ClearResolvedData` on the written-back scripts, as the editor does); the next compile restores the data.
- **Staged copies.** Packages under `/Temp/` (the copies an apply's save stages) are not dependents.
- **Versioned renderers.** A new emitter version copies the renderers under new object names: a document that adds a
  version omits `Renderers`.
- **Static switch rename.** `UNiagaraGraph::RenameStaticSwitch` (not exported); the same result: the parameter's script
  variable is copied under the new name with a new metadata guid, so its Details survive the rename.
- **Not exported.** `UNiagaraNodeStaticSwitch` (name, type: `ChangeSwitchParameterName`, `OnSwitchParameterTypeChanged`),
  `FNiagaraOpInfo`, `UNiagaraGraph::SetMetaData` / `RenameStaticSwitch`, the function call's propagated-switch helpers
  and `FNiagaraEditorUtilities::InitializeParameterInputNode` give the same result through reflection and the exported API.
