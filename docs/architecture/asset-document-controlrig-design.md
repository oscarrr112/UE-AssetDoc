# AssetDocument Control Rig profile: design

Decisions in §8. Support matrix and results: [asset-document-controlrig-matrix.md](asset-document-controlrig-matrix.md).
Open-editor apply: close, apply, reopen (test `ControlRig.OpenEditorApply`).
Governing rules: [design rules](asset-document-design-rules.md) §3 (rollback = functional
equivalence), §3.1 (Undo history), §3.2 (dependent assets), §4 (editor-equivalent code; raw paths allowed when the
editor does it that way, in their own file with a behaviour test; no private headers, no unexported symbols) and §5
(support matrix). The reference is the **Control Rig editor** of UE 5.7 (`Engine/Plugins/Animation/ControlRig`, on top
of `Engine/Plugins/Runtime/RigVM`), its hierarchy tab, Module Assembly panel, My Blueprint panel, Class Settings and
Content Browser actions. Acceptance is editor fidelity on real
engine/plugin content, free edits, and an AI writing rigs from scratch.

Source paths: `CR/` = `Engine/Plugins/Animation/ControlRig/Source/`, `RV/` = `Engine/Plugins/Runtime/RigVM/Source/`.
Modules: `ControlRig` (runtime: hierarchy, hierarchy controller, modular rig controller, shape library),
`ControlRigDeveloper` (UncookedOnly: `UControlRigBlueprint`, compiler), `ControlRigEditor` (editor: factory, panels),
`RigVM` / `RigVMDeveloper` (UncookedOnly: `URigVMBlueprint`, the graph model, `URigVMController`, compiler) /
`RigVMEditor`. All four plugins involved (ControlRig, RigVM, ControlRigModules (beta, content only),
ControlRigSpline (beta)) are enabled by default.

## 1. Classes in scope

| Class / kind | Scope | Reason |
|---|---|---|
| `/Script/ControlRigDeveloper.ControlRigBlueprint`, parent `UControlRig` (an "independent rig") | **Exact profile** `profile.control_rig.v1` | A Blueprint subclass whose content is a RigVM graph model, a rig hierarchy and Class Settings. The generic route refuses it (`FactoryCreate.BlueprintClassUnsupported`, coverage audit) and the Blueprint profile cannot own it: the K2 pipeline is turned off for it (`FRigVMBlueprintCompilerContext` no-ops `ValidateNode` / `CreateFunctionList` / `ProcessOneFunctionGraph`), its EdGraph is a derived view and its nodes have no stable GUID (§2.3). |
| Rig **module** (same class, `RigModuleSettings.Identifier.Name` non-empty: `IsControlRigModule`) | Same profile, `Body.RigType: "Module"` + `Body.Module` | Not a class: a rig with module settings and connectors (`TurnIntoControlRigModule`, `TurnIntoStandaloneRig`). |
| **Modular rig** (same class, `ParentClass = UModularRig`, `IsModularRig`) | Same profile, `Body.RigType: "Modular"` + `Modules` / `Connections` | Not a class either: the factory creates it with `ParentClass = UModularRig` and adds a `Root` module. It has no event graphs, functions or local variables (`SupportsEventGraphs/Functions/LocalVariables` return false). |
| Other `UControlRig` native parents (`IsBlueprintBase`, picked in the factory's class picker) | Same profile, `Body.ParentClass` | The factory dialog offers native `UControlRig` subclasses only; Blueprint parents are not allowed by the editor ("won't work well because of multi rig graph"). |
| `/Script/ControlRig.ControlRigShapeLibrary` | **Generic profile** (no exact profile) | Plain `EditAnywhere` properties with no custom details. The coverage audit already passes create / extract / edit / reload / no-op with 5 of 5 properties. `PostEditChangeChainProperty` renames duplicate shape names (`_N`) and resets `DefaultShape.ShapeName`; the generic preflight already reports that as `Generic.PropertyValueNotStoredExactly`. The rig profile adds shape checks and Inspect facts (§3.8). |
| `/Script/RigVMDeveloper.RigVMBlueprint` (plain RigVM asset, no hierarchy) | Out | Not creatable from the Content Browser. It is only a base class. |
| Hierarchy **components** (`FRigBaseComponent`; the only implementations are in the experimental `ControlRigPhysics` plugin) | Out, `ControlRig.ComponentsUnsupported` | Experimental, not in shipped content. Extract lists them under `_Skipped`, so applying an extract never deletes them (§3.6). |
| Procedural elements (created by the Construction event at runtime) | Out (derived) | They exist only in instances. The Blueprint's hierarchy holds the static elements only. |
| The Control Rig's users: AnimBP `AnimGraphNode_ControlRig`, Level Sequence Control Rig tracks, `UControlRigComponent` | Out here | AnimBP nodes go through the AnimBlueprint profile. Sequences are the Level Sequence design ([asset-document-sequencer-design.md](asset-document-sequencer-design.md)). A sequence stores a binding to a rig class plus channels keyed by control name; this profile never edits sequences, it reports them as dependents (§5.4). |
| `UControlRigValidator` passes, `RigGraphDisplaySettings` profiling fields, the debug instance | Out | Runtime validation (not run on compile), transient UI state. |
| The new non-Blueprint RigVM editor (`RigVM.UseNewEditor`, off) and any 5.8 non-Blueprint asset | Out, tracked as risk (§7.1) | 5.7 keeps the data on `UControlRigBlueprint` / `URigVMBlueprint` (`*Legacy.h`). The logic already moved into `IControlRigAssetInterface` / `IRigVMAssetInterface`. |

A Control Rig is made once and then edited for a long time. Most of the human's work is graph wiring and hierarchy
editing (controls, shapes, limits, spaces). Modular rigs are mostly *assembly*: picking modules, connecting connectors,
setting config values. The plan (§7.2) follows that order.

## 2. Editor data model: authored vs derived

### 2.1 Where things live (`CR/ControlRigDeveloper/Public/ControlRigBlueprintLegacy.h`, `RV/RigVMDeveloper/Public/RigVMBlueprintLegacy.h`)

**Authored, Body-owned (Extract emits, Apply writes through controllers):**

- `RigVMClient` (`FRigVMClient`, not a Details property): `Models` (the event graphs; `Models[0]` is the default model
  `"RigVMModel"`, more are added with `AddModel`, named `"RigVMModel <Name>"`) and `FunctionLibrary`
  (`URigVMFunctionLibrary`, `"RigVMFunctionLibrary"`). Each `URigVMGraph` holds `Nodes`, `Links` and `LocalVariables`
  (function graphs). Each function is a `URigVMCollapseNode` in the library whose `ContainedGraph` is the body. The
  library also holds `PublicFunctionNames` and `FunctionToVariant`.
- `Hierarchy` (`URigHierarchy` default subobject, custom binary serialization). Only the **initial** state is saved:
  `URigHierarchy::Save` writes element keys, per-element static data (bone type, control settings, initial local pose,
  initial offset and shape transforms, initial control value, connector settings, curve value), parent constraints with
  their initial weights, metadata and components. Global and current transforms are not saved.
- `ModularRigModel` (`Modules[]` of `FRigModuleReference {Name, ParentModuleName, Class, ConfigOverrides, Bindings}` and
  `Connections` of `{Connector, Targets[]}`), `ModularRigSettings` (`bAutoResolve`).
- `RigModuleSettings` (`Identifier{Name, Type}`, `Icon`, `Category`, `Keywords`, `Description`). `ExposedConnectors` is
  derived.
- `NewVariables` (inherited from `UBlueprint`): member variables, edited with `AddMemberVariable` / `Remove` /
  `Rename` / `ChangeMemberVariableType` and the `SetVariable*` setters.
- `ParentClass` (fixed after creation; `UModularRig` makes a modular rig).

**Authored, Class Settings Details (generic `Properties`):** `PreviewSkeletalMesh`, `SourceHierarchyImport`,
`SourceCurveImport`, `ShapeLibraries`, `DrawContainer`, `HierarchySettings`, `VMCompileSettings` (its `ASTSettings` are
transient), `VMRuntimeSettings`, `bAllowMultipleInstances`, `CustomThumbnail` (overwritten by the module icon on save),
the authored parts of `RigGraphDisplaySettings`, and `AssetVariant` tags.

**Derived (never emitted):**

| State | Recomputed by |
|---|---|
| `UbergraphPages` / `FunctionGraphs` EdGraphs (`URigVMEdGraph`, `URigVMEdGraphNode`, `FunctionLibraryEdGraph`) and their node GUIDs | model notifications (`URigVMEdGraph::HandleModifiedEvent`, `CreateNewGuid` on every node creation) and `RebuildGraphFromModel` (editor open, registry change) |
| `GeneratedClass`, CDO VM (bytecode, literal / work memory), `PinToOperandMap`, `GraphFunctionStore` | full compile (`FKismetEditorUtilities::CompileBlueprint` → `OnPostCDOCompiled` → `RecompileVM`). In the editor the VM is always recompiled on load (`HandlePackageDone`) |
| CDO hierarchy, `DrawContainer`, `Influences` copies, instance hierarchies | compile (`CopyTermDefaultsToDefaultObject`) and `PropagateHierarchyFromBPToInstances` / `PropagateDrawInstructionsFromBPToInstances` / `PropagatePoseFromBPToInstances` |
| `PublicGraphFunctions`, `FunctionReferenceNodeData`, `SupportedEventNames`, `ReferencedObjectPaths*`, `UserDefined*ToPathName` | `PreSave` / `RecompileVM` |
| `ControlRigType`, `ItemTypeDisplayName`, `bExposesAnimatableControls`, `ModuleReferenceData`, `RigModuleSettings.ExposedConnectors`, `CustomThumbnail` of a module | `PreSave` (`UpdateExposedModuleConnectors` …) |
| `ArrayConnectionMap` | `UpdateConnectionMapFromModel` (modular), connector resolves |
| Current / global pose, topology and pose versions, element indices, selection | hierarchy runtime |
| Pin sub-pins, pin types of resolved templates, orphaned pins | controller (node creation, `ResolveWildCardPin`, repopulate on load) |
| `bSupportsInversion`, `bSupportsControls` | vestigial (only copied to the generated class) |
| `Validator`, `DebugBoneRadius`, `ModulesRecompilationBracket`, deprecated `*_DEPRECATED` properties | runtime / migration (`PostSerialize`, `PostLoad` patches) |

### 2.2 Two engine behaviours that shape the read path

1. **Load patches.** `PostLoad` / `HandlePackageDone` patch older assets in memory without dirtying the package:
   deprecated models, `PatchRigElementKeyCacheOnLoad`, `PatchVariableNodesOnLoad`, `CreateMemberVariablesOnLoad`, and a
   VM recompile. The profile reads the patched in-memory state, as the editor shows it.
2. **First open adds the Forwards Solve event.** The factory creates the root EdGraph `Rig` but no nodes. Opening the
   editor on an empty default model adds `FRigUnit_BeginExecution` (`FControlRigBaseEditor::CreateEmptyGraphContentImpl`,
   private). So a human never sees an empty rig graph. A `Create` document without `Graphs` therefore gets the same
   node (§4.3).

### 2.3 Identity in the model

- **Nodes are identified by name**, unique per graph (`URigVMSchema::GetValidNodeName`), and there is no node GUID.
  EdGraph GUIDs are random and recreated on every sync, so they are not usable. Node paths nest with `|`
  (`Collapse|Inner`).
- **Pins** are `.` paths below the node: `Node.Pin.SubPin`, with array elements as `Node.Array.0`.
- **Links** are pin-path pairs (`SourcePinPath`, `TargetPinPath`), with no id of their own.
- **Graphs**: model names (`"RigVMModel"`, `"RigVMModel <Name>"`). A function is the collapse node name in the
  library. A collapse graph is its owner node's path.
- **Hierarchy elements**: `FRigElementKey {Type, Name}` (`"Bone(spine_01)"`). Module elements are named
  `"<Module>/<Element>"`.
- **Modules**: `FRigModuleReference::Name`, unique and flat in 5.7 (short names and parent paths are deprecated).
- **Variables**: name (`NewVariables` also carries a `VarGuid`, kept by rename).
- **Function variants** carry the only GUID (`FRigVMVariant.Guid`). It belongs to functions and is not authored
  (§3.6).

All of these are names stored in the asset. They survive save, reload and Content Browser duplication.
`PostDuplicate` re-points function identifiers (`HostObject` = the new generated class), which the document never
spells for local functions (§3.5).

## 3. Document shape

### 3.1 Example (small, complete: a one-control rig on the UE5 mannequin)

```jsonc
{
  "SchemaVersion": 1, "Target": "/Game/Characters/Rigs/CR_HeadAim", "Class": "/Script/ControlRigDeveloper.ControlRigBlueprint",
  "Action": "CreateOrUpdate",
  "Properties": {
    "PreviewSkeletalMesh": "/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple",
    "SourceHierarchyImport": "/Game/Characters/Mannequins/Meshes/SK_Mannequin.SK_Mannequin"
  },
  "Body": {
    "RigType": "Rig",
    "Variables": [
      {"Name": "AimWeight", "Type": "float", "Default": 1.0, "Public": true, "ExposeToCinematics": true}
    ],
    "Hierarchy": [
      {"Type": "Bone", "Name": "root", "Imported": true},
      {"Type": "Bone", "Name": "pelvis", "Parent": "Bone(root)", "Imported": true},
      {"Type": "Bone", "Name": "neck_01", "Parent": "Bone(pelvis)", "Imported": true},
      {"Type": "Bone", "Name": "head", "Parent": "Bone(neck_01)", "Imported": true},
      {"Type": "Null", "Name": "head_space", "Parent": "Bone(neck_01)",
       "Transform": {"Translation": {"X": 0, "Y": 0, "Z": 12}}},
      {"Type": "Control", "Name": "head_ctrl", "Parent": "Null(head_space)",
       "Offset": {"Translation": {"X": 60, "Y": 0, "Z": 0}},
       "Settings": {"ControlType": "Position", "ShapeName": "Sphere_Solid", "ShapeColor": {"R": 1, "G": 0.8, "B": 0, "A": 1},
                    "LimitEnabled": [{"bMinimum": true, "bMaximum": true}, {}, {}],
                    "bDrawLimits": true},
       "Minimum": {"X": -30, "Y": 0, "Z": 0}, "Maximum": {"X": 30, "Y": 0, "Z": 0},
       "Spaces": ["Bone(pelvis)", "Bone(root)"]},
      {"Type": "Curve", "Name": "CTRL_head_aim", "Value": 0.0}
    ],
    "Graphs": {
      "Rig": {
        "Nodes": {
          "ForwardsSolve": {"Unit": "RigUnit_BeginExecution", "Position": [0, 0]},
          "GetCtrl":  {"Unit": "RigUnit_GetTransform", "Position": [0, 200],
                       "Defaults": {"Item": "Control(head_ctrl)", "Space": "GlobalSpace"}},
          "GetWeight": {"Variable": "AimWeight", "Position": [0, 400]},
          "Aim": {"Unit": "RigUnit_AimItem", "Position": [400, 0],
                  "Defaults": {"Item": "Bone(head)",
                               "Primary": {"Axis": {"X": 1, "Y": 0, "Z": 0}, "Kind": "Location"}},
                  "Inputs": {"ExecuteContext": "ForwardsSolve.ExecuteContext",
                             "Primary.Target": "GetCtrl.Transform.Translation",
                             "Weight": "GetWeight.Value"}},
          "Note": {"Comment": "Head looks at head_ctrl", "Position": [-40, -80], "Size": [760, 560]}
        }
      }
    }
  }
}
```

The struct, pin and enum names are 5.7's (`RigUnit_BeginExecution.h`, `RigUnit_GetTransform.h`; `RigUnit_AimItem`'s
pin names are illustrative and come from `list_types` at authoring time). Imported bones without `Transform` take the
reference pose of `SourceHierarchyImport` (§3.4). Diagnostics point into this shape, e.g.
`/Body/Graphs/Rig/Nodes/Aim/Inputs/Primary.Target`.

