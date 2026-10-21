# StateTree Profile

Classes: /Script/StateTreeModule.StateTree

Class `/Script/StateTreeModule.StateTree` (Python runtime, `state_tree_profile`). `Properties` must be `{}` (the schema settings are `Body.Schema.Properties`). Apply edits the editor data the way the StateTree editor does, then compiles like the editor's Compile button: a compile error fails the apply (`StateTree.CompileError` at the state / node it names) and the asset is rolled back; warnings come back with the successful result. Apply / Extract / Inspect results carry `Compile: {Status, Messages}` as for Blueprints.

Apply works with the tree open in the StateTree editor: the state tree view and Details show the applied (or rolled-back) tree.

```jsonc
"Body": {
  "Schema": {"Class": "/Script/GameplayStateTreeModule.StateTreeComponentSchema",
             "Properties": {"ContextActorClass": "/Script/Engine.Pawn"}},
  "Parameters": [{"Name": "WaitTime", "Type": "float", "Value": 2.0}],
  "GlobalTasksCompletion": "Any",
  "Evaluators": [{"Type": "/Script/MyGame.MySensorEvaluator", "Name": "Sensor"}],
  "GlobalTasks": [],
  "States": [
    {"Name": "Root", "Children": [
      {"Name": "Idle",
       "EnterConditions": [{"Type": "/Script/StateTreeModule.StateTreeCompareIntCondition", "Instance": {"Right": 2},
                            "Bindings": {"Left": "Evaluators/Sensor:Count"}}],
       "Tasks": [{"Type": "/Script/StateTreeModule.StateTreeDelayTask", "Name": "Wait",
                  "Instance": {"RandomDeviation": 0.5}, "Bindings": {"Duration": "Parameters:WaitTime"}}],
       "Transitions": [{"Trigger": "OnStateCompleted", "To": "Root/Walk"},
                       {"Trigger": "OnEvent", "Event": {"Tag": "Event.Stop"}, "To": "Succeeded",
                        "Priority": "High", "Delay": {"Duration": 0.5}}]},
      {"Name": "Walk", "Selection": "TryEnterState",
       "Transitions": [{"Trigger": "OnStateSucceeded", "To": "NextSelectableState"}]},
      {"Name": "CallSub", "Type": "Linked", "LinkedSubtree": "Sub", "ParameterValues": {"Speed": 2.5}},
      {"Name": "CallAsset", "Type": "LinkedAsset", "LinkedAsset": "/Game/AI/ST_Shared.ST_Shared",
       "ParameterBindings": {"Speed": "Parameters:WaitTime"}}]},
    {"Name": "Sub", "Type": "Subtree", "Parameters": [{"Name": "Speed", "Type": "float", "Value": 1.0}]}]}
```

