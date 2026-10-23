# Control Rig support matrix and corpus results

Design: [asset-document-controlrig-design.md](asset-document-controlrig-design.md).
Rules: [design rules](asset-document-design-rules.md) §3 (rollback = functional equivalence), §4
(editor-equivalent code + behaviour test), §5 (support matrix). Reference editor: the UE 5.7 Control Rig editor (rig
graph, My Blueprint, Rig Hierarchy tab, Module Assembly panel, Class Settings, compiler results). The agent-facing
description is `MCP/schemas/assetdocument/ControlRig.md`.

| Class | Profile | State |
|---|---|---|
| `ControlRigBlueprint` (rig, rig module, modular rig) | `profile.control_rig.v1` | Extract / Inspect / Diff / Validate; Create and Apply of rigs and rig modules (Variables, Hierarchy, Module, rig type, Class Settings, Graphs, Functions, collapse nodes, function references) and of modular rigs (Modules, Connections, ModularSettings); Create from a mesh (`FactoryContext`); Inspect `ModularRigSuggestions` |
| `ControlRigShapeLibrary` | generic profile | Generic Extract / Apply; Inspect adds `ShapeLibrary` facts |

Classification: **Supported**, **Read only**, **Refused** (an error code), **Not reproduced** (no document content).

`ControlRigBlueprint` is a Blueprint subclass. The core class resolver admits it as an exact-profile class through
`RegisterAssetDocumentExactBlueprintClass` (registered by `AssetDocumentRig` at startup); other Blueprint subclasses
stay refused.

## Editor surfaces

