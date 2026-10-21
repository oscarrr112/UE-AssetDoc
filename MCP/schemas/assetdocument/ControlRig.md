# Control Rig Profile

Classes: /Script/ControlRigDeveloper.ControlRigBlueprint

Exact profile for Control Rigs, rig modules and modular rigs (Python runtime, `rig_profile`). `extract_asset_document`, `inspect_asset_document_target` and `diff_asset_document` read the rig as the Control Rig editor shows it. Applying writes the hierarchy, variables, module settings, rig type, Class Settings, event graphs, local functions and a modular rig's modules and connections. Shape libraries (`ControlRigShapeLibrary`) use the generic profile, and their Inspect adds `ShapeLibrary: {DefaultShape, Shapes[{Name, Mesh, Transform?}]}`.

## Body

```jsonc
"Body": {
  "RigType": "Module",                       // "Rig" (omitted), "Module" or "Modular"
  "ParentClass": "/Script/MyGame.MyRig",     // omitted for /Script/ControlRig.ControlRig (ModularRig for Modular)
  "Module": {"Category": "Limbs", "Description": "..."},   // RigModuleSettings of a module (Name only when not the asset name)
  "Variables": [{"Name": "AimWeight", "Type": "float", "Default": 1.0, "Public": true, "ExposeToCinematics": true}],
  "Hierarchy": [
    {"Type": "Bone", "Name": "root", "Imported": true},
    {"Type": "Bone", "Name": "head", "Parent": "Bone(neck_01)", "Imported": true},
    {"Type": "Null", "Name": "head_space", "Parent": "Bone(neck_01)", "Transform": {"Translation": {"X": 0, "Y": 0, "Z": 12}}},
    {"Type": "Control", "Name": "head_ctrl", "Parent": "Null(head_space)", "Settings": {"ControlType": "Position", "ShapeName": "Sphere_Solid"},
     "Offset": {"Translation": {"X": 60, "Y": 0, "Z": 0}}, "Minimum": {"X": -30, "Y": 0, "Z": 0}, "Spaces": ["Bone(pelvis)"]},
    {"Type": "Null", "Name": "blend", "Parents": [{"Parent": "Bone(root)", "Weight": 1}, {"Parent": "Bone(pelvis)", "Weight": 0.5, "Label": "Pelvis"}]},
    {"Type": "Curve", "Name": "CTRL_aim", "Value": 0.25}
  ],
  "Graphs": {"Rig": {"Nodes": {
    "ForwardsSolve": {"Unit": "RigUnit_BeginExecution", "Position": [0, 0]},
    "GetCtrl": {"Unit": "RigUnit_GetTransform", "Position": [0, 200], "Defaults": {"Item": "Control(head_ctrl)"}},
    "Aim": {"Unit": "RigUnit_SetTransform", "Position": [400, 0], "Defaults": {"Item": "Bone(head)"},
            "Inputs": {"ExecutePin": "ForwardsSolve.ExecutePin", "Value.Translation": "GetCtrl.Transform.Translation"},
            "Bindings": {"Weight": "AimWeight"}},
    "Note": {"Comment": "Head follows head_ctrl", "Position": [-40, -80], "Size": [760, 560]}}}},
  "Functions": {"Align": {"Public": true, "Category": "Chains",
    "Inputs": [{"Name": "ExecuteContext", "Type": "/Script/RigVM.RigVMExecuteContext", "Direction": "IO"}, {"Name": "Amount", "Type": "float", "Default": 1.5}],
    "Outputs": [{"Name": "Result", "Type": "float"}], "LocalVariables": [{"Name": "Factor", "Type": "float", "Default": 2.0}],
    "Nodes": {"Times": {"Unit": "RigVMFunction_MathFloatMul", "Inputs": {"A": "Entry.Amount"}, "Bindings": {"B": "Factor"}}},
    "Entry": {"Position": [-400, 0]}, "Return": {"Position": [400, 0], "Inputs": {"ExecuteContext": "Entry.ExecuteContext", "Result": "Times.Result"}}}},
  "Modules": [{"Name": "Spine", "Class": "/ControlRigModules/Modules56/Spine.Spine_C", "Parent": "Root",
               "Config": {"Body Control Adjustment": 1.5}, "Bindings": {"FK IK Default": "UseIK"}}],   // Modular only
  "Connections": {"Spine/Spine Primary": "Socket(Root/spine_01_socket)"},                             // Modular only
  "ModularSettings": {"AutoResolve": false}                                                           // Modular only
}
```

