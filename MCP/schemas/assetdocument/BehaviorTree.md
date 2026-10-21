# BehaviorTree Profile

Classes: /Script/AIModule.BehaviorTree

Class `/Script/AIModule.BehaviorTree` (Python runtime). `Properties` holds reflected `UBehaviorTree` properties (usually `{}`). `AssetType`, if present, must be `"GenericAsset"`.

Apply works with the tree open in the Behavior Tree editor: the editor is closed for the apply and reopened on the result (Info `BehaviorTree.EditorReopened`), because the apply replaces the graph it shows. Opening a tree saved with an older graph version upgrades its graph, which marks the asset unsaved; apply accepts that until you edit the tree yourself (then save first).

`Body` keys (unknown keys fail with `UnknownBodyKey`):

- `BlackboardAsset`: optional. Omitted = keep the current Blackboard; `null` = clear it; otherwise an object path string `"/Game/AI/BB_Enemy.BB_Enemy"` or `{"Kind": "AssetRef", "Path": "/Game/AI/BB_Enemy.BB_Enemy"}`. A path that is not an existing BlackboardData fails with `BehaviorTree.BlackboardNotFound` at `/Body/BlackboardAsset` (create the Blackboard first).
- `Tree`: optional. Omitted = keep the current graph unchanged. When present it is the complete authoritative tree: the whole editor graph is rebuilt from it, so nodes, decorators, services, and comments that are not listed are removed. `Tree: null` is invalid.

`Tree` fields:

- `Root` (required): the root composite node object, or `{}` for an empty tree. The root must be a composite class (`InvalidBehaviorTreeNodeClass` otherwise).
- `GraphGuid` (optional): 32-hex GUID of the editor graph. Omit it (the existing GUID is reused) or copy it from extract; a different GUID for an existing graph fails with `BehaviorTreeGraphGuidReplacementRejected`.
- `Comments` (optional array): comment boxes.

Every node, decorator, service, comment, and bound-graph node has an `Id` that must be a non-zero 32-character hex GUID (for example `"0000000000000000000000000000000A"`), unique across the whole tree (`InvalidBehaviorTreeNodeGuid`, `DuplicateBehaviorTreeNodeGuid`). Reuse the ids from extract to keep node identity.

Main node (root and every entry of `Children`), allowed fields `Id`, `Class`, `Properties`, `Editor`, `Children`, `Decorators`, `Services`:

- `Class` (required): exact node class path, for example `/Script/AIModule.BTComposite_Sequence`, `/Script/AIModule.BTComposite_Selector`, `/Script/AIModule.BTTask_Wait`, `/Script/AIModule.BTTask_MoveTo`, or a Blueprint task `/Game/AI/BTT_Attack.BTT_Attack_C`. Tasks cannot have `Children` (`InvalidBehaviorTreeParentClass`).
- `Children`: array of nested main node objects (not ids), in execution order.
- `Decorators`: array of decorator objects `{Id, Class, Properties?, Editor?}`, or composite decorators (below).
- `Services`: array of service objects `{Id, Class, Properties?, Editor?}`.
- `Properties`: the node class's reflected properties. Omitted properties are reset to the class defaults (this is a full replacement, not a sparse patch). Unknown names fail with `UnknownProperty`.
- `Editor` (optional): `{"Position": {"X": int, "Y": int}, "NodeComment": string, "bCommentBubblePinned": bool, "bCommentBubbleVisible": bool}`. If `Position` is omitted a layout is generated from the parse order. Children of an ordinary composite must have strictly increasing `Editor.Position.X` (`BehaviorTreeLayoutOrderConflict`, `DuplicateBehaviorTreeSiblingCoordinate`), because Unreal orders children by X.

Node property value shapes:

- `FBlackboardKeySelector`: `{"Key": "TargetActor"}` only (`"None"` or `""` = unset). Fields like `SelectedKeyName`, `AllowedTypes`, `SelectedKeyID` are derived and rejected (`NonAuthoredProperty`). The key must exist in the effective Blackboard (`UnknownBlackboardKey`, naming close keys) and match the node's key filter (`IncompatibleBlackboardKeyType`); a selector with a key requires a `BlackboardAsset` (`MissingBehaviorTreeBlackboard`).
- `FValueOrBBKey_*` (for example `BTTask_Wait.WaitTime`): `{"DefaultValue": 2.0, "Key": "None"}`; set `Key` to bind a Blackboard key instead (`UnknownValueOrBlackboardKey`, `IncompatibleValueOrBlackboardKeyType`).
- Enums: enumerator name string (or its integer value). Numbers: JSON numbers. Structs: object; omitted fields take defaults. Arrays/sets/maps: replace the whole container.
- Object/class references: path string or `{"Kind": "AssetRef" | "ClassRef", "Path": "..."}`; `null` or `""` clears.
- Instanced UObject: `{"Class": "...", "Properties": {...}}`. `FInstancedStruct`: `{"Struct": "/Script/Module.Struct", "Properties": {...}}`.

Composite decorator (logic graph of decorator tests):

