# EnvQuery Profile

Classes: /Script/AIModule.EnvQuery

An Environment Query (EQS) as the Environment Query editor shows it: `Root`, the options wired to it (one generator each, with its tests) and comment boxes. The runtime query is rebuilt from this graph, as the editor does: connected options run left to right (their `Position` X), each runs its *enabled* tests in list order. Apply uses the editor's own operations (palette drop, Add Test, Delete, test drag-drop, the test checkbox, wires, the Details panel, Create Comment) and then rebuilds and validates the query. Create uses the editor's Environment Query factory. Apply works with the query open in the Environment Query editor: its graph shows the applied (or, after a failure, rolled-back) query. Opening the editor recomputes the test weights its graph displays, which marks the asset unsaved; apply accepts that until you edit the query yourself (then save first). `Properties` stays `{}`.

```jsonc
"Body": {
  "Root": {"Position": [0, 0]},
  "Options": [                                              // connected options in run order
    {"Key": "Grid", "Position": [0, 160],
     "Generator": {"Class": "SimpleGrid", "Properties": {
       "GridSize": {"DefaultValue": 800.0,                  // bound to the query param "Radius"
                    "DataBinding": {"Class": "/Script/AIModule.AIDataProvider_QueryParams", "Properties": {"ParamName": "Radius"}},
                    "DataField": "FloatValue"},
       "ProjectionData": {"TraceMode": "None"}}},
     "Tests": [
       {"Key": "Near", "Class": "Distance", "Properties": {"TestPurpose": "Score", "ScoringEquation": "InverseLinear"}},
       {"Key": "Visible", "Class": "Trace", "Properties": {"BoolValue": {"DefaultValue": false}}},
       {"Key": "Facing", "Class": "Dot", "Enabled": false, "Comment": "try later"}]},
    {"Key": "Fallback", "Position": [320, 160],
     "Generator": {"Class": "ActorsOfClass", "Properties": {"SearchedActorClass": "/Script/Engine.TargetPoint"}},
     "Tests": []},
    {"Key": "Spare", "Generator": {"Class": "Donut"}, "Connected": false, "Position": [640, 160]}
  ],
  "Comments": {"Note": {"Text": "cover search", "Position": [-64, -160], "Size": [960, 480]}}
}
```

- `Options`, an option's `Tests` and `Comments` omitted = keep; present = complete (unlisted ones are deleted, as the editor's Delete does). `Root` omitted = keep.
- Keys are object names (letters, digits, `_`, `-`; unique in the query): Extract writes the editor's names (`EnvQueryOption_1`, `EnvQueryTest_Distance_0`); a new keyed entry gets that name. An apply also records each option / test key on its graph node, so the editor's Undo / Redo (which renames the query objects) keeps the keys; a node the editor added and no apply has saved since keeps its generated key only until such an Undo. An entry without `Key` takes the first unused option / test of the same class (in order), else it is new. A test listed under another option is moved there (the editor's drag-drop).
- `Class`: engine generators and tests by short name without `EnvQueryGenerator_` / `EnvQueryTest_` (`SimpleGrid`, `ActorsOfClass`, `Donut`, `OnCircle`, `Cone`, `PathingGrid`, `CurrentLocation`, `PerceivedActors`, `Composite`; `Distance`, `Dot`, `Trace`, `Pathfinding`, `PathfindingBatch`, `Project`, `Overlap`, `GameplayTags`, `Volume`; deprecated classes such as `Random` are not offered), other classes (Blueprint generators, plugin tests) by class path. `list_types` kinds `env_query_generator` / `env_query_test` list the palette and Add Test classes with their item types and new-node Details; contexts (`DistanceTo`, `GenerateAround`, ...) are classes: `list_types` kind `class`, base `/Script/AIModule.EnvQueryContext`. An unknown class fails with `EnvQuery.UnknownClass` and close names. A different generator `Class` on a key replaces the option (its tests move to the new one).
- `Properties` are the Details values of the generator / test, complete: an omitted property has the value of a new node of that class (Extract lists only differences; `diff_only: false` lists all). Unknown names fail with `EnvQuery.UnknownProperty`. Value forms as in the Generic section: enums by name, `FAIDataProvider*Value` as `{"DefaultValue": ...}` plus an optional `DataBinding` (a query param: `AIDataProvider_QueryParams` with `ParamName`, `DataField` `"FloatValue"` / `"IntValue"` / `"BoolValue"`), contexts and classes by class path, instanced objects (`Composite`'s `Generators`) as `{"Class", "Properties"}`. Trace settings (`ProjectionData`, `TraceData`) carry what their Details show (`TraceMode`, `TraceChannel`, `TraceShape`, extents, `ProjectDown` / `ProjectUp`, ...); `VersionNum`, `SerializedChannel` and the `bCan*` flags are not authorable.
- `Position` of a connected option sets its run order: X must increase along `Options` (`EnvQuery.OptionOrder`; equal X gives `EnvQuery.OptionOrderTie`). Omitted: kept when it fits the order, else placed between its neighbours (or 300 right of the previous one). `Connected` omitted = `true`; `false` keeps the option in the graph without running it.
- `Enabled` omitted = `true` (a disabled test stays in the graph, the runtime skips it). `Comment` / `CommentVisible`: the node's comment bubble. Comment boxes: `Text`, `Position`, `Size`, optional `Color`, `FontSize`, `BubbleVisible`, `ColorBubble`, `MoveMode`, `Details`.
- Validation after each apply: errors the query did not have before fail the apply and roll it back (`EnvQuery.ValidationFailed`: a Composite whose generators make different item types, a missing class); errors it already had and warnings come back as `EnvQuery.ValidationError` / `EnvQuery.ValidationWarning` (a test that does not support its generator's item type is dropped at runtime, nothing wired to Root, deprecated classes). When the query's params change and other assets use it (Behavior Tree *Run EQS Query*, services, Blueprints), `EnvQuery.DependentParams` names them: their `QueryConfig` still lists the old params.
- Extract and Inspect carry `Compile: {Status, Messages}`. Inspect adds `Params` (`[{Name, Type}]`, what a *Run EQS Query* `QueryConfig` binds), `NodeFacts` (per key: class, item type, wired, run index, enabled, test support, weight), `Dependents` and `OpenInEditor`.
- A Behavior Tree runs the query with a *Run EQS Query* task (`BTTask_RunEQSQuery`, `EQSRequest.QueryTemplate`, result in `BlackboardKey`) or an EQS service. The BehaviorTree profile does not create these nodes yet: a new one fails with `MissingBlackboardKey` at `.../EQSRequest/EQSQueryBlackboardKey/Key` (a key the node reads only in Blackboard-key mode). Add the node in the Behavior Tree editor; the rest of the tree can still be applied.