- Graphs are keyed by model (`Rig` is the default model), nodes by their node name. Each node has one kind key: `Unit` (struct short name, or path when ambiguous; `Method` when not Execute), `Template` (notation, or the dispatch factory name such as `DISPATCH_RigVMDispatch_If`, with `PinTypes`: the pins that decide the others, which the template resolves from them), `Variable` (+ `Setter`), `Function` (local name, or `<host path>:<name>` for another rig's public function; `VariableMap`), `Collapse` (its graph: `Inputs`, `Outputs`, `Category`/`Keywords`/`Description`, `Entry`, `Return`, `Nodes`, `Order`), `Aggregate` (operand pins, next to the inner `Unit`/`Template`), `Reroute` (type), `Enum` (path), `InvokeEvent` (event name), `Comment` (+ `Size`, `FontSize`, `Bubble`, `ColorBubble`).
- Common node fields: `Position`, `Title` (renamable nodes such as Constant), `Color` (comments, collapse and function nodes), `Defaults` (pin path -> typed value; list only values that differ from the node's default), `Inputs` (input pin path -> `"<node>.<pin path>"`; sub-pins such as `Value.Translation` allowed; array elements `Items.2`), `Bindings` (pin -> variable), `Expanded`, `Traits` (`{name: {Type}}`), `Injections` (units in a top-level pin, as the pin menu's Add Visual Debug / Add Interpolate: `{pin: [{Unit, Input, Output, Name?, Defaults?}]}`; links still name the host pin).
- Element keys are `"Type(Name)"` strings (`"Bone(spine_01)"`; incomplete keys as pins hold them: `"Bone()"`, `"None(spine_01)"`; `""` no key); collections are lists of them. Hierarchy order is the editor's. Bones with `Imported: true` and no `Transform` sit at the reference pose of the rig's `SourceHierarchyImport`. Controls: `Settings` is sparse over a new control of that `ControlType`; `Value`, `Minimum`, `Maximum` are typed by it; `Offset`, `Shape` transforms; `Spaces`; animation channels carry `Hosts` and a plain `Parent` (their parent weight is fixed). Any element may carry `Metadata: {name: value}`.
- Pin, variable and element values use the typed value codec (numbers, enum entry names, `{X,Y,Z}`, transforms as `{Translation?, Rotation?, Scale3D?}` without identity parts).
- `Properties` holds the Class Settings (preview mesh, shape libraries, source imports, VM settings); Body-owned data (graphs, hierarchy, variables, modules) is never in `Properties`. With `diff_only` (the default) only values that differ from a new rig's are listed. Inspect of the class path (no asset) lists the Class Settings with their defaults, the Body sections and how to create each rig type.

## Apply

Authorable: `Properties` (Class Settings; `PreviewSkeletalMesh` is the editor's preview mesh picker), `RigType` `Rig` <-> `Module` ("Turn into Rig Module" / "Turn into Standalone Rig"), `Module`, `Variables`, the whole `Hierarchy` and `Graphs`. Edits are the editor's own (hierarchy tab, Details, My Blueprint, graph panel, Module Assembly), then the rig compiles as the Compile button does. Apply works with the rig open in the Control Rig editor: the editor is closed for the apply and reopened on the result (Info `ControlRig.EditorReopened`), so its graph, hierarchy tab and preview show the applied (or rolled-back) rig. The Forwards Solve the editor adds when it first opens an empty rig is not an unsaved change of yours.

- `Hierarchy` is the complete hierarchy in the editor's order, parents first: unlisted elements are deleted, new ones added, the rest written. Imported bones stay listed (`Imported: true`). `Reference` elements are procedural, never authored.
- `Variables` is the complete list in order. Renaming or retyping one updates the nodes and bindings that use it, as My Blueprint does.
- `"RenamedFrom": "OldName"` on a variable, element, graph, node, function, exposed pin or local variable renames it (keeps its references) instead of deleting and adding.
- `Graphs` (when present) is every event graph and node: nodes not listed are deleted, new ones added with exactly their key as name, the rest edited (links, values, bindings, pin types, operands, traits, layout). A different kind / unit on a key re-makes the node. Layout fields and `PinTypes` left out keep the rig's; an unpositioned new node goes right of the graph; a new node's pins start collapsed unless in `Expanded`. `Rig` is the default graph (never removed or renamed); other keys are event graphs ("+" in My Blueprint). A graph's `Order` is its node order (compile order, so the VM hash): new nodes are added last, in `Order`; existing nodes keep their place, as the editor has no command to reorder them. `Order` is therefore not compared (diff, verification): an `Order` that moves existing nodes warns `ControlRig.OrderKept` with the rig's order. Leave it out when order does not matter. `GraphOrder` (with several graphs) is the model order, which is the order events run in: new graphs are added in it, and existing ones keep their place in the same way. An event exists once per rig (`ControlRig.EventAlreadyExists`); a user event takes its name from `Defaults.EventName`.
- Without `Graphs`, a new or empty rig gets the Forwards Solve event the editor adds on first open.
- `Functions` (when present) is every local function: unlisted ones are removed, new ones added, the rest edited. A function has `Public`, `Category`, `Keywords`, `Description`, `Color`, `Inputs` / `Outputs` (exposed pins in order; `Direction: "IO"` marks the one execute pin; `Default` is the argument default), `LocalVariables`, `Nodes` and `Order` like a graph, and its interface nodes `Entry` (inputs as sources: `"Entry.Amount"`) and `Return` (outputs as `Inputs` / `Defaults`); `Color`, `Entry` and `Return` left out keep the rig's. Removing a function removes the nodes that call it, as the editor does. A function node whose function no longer exists is not compiled: the compile status reports it as an error (`ControlRig.DanglingFunctionReference`, with its path), so an apply that leaves one fails with `ControlRig.CompileFailed`.
- A collapse node's `Collapse` is its graph, edited the same way (pin values go on the node's `Defaults`; its `LocalVariables`, from older versions, are listed unchanged). A `Variable` node in a function may read a local variable or input.
- A `Function` node calls a local function, or another rig's public function as `"<package>.<asset>:<function>"` (`ControlRig.FunctionNotFound` lists that rig's public functions; a private one is refused, as the editor places only public functions of another rig); `VariableMap` maps the called function's member variables to this rig's.
- Changing a public function that other rigs call warns `ControlRig.DependentsAffected` with those rigs; they update on their next load or compile.
- New nodes of deprecated units are refused naming the replacement (`ControlRig.DeprecatedUnit`); existing ones are kept (a Create warns). A value reroute needs its whole `Value` input linked (`ControlRig.UnstableReroute`).
- The parent class and Rig <-> Modular cannot change.
- To copy a rig, use `duplicate_asset`: the Content Browser's Duplicate on UE 5.8.3 and later can drop the copy's reroutes and the links through them.
- `FactoryContext` on a Create (a Skeletal Mesh or Skeleton: `/Game/Characters/SKM_Hero` or `/Game/Characters/SKM_Hero.SKM_Hero`) is the editor's Create Control Rig (Create Modular Rig with `RigType: "Modular"`) on that mesh: its bones and curves are imported, preview mesh and import sources set. The `Hierarchy` then needs only what the rig adds.
- `validate_asset_document` runs the apply on a transient copy of the rig (a new transient rig for Create): pin, node, element, module and compile errors come back; nothing is saved.
- Unknown names list close ones (`Close: Bone(head), ...`); a missing pin also lists the node's pins.
- New compile errors fail the apply (`ControlRig.CompileFailed`); errors the rig already had and warnings come back as warnings (`ControlRig.CompileError`, `ControlRig.CompileWarning`).

