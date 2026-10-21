# AssetDocument Niagara profile: design

Status: design only (2026-10-06). No product code. Governing
rules: [design rules](asset-document-design-rules.md) §3 (rollback = functional equivalence),
§3.1 (Undo history), §3.2 (dependent assets), §4 (editor-equivalent raw code + behaviour test, no private headers,
no unexported symbols) and §5 (support matrix). Reference editor: the Niagara System / Emitter editor (stack view
models, `FNiagaraSystemViewModel`), the Niagara Script editor and the Scratch Pad. The legacy generators are not a
reference. Engine source read: `/Users/Shared/Epic Games/UE_5.7/Engine/Plugins/FX/Niagara/Source` (paths below are
relative to it unless they start with `/`).

## 1. Classes in scope

| Class | Decision | Profile | Phase | Reason |
|---|---|---|---|---|
| `/Script/Niagara.NiagaraSystem` | exact profile | `profile.niagara_system.v1` | N1–N4 | The main VFX asset. Its content (emitter handles, module stacks, renderers, user parameters, scratch pads) lives in sub-object graphs that generic property admission cannot see (coverage audit §3, row 4). |
| `/Script/Niagara.NiagaraEmitter` | exact profile, same emitter body | `profile.niagara_emitter.v1` | N2 | Reusable emitters / parents of system emitters. The body of one emitter is the same component as a system's emitter entry. |
| `/Script/Niagara.NiagaraScript` with usage Module / Function / DynamicInput | exact profile (script graph) | `profile.niagara_script.v1` | N4 | Modules, functions and dynamic inputs are node graphs (Script editor). The same graph component authors scratch pad scripts inside systems and emitters, which real systems use (fidelity needs it). Generic create fails today (`ScratchCreate.InvalidIdentity`). |
| `/Script/Niagara.NiagaraParameterCollection` (+ `...Instance`) | small exact profile | `profile.niagara_parameter_collection.v1` | N5 | The parameter list is generic-visible, but the values live in an `FNiagaraParameterStore` byte buffer that the generic codec cannot read or write. Systems link inputs to `NPC.<Collection>.<Name>`, so an AI building a system from scratch needs it. Uses the same Niagara value codec; cheap. |
| `NiagaraEffectType`, `NiagaraValidationRuleSet`, `NiagaraSystemCollection`, `NiagaraSimCache` (read) | generic route | — | — | Pure Details-panel assets; the coverage audit creates and edits them generically. Nothing graph-shaped. |
| `NiagaraParameterDefinitions` | generic route, Unsupported for structure | — | — | Rare (shared parameter libraries). Its script variables are the same component as script parameters; can be added after N4 if asked. |
| `NiagaraStatelessEmitterTemplate`, `NiagaraDataChannel*` assets | out of scope | — | — | Template authoring for lightweight emitters is engine-internal (`Niagara/Internal/Stateless`, `NiagaraEditor/Internal/ViewModels/NiagaraStatelessEmitterTemplateViewModel.h`); data channel layouts are rare. Stable `Unsupported` diagnostics; generic route stays for their reflected properties. |
| Lightweight (stateless) emitters inside a system | in scope, later | emitter entry `Mode: "Stateless"` | N3 | Plain reflected objects (`UNiagaraStatelessEmitter`: spawn infos, `UNiagaraStatelessModule` list, renderers); read via reflection without the Internal header. |

Not authored anywhere (editor state, not content): the System Overview graph layout and folders, viewport/preview
settings, playback range, thumbnails, stack expansion and summary-view layout (`SummaryViewRoot`), dismissed issues,
baker settings, scalability preview state, the message store. They are kept as they are (Body-owned, never written).
The summary-view toggle (`bShowSummaryView`) is editor state too, but it changes what the stack view model lists: with
the summary on, an emitter's stack has only the Emitter Summary group. Reads and edits therefore work on the full stack
(the toggle cleared for the duration, as a user who turns the summary off, and put back afterwards; `FFullStackView`).

## 2. Editor data model: authored vs derived

### 2.1 Authored state (Extract emits it)

`UNiagaraSystem` (`Niagara/Classes/NiagaraSystem.h`)

- Details-panel properties (`EditAnywhere`: `WarmupTime`, `bDeterminism`, `RandomSeed`, `bFixedBounds`/`FixedBounds`,
  `EffectType`, `Platforms`, `SystemScalabilityOverrides`, rendering overrides, `bFixedTickDelta`, ...): generic `Properties`.
