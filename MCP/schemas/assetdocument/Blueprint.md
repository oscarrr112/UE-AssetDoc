# Blueprint Profile

Classes: /Script/Engine.Blueprint, /Script/Blutility.EditorUtilityBlueprint

Class `/Script/Engine.Blueprint` (ordinary Blueprints; Widget and Anim Blueprints have their own profiles). A Blueprint subclass without its own profile (Editor Utility, Gameplay Ability) uses this one and keeps its `Class`; Create of one is refused (`UBlueprint.SubclassCreateUnsupported`). Subclass handling: part `known_limits`. A Level Blueprint (inside its map) has no document; Level Script and profile-less RigVM Blueprints: `ClassResolve.ProfileRequired`. A new Blueprint is created from `ParentClass` and populated in the same apply (it compiles; a compile error fails the apply with `Graph.FinalizeFailed` and rolls it back, see the core guide's compile status). Every region is **omitted = keep, present = complete state, `[]`/`{}` = clear**; top-level `Properties` are the Class Settings and merge.

`Body` keys:

- `BlueprintType`: part `members` (Macro Library, Interface, Function Library).
- `ParentClass`: class path string (`"/Script/Engine.Actor"`, `"/Game/BP_Base.BP_Base_C"`). `ImplementedInterfaces`: array of interface class paths.
- `Variables`: `[{"Name": "Health", "Type": "float", "DefaultValue": 100.0, "Category": "Stats", "Tooltip": "", "InstanceEditable": true, "ReadOnly": false, "ExposeOnSpawn": false, "Private": false}]`. `Type` shorthands: `bool`, `int`, `int64`, `byte`, `float` (UE5 Float = double), `float32`, `string`, `name`, `text`, `<category>:<object path>` (`object:/Script/Engine.Actor`), `array:` / `set:` prefixes, or the full object form `{"Category", "SubCategory", "SubCategoryObject", "Container", "IsRef", "IsConst"}`. Omitted flags/Category/Tooltip are defaults; an omitted `DefaultValue` keeps the current value. More Details settings: part `members`. A user enum value (variable default, pin default) is the enumerator's display name, as extract writes it; the internal `NewEnumeratorN` is accepted too. Other values the generic property form takes (ClassDefaults, component and notify properties, DataTable rows) are exactly the display name.
- `Components`: `[{"Name": "Mesh", "Class": "/Script/Engine.StaticMeshComponent", "AttachTo": "Root", "Properties": {...}}, {"Name": "CharacterMovement", "Scope": "Native", "Properties": {...}}]`. Owned components are listed with `Class`; inherited and native components are overrides (`Scope`: `Inherited` or `Native`), and unlisted overrides are reset; the DefaultSceneRoot, `Category`, `Tooltip`: part `members`. Components are matched by `Name` (any order); `AttachTo` names a component (a native one by its component name or its subobject name); `AttachSocket` (needs `AttachTo`) is the Parent Socket, a socket or bone of that component's mesh (set after the component values, so the mesh may be set in the same document; an unknown name fails with `BlueprintMember.InvalidAttach` listing the sockets). Inspect lists `MeshComponents`, each with its mesh's `MaterialSlots`: the indices of `OverrideMaterials`.
- `ClassDefaults`: `{"InitialLifeSpan": 5.0}`: CDO property values; unlisted ones are reset to the parent value.
- `EventDispatchers`: `[{"Name": "OnDied", "Inputs": [{"Name": "Killer", "Type": "object:/Script/Engine.Actor"}], "Category": "", "Tooltip": ""}]`. A dispatcher is the delegate variable plus its signature graph. An omitted `Inputs` means no parameters.
- `Timelines`: part `members`.
- `UbergraphPages`, `FunctionGraphs`, `MacroGraphs`: graph regions, below.

### Graph regions

```json
{"Name": "AddHealth",
 "Signature": {"Inputs": [{"Name": "Amount", "Type": "float"}], "Outputs": [{"Name": "ReturnValue", "Type": "float"}], "Pure": false},
 "Nodes": [
   {"Id": "Branch", "Action": "K2:Branch", "PinOverrides": [{"Pin": "Condition", "DefaultValue": "true"}]},
   {"Id": "Print", "Action": "K2:PrintString", "PinOverrides": [{"Pin": "InString", "DefaultValue": "hi"}]},
   {"Id": "Died", "Action": "K2:CustomEvent", "Name": "Died", "Inputs": [{"Name": "Amount", "Type": "float"}]}],
 "Links": [{"From": "Entry:then", "To": "Branch:execute"}, {"From": "Branch:then", "To": "Print:execute"}]}
```

- Graph: `Name` (unique per region), `Signature` (function graphs: `Inputs`, `Outputs`, `Pure`, `Const`, `CallInEditor`, `Access`, `Category`, `Tooltip`, `Keywords`; more Details flags and macro metadata: part `graph_details`), `LocalVariables` (function graphs only, My Blueprint > Local Variables: `[{"Name": "Count", "Type": "int", "Default": "3", "Category": "...", "Tooltip": "..."}]`, in list order; a map is typed like a member variable, `{"Category": "int", "Container": "map", "ValueType": "float"}`; omitted = keep, `[]` = none; their nodes are `K2:Get:Count` / `K2:Set:Count` inside that function: a short `K2:Get` / `K2:Set` names the function's local variable first, as My Blueprint does, also when the document leaves `LocalVariables` out), `Nodes`, `Links`, optional `Subgraphs` (collapsed graphs), optional `Layout` (below). `NodeGuid`, `GraphGuid` and `Position` are generated metadata: extract writes them; apply keeps them when omitted.
- Collapsed graphs, further return nodes, parent function overrides, node identity, a new Blueprint's placeholder (`Ghost`) events, what Validate checks: part `graph_details`.
- Node: `Id` (graph-local identity, used by links; extract writes back each node's `Id` and `Action` spelling as last applied, kept with the node across save and reload: a node placed in the editor gets a generated Id; matching without `NodeGuid`: part `graph_details`), `Action` = the action key of the node as the editor's palette would place it (`K2:Branch`, `K2:PrintString`, `K2:PromotableOperator:Multiply`, `K2:Get:Health`, `K2:Set:Health`, `K2:CustomEvent` + `Name` (Details settings: part `graph_details`), `K2:Timeline` + `Name`, `K2:CallDelegate:OnDied`, `K2:AddDelegate:...`, `K2:ComponentBoundEvent:/Script/UMG.Button:OnClicked@OkButton`, ...). A wrong key fails with `Graph.ActionNotFound` and suggestions. `list_types` with `kind: "blueprint_node"` and `name` = an action key (or search words) lists the node's pins; `base` = a Blueprint (its own members and graphs) or a parent class (default Actor), `graph` = a function graph for its locals. `target` = a class: the functions callable on an object of it, its interfaces' included (as when dragging off a pin of that class). Without `target`, search words also find other classes' functions (Context Sensitive off), exact titles first; a row's `Target` is the class to link into `self`. `PinOverrides` set pin defaults; `Properties` set node properties; `Enabled`: part `graph_details`. Operator keys, Select and pin conversions (`PinTypes`): part `operators_select_arrays`. `Properties: {"Pure": false}` on a variable get of a bool / object variable is the editor's "Convert to Branch / Validated Get" (exec pins); on a function call it is "Show Exec pins" (`true` hides them); written only when it differs from the default. A class picked on a class pin (Create Widget, Add Component by Class, Spawn Actor `Class`) is set before the links, so the typed output can be wired. `Class` + `Member` (`{"Kind": "MemberRef", "OwnerClass", "Name"}`) is the long form for nodes without an action key.
- Layout (automatic layout, node sizes from extract's `layout`, validate's overlap warnings) and comments: part `layout`. `find_assets` with `uses`: part `find_assets_uses`.

### Links, keys and graph rules

- A call on another object (a component, a variable's object) uses the key of the class declaring the function, e.g. `K2:CallFunction:/Script/Engine.PrimitiveComponent:CreateDynamicMaterialInstance`, and gets its target from a link into its `self` pin (`{"From": "GetMesh:Mesh", "To": "MakeMid:self"}`). A subclass in the key also finds it (extract writes the declaring class).
- Link endpoints are `"NodeId:PinName"` strings or `{"Node", "Pin"}`; a pin is named by its name, ignoring letter case, spaces and underscores, or by its editor display name when that picks one pin (a user struct's Break / Make member pin `LaunchVelocity_16_<GUID>` by the member name `LaunchVelocity`; Get Data Table Row's `then` / `ReturnValue` as `RowFound` / `OutRow`); `Entry` and `Result` are the implicit function/macro terminator nodes.
- A link between pins the editor joins only through a conversion node (an `int` into a `bool` `Condition`) fails with `Graph.ConnectNeedsConversion` at the link: a document never inserts nodes it does not list. The message gives the conversion node to add and the two links through it; or link a pin of the right type. A link the editor refuses (Vector into Boolean) fails with `Graph.ConnectDisallowed`.
- `PinOverrides` entries: `{"Pin", "DefaultValue"}` (the pin's text form: `"true"`, `"1.5"`, an enum entry name), `{"Pin", "DefaultObject"}` for object and class pins (`GetComponentByClass` `ComponentClass`), `{"Pin", "DefaultTextValue"}` for Text pins (a `DefaultValue` on a Text pin fails the post-apply check). A "(by ref)" input (Blackboard `SetValueAsObject` `KeyName`) and an array, set or map input take no literal: `Graph.PinDefaultInvalid` names the node to link in instead (`K2:CallFunction:/Script/Engine.KismetSystemLibrary:MakeLiteralName` or another Make Literal, pin `Value`, output `ReturnValue`; `K2:MakeArray`), or link a variable. A value equal to the pin's own default is a no-op (extract leaves it out). Numbers compare as numbers (`"1"` = `"1.0"`). A function's input `Default` lives in `Signature.Inputs`; return node pin values (`Result` node `PinOverrides`) are kept per return node, and an omitted one is reset to the type's zero.
- Keys that are easy to get wrong: `K2:BeginPlay` is the Actor's Begin Play; events of other classes are `K2:Event:<declaring class>:<function>` (`K2:Event:/Script/Engine.ActorComponent:ReceiveBeginPlay`, `K2:Event:/Script/AIModule.BTTask_BlueprintBase:ReceiveExecuteAI`, `K2:Event:/Script/UMG.UserWidget:Tick`, `K2:Event:/Script/Engine.Controller:ReceivePossess`); a key the graph's class does not offer fails with `Graph.ActionRequiresContext`, naming this graph's key for it (`details.use`). Casts: `K2:DynamicCast:<class path>` (pins `Object`, output `As<class display name>`, e.g. `AsBP Arena Game Mode`; any spacing of it or `Result` names that output). Members of another Blueprint: `K2:Get:<class path>:<Variable>` / `K2:Set:...` and `K2:CallFunction:<class path>:<Function or custom event>`, with the object linked into `self`; the Blueprint's own functions: `K2:CallFunction:Self:<Name>`. The short forms (`K2:Get:Combat`, `K2:CallFunction:Self:CanAct`, `K2:AddDelegate:OnHit`) also name members inherited from parent Blueprints. Standard macros: `K2:MacroInstance:/Engine/EditorBlueprintResources/StandardMacros.StandardMacros:ForEachLoop` (pins `Exec`, `Array`, `LoopBody`, `Array Element`). Async action nodes: part `graph_details`. A variable Set node's output pin is `Output_Get`. Enhanced Input: the action event is `K2:EnhancedInputAction:<InputAction object path>` (exec `Triggered`, `Started`, `Completed`, ...; `ActionValue`, an Axis2D's split as `ActionValue_X` / `_Y`); the subsystem is `K2:GetSubsystemFromPC:/Script/EnhancedInput.EnhancedInputLocalPlayerSubsystem`, and its functions are declared on the interface: `K2:CallFunction:/Script/EnhancedInput.EnhancedInputSubsystemInterface:AddMappingContext` (`RemoveMappingContext`, `GetUserSettings`), with the subsystem linked into `self`.
- A present graph region lists every managed graph. Editor-generated empty graphs (empty EventGraph, construction script, interface stubs) need not be listed. A graph the editor cannot delete (EventGraph, construction script, an implemented interface function) that a present region leaves out is emptied: `"UbergraphPages": []` removes every EventGraph node.

Example (create):

```json
{
  "SchemaVersion": 1,
  "Target": "/Game/BP/BP_Enemy",
  "Class": "/Script/Engine.Blueprint",
  "Action": "Create",
  "Body": {
    "ParentClass": "/Script/Engine.Actor",
    "Variables": [{"Name": "Health", "Type": "float", "DefaultValue": 100.0}],
    "EventDispatchers": [{"Name": "OnDied"}],
    "UbergraphPages": [{"Name": "EventGraph", "Nodes": [
      {"Id": "Begin", "Action": "K2:BeginPlay"},
      {"Id": "Died", "Action": "K2:CallDelegate:OnDied"}],
      "Links": [{"From": "Begin:then", "To": "Died:execute"}]}]
  }
}
```