Edit example (add a control under `hand_r`, rename a variable, insert a Set Transform after the Forwards Solve; the rest unchanged):

```jsonc
"Variables": [{"Name": "BlendWeight", "RenamedFrom": "Weight", "Type": "float", "Default": 0.5}, ...],
"Hierarchy": [..., {"Type": "Control", "Name": "hand_ik", "Parent": "Bone(hand_r)",
  "Settings": {"ControlType": "Float", "ShapeName": "Box_Thick"}, "Value": 0.5}],
"Graphs": {"Rig": {"Nodes": {...,
  "SetSpine": {"Unit": "RigUnit_SetTransform", "Position": [250, 300], "Defaults": {"Item": "Bone(spine_01)", "Space": "LocalSpace"},
               "Inputs": {"ExecutePin": "ForwardsSolve.ExecutePin"}, "Bindings": {"Weight": "BlendWeight"}},
  "Aim": {..., "Inputs": {"ExecutePin": "SetSpine.ExecutePin", ...}}}}}
```

Function edit example (rename `Align` to `AlignChain`, add a `Weight` input, call it once more):

```jsonc
"Functions": {"AlignChain": {"RenamedFrom": "Align", ..., "Inputs": [..., {"Name": "Weight", "Type": "float", "Default": 1.0}]}},
"Graphs": {"Rig": {"Nodes": {..., "Align2": {"Function": "AlignChain", "Position": [900, 0], "Defaults": {"Weight": 0.5}}}}}
```