- `EmitterHandles` (`FNiagaraEmitterHandle`: `Name`, `Id`, `bIsEnabled`, `EmitterMode`, `VersionedInstance` = the
  system's own copy of an emitter, optionally `StatelessEmitter`).
- `SystemSpawnScript` / `SystemUpdateScript` share one `UNiagaraScriptSource` → `UNiagaraGraph`; the stack is the chain
  of `UNiagaraNodeFunctionCall` modules between the input node and the `UNiagaraNodeOutput` of each usage.
- `ExposedParameters` (User parameters: names, types, values) plus `UNiagaraSystemEditorData::UserParameterMetaData`
  (descriptions, categories via `UserParameterHierarchy`).
- `ScratchPadScripts` (system-level scratch pad `UNiagaraScript`s).
- Declared but unwritten system-scope parameters: `UNiagaraEditorParametersAdapter::EditorOnlyScriptVars`.
- Stack notes and custom stack display names: `UNiagaraStackEditorData::StackNotes`, `StackEntryKeyToDisplayName`.

`UNiagaraEmitter` / `FVersionedNiagaraEmitterData` (`Niagara/Classes/NiagaraEmitter.h:239`)

- Emitter Properties (`bLocalSpace`, `bDeterminism`, `RandomSeed`, `InterpolatedSpawnMode`, `SimTarget`,
  `CalculateBoundsMode`, `FixedBounds`, `bRequiresPersistentIDs`, `AllocationMode`, `PreAllocationCount`,
  `MaxGPUParticlesSpawnPerFrame`, `Platforms`, `ScalabilityOverrides`, `EmitterDependencies`, `AttributesToPreserve`,
  `SimStageExecutionLoopEditorData`, ...). These are members of a USTRUCT inside `VersionData`, not UObject properties.
- One `GraphSource` graph holding every emitter stack: Emitter Spawn/Update, Particle Spawn/Update, each event handler
  (`ParticleEventScript` + usage id) and each simulation stage (`ParticleSimulationStageScript` + usage id).
- `EventHandlerScriptProps` (`FNiagaraEventScriptProperties`: source emitter / event, execution mode, spawn number, ...).
- `SimulationStages` (`UNiagaraSimulationStageBase` objects, usually `UNiagaraSimulationStageGeneric`).
- `RendererProperties` (`UNiagaraRendererProperties` subclasses: sprite, mesh, ribbon, light, component, decal, volume, ...).
- `ScratchPads` (`UNiagaraScratchPadContainer` → scratch pad scripts), `VersionedParent` (inheritance), the
  emitter's own versions when versioning is enabled (`bVersioningEnabled`, `ExposedVersion`).

Module input values (one stack input)

- Local value of a "rapid iteration" type (float, int, vectors, colors, quats, matrices; not bool, enum, static,
  UObject, data interface): `UNiagaraScript::RapidIterationParameters` of every script of that usage, entry
  `Constants.<UniqueEmitterName>.<ModuleName>.<Input>` (system modules: no emitter segment). Authored, although it
  sits on the script object.
- Other local values (bool, enum, static switch inputs): default value text of the override pin (on the module's
  override `UNiagaraNodeParameterMapSet`, static switches on the function call node itself).
- Linked parameter, dynamic input, custom expression, data interface, object asset: nodes hanging off the override
  pin (`UNiagaraNodeParameterMapGet`, nested `UNiagaraNodeFunctionCall`, `UNiagaraNodeCustomHlsl`,
  `UNiagaraNodeInput` with a `DataInterface` / `ObjectAsset`).
- Edit condition toggles are inputs of their own.

`UNiagaraScript` assets and scratch pad scripts

- Per version (`FVersionedNiagaraScriptData`): `ModuleUsageBitmask`, `Category`, `Description`, `Keywords`,
  `LibraryVisibility`, `bSuggested`, `ProvidedDependencies` / `RequiredDependencies`, deprecation and experimental
  messages, `NumericOutputTypeSelectionMode`, inline formats, `ScriptMetaData`, versions (`bVersioningEnabled`,
  `ExposedVersion`), and the `Source` graph.
- The graph: nodes (`UNiagaraNodeInput`, `UNiagaraNodeOutput`, `UNiagaraNodeParameterMapGet/Set`, function calls,
  `UNiagaraNodeOp`, custom HLSL, static switch, if / select, convert, reroute, comments, ...), pin defaults, links and
  the script variables (`UNiagaraGraph::VariableToScriptVariable`: per-parameter metadata, default mode, default
  value, edit/visible conditions, inline toggles, category and sort priority, widget customization).

`UNiagaraParameterCollection`: `Namespace`, `Parameters` (`FNiagaraVariable` list), values in
`DefaultInstance->ParameterStorage`, `SourceMaterialCollection`.

### 2.2 Derived state (never emitted; recomputed by compile or by the editor)

- Script compile output: `CachedScriptVM` (`FNiagaraVMExecutableData`), `CachedScriptVMId`, parameter collection /
  data interface / UObject caches, `ScriptExecutionParamStore`, shader maps (`ScriptResource`,
  `LoadedScriptResources`), `RapidIterationParametersCookedEditorCache`. `SetVMCompilationResults` overwrites them
  without `Modify()`.
- Copies of rapid iteration parameters: emitter spawn/update values copied into the system spawn/update scripts,
  particle values copied into `GPUComputeScript` (and into the spawn script with interpolated spawning), and entries
  that `PrepareRapidIterationParametersForCompilation` adds at module defaults. Only a script's own-usage entries
  that differ from the module default are authored.
- `GPUComputeScript` content, `SystemCompiledData`, `EmitterCompiledData`, emitter execution order, renderer draw /
  completion orders, `SharedEventGeneratorIds`, `SimStageExecutionData`, max instance / allocation counts, runtime
  estimates, `bIsReadyToRunCached`.
- Change ids and hashes: `UNiagaraEmitter::ChangeId`, `UNiagaraGraph::ChangeId`, `CachedUsageInfo` (`CompileHash*`),
  traversal caches, `LastBuiltTraversalDataChangeId`.
- `VersionedParentAtLastMerge` (the merge snapshot of the parent), merge messages.
- Entries the compile adds to `ExposedParameters` (user variables written by scripts but not declared) are reported,
  not authored.

Extract rule: emit the authored state above, using the stack's notion of "overridden" for inputs (an input whose
value differs from its module default, i.e. what the stack's Reset arrow shows). Everything in 2.2 and the editor
state in §1 is Body-owned and not writable through `Properties` (`Niagara.NotAuthoredProperty`).

## 3. Document shape

### 3.1 System

```jsonc
{
  "SchemaVersion": 1, "Target": "/Game/FX/NS_Sparks", "Class": "/Script/Niagara.NiagaraSystem",
  "Action": "CreateOrUpdate",
  "Properties": {"WarmupTime": 0.0, "bDeterminism": false},                  // System Details (generic, sparse)
  "Body": {
    "UserParameters": [
      {"Name": "User.SparkColor", "Type": "LinearColor", "Value": {"R": 1, "G": 0.6, "B": 0.2, "A": 1},
       "Description": "Tint of the sparks", "Category": "Look"},
      {"Name": "User.Count", "Type": "int32", "Value": 50}],
    "SystemSpawn": [],
    "SystemUpdate": [
      {"Name": "SystemState", "Script": "/Niagara/Modules/System/SystemState.SystemState",
       "Inputs": {"Loop Behavior": "Once"}}],
    "Emitters": [
      {"Name": "Sparks",
       "Properties": {"SimTarget": "CPUSim", "CalculateBoundsMode": "Dynamic"},  // Emitter Properties (sparse)
       "EmitterUpdate": [
         {"Name": "EmitterState", "Script": "/Niagara/Modules/Emitter/EmitterState.EmitterState",
          "Inputs": {"Life Cycle Mode": "Self", "Loop Behavior": "Once"}},
         {"Name": "SpawnBurst_Instantaneous",
          "Script": "/Niagara/Modules/Emitter/SpawnBurst_Instantaneous.SpawnBurst_Instantaneous",
          "Inputs": {"Spawn Count": {"Link": "User.Count"}}}],
       "ParticleSpawn": [
         {"Name": "InitializeParticle",
          "Script": "/Niagara/Modules/Spawn/Initialization/V2/InitializeParticle.InitializeParticle",
          "Inputs": {"Lifetime": {"Dynamic": {"Script": "/Niagara/DynamicInputs/UniformRange/UniformRangedFloat.UniformRangedFloat",
                                              "Inputs": {"Minimum": 0.4, "Maximum": 0.8}}},
                     "Color": {"Link": "User.SparkColor"}}},
         {"Name": "AddVelocityInCone",
          "Script": "/Niagara/Modules/Spawn/Velocity/AddVelocityInCone.AddVelocityInCone",
          "Inputs": {"Velocity Strength": 400.0}}],
       "ParticleUpdate": [
         {"Name": "GravityForce", "Script": "/Niagara/Modules/Update/Forces/GravityForce.GravityForce"},
         {"Name": "ScaleColor", "Script": "/Niagara/Modules/Update/Color/ScaleColor.ScaleColor",
          "Inputs": {"Scale Alpha": {"Expression": "1 - Particles.NormalizedAge"}}},
         {"Name": "SolveForcesAndVelocity", "Script": "/Niagara/Modules/Solvers/SolveForcesAndVelocity.SolveForcesAndVelocity"}],
       "Renderers": [
         {"Name": "Sprite", "Class": "SpriteRenderer",
          "Properties": {"Material": "/Niagara/DefaultAssets/DefaultSpriteMaterial.DefaultSpriteMaterial",
                         "Alignment": "VelocityAligned"}}]}]
  }
}
```

(Module input names are the ones the stack shows, e.g. `SpawnRate` / `Spawn Probability` on SpawnRate,
`Minimum` / `Maximum` on UniformRangedFloat, `Gravity` on GravityForce; read them from Inspect, §3.6.)

### 3.2 Regions

System `Body`:

| Key | Content | Present = |
|---|---|---|
| `UserParameters` | list of `{Name, Type, Value?, Description?, Category?, RenamedFrom?}` (`Name` with its `User.` namespace) | complete (unlisted user parameters are removed; order = panel order) |
| `Parameters` | declared, unwritten system-scope parameters `{Name, Type, Description?}` (parameter panel) | complete |
| `SystemSpawn`, `SystemUpdate` | module lists (§3.3) | complete, ordered |
| `Emitters` | emitter entries (§3.4) | complete (unlisted emitters are deleted) |
| `ScratchPads` | scratch pad scripts `{Name, Usage, Script-graph body}` (§3.5) | complete |

Emitter asset `Body`: exactly the keys of one emitter entry except `Name`, `Enabled` and `Mode` (an emitter asset
has no handle). Its `Parent` is the asset's inheritance parent; `Versions` (N2+) lists the versions of a versioned
emitter (`{Version, Exposed?, ChangeDescription?}`); the body describes the exposed version.

Emitter entry:

| Key | Content | Present = |
|---|---|---|
| `Name` | handle name, unique in the system (identity) | required |
| `RenamedFrom` | old name (rename keeps the handle id and updates references, as the overview's rename does) | — |
| `Enabled` | handle enabled | omitted = true for a new emitter, keep for an existing one |
| `Mode` | `Standard` (default) / `Stateless` | omitted = keep |
| `Parent` | `{"Emitter": "<path>", "Version"?: "<guid>"}` or `null` | omitted = keep; `null` = Remove Parent Emitter; a different emitter is refused (`Niagara.ParentImmutable`: the editor has no reparent; delete and re-add the emitter) |
| `From` | create only: the emitter asset the "Add Emitter" dialog copies (`AddEmitterFromAssetData`); keeps inheritance when that emitter is inheritable | ignored on existing entries |
| `Properties` | Emitter Properties (sparse; values of `FVersionedNiagaraEmitterData`) | sparse |
| `EmitterSpawn`, `EmitterUpdate`, `ParticleSpawn`, `ParticleUpdate` | module lists | complete, ordered |
| `EventHandlers` | `[{Properties: {SourceEmitter, SourceEvent, ExecutionMode, SpawnNumber, MaxEventsPerFrame, ...}, Modules: [...]}]` | complete; matched by position |
| `SimulationStages` | `[{Name, Class?, Enabled?, Properties?, Modules: [...]}]` | complete, ordered; matched by `Name` (`SimulationStageName`) |
| `Renderers` | `[{Name, Class, Enabled?, Properties?}]` | complete, ordered |
| `ScratchPads` | emitter scratch pad scripts | complete |
| `Parameters` | declared emitter/particle parameters | complete |
| `Stateless` | (`Mode: Stateless`) `{Properties, SpawnInfos, Modules: [{Class, Enabled?, Properties?}], Renderers}` | complete |

Module entry (one stack item; lists are ordered = execution order):

```jsonc
{"Name": "AddVelocityInCone",              // the module's function name: identity, unique in its graph
 "Script": "/Niagara/Modules/...",         // module script asset or "ScratchPad:<name>"
 "Version": "<guid>",                      // script version; omitted = the exposed version
 "Enabled": true,                          // omitted = true
 "DisplayName": "Cone burst",              // stack rename (alternate display name); omitted = none
 "Note": {"Header": "...", "Body": "..."}, // stack note; omitted = none
 "Inputs": {"<input>": <value>, ...}}      // complete: an omitted input is reset to its default
```

`Set Parameters` modules (`UNiagaraNodeAssignment`): `{"Name", "Assign": [{"Target": "Particles.Color", "Type":
"LinearColor"}], "Inputs": {"Particles.Color": ...}}`.

Input value forms (the stack's value modes, `ENiagaraClipboardFunctionInputValueMode`):

| Form | Mode | Editor call |
|---|---|---|
| a JSON literal (`2.5`, `true`, `"Once"`, `{"X":0,"Y":0,"Z":1}`, `{"R":..}`) | Local | `UNiagaraStackFunctionInput::SetLocalValue` |
| `{"Link": "User.Count"}` | Linked | `SetLinkedParameterValue` |
| `{"Dynamic": {"Script", "Version"?, "Name"?, "Inputs": {...}}}` | Dynamic (nested, recursive) | `SetDynamicInput` |
| `{"Expression": "<hlsl>"}` | Expression | `SetCustomExpression` |
| `{"DataInterface": {"Class": "...", "Properties": {...}}}` | Data | `SetDataInterfaceValue` + Details edit of the DI |
| `{"Object": "<object path>"}` | ObjectAsset | `SetObjectAssetValue` |

Type tokens (`UserParameters[].Type`, `Assign[].Type`, script parameters): `float`, `int32`, `bool`, `Vector2`,
`Vector`, `Vector4`, `LinearColor`, `Quat`, `Position`, `Matrix`, `ID`, a Niagara enum or struct path
(`/Niagara/Enums/ENiagara_...`, `/Script/...`), a data interface class path, `Object:<class path>`. Values use the
AssetDocument value forms (float32 shortest decimal, enum names, structs as objects); the scalar wrapper structs
(`FNiagaraFloat`, `FNiagaraInt32`, `FNiagaraBool`) are unwrapped to plain JSON scalars.

### 3.3 Identity

- Emitters: handle `Name` (`RenamedFrom` to rename); the handle `Id` GUID is kept and never authored. Order:
  extract order = handle order; new emitters are appended in document order; the editor cannot reorder handles, so
  a reordered list is not a change (stated in the profile doc).
- Modules: the function call's name (`UNiagaraNodeFunctionCall::GetFunctionName`, unique in its graph: the system
  graph, or the emitter graph that holds all of an emitter's stacks). The editor derives it from the script name and
  makes it unique (`ComputeNodeName`: `AddVelocity`, `AddVelocity001`); a new module passes the document key as the
  suggested name, so the key survives. A name that appears in another stack list of the same graph is a move
  (`MoveModule` semantics; the node, its overrides and GUIDs follow). It is stable across save, reload and
  Content Browser duplicate (node properties are duplicated). The stack's display rename is `DisplayName` (stored in
  the stack editor data), not the identity.
- Inputs: the stack input name without the `Module.` namespace (`"Spawn Rate"`); nested dynamic inputs inside their
  `Dynamic.Inputs`.
- Renderers: renderer object name (as Material expressions: a new renderer is renamed to its key; Extract keeps the
  editor's names, e.g. `NiagaraSpriteRendererProperties_0`). `Class` short names: `SpriteRenderer`, `MeshRenderer`,
  `RibbonRenderer`, `LightRenderer`, `ComponentRenderer`, `DecalRenderer`, `VolumeRenderer`, ... (the registered
  `FNiagaraRendererCreationInfo` classes), other classes by path.
- Simulation stages: `SimulationStageName`; event handlers: position.
- User and declared parameters: full name; `RenamedFrom` renames like the parameter panel (references follow).
- Scratch pads: script name in the container. A module references one with `"Script": "ScratchPad:<name>"`.
- Script graphs (§3.5): node `Id` = the node's object name (as Material expressions), links by `Id:Pin`.

### 3.4 Omitted versus present

Follows the core guide: an omitted Body key keeps the region, a present list is complete. Deviations, stated per
field above: emitter / renderer / simulation stage / event handler `Properties` are sparse (only listed values are
written; Extract lists those that differ from a new object of the class), module `Inputs` are complete (omitted =
the stack's Reset to default), `Enabled` omitted means true for a new entry and keep for an existing one.

### 3.5 Script graph (module / function / dynamic input assets and scratch pads)

Reuses the Blueprint graph region shape (`Nodes`, `Links`, `PinOverrides`, `Properties`) with Niagara action keys:

```jsonc
"Body": {
  "Usage": "Module",                                  // Module | Function | DynamicInput (immutable after create)
  "Settings": {"ModuleUsageBitmask": ["Particle"], "Category": "Forces", "Description": "...", "LibraryVisibility": "Library"},
  "Parameters": [{"Name": "Module.Strength", "Type": "float", "Default": 1.0,
                  "Meta": {"Description": "...", "Category": "Main", "EditCondition": "Module.Enabled"}}],
  "Graph": {
    "Nodes": [
      {"Id": "InputMap", "Action": "Niagara:Input:ParameterMap"},
      {"Id": "Get", "Action": "Niagara:MapGet", "Pins": {"Outputs": ["Module.Strength", "Particles.Velocity"]}},
      {"Id": "Mul", "Action": "Niagara:Op:Numeric::Mul"},
      {"Id": "Set", "Action": "Niagara:MapSet", "Pins": {"Inputs": ["Particles.Velocity"]}},
      {"Id": "Out", "Action": "Niagara:Output"}],
    "Links": [{"From": "InputMap:Output", "To": "Get:Source"}, {"From": "Get:Module.Strength", "To": "Mul:A"},
              {"From": "Get:Particles.Velocity", "To": "Mul:B"}, {"From": "Mul:Result", "To": "Set:Particles.Velocity"},
              {"From": "Get:Dest", "To": "Set:Source"}, {"From": "Set:Dest", "To": "Out:Output"}]}
}
```

Action keys: `Niagara:Function:<script path>`, `Niagara:Op:<OpName>`, `Niagara:MapGet`, `Niagara:MapSet`,
`Niagara:MapFor`, `Niagara:Input:<parameter>`, `Niagara:Output`, `Niagara:CustomHlsl`, `Niagara:StaticSwitch`,
`Niagara:If`, `Niagara:Select`, `Niagara:UsageSelector`, `Niagara:SimTargetSelector`, `Niagara:Convert`,
`Niagara:Reroute`, `Niagara:Comment`, `Niagara:ReadDataSet` / `WriteDataSet`; the generic fallback key for anything
else (`graph.actions.enumerate.v1` lists them). Map Get / Set pins (`Pins.Inputs` / `Pins.Outputs`) are the
parameter names the node reads or writes; dynamic pins on ops / custom HLSL are listed the same way with types.

### 3.6 Inspect

`inspect_asset_document_target` on a system / emitter returns the document plus, per module, its input list
(`"Velocity Strength: float = 400 (overridden)"`, value mode, edit / visible condition state, static switch
options), each group's available modules (`list_types`-like: script asset paths filtered by usage, as the stack's
"+" menu lists them), renderer classes, linkable parameters per stack (the stack's "Link inputs" menu), and
`Compile` (§5). On a `/Niagara/...` or other plugin asset Inspect is read-only facts (module inputs, outputs, usage,
versions), so an AI can discover modules without reading the engine graph.

## 4. Editor-equivalent operations

The apply builds the editor's own view models headlessly, the way the engine does for merges and upgrade scripts
(`NiagaraScriptMergeManager.cpp:1443`, `NiagaraEditorUtilities.cpp:3866`): `FNiagaraSystemViewModel` with
`bIsForDataProcessingOnly = true`, `bCanAutoCompile = false`, `bCanSimulate = false`, `bCanModifyEmittersFromTimeline
= false` (`NiagaraEditor/Public/ViewModels/NiagaraSystemViewModel.h:79`), then walks the stack
(`UNiagaraStackViewModel::GetRootEntry`, `UNiagaraStackEntry::GetUnfilteredChildren`) and calls the same exported
methods the stack widgets call. Every such call opens its own `FScopedTransaction` and does the editor's follow-ups
(relayout, rapid-iteration parameter updates on all affected scripts, parameter renames, notifications). All of
`NiagaraEditor/Public/ViewModels/**` is public; the class-level macro is absent but the members below carry
`NIAGARAEDITOR_API` (checked) or are virtual (called through the vtable, no import needed).

| Edit | Editor path (what a human triggers) | Our call | New C++? |
|---|---|---|---|
| New system | Content Browser → Niagara System (dialog: template / emitters / empty) | generic factory lifecycle (`UNiagaraSystemFactoryNew`, no dialog = empty system with default nodes, `InitializeSystem`); emitters then added by the body | no (factory) |
| New emitter asset | `UNiagaraEmitterFactoryNew` (dialog fields are not UPROPERTYs) | generic factory → empty emitter (`InitializeEmitter`); `Body.From` copies a template (same result as the `EmitterToCopy` path: `StaticDuplicateObject` + `SetUniqueEmitterName` + `DisableVersioning`, raw, isolated) | raw file |
| System / emitter Details | Details panel | generic property admission (system); emitter `Properties`: Modify, write the `FVersionedNiagaraEmitterData` member with the typed value codec, `UNiagaraEmitter::PostEditChangeVersionedProperty` (virtual; what `NiagaraEmitterDetailsCustomization.cpp:54` does) | primitive op |
| Add emitter | "+ Emitter" / drag asset | `FNiagaraSystemViewModel::AddEmitter(const FVersionedNiagaraEmitter&)` / `AddEmptyEmitter` / `AddMinimalEmitter` / `AddStatelessEmitter` (exported), then `FNiagaraEmitterHandleViewModel::SetName` | primitive op |
| Delete emitter | Delete | `FNiagaraSystemViewModel::DeleteEmitters` | op |
| Rename / enable emitter | overview rename, checkbox | `FNiagaraEmitterHandleViewModel::SetName`, `SetIsEnabled(b, false)` | op |
| Remove parent / change parent version | overview context menu | `FNiagaraEmitterViewModel::RemoveParentEmitter`, `FNiagaraEditorUtilities::SwitchParentEmitterVersion` (both exported; the latter may run the parent's Python upgrade script, as the editor does) | op |
| Add module | group "+" | `UNiagaraStackItemGroup::GetAddUtilities()` → `GenerateAddActions` / `ExecuteAddAction(action, index)` (virtual) for the stack's own filtering (usage bitmask, library, deprecation); or `FNiagaraStackGraphUtilities::AddScriptModuleToStack(UNiagaraScript*, OutputNode, Index, SuggestedName, Version)` (exported) followed by the same initialization the add action does | op |
| Delete module | Delete | `UNiagaraStackModuleItem::Delete` (exported; removes override nodes, cleans rapid iteration parameters, refuses inherited modules) | op |
| Reorder / move module | drag and drop | `UNiagaraStackEntry::Drop(FDropRequest)` with an `FNiagaraStackEntryDragDropOp` (inline type) on the target group / item: the stack's `ChildRequestDropInternal` → `MoveModule`. Fallback if headless drop proves unreliable: replicate `MoveModule`'s parameter-map relinking in an isolated raw file | op |
| Enable module | checkbox | `FNiagaraStackGraphUtilities::SetModuleIsEnabled` (exported) | op |
| Module version | version menu | `UNiagaraStackModuleItem::ChangeScriptVersion` | op |
| Display name, note | rename (F2), "Add Note" | `UNiagaraStackEntry::OnRenamed` (virtual) → stack editor data; stack note via the stack editor data | op |
| Input local value | value widget | `UNiagaraStackFunctionInput::SetLocalValue(FStructOnScope)` (RI parameter vs override pin decided by the editor) | op |
| Link / dynamic / expression / DI / object | input dropdown | `SetLinkedParameterValue`, `SetDynamicInput`, `SetCustomExpression`, `SetDataInterfaceValue` (+ DI Details edit via Modify/PreEditChange/PostEditChangeProperty on `GetDataValueObject()`), `SetObjectAssetValue` | op |
| Reset input | reset arrow | `UNiagaraStackFunctionInput::Reset` | op |
| Edit condition toggle | inline checkbox | `SetEditConditionEnabled` | op |
| Set Parameters module | "+ Set new or existing parameter" | `FNiagaraStackGraphUtilities::AddParameterModuleToStack` (exported), `UNiagaraNodeAssignment::AddAssignmentTarget` / `RemoveParameter` / `RenameAssignmentTarget` (exported) | op |
| Add / delete / reorder renderer | Render "+", Delete, Move Up/Down | render group add utilities (action matching the class of `FNiagaraEditorModule::GetRendererCreationInfos`); `UNiagaraStackRendererItem::Delete`, `MoveRendererUp/Down` (exported) | op |
| Renderer Details (incl. attribute bindings, material bindings) | stack object Details | Modify, PreEditChange, write with the typed value codec, PostEditChangeProperty on the renderer, then `PostEditChangeVersionedProperty` on the emitter (what `NiagaraStackRendererItem.cpp:631` does) | op |
| Event handler / sim stage add, delete, Details | "+ Stage" menu, Delete, Details | emitter properties group add utilities (`FNiagaraStackEmitterStageAddUtilities`, virtual) → `FNiagaraEmitterViewModel::AddEventHandler` (exported) / sim stage creation; `Delete` on the groups (exported); Details as renderers | op |
| User parameters | User Parameters panel "+", rename, delete, value | value: `ExposedParameters.SetParameterData` + notification as the stack's user parameter object row does; add / rename / delete: replicate `FNiagaraEditorUtilities::AddParameter` and the system parameter panel's rename / delete (not exported, view model constructor not exported): `ExposedParameters.AddParameter` / `RenameParameter` / `RemoveParameter`, `UserParameterMetaData` script variable, hierarchy category, references renamed in graphs (`UNiagaraGraph::RenameParameter`, exported) | raw file |
| Declared parameters | parameter panel | `EditorOnlyScriptVars` script variables (raw, isolated, same file) | raw file |
| Scratch pad add / rename / delete / edit | Scratch Pad panel | `UNiagaraScratchPadViewModel::CreateNewScript`, `FNiagaraScratchPadScriptViewModel::SetScriptName`, graph edits on `GetEditScript()` then `ApplyChanges()` (all exported); delete = remove the script after its modules are gone (`DeleteActiveScript` needs an active selection: `SetActiveScriptViewModel` first) | op |
| Script graph nodes / links / pin defaults | Script editor palette, wires | generic `graph.*` primitives with a Niagara schema adapter (`UEdGraphSchema_Niagara`: `GetGraphActions`, `FNiagaraAction_NewNode::PerformAction`, `TryCreateConnection`, `TrySetDefaultValue`, all exported) and a script host adapter (asset graph, scratch pad edit graphs). Map Get / Set dynamic pins: the editor's "drag onto the Add pin" path (`TryCreateConnection` to the add pin + `CommitEditablePinName`, virtual) because `RequestNewTypedPin` / `AddParameter` are not exported | adapters |
| Script parameters / metadata | parameter panel, Details of a script variable | `FNiagaraScriptToolkitParameterPanelViewModel` (exported constructor; `AddParameter`, `RenameParameter`, `DeleteParameters` virtual) for the script asset; scratch pads via `GetParameterPanelViewModel()`; metadata = Details edit of the `UNiagaraScriptVariable` (`PostEditChangeProperty`, exported) | adapters |
| Script settings | Script Details | Modify + write `FVersionedNiagaraScriptData` member + `UNiagaraScript::PostEditChangeVersionedProperty` (virtual) | op |
| Emitter asset apply | Emitter editor "Apply" | edit a placeholder system's copy (`UNiagaraSystemFactoryNew::InitializeSystem` (exported) + `AddEmitter`, edit mode `EmitterAsset`, as `FNiagaraSystemToolkit::InitializeWithEmitter`), then write back like `FNiagaraSystemToolkit::UpdateOriginalEmitter` (`NiagaraSystemToolkit.cpp:1148`): `StaticDuplicateObject` over the original, flags, `ClearResolvedData`, `PostEditChange`, then merge loaded child emitters (`MergeChangesFromParent`, exported) and recompile using systems (`UNiagaraSystem::RequestCompileForEmitter`). The modal "exposed version" warning and slow task are not reproduced (a versioned emitter's exposed version is edited only with `Versions` semantics, §7) | raw file |

Primitives (owner `FX`, provider `fx`, new module `AssetDocumentNiagara`, PostDefault, depends on `Niagara`,
`NiagaraCore`, `NiagaraEditor`, `AssetDocumentEditor`, `AssetDocumentGraph`; `UEAssetDoc.uplugin` adds the
`Niagara` plugin dependency):

- `fx.niagara.read.v1`: paged read of the system / emitter (handles, stacks with module rows and input rows as the
  stack view model reports them, renderers, stages, events, user parameters, scratch pads, compile status), plus
  `{scripts: [...]}` facts (inputs, outputs, usage, versions) for module discovery and new-module defaults.
- `fx.niagara.edit.v1`: op list over one headless view model per call (ops: `add_emitter`, `delete_emitter`,
  `emitter`, `emitter_properties`, `add_module`, `delete_module`, `move_module`, `module`, `input`, `reset_input`,
  `assignment`, `renderer`, `stage`, `event`, `user_parameter`, `parameter`, `scratch_pad`), op paths mapped back to
  document paths as in Material.
- `fx.niagara.compile.v1`: §5.
- Script graphs: no new primitive family; `graph.*` + the two adapters (`AssetDocumentNiagara` registers them).
- Generic property admission covers system `Properties` and parameter collection settings; renderer / DI / stage
  objects use the existing typed value codec through `fx.niagara.edit.v1` because their edit must be followed by the
  emitter notification.

Python: `profiles/niagara_system.py` (system + emitter entries), `niagara_emitter.py` (asset wrapper),
`niagara_stack.py` (module lists, inputs, values: shared), `niagara_values.py` (type tokens and value forms),
`niagara_script.py` (graph profile on `graphs/` with `graphs/schemas/niagara.py`), `niagara_parameter_collection.py`.
The stack diff planner works like Material's `graph_ops`: deletes, emitter creates, module creates (in document
order, at their indices), moves, input sets (sources-forward for links into new modules), renderers, stages, then
compile.

## 5. Compile, validation, rollback

- Edits are made with `bCanAutoCompile = false`, so nothing compiles mid-apply. The compile primitive runs the
  Compile button's effect: `UNiagaraSystem::RequestCompile(false)` then `WaitForCompilationComplete(true,
  /*bShowProgress*/ false)` (both `NIAGARA_API`, `NiagaraSystem.h:438/444`; `bShowProgress = true` would open an
  `FScopedSlowTask` dialog, `NiagaraSystem.cpp:3027`). Emitter assets compile through their placeholder system and
  then `RequestCompileForEmitter`. Scripts compile through the systems that use them (Script editor "Apply" =
  `UNiagaraScript` change + `RequestCompile` of loaded users; the profile compiles only the edited asset's preview
  and reports, it does not recompile every user synchronously).
- Status is computed as the toolbar does (`FNiagaraSystemViewModel::TickCompileStatus`, not exported, so
  derived here): over system spawn/update and every enabled emitter's compilable scripts, any script not
  `AreScriptAndSourceSynchronized()` or Unknown/Dirty → `Dirty`, else any error → `Error`, any warning →
  `UpToDateWithWarnings`, else `UpToDate`. Messages from `GetVMExecutableData().LastCompileEvents` and, for GPU
  scripts, `GetRenderThreadScript()->GetCompileErrors()`.
- Shape (same as Blueprint / StateTree): `Compile: {Status, Messages: [{Severity, Message, Emitter?, Stack?,
  Module?, Path?}]}`. An event is attributed to the module whose function call `NodeGuid` appears in its
  `StackGuids` (the stack's own mapping, `NiagaraMessages.cpp:231`), and `Path` is that module's document pointer.
- Failure rule (as Material): errors the asset did not have before fail the apply with `Niagara.CompileFailed` at the
  module's path and roll back; pre-existing errors return as `Niagara.CompileError` warnings; warnings return as
  `Niagara.CompileWarning`. Stack issues the editor shows (missing dependencies, deprecated modules, renderer
  missing attributes, validation rules) are warnings `Niagara.StackIssue` with their path, never failures.
- Rollback: Core package snapshot (existing asset) or factory rollback (created asset), no inverse code. A
  derived-state rebuilder (`AssetDocumentNiagara.System`) runs after a restore: `RequestCompile(false)` +
  `WaitForCompilationComplete(true, false)` (compile output is not transacted, and the DDC usually returns the
  previous result immediately). Emitter assets: the rebuilder also re-merges loaded children and requests
  recompiles of loaded users; dependents follow §3.2 (a dirty loaded child emitter or using system refuses the apply
  before any change: `Niagara.DependentHasUnsavedChanges`; succeeded applies report `Niagara.DependentsUpdated`).
- Undo: every stack call records its own transaction; a successful apply clears the history that references the
  asset (§3.1); a failed one after edits clears it too (as StateTree).
- Open editors (`AssetDocumentNiagaraOpenEditor.cpp`): a system, emitter asset or script open in its Niagara editor is
  closed for the apply and reopened (`Niagara.EditorReopened`): the emitter and script editors write their working copy
  back on Apply, and the system editor holds view models and scratch pad copies our headless edit would bypass. Pending
  edits there (Apply / Apply Scratch enabled) refuse before any change (`Niagara.OpenInEditorUnsavedEdits`). A
  collection editor stays open (its view model rebuilds on `OnCollectionChangedDelegate`). Dependents open in editors
  follow the emitter as after the emitter editor's Apply (`UpdateExistingEmitters`: working copies merge, system
  editors `RefreshAll`). Tests: `UEAssetDoc.Niagara.OpenEditorApply.*`.
- Non-interactive: everything runs inside `FAssetDocumentEditorNonInteractiveScope`; the slow-task false
  positive (`Material.InteractiveEditRefused`) must be fixed in that scope first (or the Niagara compile must stay
  dialog-free as above), otherwise Niagara applies inherit the same bug.

## 6. Fidelity acceptance

The corpus is real content from the UE 5.7 install. An inventory on 2026-10-06 found 112 systems, 91 emitters, about 1,190 scripts and one parameter collection. They all sit under plugin mounts. The `/Game/...` ones live in `Templates/` and have to be copied into the sandbox. Each asset is read from its plugin mount and duplicated to `/Game/AssetDocumentTests/Niagara/...`. Plugin content is never modified.

### 6.1 Corpus

| Tier | Assets | Covers |
|---|---|---|
| Systems, small | `/Niagara/DefaultAssets/DefaultSystem`, `/Niagara/DefaultAssets/Templates/Systems/DirectionalBurst`, `RadialBurst`, `SimpleExplosion`, `AttributeReaderTrails` | The New System dialog's own templates. Covers multi-emitter systems, sprite / ribbon / mesh renderers, dynamic inputs, links and GPU sim. |
| Systems, features | `/Niagara/DefaultAssets/Templates/BehaviorExamples/RenderTargetTexturePainter` (custom HLSL, sim stages, Grid2D / RenderTarget DIs), `/Water/Effects/Niagara/Shoreline/NiagaraShore_System` (sim stages), `/ExampleCustomDataInterface/FollowCursorSystem` (custom DI), `/Niagara/VectorFields/VectorFieldVisualizationSystem`, `/WaterAdvanced/Niagara/Systems/NDCCollision_FountainDemo` (data channels), `/ClonerEffector/Systems/NS_ClonerBase`, `/VirtualCamera/Modifiers/TeleportModifier/Dependencies/NS_TeleportUX` | Sim stages, DI values, custom HLSL expressions, data channel DIs, and scratch pads in plugin content. |
| Systems, stateless | `/Niagara/DefaultAssets/Templates/Systems/MinimalLightweight`, `DirectionalBurstLightweight`, `FountainLightweight` | `Mode: Stateless` (N3). |
| Systems, stress | `/NiagaraFluids/Templates/Gas/2D/Systems/Grid2D_Gas_Explosion` (~135 scratch pads), `/WaterAdvanced/Niagara/Systems/Grid2D_SW_River` (13.6 MB, 106 scratch pads), `/NiagaraFluids/Templates/Gas/3D/Systems/Grid3D_Gas_Fire` | Performance budget and scratch pads (N4). |
| Emitters | The 14 `/Niagara/DefaultAssets/Templates/Emitters/*` (Minimal ... UpwardMeshBurst), `/Niagara/DefaultAssets/Templates/BehaviorExamples/{SubUVAnimation, RibbonID, KillParticles, WorkingWithAttributes, SpawnGroupsFromMultipleBones, RendererBindingOverrides}`, `/Niagara/DefaultAssets/Templates/CascadeConversion/CompletelyEmpty`, `/HairStrands/Emitters/GroomRodsEmitter` | Emitter body, renderer bindings, and a scratch pad (`WorkingWithAttributes`). |
| Scripts | **Small:** `/Niagara/DefaultAssets/{DefaultModule, DefaultFunction, DefaultDynamicInputs}`, `/Niagara/Modules/Update/Camera/CameraOffset`, `/Niagara/DynamicInputs/Math/Abs_Float`, `/Niagara/Functions/Rotation/MatrixToQuaternion`. **Static switches:** `/Niagara/DynamicInputs/Helpers/SimulationPosition`, `/Niagara/DynamicInputs/Rotation/MakeQuaternion`, `/Niagara/Modules/Spawn/Location/V2/ShapeLocation`. **Custom HLSL:** `/Niagara/Functions/MakeScaleMatrix`, `/Niagara/Modules/Update/Forces/AerodynamicDrag`. **Large:** `/Niagara/Modules/Spawn/Location/V2/SkeletalMeshLocation`, `/Niagara/Modules/Collision/Collision`. | Script graph profile (N4). |
| Parameter collections | `/Game/VFX/NPC_VRTemplate` (copied from `Templates/TemplateResources/Standard/XRFramework`), plus the DMX instance `/DMXFixtures/Fireworks/FWGlobals` (read only) | N5. |

Three things have no corpus asset, so built scenarios cover them (6.3):

- light, component and decal renderers (no shipped asset uses them);
- event handlers (whether an asset uses them can't be found by scanning its strings);
- parented emitters.

### 6.2 What is compared

For each corpus asset there are two round trips:

1. Extract it, apply the document with `CreateOrUpdate` to a new sandbox path, compile, and Extract again.
2. Duplicate it, then Extract the duplicate.

Then compare:

1. **Documents.** The round-tripped document equals the first one, with floats normalized and the same §3.3 identities.
2. **Stack facts.** These are read from the editor's view models of the source and the result, not from raw stores:
   - per stack group, the ordered list of `(module function name, script path, version, enabled)`;
   - every input's value mode and value, as `UNiagaraStackFunctionInput` reports it;
   - renderer classes and their non-default properties;
   - sim stage and event handler properties;
   - user parameters (name, type, value, metadata).

   This is what a human sees. It also hides the storage choice (RI parameter vs override pin), which can differ between equivalent assets.
3. **Compiled outputs, where cheap:**
   - compile status `UpToDate`, or the same set of warnings;
   - per script, the compiled attribute list (`GetVMExecutableData().Attributes`) and the data interface list;
   - per emitter, the resolved renderer bindings.

   There is no bytecode or shader comparison, because the hashes include GUIDs and change ids.
4. **Editor-only state** (layout, thumbnails, note positions) is not compared.

Pass bar for each phase: 100 % of its tier, or a listed reason for each asset that fails. A listed reason is an `Unsupported` diagnostic code, never a silent loss.

There is also a smoke run: Extract on the full corpus of 112 systems and 91 emitters, headless with no editor UI. It must not crash or produce an unknown diagnostic, and its time is recorded.

### 6.3 Free-edit scenarios (on duplicates of corpus assets)

- **DirectionalBurst:**
  - rename an emitter (references in links follow);
  - disable another and delete a third;
  - add `/Niagara/DefaultAssets/Templates/Emitters/Fountain` with `From`;
  - set emitter `Properties`.
- **Module edits:**
  - insert `GravityForce` between two update modules;
  - move `AddVelocity` from Particle Spawn to Particle Update (same graph);
  - delete a module;
  - change a module's version and disable it;
  - rename its display name and add a note.
- **Input edits:**
  - take one input through every value mode in turn: local → link (`User.X`) → dynamic input with a nested dynamic input → expression → reset;
  - a data interface input (curve DI values);
  - an object asset input;
  - an edit condition toggle;
  - a static switch input. It changes which inputs exist, so re-Inspect must show the new list.
- **Renderers:**
  - add mesh and light renderers;
  - reorder them through the `Renderers` order;
  - change the sprite material and an attribute binding;
  - delete one.
- **User parameters:**
  - add one, and rename one (linked inputs follow);
  - try a retype (refused);
  - delete one while it is linked. As in the editor, this gives a warning and the link becomes a missing-parameter stack issue.
- **Sim stages and event handlers:**
  - add and reorder sim stages, and change a stage's iteration source;
  - add an event handler with a spawn module. This needs a `Generate Location Event` module in the source emitter.
- **Inheritance:**
  - create an emitter asset whose parent is `/Niagara/DefaultAssets/Templates/Emitters/Fountain`;
  - add it to a system;
  - try to delete an inherited module (`Niagara.InheritedModule`);
  - Remove Parent.
- **Failure:**
  - add a particle module to System Update. The usage filter refuses it (`Niagara.ModuleUsage`).
  - set a custom expression with an HLSL error. It compiles to an error and the whole apply rolls back (`Niagara.CompileFailed`).

  After a rollback the asset compiles, and Extract gives the same document as before.
- **Dependents:** apply an emitter asset while a child emitter and a system that uses it are loaded. The dependents rule (§3.2) applies.
- **Open editor:** apply while the asset is open in its editor (`Niagara.OpenEditorApply.*`).

### 6.4 AI from scratch

A fresh AI session gets only the MCP tools and `MCP/schemas/assetdocument/Niagara.md`. It writes a campfire with:

- flames: sprite, sub-UV flipbook, color over life, curl noise;
- embers: GPU sprites with gravity and drag;
- smoke: lit sprites, slow upward velocity, scale over life;
- a point light;
- user parameters for intensity and color.

It applies the document, fixes its own diagnostics and compiles clean. Then it handles three follow-up requests:

- "embers twice as many";
- "wind from the left on all emitters";
- "turn the light off by a user bool".

The session is recorded with calls, failed calls, diagnostics and wall time, plus a human look at the result in the Niagara editor.

A second scenario runs in N4: the AI writes a module script "OrbitAroundPoint" from scratch and uses it in the campfire.

## 7. Risks, unknowns, phases

### 7.1 Risks and unknowns

**Many node classes are private, and key stack utilities are not exported.**
- The private node classes include `UNiagaraNodeParameterMapGet/Set`, static switch, if, select, convert, reroute and usage selector.
- The unexported utilities include `RemoveModuleFromStack`, `MoveModule`, `SetCustomExpressionForFunctionInput`, `ResetGraphForOutput` and the rapid iteration helpers.
- Consequence: under the §4 rule they can't be called or even included.
- Answer:
  - Go through the exported stack view model objects. Their methods call the unexported utilities internally.
  - Edit script graphs through the schema actions (`FNiagaraAction_NewNode::PerformAction`) and virtuals.
  - Read node data through reflection and `UEdGraphPin`.
  - Only the parameter panels, the emitter write-back and possibly module move fall back to isolated raw files with behaviour tests.

**Headless stack view models.**
- Consequence: the engine uses them headlessly only for merge and upgrade. Untested paths (add utilities, Drop, renderer add) may assume a toolkit or Slate.
- Answer: an N1 spike drives each operation in an automation test on a duplicate of DefaultSystem, before the edit primitive is written. Anything that needs Slate gets the raw fallback.

**One input value has two possible storages.**
- It can sit in rapid iteration parameters or on override pins. There are also copies in the system and GPU scripts, and entries the compile adds.
- Consequence: a raw comparison is noisy, and raw writes are wrong.
- Answer:
  - Always write through `SetLocalValue`.
  - Extract and fidelity read values from the stack view model.
  - Only own-usage, non-default values count.

**Inheritance.**
- This covers the versioned parent, the 3-way merge on load, and `UpdateFromMergedCopy` replacing graph objects.
- Consequence: object pointers go stale, and inherited items can't be edited.
- Answer:
  - Resolve everything by name or GUID in each call, and never cache objects across ops.
  - Refuse edits to inherited modules and renderers, as the stack does.
  - Limit a `Parent` change to remove or version.

**Emitter asset editing.**
- It is copy-and-write-back, with a modal warning and recompiles of dependents.
- Consequence: this is the biggest editor-only path, and it affects other assets.
- Answer:
  - Give it its own phase (N5).
  - Use an isolated raw file that reproduces `UpdateOriginalEmitter` without UI.
  - Apply the dependents rule (§3.2).
  - For versioned emitters, edit only the exposed version. Refuse when it is the exposed version of a library emitter (`Niagara.ExposedVersionEdit`), unless the user decides otherwise (open question).

**Async compile.**
- Shader and VM jobs run through the compiling manager. Under `GIsAutomationTesting`, the wait loop breaks early after `fx.Niagara.CompileWaitLoggingCap` logs.
- Consequence: flaky tests and a false "Dirty" status.
- Answer:
  - The primitive owns the wait loop: it polls `WaitForCompilationComplete` within a time budget.
  - On timeout it reports `Dirty` plus `Niagara.CompileTimeout` instead of failing.
  - Tests raise the cap.

**Compile performance on large systems.**
- Some NiagaraFluids systems are 10+ MB, and GPU shaders are slow to compile.
- Consequence: applies can take minutes.
- Answer:
  - One compile per apply, with a warm DDC.
  - The stress tier records time only.
  - A `"Compile": false` request option leaves the status Dirty, as Material's deferred compile does.

**On-demand compile on load, and auto-compile in open editors.**
- Consequence: an open editor or a load can change derived state during Extract.
- Answer: refuse open editors. Extract never compiles; it only reads the status.

**Stateless emitters are engine-internal** (`Niagara/Internal/Stateless`).
- Consequence: there is no typed API.
- Answer: use reflection only, on the UPROPERTYs of `UNiagaraStatelessEmitter` and its modules. This is N3, and it may stay read-only if creation needs internal code.

**Version-specific APIs.**
- 5.7 adds versioned emitters, the summary view, hierarchy-based user parameter categories, stateless emitters and `UNiagaraStackEntry::Drop`.
- Consequence: the code may break on 5.8.
- Answer:
  - Keep all Niagara C++ in one module.
  - Gate on `ENGINE_MINOR_VERSION` where signatures differ.
  - The corpus test catches regressions.

**Python upgrade scripts.**
- They run when a module version changes (`SwitchParentEmitterVersion`, `ChangeScriptVersion`).
- Consequence: arbitrary Python runs inside an apply.
- Answer: allow it, because it is what the editor does. Record it in the results; the rollback covers it.

**Slow-task modal false positive in the non-interactive scope.**
- Consequence: Niagara compile or add-emitter may show progress and trip the scope.
- Answer: fix it in the core scope before N2. Niagara never passes `bShowProgress`.

**Config oddities.**
- `DefaultEmitter` points to an asset that does not exist.
- `PackageBlockList` contains `/Niagara/Modules/Update/Color/ScaleColor` (this affects EditorDomain only).
- Some NiagaraFluids packages still carry `/NiagaraSimulationStages/` names.
- Consequence: surprising references in Extract.
- Answer: Extract writes the redirected path, and fidelity ignores redirector differences.

### 7.2 Phases (each one mergeable alone)

**N1: read.**
- Scope:
  - the module `AssetDocumentNiagara`, owner `FX`, and `fx.niagara.read.v1`;
  - the System and Emitter profiles, registered read-only (Apply returns `Niagara.ReadOnlyPhase`);
  - Extract and Inspect (module inputs, available modules, compile status), and module discovery facts;
  - the headless view model spike.
- Acceptance:
  - Extract every asset in the small, feature and emitter tiers with no `Unknown` diagnostics.
  - Extract all 203 systems and emitters without a crash, with timings recorded.
  - Inspect shows the same module inputs as the stack.
  - The read part of the profile doc `Niagara.md` is written.

**N2: system level.**
- Scope:
  - `fx.niagara.edit.v1` and `fx.niagara.compile.v1`;
  - system `Properties`, `UserParameters` and `Parameters`;
  - emitters: add (empty, minimal, or `From`), delete, rename, enable, `Parent` remove / version, and emitter `Properties`;
  - `Renderers`: add, delete, order, Details;
  - compile, rollback and the rebuilder;
  - apply with the asset open in its editor (close / apply / reopen).
- Acceptance:
  - The small-system tier round-trips with modules untouched (stack body keys omitted).
  - The emitter, renderer and user parameter free-edit scenarios pass.
  - A compile failure rolls back.
  - A new system can be built from scratch with emitters copied from templates.

**N3: stacks.**
- Scope:
  - module lists: add, delete, move, enable, version, display name, note;
  - every input value mode, including nested dynamic inputs and data interfaces;
  - Set Parameters modules, event handlers and simulation stages;
  - stateless emitters: read, and create if the reflection path holds.
- Acceptance:
  - Full round trip of the small and feature tiers. Scratch-pad modules are excluded and reported as `Niagara.ScratchPadPhase`.
  - All module and input free-edit scenarios pass.
  - The AI-from-scratch campfire (6.4) passes.

**N4: scripts and scratch pads.**
- Scope:
  - Niagara schema and host adapters for `graph.*`;
  - `profile.niagara_script.v1` for Module, Function and DynamicInput assets: settings, parameters with metadata, the graph, and versions (read-only at first);
  - the same graph component for system and emitter `ScratchPads`.
- Acceptance:
  - The script tier round-trips, including static switches and custom HLSL.
  - The stress systems Extract and round-trip within a recorded time budget.
  - The AI writes "OrbitAroundPoint".

**N5: emitter assets and parameter collections.**
- Scope:
  - emitter asset Apply: placeholder system, write-back, dependents rule, `Versions`;
  - `profile.niagara_parameter_collection.v1`: the parameter list, values through the Niagara value codec, and the material collection link.
- Acceptance:
  - The emitter tier round-trips as assets.
  - The inheritance and dependents scenarios pass.
  - `NPC_VRTemplate` round-trips, and a system links an `NPC.*` parameter.

Each of N2 to N5 adds its section to `Niagara.md`. Each also adds its corpus results to a matrix document, `asset-document-niagara-matrix.md`, as Material and StateTree have.

### 7.3 Open questions for the user

1. **Parameter collections and script assets.** Should `NiagaraParameterCollection` and the `NiagaraScript` assets (N4/N5) be part of batch 2, or follow later?
2. **Stateless (lightweight) emitters.** Is reading them enough, or must they also be creatable?
3. **Exposed versions of library emitters.** May an emitter-asset apply edit the exposed version of a versioned library emitter? This would skip the editor's warning dialog.
4. **Infrastructure additions.** Do you accept a new owner enum value (`FX`), a new module `AssetDocumentNiagara`, and a `Niagara` plugin dependency in `UEAssetDoc.uplugin`?
5. **Module move fallback.** If headless `Drop` fails, may module move fall back to an isolated raw path with the result of `MoveModule`?
6. **Stress-tier gate.** Should the large NiagaraFluids / WaterAdvanced systems be a pass gate, or only timed?

## 8. Decisions (2026-10-06)

User:
1. NiagaraScript assets (module / function / dynamic input, scratch pads) and NiagaraParameterCollection are in batch 2: N1–N5 all planned.
2. Stateless (lightweight) emitters: creatable and editable in N3 (`FNiagaraSystemViewModel::AddStatelessEmitter`, reflected `UNiagaraStatelessEmitter` / module objects). If a step can only be done through engine-internal headers, report it with evidence instead of working around it.
3. Exposed versions of library emitters: refused by default (coded diagnostic that names the downstream systems and suggests a new version); allowed only when the document explicitly opts in (the document-level equivalent of the editor's confirmation dialog).

Root:
4. New owner `FX`, module `AssetDocumentNiagara` and the `Niagara` plugin dependency in `UEAssetDoc.uplugin`: accepted.
5. Module move: headless `UNiagaraStackEntry::Drop` first; an isolated raw path with the result of `MoveModule` is allowed only with behaviour tests proving the same result as the editor path.
6. NiagaraFluids / WaterAdvanced stress systems: fidelity is a pass gate; durations are recorded, not gated.
7. N2 starts only after the slow-task / modal-guard fix is merged.