| Editor surface | Body location | Class | Mechanism |
|---|---|---|---|
| Class picker parent (`UControlRig`, native subclasses, `UModularRig`) | `RigType`, `ParentClass` | Supported: Rig <-> Module ("Turn into Rig Module" / "Turn into Standalone Rig"); parent class and Rig <-> Modular refused (`ParentClassImmutable`, `RigTypeImmutable`); a modular Create is the factory with parent `ModularRig` (module `Root` added) | `TurnIntoControlRigModule` / `TurnIntoStandaloneRig` (after their `CanTurnInto…` checks); Create through the Content Browser factory, plus the Forwards Solve node the editor adds on first open (rigs only) |
| Content Browser "Create Control Rig" / "Create Modular Rig" on a Skeletal Mesh or Skeleton | `FactoryContext` (Create; object or package path, as the asset picker resolves a package) | Supported: bones and curves imported, import sources and preview mesh set; the `Hierarchy` lists only what the rig adds (imported elements it leaves out are kept). A modular Create without it whose `Properties` set `SourceHierarchyImport` imports its `PreviewSkeletalMesh` the same way | `hierarchy.import`: the steps of `CreateControlRigFromSkeletalMeshOrSkeleton` after the factory (`ImportBones`, `ImportCurves`, `SourceHierarchyImport` / `SourceCurveImport`, `PropagateHierarchyFromBPToInstances`, `SetPreviewMesh`, `RecompileVM` for rigs) |
| Module settings (`RigModuleSettings`) | `Module` | Supported | `GetRigModuleSettings()` written as the Class Settings > Rig Module Settings Details does |
| My Blueprint: member variables (type, default, public, read only, expose to cinematics, category, tooltip) | `Variables` | Supported: add, remove, rename (`RenamedFrom`), retype, flags, category, tooltip, default (class default object), order; nodes and bindings follow renames / retypes / removals | `URigVMBlueprint` `AddMemberVariable` / `RemoveMemberVariable` / `RenameMemberVariable` / `ChangeMemberVariableType`, category / tooltip / expose setters; `FBlueprintEditorUtils` public (Blueprint-only editable) / read-only flags and `MoveVariableBeforeVariable`; each variable op between `OnPreVariableChange` / `OnPostVariableChange`; default written on the class default object |
| Rig Hierarchy: bones (imported and user), nulls, controls (every `ERigControlType`, limits, offset, shape, spaces, animation channels and hosts), curves, sockets, connectors, references; multi-parent weights and labels; element metadata | `Hierarchy` | Supported (complete region: add, remove children first, rename (`RenamedFrom`), reparent, parent weights, poses, settings, values, limits, spaces, hosts, metadata, sibling order); `Reference` refused (`ProceduralElement`) | `URigHierarchyController` (the hierarchy tab and element Details); depth-first editor order, so order means sibling order |
| Graph models (default `Rig`, extra models such as Construction) | `Graphs.<model>` | Supported: add, remove, rename (`RenamedFrom`); `Rig` cannot be removed or renamed; `Order` (the model's node order, which decides the compiled VM) places new nodes; existing nodes keep their place (no editor command reorders them) | `URigVMBlueprint::AddModel` / `RemoveModel`, `FRigVMClient::RenameModel` (My Blueprint's "+", delete, rename) |
| Nodes: unit (with `Method`), template and dispatch (resolved `PinTypes`), variable get / set, aggregate, reroute, enum, invoke entry, comment, events (one per event name in the rig) | `Nodes.<name>` (one kind key) | Supported: add, remove, rename (`RenamedFrom`), kind / identity change = remove + add; a new node of a deprecated unit refused naming the replacement (`ControlRig.DeprecatedUnit`), kept with a warning on Create; a reroute without a linked input refused (`ControlRig.UnstableReroute`); a second event of a name refused (`ControlRig.EventAlreadyExists`); a unit the editor does not offer in this rig refused (`ControlRig.EditRefused`, such as Get Candidates outside a rig module); injected units of a pin (the pin menu's Add Visual Debug / Add Interpolate) under `Injections` | `URigVMController` `AddUnitNode(WithDefaults)` / `AddTemplateNode` / `AddVariableNode` / `AddRerouteNode` / `AddEnumNode` / `AddInvokeEntryNode` / `AddCommentNode`, `RemoveNode`, `RenameNode` (the palette, delete, rename); `AddInjectedNode` / `RemoveInjectedNode` (the pin menu) |
| Node title, color, position, size, comment text / font / bubble, expansion, pin defaults (sparse over the new node's defaults), array sizes, resolved wildcard types, aggregate pins, links (sub-pins included), pin bindings, traits | node fields | Supported (`Title` on renamable nodes, `Color` on comments; other nodes keep their palette color) | `SetNodePosition` / `SetNodeSize` / `SetNodeTitle` / `SetNodeColor` / `SetCommentText` / `SetPinExpansion`, `SetPinDefaultValue` / `ResetPinDefaultValue` / `SetArrayPinSize`, `ResolveWildCardPin`, `AddAggregatePin` / `RemoveAggregatePin`, `AddLink` / `BreakLink`, `BindPinToVariable` / `UnbindPinFromVariable`, `AddTrait` / `RemoveTrait` (graph panel, pin widgets, Details) |
| Function reference nodes (local and external, variable remap), collapse nodes; Functions: signature, access, mutable, local variables, entry / return positions, inner nodes; nested collapse graphs | `Nodes.<name>.Function` / `Collapse`, `Functions` | Supported: functions added, removed, renamed (`RenamedFrom`), made public or private, header and colour; exposed pins added, removed, renamed, retyped, reordered, argument defaults; local variables added, removed, renamed, retyped, defaults; collapse nodes made from their nodes and edited the same way; function reference nodes to local and other rigs' public functions with variable remaps; dependents of public functions warned (`ControlRig.DependentsAffected`). Refused as the 5.7 editor cannot make them (`ControlRig.EditRefused`): a reference to another rig's private function, local variables on a collapse graph (listed unchanged they stay), a function or collapse node with a second execute pin | `URigVMController` `AddFunctionToLibrary` / `RenameNode` / `RemoveFunctionFromLibrary` / `AddExposedPin` / `RenameExposedPin` / `ChangeExposedPinType` / `SetExposedPinIndex` / `AddLocalVariable` and siblings / `CollapseNodes` / `AddFunctionReferenceNodeFromDescription` / `SetRemappedVariable`; `URigVMBlueprint::SetFunctionPublic` |
| Module Assembly: modules, parents, sibling order, class swap, config overrides, variable bindings, connections, Auto Resolve | `Modules`, `Connections`, `ModularSettings` | Supported: complete regions; connections made without auto-resolve, module by module down the tree, each module's primary connector first; a module's dependents rule (below) | `UModularRigController` `AddModule` / `DeleteModule` / `RenameModule` / `ReparentModule` / `ReorderModule` / `SwapModuleClass` / `SetConfigValueInModule` (override value) / `ResetConfigValueInModule` / `BindModuleVariable` / `ConnectConnectorToElements` (after `CanConnectConnectorToElements`, the debugged rig constructed first) / `DisconnectConnector` |
| Modular rig preview mesh (Class Settings picker) | `Properties.PreviewSkeletalMesh` | Supported | `modular.preview_mesh`: the steps of `HandlePreviewMeshChanged` (connectors and sockets removed, bones, curves and mesh sockets imported, the rig's own connectors and sockets added back) |
| A rig module that loaded modular rigs use (its compile refreshes them) | — | Supported: refused while one of them has unsaved changes (`ControlRig.DependentHasUnsavedChanges`), else the warning `ControlRig.DependentsUpdated` naming them | `module.dependents` before any write |
| Class Settings (preview mesh, shape libraries, source hierarchy / curve import, VM settings) | `Properties` | Supported | generic property write; `PreviewSkeletalMesh` through `SetPreviewMesh` (the picker); `SourceHierarchyImport` / `SourceCurveImport` are duplicate-transient, which the generic region does not admit, so the profile reads them (read header) and writes them (`source_import`, before the hierarchy). |
| Pins of each node as the editor shows them | Inspect `Pins` | Supported | equal to `list_types` `rigvm_node` rows (checked on the corpus) |
| Global initial transforms, shapes available, public functions, events | Inspect `GlobalTransforms`, `Shapes`, `PublicFunctions`, `Events` | Supported | hierarchy `GetGlobalTransform(.., bInitial)`, shape libraries, function library, event nodes |
| Compiler results panel, node error badges, VM | `Compile` | Read only; apply compiles (Compile button) before and after its edits | `UBlueprint::Status`, per-node `ErrorMsg` at the node's document path, compile log, CDO VM hash and instruction count; extract compiles nothing |
| Palette (units, templates, dispatches, events), variable types, module browser | `list_types` `rigvm_node`, `rigvm_type`, `controlrig_module` | Supported | `FRigVMRegistry`, `RigVMTypeUtils`, asset registry tags of module rigs |
| Shape library Details | generic `Properties`; Inspect `ShapeLibrary` | Supported (generic) | `DefaultShape`, `Shapes` with resolved names |
| Hierarchy components (experimental `ControlRigPhysics`), procedural elements | Inspect `Skipped` | Not authored: they are not in the document and apply leaves them as they are | not documented |
| Apply / Create of a Control Rig document | — | Supported for the regions above; with the rig open in the Control Rig editor the editor is closed for the apply and reopened on the result (Info `ControlRig.EditorReopened`, test `ControlRig.OpenEditorApply`); new compile errors fail and roll back (`ControlRig.CompileFailed`) | `rig.controlrig.edit.v1`, `rig.controlrig.compile.v1`, Python `control_rig_apply` |
| Auto-Connect suggestions (what the panel's auto-resolve connects after a primary connection), connector candidates | Inspect `ModularRigSuggestions {AutoConnect, Candidates, ConnectPrimaryFirst, Check}` | Supported: suggestions only, never applied implicitly (design decision 6); `Check` names an AutoConnect bone above where its module attaches (an Arm on a Spine shoulder socket gets the clavicle as `Elbow Bone`, as the editor's drop does) | `UModularRigController::AutoConnectModules` on a transient copy of the rig (the Validate transient target); candidates from `UModularRigRuleManager::FindMatches` on the copy's constructed debugged rig |
| Import Hierarchy suggestions (the bones, curves and sockets the Import would create) | — | Not reproduced | Inspect gives no hierarchy suggestion; a Create with `FactoryContext` imports them (above) and the `Hierarchy` states the elements a rig adds |
| Validate | `validate_asset_document` | Supported: the apply on a transient copy (Update) or a new transient rig (Create) | transient target maker: `StaticDuplicateObject` / the factory into the transient package, neither Public nor Standalone |

## Corpus and fixture results

Windows sandbox (UE 5.7; MoverExamples and AnimatorKit enabled, MetaHumanCharacter not enabled: those rows skip with
a note), the read tests of `UEAssetDoc.ControlRig.*`. Each asset is duplicated into `/Game`, extracted
twice (equal), inspected (pins equal the registry), diffed against its own extract (empty), checked for side effects
(no dirty package), reloaded and extracted again (equal). Times are one run, in ms; Template rigs are copied into
`/Game` from `Templates/` first.

| # | Asset | Extract | Inspect | Reload | Size | Content |
|---|---|---|---|---|---|---|
| 1 | ControlRig `StandardFunctionLibrary` | 182 | 360 | 1368 | 229k | Rig: 827 nodes, 44 functions, 0 elements, 0 variables; 368 unit pin lists = registry |
| 2 | ControlRigSpline `SplineFunctionLibrary` | 95 | 148 | 398 | 64k | Rig: 321 nodes, 5 functions, 0 elements, 0 variables; 136 unit pin lists = registry |
| 3 | ControlRig `Root` | 193 | 334 | 885 | 216k | Module: 747 nodes, 6 functions, 128 elements, 17 variables; 112 unit pin lists = registry |
| 4 | ControlRigModules `Arm` | 259 | 524 | 1598 | 275k | Module: 1143 nodes, 28 functions, 10 elements, 31 variables; 478 unit pin lists = registry |
| 4 | ControlRigModules `Leg` | 203 | 369 | 968 | 210k | Module: 814 nodes, 18 functions, 10 elements, 49 variables; 342 unit pin lists = registry |
| 4 | ControlRigModules `Spine` | 327 | 674 | 1761 | 317k | Module: 1304 nodes, 24 functions, 7 elements, 21 variables; 462 unit pin lists = registry |
| 5 | ControlRigModules `Constraint` | 54 | 66 | 163 | 17k | Module: 67 nodes, 1 functions, 4 elements, 16 variables; 25 unit pin lists = registry |
| 5 | ControlRigModules `Hinge` | 56 | 66 | 153 | 17k | Module: 60 nodes, 0 functions, 3 elements, 13 variables; 26 unit pin lists = registry |
| 5 | ControlRigModules `Wheel` | 72 | 91 | 208 | 32k | Module: 148 nodes, 0 functions, 4 elements, 15 variables; 79 unit pin lists = registry |
| 5 | ControlRigModules `Finger` | 97 | 155 | 423 | 79k | Module: 309 nodes, 5 functions, 5 elements, 5 variables; 138 unit pin lists = registry |
| 6 | ControlRigModules `Modules/Arm` (pre-5.6) | 244 | 446 | 1513 | 253k | Module: 987 nodes, 24 functions, 9 elements, 27 variables; 444 unit pin lists = registry |
| 7 | Template `CR_Mannequin_Body` | 430 | 821 | 1699 | 522k | Rig: 1531 nodes, 35 functions, 524 elements, 11 variables; 516 unit pin lists = registry |
| 8 | Template `CR_Mannequin_FootIK` | 86 | 123 | 141 | 62k | Rig: 47 nodes, 1 functions, 292 elements, 6 variables; 18 unit pin lists = registry |
| 8 | Template `CR_Mannequin_Procedural` | 112 | 196 | 316 | 92k | Rig: 176 nodes, 0 functions, 280 elements, 0 variables; 166 unit pin lists = registry |
| 9 | Template `CtrlRig_FPWarp` | 86 | 114 | 128 | 55k | Rig: 13 nodes, 0 functions, 292 elements, 0 variables; 11 unit pin lists = registry |
| 9 | Template `Ctrl_HandAdjusment` | 94 | 135 | 175 | 61k | Rig: 26 nodes, 0 functions, 295 elements, 6 variables; 19 unit pin lists = registry |
| 9 | Template `CtrlRig_SportsCar` | 50 | 52 | 122 | 7k | Rig: 11 nodes, 0 functions, 18 elements, 0 variables; 9 unit pin lists = registry |
| 9 | Template `Offroad_CtrlRig` | 77 | 109 | 301 | 41k | Rig: 78 nodes, 1 functions, 48 elements, 1 variables; 48 unit pin lists = registry |
| 10 | ControlRig `DefaultGizmoLibraryNormalized` | 44 | 228 | 92 | 15k | shapes 54 |
| 11 | MoverExamples `CR_Mannequin_Body` | 344 | 627 | 1402 | 430k | Rig: 1341 nodes, 32 functions, 446 elements, 9 variables; 477 unit pin lists = registry |
| 11 | MoverExamples `CR_Mannequin_BasicFootIK` | 89 | 131 | 157 | 65k | Rig: 47 nodes, 1 functions, 302 elements, 6 variables; 17 unit pin lists = registry |
| 11 | MoverExamples `CR_Mannequin_Procedural` | 112 | 155 | 303 | 92k | Rig: 176 nodes, 0 functions, 280 elements, 0 variables; 166 unit pin lists = registry |
| 12 | AnimatorKit `CRU_AddLocator` | 51 | 48 | 115 | 3k | Rig: 4 nodes, 0 functions, 1 elements, 0 variables; 4 unit pin lists = registry |
| 12 | AnimatorKit `CRU_MorphTargetDriver` | 52 | 62 | 134 | 9k | Rig: 35 nodes, 0 functions, 0 elements, 3 variables; 19 unit pin lists = registry |
| 12 | AnimatorKit `CRU_ChainSim` | 66 | 68 | 171 | 17k | Rig: 68 nodes, 0 functions, 1 elements, 3 variables; 30 unit pin lists = registry |
| 12 | AnimatorKit `CRU_SplinePath` | 91 | 80 | 182 | 18k | Rig: 60 nodes, 0 functions, 1 elements, 5 variables; 32 unit pin lists = registry |
| 12 | AnimatorKit `CRU_SingleSim` | 54 | 58 | 147 | 11k | Rig: 33 nodes, 0 functions, 1 elements, 2 variables; 21 unit pin lists = registry |
| 12 | AnimatorKit `CRU_BlendParent` | 52 | 103 | 164 | 10k | Rig: 22 nodes, 0 functions, 1 elements, 0 variables; 22 unit pin lists = registry |
| 12 | AnimatorKit `CRU_3Node` | 64 | 56 | 128 | 5k | Rig: 8 nodes, 0 functions, 1 elements, 0 variables; 8 unit pin lists = registry |
| 12 | AnimatorKit `CRD_Lattice` | 85 | 124 | 343 | 57k | Rig: 249 nodes, 2 functions, 11 elements, 16 variables; 149 unit pin lists = registry |
| 12 | AnimatorKit `CRD_Lattice_2x2x2` | 81 | 114 | 306 | 47k | Rig: 195 nodes, 2 functions, 12 elements, 8 variables; 124 unit pin lists = registry |
| 12 | AnimatorKit `CRD_Lattice_3x3x3` | 81 | 116 | 309 | 47k | Rig: 195 nodes, 2 functions, 12 elements, 8 variables; 124 unit pin lists = registry |
| 12 | AnimatorKit `CRD_Lattice_4x4x4` | 81 | 106 | 279 | 47k | Rig: 195 nodes, 2 functions, 12 elements, 8 variables; 124 unit pin lists = registry |
| 12 | AnimatorKit `ShapeControlLibLocator` | 40 | 41 | 94 | 0k | shapes 2 |
| 13 | MetaHumanCharacter (4 assets) | — | — | — | — | skipped (plugin not enabled) |

Size is the extract JSON in thousands of characters. Duplication into `/Game` takes 0.1–4.8 s per rig (not
included). Every rig reads `Compile.Status: "Dirty"` (see Known limits).

`CR_AllKinds` (built by the test on the tutorial mannequin: every hierarchy element type, every control type, a
multi-parent null, a channel with a host, spaces, metadata, variables, every node kind, a sub-pin link, a pin bound to
a variable, an aggregate, a collapse node, a public function with a local variable and its reference, a Construction
model) and the modular fixture (`Spine` under `Root` on the biped template) read back what the builder made and are
stable. `Status`: a compiled rig reads `UpToDate` with its VM hash; a forced `Dirty` status stays `Dirty` after an
extract with the same VM hash; a node message is reported at the node's document path.

## Apply results: hierarchy, variables, module

Windows sandbox, tests Corpus, Fidelity, FreeEdits, Rollback, UndoHistory, OpenEditor, Read, Status, ListTypes
(`UEAssetDoc.ControlRig.*`). Times are one run, in ms.

Fidelity (§6.2): each source is extracted, its `Graphs` / `Functions` dropped, and a new asset is created from the
rest (Create through the Content Browser factory; a new rig gets the Forwards Solve node the editor adds on first
open). The created rig must extract as the source minus graphs, compile, take an unchanged re-apply as a no-op (no
save) and extract equal after a reload.

| Source | Create | Re-apply | Reload | Size | Status (created / source) |
|---|---|---|---|---|---|
| fixture `CR_Hierarchy` (every element type and control type, spaces, channel host, metadata, variables) | 452 | 97 | 105 | 10k | UpToDate / UpToDate |
| fixture `CR_AllKinds` (the every-kind fixture, graphs dropped) | 427 | 102 | 110 | 10k | UpToDate / UpToDate |
| #3 ControlRig `Root` (128 elements, 17 variables) | 1889 | 114 | 107 | 14k | UpToDate / Dirty |
| #5 ControlRigModules `Constraint` | 1541 | 81 | 103 | 5k | UpToDate / Dirty |
| #5 ControlRigModules `Hinge` | 1527 | 99 | 112 | 4k | UpToDate / Dirty |
| #5 ControlRigModules `Wheel` | 1451 | 101 | 110 | 4k | UpToDate / Dirty |
| #5 ControlRigModules `Finger` | 821 | 101 | 112 | 6k | UpToDate / Dirty |

Size is the compared document (no graphs) in thousands of characters. Create includes the factory, the first compile,
the edits, the final compile, the save and the persisted read-back.

Free edits (§6.3): on two saved and reloaded copies, one is edited through the editor's own code and the other with
the resulting document (renames marked `RenamedFrom`). Edits: a limited, shaped control under `hand_r`, an available
space, a reparent keeping the global transform, a reorder, a null rename, a control delete, an animation channel with
a second host, a curve value, metadata, a moved control; a variable add, rename and retype (vector -> rotator),
category, tooltip and default. The document side extracts equal to the editor side with an equal compile status, and
a re-apply is a no-op: `CR_Hierarchy` 594 ms, `CR_AllKinds` 687 ms (its variable nodes and pin bindings follow the
rename and retype), both `UpToDate`.

Rollback: a refused op after a variable add and a hierarchy write (`ControlRig.InvalidValue` at the element's
`Settings`) leaves the rig extracting as before. A failed compile (a rig whose function reference remaps a host
function's variable to its own `Gain`; retyping `Gain` unmaps it, `ControlRig.CompileFailed` at the reference node)
is restored to an equal extract and its compile status (`UpToDate`): the rebuilder re-sets the variable remap the
restore dropped and the member defaults.

Undo: a human edit (hierarchy tab add, saved) then a document apply: the apply clears undo history that references
the rig, keeps the human edit, and undo / redo of what remains leaves the rig whole. A second human edit then a
failing apply: the rig is back at the human state and undo / redo stays safe. Open editor (`ControlRig.OpenEditorApply`): a rig created from `Body: {}` extracts like one a human made and
opened once. With the rig open in the Control Rig editor and unsaved after the open added its Forwards Solve, an apply
(a null, a Set Transform node linked to the Forwards Solve) closes the editor and reopens it on the applied rig: the new
editor's graph has the node and its debugged rig the null; a failed apply (unknown unit) rolls back and the editor shows the rolled-back rig; a
human hierarchy edit is saved, undone and redone; the editor closes without a prompt.

## Apply results: graphs

Windows sandbox, tests GraphFidelity, GraphFreeEdits, GraphRollback, GraphUndo (`UEAssetDoc.ControlRig.*`). Times are
one run, in ms.

Fidelity (§6.2): each rig without local functions or collapse nodes is reloaded from disk, compiled, extracted and
created as a new asset from the whole document (graphs included). The created rig must extract as the source, compile
with the same messages, take an unchanged re-apply as a no-op and extract equal after a reload. Both rigs are then
compared as compiled VMs (`ComputeVMHash`, instruction count, and by content: the function of each instruction, literal
values, work memory) and as a runtime pose (a new `UControlRig` of each class, Construction then two Forwards Solve
evaluations: every bone, null and control global transform within 0.01, every curve).

| Source | Nodes | Doc | Create | VM hash | Instr. | Pose | Re-apply | Reload |
|---|---|---|---|---|---|---|---|---|
| fixture `CR_AllKinds` minus functions (every node kind but function / collapse, aggregate, sub-pin link, binding, user event, two models) | 16 | 13k | 596 | equal | 12 | 83 equal | 90 | 129 |
| #8 Template `CR_Mannequin_Procedural` | 176 | 94k | 2573 | equal | 195 | 280 equal | 171 | 255 |
| #9 Template `CtrlRig_FPWarp` | 13 | 54k | 688 | equal | 18 | 292 equal | 111 | 150 |
| #9 Template `Ctrl_HandAdjusment` | 26 | 60k | 1133 | equal | 34 | 295 equal | 125 | 154 |
| #9 Template `CtrlRig_SportsCar` | 11 | 8k | 437 | equal | 14 | 18 equal | 74 | 131 |
| #11 MoverExamples `CR_Mannequin_Procedural` | 176 | 94k | 2620 | equal | 195 | 280 equal | 187 | 285 |
| #12 AnimatorKit `CRU_AddLocator` | 4 | 4k | 370 | equal | 6 | 2 equal | 70 | 121 |
| #12 AnimatorKit `CRU_MorphTargetDriver` | 35 | 10k | 760 | equal | 69 | 3 equal | 81 | 149 |
| #12 AnimatorKit `CRU_SingleSim` | 33 | 11k | 735 | equal but for -0 | 35 | 8 equal | 75 | 161 |
| #12 AnimatorKit `CRU_BlendParent` | 22 | 11k | 687 | equal but for -0 | 32 | 8 equal | 75 | 170 |
| #12 AnimatorKit `CRU_3Node` | 8 | 5k | 473 | equal | 13 | 4 equal | 64 | 146 |

Create includes the factory, compiles, every graph op, the save and the persisted read-back. "Equal but for -0": the
source holds a negative zero in a struct literal (a quaternion component); a document number has no negative zero, so
the created rig's literal is `0`: byte code, functions and work memory are equal and only the literal's sign differs.
Rigs with local functions or collapse nodes are covered under "Apply results: functions and collapse nodes". `CR_Mannequin_Procedural` keeps its deprecated `RigUnit_SequenceExecution` node on Create
with a `ControlRig.DeprecatedUnit` warning.

Free edits (§6.3): on two saved and reloaded copies of `CR_AllKinds` minus functions and of #7 `CR_Mannequin_Body`,
one is edited through the controller as the graph panel does, the other with the resulting document (the rename marked
`RenamedFrom`): a Set Transform inserted after the Forwards Solve (wire broken and re-routed), an element key and an
enum value, an expanded pin, a sub-pin link from a Get Transform, an Add with a third operand driving the weight, an
array pin resized with an element set, a user event and a Run Event of it, a comment deleted, a node renamed and moved,
an operand bound to a member variable, a new event graph with a user event and a comment. The document side extracts
equal to the editor side with the same compile status and messages, a re-apply is a no-op, and both reloaded rigs
compile to the same `ComputeVMHash` and pose: `CR_AllKinds` minus functions 621 ms (83 elements), #7 7270 ms (524
elements, 1531 nodes; the rig's functions are left as they are).

Rollback: a refused link after node adds and values (`ControlRig.LinkRefused`), a new node of a deprecated unit
(`ControlRig.DeprecatedUnit`, naming the replacement), a function reference node of a missing function (`ControlRig.FunctionNotFound`)
and an unstable reroute (`ControlRig.UnstableReroute`) leave the rig extracting as before with a clean package. Undo:
a human graph edit (saved) then a document apply keeps both, clears the history that references the rig, and undo /
redo keep it whole; a failing apply after a human edit rolls back to the saved state and undo stays safe.

What makes a document-made rig compile to the same VM:

- **Node order.** The compiler visits events and unlinked nodes in the model's node order, and literal and work memory
  follow it; a document sorts its `Nodes` keys. A graph's `Order` carries the model order, and new nodes are added in it.
  The editor has no command that reorders nodes, so existing nodes keep their place.
- **Aggregate nodes.** The first added operand turns a node into an aggregate node, which goes last in the model. A new
  node takes its pin types and operands right after its add, as an editor user does on a node just placed.
- **Value texts.** The compiler shares one literal between pins whose default texts are equal. Units are spawned as
  the palette does and their values are set as the pin widgets set them (export text, numbers as `%f`), not as one
  struct written at creation (which leaves untouched pins without a text). Only a user event is made with its values,
  since its name is a transient field.
- Element keys keep their type without a name and their name without a type (`"Bone()"`, `"None(upperarm_r)"`), and
  struct values export every field (zero fields included), so a key or a struct pin reads back what the rig holds.
- `SourceHierarchyImport` / `SourceCurveImport` are `DuplicateTransient`, which the generic admission does not admit. They are
  profile-owned `Properties` written by a `source_import` op (a skeleton or skeletal mesh) before the hierarchy, so the
  imported bones keep their reference pose.
- Core package snapshot: for a graph node whose pins were reallocated during the apply (new pin ids), the restore
  breaks the links to pins it no longer knows first and restores the captured pins (`PackageSnapshot.RestoresReallocatedGraphPins`).
- A loaded multi-parent null can hold an initial global transform its locals and weights give otherwise until a reload,
  so fidelity compares the source as loaded from disk.

## Apply results: functions and collapse nodes

Windows sandbox, tests FunctionFreeEdits, FunctionRollback, FunctionUndo and the fidelity run below
(`UEAssetDoc.ControlRig.*`). Times are one run, in ms.

Fidelity (§6.2), as for graphs but on every corpus rig and rig module, functions and collapse nodes included. Rig
modules are compared with the source's preview connector targets (see Known limits); they have no elements to pose
until a modular rig resolves their connectors, so their pose column is empty and the VM comparison carries them.

| Source | Nodes | Doc | Create | VM hash | Instr. | Pose | Re-apply | Reload |
|---|---|---|---|---|---|---|---|---|
| fixture `CR_AllKinds` (every node kind, a public function with an input default and a local variable, its reference, a collapse node) | 21 | 14k | 605 | equal | 12 | 83 equal | 94 | 143 |
| #1 ControlRig `StandardFunctionLibrary` | 827 | — | refused | — | — | — | no-op | — |
| #2 ControlRigSpline `SplineFunctionLibrary` | 321 | — | refused | — | — | — | no-op | — |
| #3 ControlRig `Root` (module) | 747 | 237k | 17105 | spellings | 1408 | 126 equal | 345 | 609 |
| #4 ControlRigModules `Arm` | 1143 | 292k | 21485 | spellings | 2558 | — | 456 | 1220 |
| #4 ControlRigModules `Leg` | 814 | — | refused | — | — | — | no-op | — |
| #4 ControlRigModules `Spine` | 1304 | 340k | 23077 | spellings | 2941 | — | 560 | 1170 |
| #5 ControlRigModules `Constraint` | 67 | 19k | 2673 | equal | 86 | — | 92 | 198 |
| #5 ControlRigModules `Hinge` | 60 | 18k | 2405 | equal but for -0 | 273 | — | 75 | 191 |
| #5 ControlRigModules `Wheel` | 148 | 34k | 3092 | spellings | 339 | — | 123 | 255 |
| #5 ControlRigModules `Finger` | 309 | 84k | 4104 | spellings | 906 | — | 155 | 388 |
| #6 ControlRigModules `Modules/Arm` (pre-5.6) | 987 | 267k | 19863 | spellings | 2419 | — | 414 | 1167 |
| #7 Template `CR_Mannequin_Body` | 1531 | 532k | 22075 | spellings | 4367 | 524 equal | 908 | 1326 |
| #8 Template `CR_Mannequin_FootIK` | 47 | 61k | 1101 | equal | 66 | 292 equal | 148 | 187 |
| #8 Template `CR_Mannequin_Procedural` | 176 | 94k | 2571 | equal | 195 | 280 equal | 203 | 293 |
| #9 Template `CtrlRig_FPWarp` | 13 | 54k | 626 | equal | 18 | 292 equal | 181 | 167 |
| #9 Template `Ctrl_HandAdjusment` | 26 | 59k | 1090 | equal | 34 | 295 equal | 177 | 176 |
| #9 Template `CtrlRig_SportsCar` | 11 | 8k | 354 | equal | 14 | 18 equal | 85 | 164 |
| #9 Template `Offroad_CtrlRig` | 78 | 40k | 1307 | equal | 39 | 48 equal | 128 | 236 |
| #11 MoverExamples `CR_Mannequin_Body` | 1341 | — | refused | — | — | — | no-op | — |
| #11 MoverExamples `CR_Mannequin_BasicFootIK` | 47 | 65k | 1287 | equal | 72 | 302 equal | 205 | 193 |
| #11 MoverExamples `CR_Mannequin_Procedural` | 176 | 94k | 2515 | equal | 195 | 280 equal | 174 | 298 |
| #12 AnimatorKit `CRU_AddLocator` | 4 | 4k | 315 | equal | 6 | 2 equal | 81 | 150 |
| #12 AnimatorKit `CRU_MorphTargetDriver` | 35 | 10k | 750 | equal | 69 | 3 equal | 76 | 162 |
| #12 AnimatorKit `CRU_ChainSim` | 68 | 18k | 1066 | equal but for -0 | 169 | 19 equal | 97 | 199 |
| #12 AnimatorKit `CRU_SplinePath` | 60 | 19k | 1237 | equal but for -0 | 156 | 16 equal | 90 | 224 |
| #12 AnimatorKit `CRU_SingleSim` | 33 | 11k | 748 | equal but for -0 | 35 | 8 equal | 74 | 188 |
| #12 AnimatorKit `CRU_BlendParent` | 22 | 11k | 638 | equal but for -0 | 32 | 8 equal | 82 | 192 |
| #12 AnimatorKit `CRU_3Node` | 8 | 5k | 411 | equal | 13 | 4 equal | 76 | 175 |
| #12 AnimatorKit `CRD_Lattice` | 249 | 60k | 5572 | spellings | 492 | 38 equal | 144 | 337 |
| #12 AnimatorKit `CRD_Lattice_2x2x2` | 195 | 49k | 3921 | spellings | 422 | 22 equal | 138 | 323 |
| #12 AnimatorKit `CRD_Lattice_3x3x3` | 195 | 49k | 3907 | spellings | 422 | 42 equal | 123 | 296 |
| #12 AnimatorKit `CRD_Lattice_4x4x4` | 195 | 49k | 3885 | spellings | 422 | 80 equal | 141 | 306 |

29 rigs created and compared, 4 refused as the 5.7 editor would (#10 and #13 hold no rig graph or are not enabled).
Instruction counts are equal in every row. "Spellings": the hashes differ, while the program is the same
(instruction count and the function of each instruction, in the same order or, for `CRD_Lattice*`, the same set) and
the pose is equal. Literals and work memory differ only by texts of equal value that no editor action makes: the
source's `True` / `False` where the pin widget writes `true` / `false`, `(Type=Bone)` against
`(Type=Bone,Name="None")`, an FName `''` against `None`, a `bInitial` with no text against `False`, an aggregate's
inner node names, and a literal under a linked sub-pin (the compiler reads the link, and the old text survives only
in the source). The compiler shares literals by text, so each of these gives the hash another value. "-0" as in the graph results.

Refused, with `ControlRig.EditRefused` naming the reason; an unchanged Update of the source copy is a no-op:

- #1: functions use Get Candidates, which the editor offers in rig modules only (`SupportsUnitFunction`).
- #2: a collapse graph holds local variables. The editor gives local variables to functions only. Listed unchanged,
  a collapse graph keeps them.
- #4 `Leg`: a collapse node with a second execute pin. The editor gives a function or collapse node one.
- #11 `CR_Mannequin_Body`: function references to the private functions of the Template `CR_Mannequin_Body`. The
  editor places another rig's public functions only.

Free edits (§6.3), FunctionFreeEdits: on two saved and reloaded copies of `CR_AllKinds` and of #7 `CR_Mannequin_Body`,
one is edited through the controller calls of My Blueprint, the function Details and the graph panel, the other with
the resulting document. First edit: a new function with two inputs and a local variable, called twice from the rig graph,
and a collapse node. Second edit: the function renamed (`RenamedFrom`), made public, an input default and the local renamed.
After each edit the document side extracts equal to the editor side with the same compile status and messages, a
re-apply is a no-op, and both reloaded rigs compile to the same `ComputeVMHash`, instruction count and pose:

| Rig | First edit | Second edit | Instr. | Pose |
|---|---|---|---|---|
| `CR_AllKinds` | 568 | 496 | 20 | 83 equal |
| #7 `CR_Mannequin_Body` | 8105 | 7215 | 4375 | 524 equal |

FunctionRollback: rigs made from documents (a public function reading a member variable, another rig calling it with
a variable map). A failing apply after function, interface, variable map and collapse edits leaves the rig extracting
as before with a clean package, the variable map restored. A rename of the public function warns
`ControlRig.DependentsAffected`, naming the caller. FunctionUndo / GraphUndo: undo / redo after a function or graph
apply, and after a failed one, keep the rig whole.

Behaviour that follows from the function tests:

- **Injected units.** The pin menu's Add Visual Debug / Add Interpolate puts a unit node inside a pin
  (`AddInjectedNode`), and links to that pin go through it. Nodes carry `Injections` (unit, input,
  output, values), applied with `AddInjectedNode` / `RemoveInjectedNode`, and links read through injected nodes to the
  host pins (without that, `CR_Mannequin_BasicFootIK` would compile to 42 instructions instead of 72).
- **Templates.** A template node is resolved to the source's permutation (`FullyResolveTemplateNode`) before its pins
  get values, as the editor does when a wildcard is linked. Bool values are written as the pin widget writes them
  (`true` / `false`).
- **Animation channels.** 5.7 refuses `SetParentWeight` on an animation channel, and `AddAnimationChannel` creates
  weight 0 where older assets hold 1. A channel reads a plain `Parent`, and a weight other than 1 is refused. The
  pose of a channel is its value.
- **Control values.** An `EulerTransform` control value is derived again from the pose on load, and a zero returns
  as float noise (3e-7). The read writes components under 1e-5 as 0, so a reload extracts equal.
- **Editor node caches after a rollback.** A refused apply that bound a headless rig's editor graphs (the compile
  before the edits) and was rolled back would leave each graph's transient node cache naming the dropped editor nodes;
  after the next garbage collection, the next bind would read freed memory. The compile clears the caches before it
  binds editor graphs, and so does the rollback rebuilder. GraphRollback refuses an apply on the reloaded rig, collects
  garbage, adds the same node again and checks that the editor graph shows it.
- `GraphOrder` is valid with `Graphs` only. A document without graphs leaves the model order to the rig.

## Apply results: modular rigs

Mac sandbox (UE 5.7), tests ModularFidelity, ModularFreeEdits, ModularCreate, ModularRollback, ModularDependents,
Validate, DiffOnly, ClassInspect, FactoryContext (`UEAssetDoc.ControlRig.*`). Times are one run, in ms. The modular fixtures are built only with the Module Assembly panel's calls
(`UModularRigController`, the primary connector connected with auto-resolve, as a drop on a socket does) on the
ControlRigModules template meshes.

| Source | Modules | Connections | Doc | Create | Model | Pose | Re-apply | Update (one config) | Reload |
|---|---|---|---|---|---|---|---|---|---|
| fixture `CR_Modular_Biped` (Root, Spine, Neck, arms, legs, feet, five fingers; a config override, a host variable binding) | 14 | 74 | 5k | 1383 | equal | 493 equal | 27 (no-op) | 215 | 76 |
| fixture `CR_Modular_Car` (Root, four wheels, suspension, piston) | 7 | 19 | 2k | 200 | equal | 54 equal | 25 (no-op) | 174 | 68 |

- **Free edits** (ModularFreeEdits): on a copy of the biped, delete `Arm_r`, add `Arm_mirror` on the right shoulder
  socket, change a Spine config value, bind the new arm's `FK IK Default`, swap Neck to the pre-5.6 class, move `Leg_r`
  before `Leg_l`. The editor's calls take 450; the document (module list and the primary connection) applies in 288,
  Inspect's AutoConnect (145) accepted in a second apply of 321. Extract, module model and runtime pose (493
  elements) equal the editor's.
- **From scratch** (ModularCreate): a Create with `FactoryContext` = the biped mesh, modules Root / Spine / Neck and
  the two connections the author knows (195), then Inspect rounds (60, 87, 21) whose `AutoConnect` and Neck's
  `Candidates` entry `Socket(Spine/neck_socket)` are accepted (458 in all) equal the rig the editor's calls build
  (extract, model, pose). Validate of the Create (a transient rig) passes and makes no asset.
- **Rollback** (ModularRollback): refused applies (a connection to a missing socket after a module add with config,
  unknown class, a class that is not a rig module, unknown connector, unknown parent, a module that is its own parent)
  leave the extract and the package as they were, and name close candidates. Refuse, garbage collection, then the same
  module edited again applies. A human module add (one transaction) then an AI apply: undo cannot reach the rig; a
  failed AI apply after it changes nothing.
- **Dependents** (ModularDependents): a project rig module used by a loaded modular rig: the module edit is refused
  while that rig has unsaved changes, applies with `ControlRig.DependentsUpdated` once it is saved.
- **Validate** (Validate, DiffOnly, ClassInspect, FactoryContext): Create and Update validate on a transient
  rig (87 / 105) and report unknown units, pins, nodes and elements with close names and the node's pins; the rig, its
  package and the undo buffer are untouched and no transient rig survives garbage collection. `diff_only` extract of
  MoverExamples `CR_Mannequin_BasicFootIK`: `Properties` 20 -> 3 entries, 58013 -> 56934 characters (the rest is the
  hierarchy and graphs); Hinge 18107 -> 16926. Create with `FactoryContext` (107) equals Create Control Rig on the mesh
  plus the same null, and the short document re-applies as a no-op with an empty Diff.
- **Connection order.** Spine's sockets (`neck_socket`, `shoulder_l`, ...) appear only once Spine's secondary
  connectors resolve, so a whole document's connections are made module by module down the tree, each module's
  primary connector first (the read header lists the primary connectors).
- **AutoConnect.** The panel's auto-resolve runs one connector after the other and the rig is constructed again
  after each (a connection-changed notification recompiles the modular rig), so a later connector's default target
  can depend on an earlier one (an arm's `Shoulder Bone`). Inspect therefore runs `AutoConnectModules` itself on a
  transient copy of the rig instead of matching each connector once.
- **Config overrides** keep their value as text too (what they save and compare). An override is set from the decoded
  value's exported text, as the Details panel's property write does.
- The Finger module logs empty-array reads from its VM while its connectors are not resolved (in the editor too); the
  biped tests expect them.

## Modular rig and extract-size checks

Tests `UEAssetDoc.ControlRig.{RefusedCalls,ModularOneApply,ExtractSizes,AutoConnectProbe}`, Mac and PC sandboxes.

- **Module cycle.** A `Parent` chain that comes back to a module is refused before any edit as
  `ControlRig.ModuleCycle` naming the chain (`Spine -> Neck -> Spine`), before the ordering checks (a cycle is not a
  `ParentAfterChild` problem that a reorder could fix).
- **One apply for a modular rig.** A Create that lists every connection builds the biped (Root, Spine, Neck, arms,
  legs, a foot: 8 modules) in one apply, equal to the rig the Module Assembly panel's drops build (extract, model), with
  the map listed in reverse; a re-apply is a no-op. Connections are made module by module down the tree, each primary
  connector first (a module's sockets exist once its connectors resolve). A client that knows only the primary
  connections and takes the rest from Inspect's `AutoConnect` needs several Inspect / apply rounds, since
  `AutoConnect` needs each level connected first. Automatic resolution stays an Inspect suggestion (design decision 6):
  `Connections` takes element keys only.
- **AutoConnect picks what the editor picks.** An Arm dropped on Spine's `shoulder_l` socket gets `Elbow Bone` =
  `clavicle_l` in the editor too (SKM_Manny_Simple and SKM_Biped_Template; `lowerarm_l` once a Shoulder module is
  there). A suggested bone above where its module attaches is listed in `ModularRigSuggestions.Check` with the other
  matches, so it is not accepted unseen.
- **Inspect of a modular rig** lists the elements its modules build (`GlobalTransforms`, `Counts.module_elements`:
  149 for the biped of 489 constructed).
- **FactoryContext** takes a package path (`/Game/Characters/SKM_Hero`), as the asset picker resolves one.
- **Sparse Update** without `RigType` keeps the rig's type; a function rename diffs as one rename
  (`/Body/Functions/<old>/Name`).
- **Sizes** (`diff_only` extracts, condensed characters): Game `CR_Mannequin_Body` 511907 -> 492123 (`PinTypes`
  38903 -> 19119: a template node lists only the pins that decide the others' types; Verify and Diff compare over the
  full types), MoverExamples `CR_Mannequin_BasicFootIK` 56930 -> 56876, MoverExamples `CR_Mannequin_Body`
  428485 -> 411564; `list_types controlrig_module` 58.7 KB -> 13.2 KB (pretty; `detail: true` 50.9 KB). The fidelity
  documents of GraphFidelity shrink the same way (Root 236533 -> 210042, Spine 339268 -> 314633); VM hashes, instruction
  counts and poses are unchanged.
- **Not done.** Omitting an imported bone's Transform when the rig has no `SourceHierarchyImport` (against the preview
  mesh's reference pose) would take Game `CR_Mannequin_Body` to 229 of 342 bones with Transform, and rounding float
  noise to 0 would drop more digits; both change the rig built from the document: a rig stores some bones with the
  other quaternion sign than the mesh (and an identity rotation of W = -1 comes back +1 after a save), and aims on
  those bones then solve to the other sign (SportsCar, Offroad poses). Imported bones keep their Transform unless they
  equal `SourceHierarchyImport`'s reference pose.

## Known limits

- Rigs that are loaded but not compiled in this session read `Compile.Status: "Dirty"`. The Control Rig loader
  rebuilds the VM (`VMHash`, `Instructions` are present) but not the Blueprint, and the load-time compile messages
  are not kept. Apply compiles (the Compile button), so an applied rig reads `UpToDate` while an untouched copy of the
  same asset reads `Dirty`; the fidelity comparison leaves `Compile` out.
- A free reroute without links is removed when the rig loads (`PatchRerouteNodesOnLoad`; one with only a source link
  becomes a Constant). The read shows the loaded state; apply refuses a value reroute without a linked input
  (`ControlRig.UnstableReroute`), as it would not survive a reload.
- MoverExamples `CR_Mannequin_Body` logs compile errors on load in 5.7 (stale pins on Setup / Forward Fingers,
  unmapped variables on Forward Spine). The asset still reads and is stable; the test expects those messages.
- `list_types` `rigvm_node` pins list hidden pins (`Direction: "Hidden"`, such as `CachedIndex`) because nodes have
  them too. Execute pins read as `/Script/RigVM.RigVMExecuteContext`, as on nodes.
- Node messages live on the editor graph nodes. A rig created headlessly has empty editor graphs until the editor
  (or its refresh) rebuilds them from the model, so its node messages appear only after that.
- `Expanded` lists every expanded pin, including pins a new node expands by default (`ExpandByDefault`, such as
  `Item` on Get Transform). `Title` is read only on renamable nodes (Constant, Make Struct); units keep their
  display name.
- Hierarchy transforms are stored at float precision and read as the shortest decimal of that float.
- The `rig.controlrig.catalog.v1` primitive is tested through `list_types` rows only.
- Module connections are read from the model; whether a connector resolves at runtime is not reported (Inspect's
  `Candidates` lists what it could resolve to).
- MetaHuman content (#13) is not covered on the CI sandbox (plugin not enabled).
- Engine behaviour the profile works around (UE 5.7): `URigVMBlueprint::SetVariablePublic` passes its flag as "Blueprint only
  editable", the inverse of public, so the public flag goes through `FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag`;
  the variable-change delegates that update nodes and bindings are bound only in `PostLoad`, so each variable op calls
  `OnPreVariableChange` / `OnPostVariableChange` itself; a function reference node repopulated during a rollback drops
  variable remaps whose types no longer match, so the rebuilder sets them again.
- Editor graphs of a rig created headlessly are bound to the asset before its first compile (as opening the editor
  does); the rollback rebuilder does not refresh them from the registry, which would replace the restored editor graphs.
- A negative zero in a pin value (a struct component the source stores as `-0`) is written as `0`: the VM differs from
  the source's only by that literal's sign (`ComputeVMHash` differs, the pose does not).
- An `Order` that moves existing nodes is not applied (no editor command reorders nodes) and ends as a
  `ControlRig.GraphDiffers` warning.
- A variable default is omitted when it reads as its type's default. An element key without a name reads `"Type()"`, one without a type `"None(Name)"`
  (`"Control()"`, Finger's `Parent Control`, which keeps the type of pin defaults such as an aim's
  `Target Space`); `""` is the key without a type.
- A rig module's preview connector targets (`ArrayConnectionMap`, which the hierarchy tab's connector resolve sets)
  are editor preview state. The document does not carry them, and a rig module created from a document has none until
  a person resolves its connectors. Fidelity copies the source's targets onto the created module before it poses the
  rigs. Decision: `ArrayConnectionMap` is not authored. On a modular rig it is derived from the model's
  connections (`UpdateConnectionMapFromModel` after every connection change), so `Connections` already carries it; on
  a rig module it stays preview state.
- The 5.7 editor cannot make these, so apply refuses them (`ControlRig.EditRefused`) and reads them as they are: Get
  Candidates and other connector units outside a rig module, local variables on a collapse graph (kept when listed
  unchanged), a function or collapse node with a second execute pin, a reference to another rig's private function.
  A rig that holds one reads and stays editable everywhere else.
- A VM made from a document can differ from the source's by texts of equal value that the source holds and no editor
  action writes (see "Apply results: functions and collapse nodes"). The program, its instruction count and the pose are equal.
- An animation channel's parent weight is not read or written: 5.7 refuses to set it, and a new channel has weight 0
  where older assets hold 1.
- Control values under 1e-5 in an `EulerTransform` component read as 0 (load-time float noise).