## Modular rigs

The Module Assembly panel's edits (add, delete, rename, reparent, reorder, swap class, config, bindings, connect).

- `Modules` is the complete module tree in sibling order: `Name`, `Class` (rig module class; a different class is Swap Module Class), `Parent` (omitted: a root module), `Config` (config overrides by property name, complete; unlisted ones are reset), `Bindings` (module variable -> host variable or `<module>/<variable>`, complete), `RenamedFrom`. Unlisted modules are deleted.
- `Connections` is complete: `"<module>/<connector>"` -> element key (a list for array connectors). They are made without auto-resolve, module by module down the tree, primary connector first whatever order the map lists them in (a module's sockets exist once its own connectors resolve), so one apply builds every level of the rig exactly as listed (a whole biped in one apply). `ModularSettings.AutoResolve` is the panel's toggle.
- Create: `RigType: "Modular"` (the factory adds module `Root`) with `FactoryContext` the mesh (without it, a `PreviewSkeletalMesh` with `SourceHierarchyImport` set is imported the same way). If you know the targets, list every connection and apply once. Otherwise follow Inspect's `ModularRigSuggestions`: `AutoConnect` (what the editor's auto-resolve connects once a module's primary connector is connected; copy it into `Connections` to accept), `Candidates` (targets of connectors still open after that; primary connectors are always yours to pick, as where a module is dropped), `ConnectPrimaryFirst` (connectors waiting for their module's primary connector). Nothing is connected implicitly: automatic resolution is only this suggestion. `AutoConnect` is the editor's guess; a bone it picks above where the module attaches is listed in `Check` (an Arm on a Spine shoulder socket, with no Shoulder module, gets `Elbow Bone` = the clavicle, as the editor's drop does: name the elbow, or add the Shoulder module).
- An `Update` without `RigType` keeps the rig's type. A `Parent` cycle is `ControlRig.ModuleCycle` (the message names the chain).
- Editing a rig module that loaded modular rigs use is refused while one has unsaved changes (`ControlRig.DependentHasUnsavedChanges`); otherwise they take the new version when it compiles (`ControlRig.DependentsUpdated` warning).
- A modular rig's `Hierarchy` and `Graphs` come from its modules (`ControlRig.ModularRigRegionUnsupported`); the connection map arrays are derived from `Connections`.