Omission: an omitted Body key keeps what the asset has. In a listed state or node, an omitted state field (`Description`, `Tag`, `Selection`, `Enabled`, `Weight`, `TickRate`, ...), node flag (`Enabled`, `Indent`, `Operand`, `EvaluationMode`, a task's `ConsideredForCompletion` where the editor lets it be edited) or `Node` / `Instance` value is a new state's / node's default: deleting it from an extracted document resets it, and Diff reports it. A state's kind (`Type`, `LinkedSubtree`, `LinkedAsset`), state `Parameters` / `ParameterValues`, node `Bindings` and transition `Conditions` keep what the asset has when omitted (Diff notes an omitted `Type` of a non-`State` state: `StateTree.OmittedKept`, Info). A linked state's `Selection` is always `TryEnterState`. A present list (`Parameters`, `States`, `Children`, `Evaluators`, `GlobalTasks`, `EnterConditions`, `Tasks`, `Considerations`, `Transitions`, `Conditions`) is the complete list: entries not listed are removed. A state without `Children` keeps its children. Transitions are complete values (omitted fields take the editor defaults) except `Conditions` (omitted = keep).

Body:

- `Schema`: `Class` is required to create (the New State Tree dialog's schema; `StateTree.SchemaRequired`, `StateTree.SchemaNotFound`); `list_types` with `kind: "class"` and `base: "/Script/StateTreeModule.StateTreeSchema"` lists them. The class cannot change on an existing tree (`StateTree.SchemaClassImmutable`). `Properties` are the schema's settings (sparse). A schema setting is applied as its Details row does (PostEditChange): setting `ContextActorClass` changes the `Context/Actor` type, so bindings to that class's properties (`"Context/Actor:BaseEyeHeight"`, a Blueprint variable of the actor class) resolve in the same apply.
- `Parameters[]`: `{"Name", "Type", "Value"?, "RenamedFrom"?}`; `Type` uses the Blueprint variable type forms (`"float"`, `"int"`, `"bool"`, `"string"`, `"name"`, `{"Category": "struct", "Object": "/Script/CoreUObject.Vector"}`, containers ...). Matched by name; `RenamedFrom` keeps the parameter and its bindings.
- `GlobalTasksCompletion`: `Any` | `All`.
- `Evaluators[]`, `GlobalTasks[]`: nodes (below).
- `States[]`: the root states (`Root`, `Subtree` states).

State fields: `Name` (no `/` or `:`; unique among siblings), `RenamedFrom`, `Description`, `Tag` (gameplay tag), `Type` (`State` | `Group` | `Linked` | `LinkedAsset` | `Subtree`), `Selection` (`None`, `TryEnterState`, `TrySelectChildrenInOrder`, `TrySelectChildrenAtRandom`, `TrySelectChildrenWithHighestUtility`, `TrySelectChildrenAtRandomWeightedByUtility`, `TryFollowTransitions`), `TasksCompletion` (`Any` | `All`), `Enabled`, `Weight`, `TickRate` (number, `null` = none), `CheckPrerequisites`, `EnterEvent` (`{"Tag", "Payload"?, "Consume"?}` or `null`), `LinkedSubtree` (path of a `Subtree` state, `Linked` only), `LinkedAsset` (StateTree object path, `LinkedAsset` only), `Parameters` (state parameters; not on Linked / LinkedAsset states, whose layout comes from the linked tree), `ParameterValues` (Linked / LinkedAsset: overridden values by name), `ParameterBindings`, `EnterConditions`, `Tasks`, `Considerations`, `Transitions`, `Children`. Linked states always use `TryEnterState`; `Group` and linked states have no tasks.

Nodes: `{"Type", "Name"?, "Enabled"?, "ConsideredForCompletion"?, "EvaluationMode"? (conditions: `Evaluated` | `ForcedTrue` | `ForcedFalse`), "Indent"?, "Operand"? (`And` | `Or`, conditions / considerations), "Node"? {...}, "Instance"? {...}, "Bindings"? {...}}`. `Type` is a node struct (`/Script/Module.Struct`, or its name) or a Blueprint node class (`/Game/AI/BTT_Run.BTT_Run_C`); `list_types` with the node base (`/Script/StateTreeModule.StateTreeTaskBase`, `...EvaluatorBase`, `...ConditionBase`, `...ConsiderationBase`) and `schema` (the tree's schema class) lists what the node picker offers for that schema (without `schema`: every non-hidden node struct of the base, e.g. also other schemas' nodes); hidden abstract bases and test-module types are never listed. Inspect's `TreeDetails.NodeTypes` and `Allows` say which lists and bases the schema accepts. `inspect_asset_document_target` on a node struct path shows `Fields` (what `Node` takes), `InstanceType` and `InstanceFields` (what `Instance` takes; `Usage`: `Parameter` = set or bind, `Input` = bind it), and `Outputs` (bindable sources, not authored); for example `/Script/StateTreeModule.StateTreeCompareBoolCondition`: `bInvert`; `bLeft` (Input), `bRight`. `Node` holds the node struct's values, `Instance` the instance data values (sparse; output and context properties are not set by value). A new node, or a node whose `Type` changes, must use a type the node picker offers (not hidden, abstract or deprecated; otherwise `StateTree.UnknownType`); an existing node keeps its type even if that type is hidden.

Transitions: `{"Trigger", "Event"?, "To", "Priority"?, "Delay"?, "Enabled"?, "Conditions"?}`. `Trigger`: `OnStateCompleted` (default), `OnStateSucceeded`, `OnStateFailed`, `OnTick`, `OnEvent` (needs `Event`). `To`: a state path (`Root/Walk`) or `None`, `Succeeded`, `Failed`, `NextState`, `NextSelectableState`. `Priority`: `Low`, `Normal`, `Medium`, `High`, `Critical`. `Delay`: `{"Duration", "Variance"?}` (not on completion triggers).

Bindings (`Bindings` of a node, `ParameterBindings` of a state): `{"<target property path>": "<source>:<property path>"}`. `"Node:<path>"` targets a property of the node struct instead of its instance data. A source without `:<path>` binds the whole struct (`"Context/Actor"`). A property function: `{"<target>": {"Function": "/Script/StateTreeModule.StateTreeAddFloatPropertyFunction", "Instance": {...}, "Bindings": {"Left": "Parameters:WaitTime"}}}`. Sources: `Parameters`, `Context/<Name>`, `Evaluators/<key>`, `GlobalTasks/<key>`, `<StatePath>/Parameters`, `<StatePath>/EnterEvent`, `<StatePath>/{Tasks|EnterConditions|Considerations}/<key>`, `<StatePath>/Transitions/<i>/{Event|Conditions/<key>}` (key = node `Name` or list index). A node's `Bindings` is its complete binding list; omitted = keep. Errors: `StateTree.BindingSourceNotFound`, `StateTree.BindingSourceNotAccessible`, `StateTree.BindingTypeMismatch`, `StateTree.InvalidBinding` at the binding.

Identity: states by path or `RenamedFrom` (old name under the same parent, or the old full path to move a state); a state keeps its ID, so transitions to it follow a rename or move. Nodes by `Name`, otherwise the k-th unnamed node of the same `Type` in the same list; a matched node keeps its ID, values and bindings. Transitions by position. Re-applying an extraction is a no-op.

Not authored: `OnDelegate` transitions' delegate listener, output bindings, editor colors / themes / expansion state, and schema settings the value codec cannot write (Inspect `TreeDetails.Schema.UnsupportedProperties`). `inspect_asset_document_target` on a tree returns `TreeDetails` (schema, context data with their binding source names, which node lists the schema allows, compile state, and the `list_types` bases of each node list).