```json
{
  "Id": "00000000000000000000000000000020",
  "Kind": "Composite",
  "Properties": { "CompositeName": "Gate", "bShowOperations": true },
  "BoundGraph": {
    "GraphGuid": "00000000000000000000000000000021",
    "Nodes": [
      { "Id": "00000000000000000000000000000022", "Kind": "Sink" },
      { "Id": "00000000000000000000000000000023", "Kind": "And" },
      { "Id": "00000000000000000000000000000024", "Kind": "Test", "Class": "/Script/AIModule.BTDecorator_Blackboard", "Properties": { "BlackboardKey": { "Key": "TargetActor" } } },
      { "Id": "00000000000000000000000000000025", "Kind": "Test", "Class": "/Script/AIModule.BTDecorator_Blackboard", "Properties": { "BlackboardKey": { "Key": "bCanSeeTarget" } } }
    ],
    "Links": [
      { "From": "00000000000000000000000000000023", "To": "00000000000000000000000000000022", "ToInput": 0 },
      { "From": "00000000000000000000000000000024", "To": "00000000000000000000000000000023", "ToInput": 0 },
      { "From": "00000000000000000000000000000025", "To": "00000000000000000000000000000023", "ToInput": 1 }
    ]
  }
}
```

Bound graph rules: node `Kind` is `Sink`, `Test`, `And`, `Or`, or `Not`; only `Test` nodes have `Class`/`Properties`; exactly one `Sink`; every node reaches the Sink through exactly one outgoing link; `And`/`Or` need at least 2 contiguous inputs, `Not` and `Sink` exactly 1, `Test` 0; no cycles. Violations use codes such as `InvalidBehaviorTreeDecoratorSink`, `InvalidBehaviorTreeDecoratorArity`, `UnreachableBehaviorTreeDecoratorNode`, `BehaviorTreeDecoratorGraphCycle`, `DanglingBehaviorTreeDecoratorLink`.

Comment fields: `Id` (required), `Text`, `Position` `{X, Y}`, `Size` `{Width, Height}` (positive), `Color` `{R, G, B, A}` floats, `CommentDepth`, `FontSize` (>= 1), `MoveMode` (`GroupMovement` or `NoGroupMovement`), `NodeDetails`, `bCommentBubblePinned`, `bCommentBubbleVisible`, `bCommentBubbleVisible_InDetailsPanel`, `bColorCommentBubble`. For a new comment omitted fields get defaults; for an existing comment `Id`, omitted fields keep their current values.

Example:

```json
{
  "SchemaVersion": 1,
  "Target": "/Game/AI/BT_Enemy",
  "Class": "/Script/AIModule.BehaviorTree",
  "Action": "CreateOrUpdate",
  "Definitions": {},
  "Properties": {},
  "Body": {
    "BlackboardAsset": { "Kind": "AssetRef", "Path": "/Game/AI/BB_Enemy.BB_Enemy" },
    "Tree": {
      "Root": {
        "Id": "00000000000000000000000000000001",
        "Class": "/Script/AIModule.BTComposite_Selector",
        "Services": [
          { "Id": "00000000000000000000000000000002", "Class": "/Script/AIModule.BTService_DefaultFocus" }
        ],
        "Children": [
          {
            "Id": "00000000000000000000000000000003",
            "Class": "/Script/AIModule.BTComposite_Sequence",
            "Editor": { "Position": { "X": 0, "Y": 200 } },
            "Decorators": [
              {
                "Id": "00000000000000000000000000000004",
                "Class": "/Script/AIModule.BTDecorator_Blackboard",
                "Properties": { "BlackboardKey": { "Key": "TargetActor" } }
              }
            ],
            "Children": [
              {
                "Id": "00000000000000000000000000000005",
                "Class": "/Script/AIModule.BTTask_MoveTo",
                "Editor": { "Position": { "X": 0, "Y": 400 } },
                "Properties": { "BlackboardKey": { "Key": "TargetActor" } }
              }
            ]
          },
          {
            "Id": "00000000000000000000000000000006",
            "Class": "/Script/AIModule.BTTask_Wait",
            "Editor": { "Position": { "X": 400, "Y": 200 } },
            "Properties": { "WaitTime": { "DefaultValue": 2.0, "Key": "None" } }
          }
        ]
      },
      "Comments": [
        { "Id": "00000000000000000000000000000007", "Text": "Chase or idle" }
      ]
    }
  }
}
```

Extract with `diff_only` (default) omits node `Properties` equal to the class default (omitted = class default, so the extract still applies as a no-op); `diff_only: false` lists them all.

Diff: `path` is a JSON Pointer into the document (`/Body/Tree/Root/Children/1/Properties/WaitTime/DefaultValue`, `/Body/Tree/Root/Decorators/0`, `/Body/Tree/Comments/0/Text`); entries also keep the BehaviorTree fields `status` (`changed`/`added`/`removed`), `current` (the asset) and `desired` (the document).

## Pitfalls

- `BTTask_MoveTo` (and every other move task) needs a navigation mesh. A level with no `NavMeshBoundsVolume` has none, so the enemy stands still while its tree runs. Add one with `edit_level`, for example `{"ops": [{"op": "spawn", "class": "/Script/NavigationSystem.NavMeshBoundsVolume", "transform": {"location": {"X": 0, "Y": 0, "Z": 0}, "scale": {"X": 40, "Y": 40, "Z": 5}}}]}`, with location and scale chosen so the volume covers the floor the AI walks on (`get_level_editing_guide` has the request fields); the engine builds a RecastNavMesh from it. In Play In Editor, `query_play` on the enemy shows the symptom: `ai.move_status` stays `Idle` and the location does not change while `ai.active_node` is the MoveTo task, or the tree falls through to another branch at once. Only the level can fix it, not the tree.