## Inspect

The extract plus `ModularRigSuggestions` (modular rigs), `Counts`, `Pins` (per node path: `[{Name, Type, Direction, Wildcard?, Trait?}]`, the resolved pins), `GlobalTransforms` (initial global transforms per element key; on a modular rig also the controls, nulls and sockets the modules make, `Counts.module_elements`), `Shapes` (names from the rig's shape libraries, `Library.Shape` when there are several), `PublicFunctions`, `Events`, `Skipped` (components and procedural elements that are not documented). By default (without `detail: true`) `Pins` and `GlobalTransforms` are left out and the `Body` is an outline.

## Compile

Results carry `Compile: {Status, Messages[{Severity, Message, Graph?, Node?, Path?}], VMHash?, Instructions?}` read from the editor's last compile (nothing is compiled by reading). `Status` is `UpToDate`, `UpToDateWithWarnings`, `Error`, `Dirty`, or `BeingCreated`. A rig loaded but not compiled in this session reads `Dirty` (not an error; load-time messages are not kept). A node message's `Path` is the node's document path (`/Body/Graphs/Rig/Nodes/Aim`).

## Discovery

`list_types` kind `rigvm_node`: units and events (`Event`, `CanOnlyExistOnce`), dispatch factories and templates (`Notation`, `Permutations`), with `Pins[{Name, Type, Direction, Default?, Expand?}]` as a new node has them. Kind `rigvm_type`: value types for variables and pins (arrays are `TArray<...>` of them). Kind `controlrig_module`: rig modules, one compact row each (`Class` for `Modules`, `Category`, `Connectors` as `"Name (primary|optional, array)"`); `detail: true` adds descriptions, keywords and connector objects. Filter with `name` (substring, wildcard, or a keyword).

## Errors

`ControlRig.InvalidBody`, `UnknownBodyKey`, `DuplicateElement`, `DuplicateVariable`, `ElementNotFound`, `VariableNotFound`, `ParentAfterChild`, `InvalidParent`, `NameTaken`, `InvalidValue` (a value or settings field; the message names the field), `UnsupportedVariableType`, `ProceduralElement`, `UnknownMesh`, `RigTypeImmutable`, `ParentClassImmutable`, `ConversionRefused` (Turn into Rig Module keeps only imported bones, curves and connectors: list the module's `Hierarchy`), `GraphNotFound`, `NodeNotFound`, `PinNotFound`, `InvalidNodeName`, `UnknownUnit`, `AmbiguousUnit`, `UnknownTemplate`, `DeprecatedUnit`, `EventAlreadyExists`, `LinkRefused`, `BindingRefused`, `UnstableReroute`, `FunctionNotFound`, `InvalidFactoryContext`, `ModularRegionUnsupported`, `ModularRigRegionUnsupported`, `NotModular`, `DuplicateModule`, `ModuleNotFound`, `ModuleCycle`, `InvalidName`, `ClassNotFound`, `ClassNotARigModule`, `ConnectorNotFound`, `ConnectionRefused`, `ConfigNotFound`, `ConfigRefused`, `DependentHasUnsavedChanges`, `EditRefused` (with the editor's message), `CompileFailed`. Warnings: `ControlRig.GraphDiffers` (the editor normalized part of the graphs), `ControlRig.OrderKept`, `ControlRig.DependentsAffected`, `ControlRig.DependentsUpdated`. Each is reported at the document path it concerns; a failed apply changes nothing.