### 3.2 Body regions

All regions follow the core rule: **omitted = keep, present = complete state, `[]`/`{}` clears**. Deviations are
listed.

| Region | Shape | Notes |
|---|---|---|
| `RigType` | `"Rig"` (default, extract omits it), `"Module"`, `"Modular"` | `Modular` is chosen at creation (the factory's `ParentClass = UModularRig`) and cannot change (`ControlRig.RigTypeImmutable`). `Rig` ↔ `Module` are the editor's "Turn into Rig Module" / "Turn into Standalone Rig" commands. |
| `ParentClass` | class path, default `/Script/ControlRig.ControlRig` (`/Script/ControlRig.ModularRig` for `Modular`) | Create only, as in the factory dialog (native `UControlRig` subclasses with `IsBlueprintBase`). Later changes fail `ControlRig.ParentClassImmutable`. |
| `Module` | `{Name?, Icon?, Category?, Keywords?, Description?}` | `RigModuleSettings` of a module. `Name` defaults to the asset name (what `TurnIntoControlRigModule` sets). Present only when `RigType` is `Module`. |
| `Variables` | ordered array of `{Name, Type, Default?, Public?, ReadOnly?, ExposeToCinematics?, Category?, Tooltip?, RenamedFrom?}` | My Blueprint member variables. `Type` is the RigVM CPP type the editor's type picker offers: `bool`, `int32`, `float`, `double`, `FName`, `FString`, `FText`, a struct or enum object path (`/Script/CoreUObject.Vector`, `/Script/ControlRig.RigElementKey`), and `TArray<…>` of those. Anything else fails `ControlRig.UnsupportedVariableType` before any engine call, because `ExternalVariableFromCPPTypePath` `check`s on an unknown type. `Default` uses the typed value codec. |
| `Hierarchy` | ordered array of elements (§3.4) | Hierarchy order is the editor's order (children in index order). Not allowed for `Modular`; module instances' elements are derived. |
| `Graphs` | `{"<Graph>": {Nodes, Order?, RenamedFrom?}}` | The event graphs (models). `Rig` is the default model and cannot be removed. Other keys are `AddModel` names without the `RigVMModel ` prefix. Not allowed for `Modular` (`ControlRig.ModularRigRegionUnsupported`). `Order` is the model's node order (extract writes it for two nodes or more): the compiler visits events and unlinked nodes in it, so it decides instruction and memory order and `ComputeVMHash`. Omitted keeps the order; new nodes are added last in `Order` (unlisted ones after them in name order, names `Nodes` lacks are ignored). Existing nodes keep their place, as the editor has no command that reorders nodes: an `Order` that moves them ends as a `ControlRig.GraphDiffers` warning. |
| `Functions` | `{"<Function>": Function}` (§3.5) | The local function library. Not allowed for `Modular`. |
| `GraphOrder` | array of graph names | Model order of the graphs (extract writes it for two graphs or more): the VM runs each event's instructions in the order of the graphs that hold them (`Rig`, then Construction, then Backwards Solve in the templates). New graphs are added in it; existing graphs keep their place. |
| `Modules` | ordered array of `{Name, Class, Parent?, Config?, Bindings?, RenamedFrom?}` (§3.7) | Modular rigs only. |
| `Connections` | `{"<Module>/<Connector>": "<element key>" \| ["<element key>", …]}` | Modular rigs only: the connector → target map (array connectors take a list). |
| `ModularSettings` | `{AutoResolve?}` | `ModularRigSettings` (Modular only). |

`Properties` (top-level, sparse, generic admission) carry the Class Settings. They are Body-owned and refused there
(`ControlRig.NotAuthoredProperty`, with the region to use): `RigVMClient`, `Hierarchy`, `ModularRigModel`,
`ModularRigSettings`, `RigModuleSettings`, `NewVariables`, `ParentClass`, and every derived property of §2.1. The
transient profiling fields of `RigGraphDisplaySettings` (`*MicroSeconds`) are never compared.

### 3.3 Graph nodes and pins

```jsonc
"<node name>": {
  // exactly one kind key:
  "Unit": "RigUnit_SetTransform" | "/Script/Module.Struct",  // FRigVMStruct unit (method "Execute"; "Method" when other)
  "Template": "Add::Execute(in A,in B,out Result)" | "DISPATCH_RigVMDispatch_ArrayAdd",  // template / dispatch notation
  "Variable": "AimWeight", "Setter": true,                  // member, local variable or function input getter / setter
  "Function": "MyFunc" | "/ControlRig/StandardFunctionLibrary/StandardFunctionLibrary.StandardFunctionLibrary:Name",
  "Collapse": {"Inputs": [...], "Outputs": [...], "Nodes": {...}},  // collapsed graph (same shape as a function body)
  "Reroute": "FVector" | "/Script/CoreUObject.Vector",       // free reroute of that type
  "Enum": "/Script/Module.EEnum",
  "InvokeEvent": "Forwards Solve",                           // "Run Event" node
  "Comment": "text", "Size": [w, h], "FontSize": 18, "Bubble": false, "ColorBubble": false,
  // common fields:
  "Position": [x, y], "Title": "...", "Color": {"R":..,"G":..,"B":..,"A":..},
  "PinTypes": {"A": "FVector"},                  // resolved wildcard pins of a template / dispatch node, in order
  "Aggregate": ["A", "B", "C"],                  // input pins of an aggregate node (Add with 3 operands)
  "Defaults": {"<pin path>": <value>},           // complete: omitted pin = node default
  "Inputs": {"<input pin path>": "<node>.<output pin path>"},   // complete: omitted = unlinked
  "Bindings": {"<pin path>": "<variable>[.<sub path>]"},        // pin bound to a variable (injected variable node)
  "Expanded": ["Primary", "Item"],               // expanded pins (UI state; extract only emits non-default)
  "VariableMap": {"<function variable>": "<host variable>"},    // function reference remapping
  "Traits": {"<trait name>": {"Type": "/Script/...", "Defaults": {...}}},
  "Injections": {"<pin>": [{"Unit": "RigUnit_VisualDebugVectorItemSpace", "Input": "Value", "Output": "Value",
                            "Name": "RigUnit_VisualDebugVectorItemSpace_1", "Defaults": {...}}]},  // units in a pin
  "RenamedFrom": "OldName"
}
```

- **Keys are node names**, the model's own identity. The kind does not change the name (unlike Material keys). A new key
  creates a node with exactly that name (`InNodeName`). A name the schema would change (characters outside its rules)
  fails `ControlRig.InvalidNodeName` instead of being silently renamed. `RenamedFrom` is `RenameNode`. Without it a new
  key is remove + add. A different kind or unit on a key is delete + create, as in Material.
- **Unit names.** A short struct name (`RigUnit_SetTransform`, `RigVMFunction_MathFloatAdd`) is accepted when exactly
  one registered `FRigVMStruct` has it. Otherwise the object path is required (`ControlRig.AmbiguousUnit` lists them).
  Extract writes the short name when it is unambiguous. A unit that belongs to a template is written as `Unit` (the
  controller creates the template node resolved to that permutation, `AddUnitNode`). A template node that is not
  resolved to a unit permutation is written as `Template` + `PinTypes`. Deprecated units (decision 9): existing nodes
  are kept on read and round trip; adding a new node of one is refused (`ControlRig.DeprecatedUnit`, naming the
  replacement from the unit's upgrade info, as the palette no longer offers it). A `Create`, or an apply to a rig
  whose graphs are empty, is a round trip: the node is added with a `ControlRig.DeprecatedUnit` warning.
- **Events** are units (`RigUnit_BeginExecution` "Forwards Solve", `RigUnit_InverseExecution`,
  `RigUnit_PrepareForExecution` "Construction", `RigUnit_PostPrepareForExecution`, `RigUnit_InteractionExecution`, …) or
  `RigVMFunction_UserDefinedEvent` with `Defaults.EventName`. They must be in a top-level graph, and an event name may
  exist once across all graphs (`ControlRig.EventAlreadyExists`, the controller's rule).
- **Pin values.** RigVM stores export-text strings per pin. The document holds typed JSON through the Editor typed
  value codec, keyed by the pin's CPP type: numbers, bools, enum entry names (`"GlobalSpace"`), `{X,Y,Z}`, transforms
  as `{Translation, Rotation, Scale3D}`, arrays as JSON arrays (array size follows the value: `SetPinDefaultValue` with
  `bResizeArrays`). Two RigVM-specific short forms: an `FRigElementKey` is `"Bone(spine_01)"` (the engine's
  `ToString` form) and an `FRigElementKeyCollection` / array is a list of those. Values are compared semantically
  (struct compare after import), so `1` and `1.000000` do not diff. `Defaults` may address a sub-pin
  (`"Primary.Axis"`); extract writes the top-level pin unless a sub-pin of it is linked. A value equal to the node's
  default is not extracted, and writing it resets the pin (`ResetPinDefaultValue`).
- **Links.** `Inputs` maps an input (or IO) pin path to its source `"<node>.<pin path>"`. Execution is the IO pin
  `ExecuteContext`, so `"ExecuteContext": "Prev.ExecuteContext"` is a link. An input holds at most one link (RigVM
  rule), and fan-out is several inputs naming the same source. Sub-pin links (`"Primary.Target"`) are allowed as in
  the editor.
- **Injected units** (the pin menu's Add Visual Debug / Add Interpolate, `AddInjectedNode`) are pin state, not nodes:
  `Injections` lists them per top-level pin in injection order (`Name` and `Method` optional). A link into a pin with
  injected units ends on the last unit's input pin and one out of such an output pin starts on the last unit's output
  pin (`URigVMPin::GetPinForLink`); the read writes both on the host pin, as the editor draws them, and `AddLink`
  redirects them the same way. A changed list removes the pin's units (`RemoveInjectedNode`) and injects the listed
  ones (`node.injections`); their values are set like other pin values. Variable bindings are the other injected nodes
  (`Bindings`).
- `Position` omitted keeps the node in place (a new node goes to a column layout like Material's). `Size`, `Title`,
  `Color`, `Expanded` and `PinTypes` omitted also keep the rig's (layout and editor-derived state); present, they are
  complete.
- **Reroutes** must survive load (`PatchRerouteNodesOnLoad`): a value reroute needs its whole `Value` input linked and
  no sub-pin links (otherwise load removes it or turns it into a constant / make / break struct node). Others are
  refused (`ControlRig.UnstableReroute`); execute reroutes are kept as written.

### 3.4 Hierarchy elements

```jsonc
{"Type": "Bone" | "Null" | "Control" | "Curve" | "Connector" | "Socket",
 "Name": "spine_01", "RenamedFrom": "spine01",
 "Parent": "Bone(pelvis)",                                   // single parent (Bone, Socket; Null / Control: first parent)
 "Parents": [{"Parent": "Bone(pelvis)", "Weight": 1.0, "Label": "Pelvis"}, ...],   // multi-parent (Null, Control)
 "Transform": {...},            // initial local transform (Bone, Null, Socket)
 "Imported": true,              // Bone: ERigBoneType::Imported
 // Control only:
 "Settings": {...},             // FRigControlSettings, sparse over a new control of that ControlType
 "Offset": {...}, "Shape": {...},   // initial offset / shape transforms
 "Value": ..., "Minimum": ..., "Maximum": ...,   // initial / limit values typed by ControlType
 "Spaces": ["Bone(pelvis)", {"Space": "Null(world)", "Label": "World"}],     // available spaces, in order
 "Hosts": ["Control(foot_ctrl)"],                                           // channel hosts (animation channels)
 // Curve: "Value"; Connector: "Settings" (Description, Type Primary/Secondary, bOptional, bIsArray, bPostConstruction, Rules);
 // Socket: "Color", "Description"
 "Metadata": {"<name>": {"Type": "Float", "Value": 1.0}}
}
```

- Identity is `Type` + `Name`. `RenamedFrom` is `RenameElement` (or `SetDisplayName(…, bRenameElement)` for animation
  channels, as the hierarchy tab does). The engine then rewrites this asset's pin defaults, influence map and connection
  map that use the old key (`HandleHierarchyElementKeyChanged`). The document must use the new name everywhere (stale
  references fail `ControlRig.ElementNotFound`).
- Order: the array is the hierarchy's element order (`ReorderElement`). Parents come before children
  (`ControlRig.ParentAfterChild`).
- **Imported bones.** `Transform` omitted on an `Imported` bone means "the reference pose of `SourceHierarchyImport`"
  (the skeleton that the editor's Import / Refresh used). Extract omits it when equal within 1e-4. This keeps a 90-bone
  mannequin skeleton readable without a non-declarative "import" directive. Without `SourceHierarchyImport`, every
  bone's `Transform` is extracted.
- Controls: `Settings.ControlType` decides how `Value` / `Minimum` / `Maximum` are typed (`Float`/`ScaleFloat` number,
  `Integer` int or enum entry with `ControlEnum`, `Bool`, `Vector2D` `{X,Y}`, `Position`/`Scale` `{X,Y,Z}`, `Rotator`
  `{Pitch,Yaw,Roll}`, `EulerTransform` / `Transform` / `TransformNoScale` transforms). `Value` omitted is the type's
  identity value. Animation channels are controls with `Settings.AnimationType: "AnimationChannel"` and a control
  parent. A channel's parent weight is not read or written: `URigHierarchy::SetParentWeight` refuses it (a channel is
  not a 3D thing), the hierarchy controller adds a channel at weight 0 and older versions at 1, so a channel always
  reads as a plain `Parent` and a weight other than 1 on one is refused. An `EulerTransform` control value reads
  components under 1e-5 as 0: the hierarchy derives the value from the stored pose on load, and a zero comes back as
  float noise. `ShapeName` is checked against the rig's shape libraries (§3.8).
- Transient fields (`bIsCurve`, `bIsTransientControl`, `ShapeTransform`, current pose) are never emitted or accepted.

### 3.5 Functions and collapsed graphs

```jsonc
"Functions": {
  "AlignChain": {
    "Public": true, "Category": "Chains", "Keywords": "", "Description": "", "Color": {...},
    "Inputs":  [{"Name": "ExecuteContext", "Type": "/Script/RigVM.RigVMExecuteContext", "Direction": "IO"},
                {"Name": "Items", "Type": "TArray<FRigElementKey>", "Default": []}],
    "Outputs": [{"Name": "Success", "Type": "bool"}],
    "LocalVariables": [{"Name": "Index", "Type": "int32", "Default": 0}],
    "Entry": {"Position": [-400, 0]},      // interface nodes: node-like (Position, Inputs, Defaults, Expanded)
    "Return": {"Position": [600, 0], "Inputs": {"ExecuteContext": "Entry.ExecuteContext", "Success": "Check.Result"}},
    "Nodes": {...}, "Order": [...],        // same shape as a graph; links read "Entry.<input>"
    "RenamedFrom": "Align"
  }
}
```

- `Public` is `MarkFunctionAsPublic`. There is no `Mutable` field: an execute pin is an `Inputs` entry with
  `Direction: "IO"` (as the editor lists it). A new function whose first input is `ExecuteContext` `IO` is created
  mutable (`AddFunctionToLibrary` `bMutable`, which also adds the Entry → Return execute link); other execute pins
  are `AddExposedPin` with direction IO. `Inputs` / `Outputs` are the exposed pins in order (`AddExposedPin` /
  `RemoveExposedPin` / `RenameExposedPin` via `RenamedFrom` / `ChangeExposedPinType` / `SetExposedPinIndex`, whose
  index counts all pins of the library node, inputs first). `Default` is the library node's pin default.
- `Color`, `Entry` and `Return` left out keep the asset's (the editor places Entry / Return itself); present, they are
  set. `LocalVariables` are `AddLocalVariable` / `RemoveLocalVariable` / `RenameLocalVariable` (`RenamedFrom`) /
  `SetLocalVariableType` / `SetLocalVariableDefaultValue`. A `Variable` node inside a function resolves a local
  variable or input before a member variable.
- `Order` of a function or collapse graph is its model order, as for graphs: the compiled VM follows it.
- **Function references:** a local function by name, another asset's public function as
  `"<host object path>:<function>"` (`AddFunctionReferenceNodeFromDescription` with the host's header,
  `FRigVMGraphFunctionHeader::FindGraphFunctionHeader`). An external host must be a loadable Control Rig with that
  public function (`ControlRig.FunctionNotFound` lists the public functions). `VariableMap` is
  `SetRemappedVariable`, written as a complete map (unlisted inner variables are unmapped).
- **Collapse nodes** carry the same graph shape in `Collapse` (`Inputs` / `Outputs` without `Default`, metadata,
  `Entry` / `Return`, `Nodes`, `Order`). A new one is made the editor's way, `CollapseNodes` on a seed comment that is
  then deleted inside, and filled like any graph before the parent graph's links, as the K2 composite is.
- **What the 5.7 editor cannot make is refused, not forced** (`ControlRig.EditRefused`; the rig as it is stays
  editable, its unchanged parts are no ops): local variables on a collapse graph (the controller gives them to
  function graphs only; a collapse graph of an older version lists its own `LocalVariables`, which must stay as
  they are), a second execute pin on a function or collapse node (the editor keeps one), another rig's private
  function (`AddFunctionReferenceNodeFromDescription` places public ones only), and a unit the schema does not offer
  in the rig (`UControlRigSchema::SupportsUnitFunction`: connector units such as Get Candidates are for rig modules).
  #1 `StandardFunctionLibrary`, #2 `SplineFunctionLibrary`, #4 `Leg` and #11 `CR_Mannequin_Body` hold one of these.
- **Variants:** the variant GUIDs and tags are not authored. Extract omits them and apply never changes them.
  `CreateFunctionVariant` / `Split` / `Join` are rare commands and are refused (`ControlRig.VariantsNotAuthored`).

### 3.6 Omitted vs present, per item

| Item | Omitted | Present |
|---|---|---|
| `Variables`, `Hierarchy`, `Graphs`, `Functions`, `Modules`, `Connections`, a graph's `Nodes`, a function's `LocalVariables` | keep | complete. Unlisted entries are removed (`RemoveMemberVariable`, `RemoveElement`, `RemoveModel`, `RemoveNode`, `RemoveFunctionFromLibrary`, `DeleteModule`, `DisconnectConnector`) |
| `Graphs.Rig` on `Create` | the rig graph gets the Forwards Solve event (§2.2) | complete as written |
| Node `Defaults` / `Inputs` / `Bindings` / `PinTypes` / `Aggregate` / `Traits` | node defaults / unlinked / unbound / unresolved / two operands / none | complete |
| Node `Position`, `Title`, `Color`, `Expanded` | keep (new: layout, unit defaults) | set |
| Element `Transform` | Imported bone: source ref pose; otherwise identity | set |
| Element `Settings` (control / connector) | new-element settings of that type | sparse over that default |
| Module `Config` / `Bindings` | none (class defaults, unbound) | complete |
| `Module` (settings), `ModularSettings` | keep | complete over its fields |
| Components and procedural elements | — | never authored. Extract reports components under `_Skipped` so the AI knows they exist; apply keeps them. |

### 3.7 Modular rigs and modules

```jsonc
"RigType": "Modular",
"Properties": {"PreviewSkeletalMesh": "/ControlRigModules/Modules/Meshes/SKM_Biped_Template.SKM_Biped_Template"},
"Body": {
  "Modules": [
    {"Name": "Root",  "Class": "/ControlRig/Modules/Modules56/Root.Root_C"},
    {"Name": "Spine", "Class": "/ControlRigModules/Modules56/Spine.Spine_C", "Parent": "Root"},
    {"Name": "Arm_l", "Class": "/ControlRigModules/Modules56/Arm.Arm_C", "Parent": "Spine",
     "Config": {"Settings.bUseIK": true}, "Bindings": {"Stretch": "Spine/Stretch"}}
  ],
  "Connections": {
    "Root/Root": "Bone(root)",
    "Spine/Root": "Bone(pelvis)", "Spine/Chest": "Bone(spine_05)",
    "Arm_l/Root": "Bone(clavicle_l)"
  },
  "ModularSettings": {"AutoResolve": true}
}
```

- Modules are identified by `Name` (`RenamedFrom` = `RenameModule`). The array order is the module list order
  (`ReorderModule`). `Parent` is `ReparentModule`, and a changed `Class` is `SwapModuleClass` (the editor's "Swap Module
  Class"). `Class` must be a rig module (`ControlRig.ClassNotARigModule`, as `AddModule` checks `IsRigModule`).
- `Config` holds the module's config overrides (`ConfigOverrides`, keyed by property path, typed through the module
  class's property). It is applied with `SetConfigValueInModule(FControlRigOverrideValue)` and reset with
  `ResetConfigValueInModule`. `Bindings` maps a module variable to `"<host variable>"` or `"<module>/<variable>"`
  (`BindModuleVariable` / `UnBindModuleVariable`, checked with `CanBindModuleVariable`).
- `Connections` is complete. Apply connects with `bAutoResolveOtherConnectors = false` in document order, so the result
  is exactly the document. The editor's auto-resolve (`AutoConnectSecondaryConnectors`) is offered as an Inspect
  suggestion (§3.8). Extract writes every connection, including ones auto-resolve made, and connectors left unresolved
  are reported by status (§5).
- A **rig module** (`RigType: "Module"`) authors its connectors as `Connector` elements in `Hierarchy` (one `Primary`,
  as the controller requires) and its `Module` settings. The editor's first-connector auto-conversion
  (`TurnIntoControlRigModule` on the first added connector) is not reproduced implicitly: the document says
  `RigType: "Module"`.

### 3.8 Discovery (read-only additions)

- `list_types` gains:
  - `kind: "rigvm_node"`: units, templates and dispatch factories from the registry: short name, path, display
    name, category, keywords, `Deprecated`, event name and `CanOnlyExistOnce`, pins (`name: CPPType [= default]`,
    direction, `Expand`, wildcard).
  - `kind: "rigvm_type"`: the variable / pin types `ExternalVariableFromCPPTypePath` accepts.
  - `kind: "controlrig_module"`: rig module assets (asset-registry tags: identifier, category, exposed
    connectors, config properties with defaults, bindable variables).
- `inspect_asset_document_target`:
  - On a Control Rig: the extract plus pins per node (with resolved types), the hierarchy tree with global initial
    transforms, shape names available from `ShapeLibraries` (namespaced `Library.Shape` when there are several, as the
    rig resolves them), public functions, events, and compile status (§5).
  - On a modular rig: `ModularRigSuggestions.AutoConnect` (connections `AutoConnectSecondaryConnectors` /
    `AutoConnectModules` would make, computed on a transient duplicate).
  - On a ShapeLibrary: the resolved shape list.
- Suggestions are never document directives, as decided for IK Rig (automatic tools stay suggestions, so re-apply
  stays a no-op).

### 3.9 Free edits → operations

| Free edit | Document change | Operation (§4) |
|---|---|---|
| Insert a node into an execution chain | new key; re-point the next node's `ExecuteContext` input | `AddUnitNode`, `BreakLink`, `AddLink` ×2 |
| Delete / rename a node | remove the key / new key + `RenamedFrom` | `RemoveNodeByName` / `RenameNode` |
| Reconnect / disconnect a pin | change / remove an `Inputs` entry | `BreakLink` + `AddLink` |
| Change a pin value | `Defaults` | `SetPinDefaultValue` / `ResetPinDefaultValue` |
| Add a control under a bone, set its shape and limits | new `Hierarchy` entry | `AddControl` + `SetControlSettings` + `SetControlValue` (Initial / Min / Max) |
| Reparent / reorder an element | `Parent` / array position | `SetParent` (keep global, as drag-and-drop does) / `ReorderElement` |
| Rename a bone used by graph pins | `RenamedFrom` on the element and new names in `Defaults` | `RenameElement` (rewrites this asset's pins) |
| Add a space to a control | `Spaces` | `AddAvailableSpace` / `SetAvailableSpaceIndex` |
| New member variable, use it in the graph | `Variables` + a `Variable` node | `AddMemberVariable`, `AddVariableNodeFromObjectPath` |
| Collapse nodes into a function | new `Functions` entry, nodes moved, a `Function` node in place | `AddFunctionToLibrary` + body + reference node (not `CollapseNodes`: the end state is equal, §7.1) |
| Add a Construction event graph | `Graphs.Construction` with a `RigUnit_PrepareForExecution` node | `AddModel` + `AddUnitNode` |
| Add a module and connect it | `Modules` entry + `Connections` | `AddModule`, `ConnectConnectorToElement(s)` |
| Change a module setting | `Modules[i].Config` | `SetConfigValueInModule` |
| Swap the preview mesh | `Properties.PreviewSkeletalMesh` | `SetPreviewMesh` (via the settings hook, §4.2) |

## 4. Editor-equivalent operations

### 4.1 Mapping

The editor's pattern: a panel or spawner opens a transaction or undo bracket and calls the **controller**. These are
`URigVMController` for graphs, `URigHierarchyController` for the hierarchy and `UModularRigController` for modules
(all public, `MinimalAPI` with exported members, mostly `BlueprintCallable`). The asset reacts to notifications:
`HandleModifiedEvent` requests a VM recompile, `HandleHierarchyModified` propagates to instances, and
`HandleRigModulesModified` runs `RecompileModularRig`. The profile makes the same controller calls.

| Editor action (entry point) | Call the profile makes | Notes |
|---|---|---|
| New Control Rig / Modular Rig (Content Browser; `UControlRigBlueprintFactory::FactoryCreateNew`, `CreateNewControlRigAsset(path, bModularRig)`) | the factory with `ParentClass` set (no `ConfigureProperties` dialog), inside the non-interactive scope | creates the root EdGraph `Rig`; modular: `AddModule("Root", DefaultRootModule)` (§4.3) |
| "Create Control Rig" on a mesh (`CreateControlRigFromSkeletalMeshOrSkeleton`) | not a separate path: `Create` + `Hierarchy` + `Properties` | the action is `ImportBones` + `ImportCurves` + `SetPreviewMesh` + source imports, which the document states |
| Palette: unit (`FRigVMEdGraphUnitNodeSpawner::Invoke`) | `AddUnitNode(Struct, Method, Pos, Name)`; a user event: `AddUnitNodeWithDefaults(…, "(EventName=…)")` | the spawner's auto-link of mutable nodes (`HookupMutableNode`) is UI convenience; documents list links |
| Palette: template / dispatch | `AddTemplateNode(Notation, …)` + `ResolveWildCardPin` per `PinTypes` | |
| Variable get/set (`VariableNodeSpawner`, drag from My Blueprint) | `AddVariableNodeFromObjectPath(Name, CPPType, ObjPath, bIsGetter, "", Pos, NodeName)` | |
| Function reference (`FunctionRefNodeSpawner`) | `AddFunctionReferenceNode` / `AddExternalFunctionReferenceNode` / `AddFunctionReferenceNodeFromDescription`, then `SetRemappedVariable` | |
| Enum, Run Event, comment, reroute | `AddEnumNode`, `AddInvokeEntryNode`, `AddCommentNode` / `SetCommentText` (the EdGraph comment is intercepted into this), `AddFreeRerouteNode` / `AddRerouteNodeOnLinkPath` | |
| Connect / break (`URigVMEdGraphSchema::TryCreateConnection`) | `AddLink` / `BreakLink` / `BreakAllLinks` | |
| Pin value, expansion, bind to variable | `SetPinDefaultValue`, `SetPinExpansion`, `BindPinToVariable` / `UnbindPinFromVariable` | |
| Array / aggregate pins (+ / ×) | `SetArrayPinSize` / `InsertArrayPin` / `RemoveArrayPin`, `AddAggregatePin` / `RemoveAggregatePin` | |
| Move, rename, title, colour, delete | `SetNodePositionByName`, `RenameNode`, `SetNodeTitle`, `SetNodeColor`, `RemoveNodeByName` | |
| Traits | `AddTrait` / `RemoveTrait` | |
| New event graph (My Blueprint "+") | `URigVMBlueprint::AddModel(Name)` / `RemoveModel` / `RenameGraph` | public `UFUNCTION`s on `URigVMBlueprint` (protected on the interface) |
| New function, signature, access, local variables | function-library controller: `AddFunctionToLibrary`, `RenameFunction`, `RemoveFunctionFromLibrary`, `MarkFunctionAsPublic`, `SetNodeCategory/Keywords/Description/Color`; the function graph's controller: `AddExposedPin`…`SetExposedPinIndex`, `AddLocalVariable`… | |
| Member variables (My Blueprint) | `AddMemberVariable`, `RemoveMemberVariable`, `RenameMemberVariable`, `ChangeMemberVariableType`; `SetVariableCategory/Tooltip/ExposeToCinematics/Public…` through `FRigVMAssetInterfacePtr` (public there) | `AddMemberVariable` compiles synchronously and opens no transaction; the edit primitive brackets it |
| Hierarchy tab: new, delete, rename, duplicate, mirror, paste, drag-drop reparent, unparent | `AddBone/AddNull/AddControl/AddAnimationChannel/AddCurve/AddConnector/AddSocket`, `RemoveElement`, `RenameElement` / `SetDisplayName`, `SetParent` / `AddParent` / `RemoveAllParents` + `ReorderElement` | done in an interaction bracket with notifications suspended, then one `PropagateHierarchyFromBPToInstances`, as `HandleDeleteItem` / `ImportHierarchy` do |
| Import / Refresh hierarchy | `ImportBones(Skeleton, …, bReplace=false, bRemoveObsolete=false)` + `ImportCurvesFromSkeletalMesh` + `ImportSocketsFromSkeletalMesh` when all bones of the source are missing; otherwise per-bone `AddBone(…, Imported)` | same end state; behaviour test compares both |
| Element Details (pose, offset, shape, settings, spaces, channel hosts, socket colour) | `URigHierarchy::SetLocalTransform(bInitial)`, `SetControlOffsetTransform(bInitial)`, `SetControlShapeTransform(bInitial)`, `SetControlValue(Initial/Minimum/Maximum)`, `SetControlSettings`, `SetConnectorSettings`, `AddAvailableSpace` / `RemoveAvailableSpace` / `SetAvailableSpaceIndex` / `SetAvailableSpaceLabel`, `AddChannelHost` / `RemoveChannelHost`, `SetParentWeight(bInitial)`, metadata setters | then `PropagatePoseFromBPToInstances` (what `OnWrappedPropertyChangedChainEventImpl` does for initial edits) |
| Turn into Rig Module / Standalone Rig | `TurnIntoControlRigModule(false)` / `TurnIntoStandaloneRig()` | then the Body's `Hierarchy` is reconciled (the command adds a `Root` connector + socket when none exists) |
| Module Assembly panel | `AddModule`, `DeleteModule`, `RenameModule`, `ReparentModule`, `ReorderModule`, `SwapModuleClass`, `ConnectConnectorToElement(s)` (`bAutoResolveOtherConnectors=false`), `DisconnectConnector`, `SetConfigValueInModule`, `ResetConfigValueInModule`, `BindModuleVariable`, `UnBindModuleVariable` | inside `FModularRigControllerCompileBracketScope`, so `RecompileModularRig` runs once |
| Class Settings Details | generic property admission + settings hook (§4.2) | |
| Compile button | finalize (§5) | |

Not reproduced, by design: Mirror (`MirrorElements`, `MirrorModule`) and Duplicate. These are commands whose result
the document states explicitly; there is no mirror suggestion. Also not reproduced: copy/paste (the document is
the copy format), Python script export, selection, pin watches, debug and preview, the validation tab, Zero Controls /
"Set Initial from Current" (they read the debugged pose; the document writes the resulting initial values), and the
editor's auto-conversion to a module on the first connector.

### 4.2 Primitives, module and settings hook

Generic property admission covers the Class Settings, but several of the editor's follow-ups live in editor callbacks,
not in `PostEditChangeProperty`:

- `VMCompileSettings` → recompile;
- `VMRuntimeSettings` → `PropagateRuntimeSettingsFromBPToInstances`;
- `HierarchySettings` / `RigModuleSettings` → propagate the hierarchy;
- `DrawContainer` → `PropagateDrawInstructionsFromBPToInstances` (`ControlRigEditor.cpp:3783-3801`);
- `PreviewSkeletalMesh` → `SetPreviewMesh` (it also clears the source imports of a module).

The profile does not re-implement those per property. Finalize always runs the full compile and the propagations
(§5.1), and `PreviewSkeletalMesh` is routed through a `blueprint.member.setting.set.v1` settings adapter
(`IAssetDocumentBlueprintSettingsAdapter`, as AnimBlueprint's preview mesh). `ShapeLibraries` changes take
`PostEditChangeChainProperty` (the generic write fires it), which pushes them to the CDO.

Everything else needs a provider, following the IK Rig / Material pattern (one provider, read / edit / compile
primitives, Python orchestrates):

- **Module.** `AssetDocumentRig` gains Control Rig files (decision §8.1), provider id `rig`, owner `Rig`.
  New private dependencies: `ControlRig`, `ControlRigDeveloper`, `ControlRigEditor`, `RigVM`, `RigVMDeveloper`,
  `RigVMEditor` (all public headers). `UEAssetDoc.uplugin` gains `ControlRig` and `RigVM` explicitly (IKRig already
  depends on ControlRig, so nothing new is enabled).
- `rig.controlrig.read.v1` returns the normalized authored state of §2.1 as paged records:
  - header: rig type, parent class, module settings;
  - variables;
  - hierarchy elements with initial transforms, settings, values, spaces, metadata, and the source-skeleton reference
    pose for the omission rule;
  - per model and function: nodes with kind, struct / notation, name, position, title, colour, pins with CPP type,
    default (export text and typed JSON), expansion, binding and links, plus the new-node defaults of each used
    unit, read on a transient template node through the controller's `TemplateModel` / `AddUnitNode` on a transient
    graph;
  - functions with headers;
  - modules, connections and settings.
- `rig.controlrig.edit.v1` is an op list run with the three controllers inside one transaction and with
  `SetAutoVMRecompile(false)` (restored after; the Python export does the same):
  - `variable.*`;
  - `element.add/remove/rename/reorder/parents/transform/settings/value/spaces/hosts/metadata`, `hierarchy.import`;
  - `graph.add/remove/rename`;
  - `node.add/remove/rename/set` (position, title, colour, expansion, defaults, pin types, aggregate, traits,
    bindings, variable map), `link.add/break`;
  - `function.add/remove/rename/set` (public, colour, category, keywords, description; on a function or a collapse
    graph), `function.pin.add/remove/rename/type/index/default`, `local.add/remove/rename/type/default`,
    `node.remap` (function reference variable map), `function.dependents` (warn-only, §5.4);
  - `module.add/remove/rename/reparent/reorder/swap/config/bind/unbind`, `connect`, `disconnect`;
  - `rig.to_module/to_standalone`.

  Controllers report refusals through `ReportError` rather than return codes alone. The primitive enables reporting,
  captures the messages of each op and returns them with `/ops/<n>` paths that Python maps back (as Material).
  Controller undo flags are `bSetupUndoRedo = false` / `bSetupUndo = false` inside the primitive's own transaction,
  and the Python-print flags are false (§4.4).
  A unit that belongs to a template is placed by `AddUnitNode`, which resolves the template's wildcards and can pick
  another permutation (a float Exponential placed as the double one); when the placed struct is not the asked one,
  the op pins the asked permutation (`FullyResolveTemplateNode` with `FRigVMTemplate::FindPermutation`), as picking
  that unit in the palette does. `PinTypes` on a template node are resolved pin by pin, then, when a pin's type
  needs the others (an If whose branches and result change together), as one permutation of all the listed types.
  A bool pin's value is written `true` / `false` as the pin widget writes it; the compiler shares literals by text.
- `rig.controlrig.compile.v1` runs finalize (§5.1).
- `rig.controlrig.catalog.v1` serves the `list_types` kinds (§3.8).
- `rig.controlrig.suggest.v1` serves the read-only suggestions (§3.8).
- A derived-state rebuilder `AssetDocumentRig.ControlRig` handles rollback (§5.3).

Python: `profiles/control_rig.py` (regions, Meta, extract / diff), `profiles/control_rig_graph.py` (node keys, pin
paths, link planning, template resolution order), `profiles/control_rig_hierarchy.py` (element model, ordering,
imported-bone omission), `profiles/control_rig_modular.py`. Reused: `data_table_common.PropertiesRegion`, the
`SNAPSHOT_COVERED` preflight, the typed value codec, `blueprint.member.*` settings adapter plumbing. The shared K2
`graph.*` primitives are **not** reused: they address nodes by EdGraph GUID, which RigVM regenerates (§2.3), and
RigVM edits must go through `URigVMController`, not `UEdGraphSchema` (decision §8.3).

### 4.3 Create

- `RigType: "Rig"` / `"Module"`: run the factory with `ParentClass` (no dialog). The root EdGraph `Rig` exists after
  that. Then:
  - if `Body.Graphs.Rig` is absent, add `RigUnit_BeginExecution` named `RigUnit_BeginExecution` at (0, 0), which is
    what the editor's first open does (§2.2);
  - for `Module`, run `TurnIntoControlRigModule(false)` before the hierarchy ops, on the still-empty hierarchy;
  - then the Body regions.
- `RigType: "Modular"`: the factory with `ParentClass = UModularRig` adds module `Root` from `DefaultRootModule`
  (`/ControlRig/Modules/Modules56/Root`). A `Create` document with `Modules` reconciles from that, so the extract of the
  result equals the document. Without `Modules` the `Root` module stays.
- The `CreateControlRigFromSkeletalMeshOrSkeleton` action is the document `Create` + `Hierarchy` (imported bones and
  curves) + `PreviewSkeletalMesh` + `SourceHierarchyImport`/`SourceCurveImport`; a `Create` with `FactoryContext`
  does it in one step.

### 4.4 Ordering inside one edit call (Python plan)

1. `Properties` (preview mesh and shape libraries first: element checks use them).
2. Rig type conversion (`to_module` / `to_standalone`).
3. Variables (add / rename / type), before nodes that reference them. `AddMemberVariable` compiles synchronously, so
   new variables are added in one batch first.
4. Hierarchy:
   1. renames;
   2. adds in array order (parents first);
   3. reparent;
   4. reorder;
   5. settings / values / spaces / hosts / metadata;
   6. removals last (children first), so pins that reference removed elements are already rewritten by the document.
5. Functions: the dependents warning first (it reads the referencers before the rename), renames, adds (bodies
   empty), then the interface of every function: header (public, colour, metadata), exposed pins (renames, removes,
   adds, types, order, defaults) and local variables, so references and bodies see their final signature.
6. Graphs (models): add / rename.
7. Per graph, function body and collapse graph, in rounds (a round plans the scopes that exist; a collapse node added
   in one round gets its interface and body in the next):
   1. node renames, deletes, adds (adds in the graph's `Order`; a new node's `PinTypes` and aggregate pins right after
      its add, since the first operand turns it into an aggregate node, which goes last in the model);
   2. `PinTypes` of existing nodes;
   3. aggregate pins of existing nodes;
   4. defaults of type-deciding pins;
   5. links (breaks, then adds);
   6. remaining defaults (array sizes, sub-pins);
   7. bindings, traits, layout, comments, function reference variable maps, Entry / Return positions.
8. Removals of unlisted functions (after their references are gone), graphs and variables.
9. Modules: renames, adds (parents first), swap, reparent, reorder, config, bindings, connections (disconnects, then
   connects), deletes.
10. Compile (§5).

## 5. Compile, validation and rollback

### 5.1 What compile is

The editor's Compile button is `URigVMBlueprint::CompileBlueprint()` → `FKismetEditorUtilities::CompileBlueprint`. That
regenerates the class (member variables become properties), runs `OnPostCDOCompiled` → `RecompileVM()` (the
`URigVMCompiler`, which reports through `HandleReportFromCompiler` to the compile log and the EdGraph nodes), copies the
hierarchy, draw container and influences to the CDO, and propagates to instances. Hierarchy topology changes in the
editor also request a VM recompile (`FControlRigBaseEditor::OnHierarchyChanged`). A modular rig has no VM of its own:
its "compile" is `RecompileModularRig` (refresh connectors, copy the model to the CDO, request construction), plus the
class compile.

`rig.controlrig.compile.v1` (finalize, also the Graph core's derived-state hook) does:

1. Restore `SetAutoVMRecompile(true)`.
2. For a modular rig, `RecompileModularRig()`.
3. `FKismetEditorUtilities::CompileBlueprint` (with the results log).
4. `PropagateHierarchyFromBPToInstances`, `PropagateDrawInstructionsFromBPToInstances`,
   `PropagateRuntimeSettingsFromBPToInstances` (what the editor's Details callbacks do).
5. Collect the status:
   - `UBlueprint::Status`;
   - the compile log messages with their node (`@@` tokens resolved to model node paths through `ModelNodePath`);
   - the controller messages captured during the edit ops;
   - the editor's own warnings: "rig has ≤ 1 instruction" (`RigVMEditor.cpp:1108-1190`) and unresolved
     non-optional connectors of a modular rig;
   - shape names that do not resolve (the rig falls back to the default shape: `ControlRig.UnknownShape`, a
     warning).

### 5.2 Failure policy and status shape

As in Material and Blueprint: the before-status is read first (Extract never compiles). **New errors** fail the apply
with `ControlRig.CompileFailed` at the document path of the node (`/Body/Graphs/Rig/Nodes/Aim`, or
`/Body/Functions/<F>/Nodes/<n>`) and roll back. Errors the asset already had come back as `ControlRig.CompileError`
warnings, and warnings come back with the successful result (`ControlRig.CompileWarning`). Controller refusals during
the edit (a link the schema refuses, an event that already exists, a parent that would make a cycle, a module class
that is not a module) fail at their op's document path with the controller's message
(`ControlRig.LinkRefused`, `ControlRig.EventAlreadyExists`, `ControlRig.InvalidParent`, `ControlRig.ClassNotARigModule`,
…).

`compile` on Extract / Apply / Inspect uses the Blueprint shape:

```json
{"Status": "UpToDateWithWarnings",
 "Messages": [{"Severity": "Warning", "Message": "Node 'Aim' … deprecated …", "Graph": "Rig", "Node": "Aim",
               "Path": "/Body/Graphs/Rig/Nodes/Aim"}],
 "VMHash": 3735928559, "Instructions": 42}
```

`Status`: `UpToDate`, `UpToDateWithWarnings`, `Error`, `Dirty`, `Unknown`. `VMHash` (`URigVM::ComputeVMHash`) and the
instruction count are cheap fidelity facts.

### 5.3 Rollback

- **Existing asset**: Core's package snapshot restores the blueprint, the hierarchy subobject, the models, the EdGraphs
  and the generated class. The rebuilder then does what editor Undo does (`IControlRigAssetInterface::PostTransacted`):
  1. `OnRigVMRegistryChanged()` (public: `RefreshAllModels` + `RebuildGraphFromModel` + `RefreshAllNodes`);
  2. `RecompileModularRig()` for modular rigs;
  3. a full compile;
  4. `PropagateHierarchyFromBPToInstances`.

  Objects created by the apply (nodes, pins, links, graphs) are new authored objects that the snapshot removes. Graph
  nodes follow the existing rule of breaking pin links before removal (`FAssetDocumentPackageSnapshot`). Each editor
  graph caches its editor node per model node name (`URigVMEdGraph::ModelNodePathToEdNode`). The cache is transient,
  and the garbage collector does not see it. A restore puts the graph's `Nodes` back without updating the cache, so
  the cache can still name editor nodes the restore dropped. The next garbage collection frees those nodes, and the
  next notification for that name reads freed memory. The rebuilder therefore clears the caches, as does the compile
  before it binds editor graphs. It sends `GraphChanged`, the one notification that clears the cache, with the
  graph's nodes set aside so that they stay in the graph.
- **Created asset**: the factory rollback deletes the package (no registry outside the package, unlike MetaSound).
- **Undo history**: the controllers run with their undo flags off inside one primitive transaction. A successful apply
  clears the history that references the asset (§3.1). The `URigVMActionStack` (transient, per client) is not used, so
  editor Undo cannot replay half an AI apply. Behaviour test `ControlRig.UndoHistory`.

### 5.4 Dependents

- **Modular rigs using a module**: when a rig module is recompiled, the engine updates loaded modular rigs that use it,
  in memory (`OnModularDependencyVMCompiled`, `OnModularDependencyChanged`). The §3.2 dependents rule applies:
  - loaded referencing modular rigs with unsaved changes refuse the apply (`ControlRig.DependentHasUnsavedChanges`);
  - those the engine touched are reported (`ControlRig.DependentsUpdated`);
  - on rollback they are refreshed again after the module is restored.
- **AnimBlueprints and Level Sequences** reference controls, curves and variables by name. The editor does not update
  them when an element, variable or public function is renamed or removed. The profile follows the editor and warns
  with `ControlRig.DependentsAffected`, listing the asset-registry referencers and the renamed or removed names. It never
  edits other packages (same decision as IK Rig chains). Level Sequence documents (the Sequencer profile) store the rig class binding and
  control channel names; fixing them is that profile's job.
- **Rigs calling this asset's public functions**: the function header of a changed public function is refreshed on
  their next load or compile. They are reported as `ControlRig.DependentsAffected`. The controller itself would edit
  them: a function rename, removal, colour or pin change walks `URigVMBuildData`'s reference list (loading the
  referencing packages) and edits their reference nodes, outside the snapshot. During an apply the references held by
  other packages are unregistered and registered again afterwards under the identifier they had, so only this
  package changes; the referencing rigs are left as they are and named in the warning (decision 5).
- **Open editor**: a rig open in the Control Rig editor is closed for the apply and reopened on the result
  (Info `ControlRig.EditorReopened`). The editor listens to every model and hierarchy notification: an in-place
  rollback (the package snapshot re-serialized into the live objects) runs those listeners on half-restored graphs and
  asserts in the engine, and its debugged rig, graph and hierarchy tabs are only rebuilt by the editor itself. The
  Forwards Solve the editor's first open adds to an empty rig is accepted as an open-time change.

## 6. Fidelity acceptance plan

### 6.1 Content found in the UE 5.7 install

Enabled by default (no sandbox change), and Template content copied as the IK Rig recipe already does:

| # | Asset | Size | Why |
|---|---|---|---|
| 1 | `/ControlRig/StandardFunctionLibrary/StandardFunctionLibrary` | 12.0 MB | Function-library rig: many public functions, collapse nodes, local variables, templates |
| 2 | `/ControlRigSpline/SplineFunctionLibrary/SplineFunctionLibrary` | 7.5 MB | Second library, older format (no type tag) |
| 3 | `/ControlRig/Modules/Modules56/Root` | 5.8 MB | Rig module: connectors, sockets, construction graph |
| 4 | `/ControlRigModules/Modules56/Arm`, `Leg`, `Spine` | 14 / 10 / 14 MB | Large real modules: secondary connectors, config properties, external function references to #1 |
| 5 | `/ControlRigModules/Modules56/Constraint`, `Hinge`, `Wheel`, `Finger` | 0.6–3.3 MB | Small modules (fast tier) |
| 6 | `/ControlRigModules/Modules/Arm` (the pre-5.6 set) | 13.6 MB | Older module format (load patches) |
| 7 | `/Game/Characters/Mannequins/Rigs/CR_Mannequin_Body` (from `Templates/TemplateResources/High/Characters/Content`) | 17.1 MB | The canonical body rig: hundreds of controls, spaces, FBIK, forwards and backwards solve |
| 8 | `/Game/Characters/Mannequins/Rigs/CR_Mannequin_FootIK`, `CR_Mannequin_Procedural` | 0.5 / 2.6 MB | Mid-size rigs with variables used by AnimBPs |
| 9 | `CtrlRig_FPWarp` (TP_FirstPerson), `Ctrl_HandAdjusment` (Variant_Shooter), `CtrlRig_SportsCar`, `Offroad_CtrlRig` (Vehicles) | 0.25–1.5 MB | Small gameplay rigs; copied to `/Game/...` the same way |
| 10 | `/ControlRig/Controls/DefaultGizmoLibraryNormalized`, `DefaultGizmoLibrary` | 28 KB | Shape libraries (generic route; legacy class name redirect) |

Off-by-default plugins (sandbox only, decision §8.2):

| # | Asset | Plugin | Why |
|---|---|---|---|
| 11 | `/MoverExamples/Characters/Mannequins/Rigs/CR_Mannequin_Body`, `CR_Mannequin_BasicFootIK`, `CR_Mannequin_Procedural` | MoverExamples (already enabled for IK Rig) | Older serializations (one without the class tag) |
| 12 | `/AnimatorKit/UtilityRigs/CRU_*` (7 rigs, 150–950 KB), `/AnimatorKit/DeformerRigs/.../CRD_Lattice*` (2 MB), `/AnimatorKit/Locator/ShapeControlLibLocator` | AnimatorKit (beta, off) | Small, varied, current-format rigs: simulation, splines, morph drivers, custom shape library. Recommended corpus plugin |
| 13 | `/MetaHumanCharacter/Animation/CR_PlaceHolder` (21 KB), `/MetaHumanCharacter/Controls/CR_MH_Function_Library`, `/MetaHumanCharacter/Common/MetaHuman_ControlRig` (22 MB), `CRSL_MetaHuman_Gizmo` | MetaHumanCharacter (beta, heavy) | Optional; skipped with a note when absent, as for IK Rig |

No modular rig ships in the install (none carries `ControlRigType=ModularRig`). The tests build these with the editor's
own calls (`CreateNewControlRigAsset(…, true)`, `AddModule`, `ConnectConnectorToElements`, `SetConfigValueInModule`,
`BindModuleVariable`), then save, reload and use them as input:

- `CR_Modular_Biped` on `/ControlRigModules/Modules/Meshes/SKM_Biped_Template`: Root, Spine, Neck, Arm ×2, Leg ×2,
  Foot ×2, Finger ×5, connected with auto-resolve as the editor does, with one config override and one variable
  binding;
- `CR_Modular_Car` on `SKM_Car_Template` with Wheel ×4, Suspension, Piston;
- `CR_AllKinds`, built through the controllers: every node kind of §3.3, two event graphs, a user event, a function
  with locals, a collapse node, an aggregate node, a bound pin, a dispatch node with resolved types, every element
  type, multi-parent nulls with weights, every control type with limits, channels with hosts, and metadata.

### 6.2 What is compared

For each asset:

1. Duplicate it into `/Game` and save.
2. Extract.
3. Apply the extract as a new asset (`Create`).
4. Compare:
   - **document**: the two extracts are equal;
   - **model**, normalized: per graph and function, nodes by name with class, struct or notation, resolved pin
     types, defaults (semantic struct compare), positions, titles, colours, expansions, bindings, links as pin-path
     pairs; library public flags and headers. Ignored: EdGraph GUIDs, `URigVMGraph::LastStructureHash`, variant GUIDs;
   - **hierarchy**: elements in order with type, parents and weights, initial local transforms (1e-4), settings
     (`CompareScriptStruct` minus transient fields), values, spaces, metadata;
   - **modular**: modules (name, parent, class, overrides, bindings), connections;
   - **compile** (cheap): status and messages equal, **`ComputeVMHash` equal**, instruction count equal;
   - **runtime** (cheap, deterministic): instantiate both rigs on the preview mesh, run Construction then Forwards
     Solve once at the initial pose, and compare every bone's global transform (1e-3 cm / 1e-4 rad) and every curve
     value. Simulation rigs (`CRU_ChainSim`, …) run one frame only.
5. Re-apply: no-op.
6. Reload from disk and extract again: equal.

A second oracle, for diagnosing failures only: the engine's `GeneratePythonCommands` on both assets, sorted, shows the
first construction difference in the editor's own terms.

### 6.3 Free edits (document vs the editor's controller calls on two duplicates of #7, #4 and `CR_Modular_Biped`)

The same edit is made through a document and through the calls the editor's spawners, panels and Details callbacks
make (§4.1). Model, hierarchy, compile status, VM hash and runtime pose must come out equal:

- insert a `RigUnit_SetTransform` into the Forwards Solve chain; delete it again; reconnect a sub-pin
  (`Value.Translation`);
- change a pin value, an enum value and an element-key value; resize an array pin; add an aggregate operand;
- add a member variable, read it in the graph, rename it, change its type;
- add a user-defined event and a Run Event node; add a Construction graph;
- new function with two inputs and a local variable; reference it twice; rename it; make it public;
- hierarchy: add a control under `hand_r` with shape, colour and limits; add a space; reparent a null (keep global);
  reorder; rename a bone that graph pins reference; delete a control used by a pin (compile warning or error equals the
  editor's);
- add an animation channel to a control with a second host;
- swap the preview mesh to `SKM_Quinn_Simple`;
- turn a rig into a module and back;
- modular: add an Arm module, connect it, mirror-equivalent second arm written explicitly, change a config value,
  bind a variable, swap a module class, reorder, delete.

Undo test: human edit in the Control Rig editor, save, AI apply, Ctrl+Z behaviour per §3.1.

### 6.4 AI from scratch

- **Python test.** Starting from `SKM_Manny_Simple` only:
  1. Create from the mesh (`FactoryContext`), which imports its bones and curves.
  2. Write `CR_HeadAim` (§3.1, extended with foot IK through two-bone IK units and a Construction event that places
     the controls at their bones).
  3. Compile: no errors. The runtime check moves the head when `head_ctrl` moves.
  4. Write `CR_Modular_Biped` from the module catalog on `SKM_Biped_Template`, with connections from the
     `AutoConnect` suggestion.

  Error cases, each at its document path with rollback: unknown unit, ambiguous short name, unknown pin, type-mismatched
  link, second Forwards Solve, unsupported variable type, element parent cycle, unknown shape (warning), non-module
  class in `Modules`, unknown connector.
- **Black-box check** (MCP only + schema section):
  - "add an IK foot-lock control rig for this mesh";
  - "add a look-at control to CR_Mannequin_Body and wire it into Forwards Solve";
  - "build a modular rig for this biped mesh".

## 7. Risks, unknowns and scope

### 7.1 Risks and unknowns

| Risk | Effect | Answer |
|---|---|---|
| 5.7 is mid-migration: data on `*Legacy` Blueprint classes, logic in `IRigVMAssetInterface` / `IControlRigAssetInterface`, a new non-Blueprint editor behind `RigVM.UseNewEditor` | 5.8 may move Control Rig to a non-Blueprint asset class | Primitives are written against the interfaces where they are public (`FRigVMAssetInterfacePtr`, `FControlRigAssetInterfacePtr`) and touch `UControlRigBlueprint` only for `UFUNCTION`s that are protected on the interface (`AddModel`, `RecompileVM`, `GetOrCreateLocalFunctionLibrary`). The document shape never names Blueprint internals. Version-pinned matrix. |
| Unexported or protected members (`IControlRigAssetInterface` is not class-exported; `FControlRigBlueprintActions`, `FControlRigEditorModule`, `CreateEmptyGraphContentImpl` are private) | link errors | Only exported or virtual members are used. Forwards Solve on create is reproduced with `AddUnitNode` (one raw file + behaviour test against opening the editor). `CreateRootGraphIfRequired` runs inside the factory itself. |
| `AddMemberVariable` `check`s on unknown types | editor crash | Python validates against the `rigvm_type` catalog and the C++ primitive re-checks with `RigVMTypeUtils` before calling |
| `AddMemberVariable` compiles synchronously per call | slow applies with many variables | variables are batched first; each add is one compile (as in the editor) |
| Controllers refuse through `ReportError` and return values; some calls uniquify names silently (`AddModule`, `AddMemberVariable`, `AddModel`) | silent drift from the document | every add compares the returned name with the requested one (`ControlRig.NameTaken`); reporting captured per op |
| Hierarchy element rename rewrites pins in this asset (and the editor would keep the document's later writes) | double edits | renames run first (§4.4); later default writes are then no-ops when they agree |
| Node identity is the name; a rename without `RenamedFrom` is remove + add | links and values are re-created; same state | functional equivalence (design rules §3) |
| `CollapseNodes` / `PromoteCollapseNodeToFunctionReferenceNode` produce the end state through other calls than the document's adds | different internal names inside collapsed graphs | the document names every inner node, so the end state is the same. The behaviour test compares the editor's command result with the document's |
| Large assets (Arm module 14 MB, CR_Mannequin_Body 17 MB, MetaHuman 22 MB; StandardFunctionLibrary has hundreds of nodes) | extract size, apply time, result budgets | paged reads, `diff_only` defaults, compact element-key strings, imported-bone pose omission. Durations are recorded, not gated (as Niagara stress) |
| Auto-resolve and engine-side connector refresh in modular rigs need a debugged modular rig or CDO (`GetDebuggedModularRig`) | behaviour without an open editor may differ | connect with auto-resolve off; behaviour test against the Module Assembly panel's calls headlessly |
| Load-time patches and `HandlePackageDone` recompiles | an extract right after load already reflects patches | the profile reads the patched state; corpus tests extract twice and after reload |
| Pin default export text vs typed JSON | false diffs (`1` vs `1.000000`) | semantic compare by importing both into the pin's struct |
| Editor-only follow-ups after Class Settings edits | instances or CDO stale | finalize always propagates; behaviour test against the editor's Details path |
| Corpus without modular rigs; MetaHuman and AnimatorKit off by default | thin modular coverage | builder fixtures (§6.1); AnimatorKit recommended in the sandbox |

### 7.2 Scope and acceptance by area

| Area | Content | Acceptance |
|---|---|---|
| **Read** | `rig.controlrig.read/catalog` primitives; Python Extract / Inspect / Diff for all three rig types; `list_types` kinds; compile-status describer (reads `Status` + log, no compile); shape library Inspect | Extract stable (twice equal, equal after reload) on corpus #1–#10 (+#11–#13 when enabled) and `CR_AllKinds`; Inspect pins equal the registry |
| **Rig apply without graphs** | edit + compile primitives; create (Rig and Module, Forwards Solve on create); `Variables`; `Hierarchy` (all element types, spaces, hosts, metadata, imported-bone omission); `Module` + rig-type conversion; Class Settings and preview-mesh adapter; rollback rebuilder; undo rule | §6.2 on #3, #5 and a hierarchy-only fixture; hierarchy and variable free edits of §6.3; rollback on a refused op and a failed compile; Undo test |
| **Graphs** | `Graphs` (all node kinds except functions and collapse, aggregate nodes included: decision 10), links, defaults, bindings, traits, multiple models, events | §6.2 on #7–#9 (rigs without local functions) and `CR_AllKinds` minus functions; graph free edits; `ComputeVMHash` and the runtime pose equal |
| **Functions** | `Functions` (signature, locals, access, metadata), collapse nodes, function references (local and external) | §6.2 on #1, #2, #4, #6, #11, #12; function free edits; dependents warning for public functions |
| **Modular rigs** | `Modules`, `Connections`, `ModularSettings`, create from the factory with `Root`, dependents rule for modules | §6.2 on the modular fixtures; modular free edits; `AutoConnect` suggestion |
| **AI surface** | `rig.controlrig.suggest.v1`, `MCP/schemas/assetdocument/ControlRig.md`, tool descriptions, open-editor apply (close, apply, reopen; editor Undo tested) | §6.4 Python test and black-box check; matrix doc `asset-document-controlrig-matrix.md` |

## 8. Decisions

1. **Module / owner.** The Control Rig files live in `AssetDocumentRig` (owner `Rig`, provider `rig`).
   `UEAssetDoc.uplugin` lists `ControlRig` and `RigVM` explicitly; nothing new is enabled, since IKRig already depends
   on ControlRig.
2. **Corpus plugins.** The test sandboxes enable **AnimatorKit**. MoverExamples stays enabled and MetaHumanCharacter is
   optional (tests skip it with a note). The Template rigs (mannequin, first person, shooter, vehicles) are copied into
   `/Game` as a project created from the template has them. No modular rig ships, so modular fidelity relies on the
   builder fixtures.
3. **Graph primitives.** RigVM gets its own primitives (`rig.controlrig.*`), keyed by model node names and pin paths.
   The shared `graph.*` K2 primitives are not used.
4. **Shape libraries.** Shape libraries stay on the generic profile, with no exact profile. Inspect adds
   `ShapeLibrary` facts (default shape and resolved shapes). The rig profile checks `ShapeName` with a warning.
5. **Dependents** of renames and removals are warn-only: `ControlRig.DependentsAffected` names them, and other
   packages are never edited. The one exception is loaded modular rigs that the engine itself updates.
6. **Automatic tools.** Auto-Connect / auto-resolve is a read-only Inspect suggestion, never a document directive.
   Import Hierarchy (a `Create` with `FactoryContext` does it) and Mirror are not suggestions.
7. **Forwards Solve on create.** A `Create` without `Graphs` gets the `Forwards Solve` event. A `Create`
   with `Graphs.Rig` is exactly as written.
8. **Open editor.** Apply to a rig open in its editor closes the editor, applies and reopens it on the result
   (`ControlRig.EditorReopened`, §5.4), behind a behaviour test (`ControlRig.OpenEditorApply`).
9. **Deprecated units.** Existing nodes of deprecated units are preserved on read and round trip (a `Create` from an
   extract, or an apply to a rig whose graphs are empty, adds them with a `ControlRig.DeprecatedUnit` warning).
   Adding a new node of a deprecated unit to an existing rig is refused with the replacement named, as the editor's
   palette no longer offers it.
10. **Aggregate nodes** (an `Add` with a third operand) belong to the graphs, not to functions: `Aggregate` is authored with the
    node's "+" / "-" (`AddAggregatePin` / `RemoveAggregatePin`).
