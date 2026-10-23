# AssetDocument IK Rig and IK Retargeter profiles: design

Governing rules: [design rules](asset-document-design-rules.md) §3 (rollback = functional
equivalence), §3.1 (Undo history), §3.2 (dependent assets), §4 (editor-equivalent code; raw paths allowed when the
editor does it that way, in their own file with a behaviour test; no private headers, no unexported symbols) and §5
(support matrix). The reference is the **IK Rig editor** and the **IK Retargeter editor** of UE 5.7
(`Engine/Plugins/Animation/IKRig`), plus their Content Browser actions. The legacy generators are not a reference.
Acceptance is editor fidelity on real engine/plugin content, free edits, and an AI writing rigs and retargeters
from scratch.

Source paths are relative to `Engine/Plugins/Animation/IKRig/Source/` unless they start with `Engine/`. `IKRig/` is
the runtime module, `IKRigEditor/` the editor module (PostEngineInit). Both controllers live in `IKRigEditor/Public`
(`IKRIGEDITOR_API`, public headers).

## 1. Classes in scope

| Class | Scope | Reason |
|---|---|---|
| `/Script/IKRig.IKRigDefinition` | **Exact profile** `profile.ik_rig.v1` | Its content (goals, the solver stack of `FInstancedStruct`s, retarget chains, excluded bones, the skeleton copied from the preview mesh) is not Details-editable: the generic route sees 4 viewport properties (coverage audit row 12). |
| `/Script/IKRig.IKRetargeter` | **Exact profile** `profile.ik_retargeter.v1` | The op stack (`TArray<FInstancedStruct>`), the chain mapping that lives inside the ops, and the retarget poses are not Details properties of the asset. |
| Solver structs (`FIKRigFullBodyIKSolver`, `FIKRigLimbSolver`, `FIKRigPoleSolver`, `FIKRigBodyMoverSolver`, `FIKRigSetTransform`, `FIKRigStretchLimbSolver`, any project `FIKRigSolverBase` subclass) | Same profile, values through the typed value codec | They are the solver stack entries. The editor enumerates every non-abstract `FIKRigSolverBase` child (`SIKRigSolverStack.cpp` `CacheSolverMetaData`), so project solvers work without code. |
| Retarget op structs (16 in IKRig: Pelvis Motion, FK Chains, Run IK Rig, IK Chains, Pole Vector, Stretch Chain, Root Motion, Curve Remap, Scale Source, Copy Base Pose, Pin Bones, Filter Bones, Stride Warping, Speed Planting, Floor Constraint, Additive Pose; plus experimental plugin ops such as `FIKRetargetRelativeIKOp`, `FIKRetargetRigMapperOp`) | Same profile | Same reasoning: struct type + reflected settings. |
| `FRetargetProfile` map on the retargeter (`Profiles`, `CurrentProfile`) | Read only (Inspect), `IKRetargeter.ProfilesNotAuthored` | Stored on the asset but the 5.7 editor has no UI for it (profiles are applied by anim nodes / batch tools at runtime). Not commonly edited; extract-only facts. |
| Deprecated UObject stacks (`Solvers_DEPRECATED`, `OpStack_DEPRECATED`, `ChainSettings_DEPRECATED`, `RootSettings_DEPRECATED`, `GlobalSettings_DEPRECATED`, `RetargetPoses`, `ChainMap_DEPRECATED`) and the default subobjects the constructor still creates for them | Out (migration only) | `PostLoad` converts them into the struct stacks; the profile reads the converted state. |
| `UIKRigController`, `UIKRetargeterController`, solver / op controllers, struct wrappers | Out (transient) | `Transient, DuplicateTransient, NonTransactional` editor helpers; the profile calls them, never serializes them. |
| Control Rig (`ControlRigBlueprint`), `ControlRigShapeLibrary`, `RetargetPoseAsset`-style exports | Out | Control Rig is a Blueprint-family graph asset and needs its own design. Pose export (`IKRetargetPoseExporter`) writes PoseAssets, which have their own profile. |
| IK Rig anim graph nodes (`AnimGraphNode_IKRig`, `AnimGraphNode_RetargetPoseFromMesh`) | Out here | They are AnimBlueprint nodes, already covered by the AnimBlueprint profile's generic anim-node path. |

Both assets are mostly made once and then tuned. That is why the plan (§7) puts read + the IK Rig first and makes
discovery (auto-characterize, auto-FBIK, auto-align suggestions) part of the AI surface: a human uses those buttons
far more than they type quaternions.

## 2. Editor data model: authored vs derived

There is **no compile step**. Both editors re-initialize a processor (`FIKRigProcessor`, `FIKRetargetProcessor`) after
each edit for the viewport and log; nothing it computes is stored. All persistent state is UPROPERTY data on the two
assets, edited through the two controllers.

### 2.1 IK Rig (`IKRig/Public/Rig/IKRigDefinition.h`)

Authored (Extract emits, Apply writes):

- `PreviewSkeletalMesh` (`TSoftObjectPtr<USkeletalMesh>`, EditAnywhere). Setting it in Details runs
  `UIKRigController::SetSkeletalMesh` (`FIKRigEditorController::OnFinishedChangingDetails`), which also rebuilds the
  stored skeleton. So it is Body-owned, not a generic property.
- `Skeleton.ExcludedBones` (`FIKRigSkeleton`): global to the rig, set with `SetBoneExcluded`.
- `Goals` (`UIKRigEffectorGoal` subobjects): `GoalName`, `BoneName`, `PositionAlpha`, `RotationAlpha`,
  `CurrentTransform` (the goal gizmo's offset, EditAnywhere), editor-only `PreviewMode`, `SizeMultiplier`,
  `ThicknessMultiplier`, `bExposePosition`, `bExposeRotation`.
- `SolverStack` (`TArray<FInstancedStruct>` of `FIKRigSolverBase`): per solver its struct type, `bIsEnabled`, its
  settings struct (`Settings`), its per-goal settings array (`AllGoalSettings`, keyed by `Goal`) and per-bone settings
  array (`AllBoneSettings`, keyed by `Bone`). Read-only fields of those structs (`RootBone`, `StartBone`, `EndBone`,
  `GoalName`, `BoneName`, `CPF_BlueprintReadOnly`) are set only through controller calls (`SetStartBone`,
  `SetEndBone`, `ConnectGoalToSolver`, `AddBoneSetting`).
- `RetargetDefinition`: `RootBone` (the "Pelvis"), `BoneChains[]` (`ChainName`, `StartBone`, `EndBone`, `IKGoalName`).
- Editor-only viewport settings: `DrawGoals`, `GoalSize`, `GoalThickness` (generic `Properties`).

Derived (never emitted):

| State | Recomputed by |
|---|---|
| `Skeleton.BoneNames`, `ParentIndices`, `RefPoseGlobal`, `CurrentPose*` | `SetSkeletalMesh` → `FIKRigSkeleton::SetInputSkeleton` from the preview mesh |
| Goal `InitialTransform` (and the default `CurrentTransform` of a new goal) | ref pose of the goal bone (`AddNewGoal`, `ResetInitialGoalTransforms` in `BroadcastNeedsReinitialized`) |
| Order of `RetargetDefinition.BoneChains` | `SortRetargetChains`: by start-bone index, then name |
| Transient solver state (`IndexInSolver`, caches) | solver `Initialize` |
| `Solvers_DEPRECATED` conversion | `PostLoad` (in memory, the package is not dirtied) |
| Processor log (errors / warnings) | `FIKRigProcessor::Initialize` → `FIKRigLogger` (reported as status, §5) |
| `Controller`, `BoneSize` | transient / internal |

Two editor behaviours shape the read path. Legacy rigs (MoverExamples `IK_Mannequin` still stores
`IKRigPBIKSolver` UObjects) are converted on load, so the profile always reads the 5.6+ struct stack. And
`SetSkeletalMesh` passes the rig's own `ExcludedBones` into `SetInputSkeleton`, which resets its input first: the
exclusions may be lost when the mesh changes (to be confirmed by the behaviour test). The apply therefore always
re-applies `ExcludedBones` after a mesh change (§4).

### 2.2 IK Retargeter (`IKRig/Public/Retargeter/IKRetargeter.h`)

Authored:

- `SourceIKRigAsset`, `TargetIKRigAsset` ("Default Target IK Rig"). Set by `UIKRetargeterController::SetIKRig`,
  which also sets the matching preview mesh and re-initializes chain mappings.
- `SourcePreviewMesh`, `TargetPreviewMesh` (editor-only soft refs; empty = fall back to the IK Rig's preview mesh).
- `RetargetOps` (`TArray<FInstancedStruct>` of `FIKRetargetOpBase`): per op its struct type, `Name` (unique, the
  identity), `ParentOpName`, `bIsEnabled`, `Settings` (the op's settings struct), and, for chain ops, an
  `FRetargetChainMapping` (target chain → source chain; in `Settings.ChainMapping` for FK Chains, on the op for Run IK
  Rig, Pole Vector, Stretch Chain).
- `SourceRetargetPoses`, `TargetRetargetPoses` (`TMap<FName, FIKRetargetPose>`: `RootTranslationOffset` (vertical
  only), `BoneRotationOffsets` (local-space quaternion deltas by bone name)) and `CurrentSourceRetargetPose`,
  `CurrentTargetRetargetPose`.
- Editor-only preview Details (`TargetMeshOffset`, `TargetMeshScale`, `SourceMeshOffset`, visibility, colour
  overrides, `bIgnoreRootLockInPreview`, `bDebugDraw`, `bProfileOps`): generic `Properties`.

Derived:

| State | Recomputed by |
|---|---|
| Flat order of `RetargetOps` (children before their parent, compact, siblings together) | `UIKRetargeter::CleanOpStack` (every op add / move / parent change and `PostLoad`) |
| Chain-mapping rows themselves (one row per target chain, `SourceIKRig` / `TargetIKRig` pointers, sort) | `FRetargetChainMapping::ReinitializeWithIKRigs` in `CleanOpStack` |
| Per-chain settings rows in op settings (`ChainsToRetarget`, `ChainsToAlign`, `ChainsToStretch`, `ChainSettings`, `ChainsToAffect`, ...) | each op's `OnReinitPropertyEdited` / `OnAddedToStack` / `RegenerateChainSettings` from the target IK Rig's chains |
| `Settings.IKRigAsset` of ops that take the target rig (FK Chains, Run IK Rig, Pole Vector, Stretch Chain) | `OnAddedToStack` / `OnAssignIKRig` from `TargetIKRigAsset` (authored only when it differs: a per-op custom target rig) |
| "Default Pose" existence, current pose names validity | `CleanRetargetPoses` (constructor, `PostLoad`) |
| Deprecated settings objects and their conversion | `PostLoad*` chain (§1) |
| `Version` (transient reinit counter), `Controller` | runtime |

## 3. Document shape

### 3.1 Example: IK Rig (small, complete; UE5 mannequin)

```jsonc
{
  "SchemaVersion": 1, "Target": "/Game/Characters/Rigs/IK_Hero", "Class": "/Script/IKRig.IKRigDefinition",
  "Action": "CreateOrUpdate",
  "Properties": {"GoalSize": 5.0},
  "Body": {
    "PreviewMesh": "/Game/Characters/Meshes/SKM_Hero.SKM_Hero",
    "ExcludedBones": ["ik_foot_root", "ik_hand_root"],
    "Goals": {
      "LeftFootIK":  {"Bone": "foot_l"},
      "RightFootIK": {"Bone": "foot_r"},
      "LeftHandIK":  {"Bone": "hand_l", "RotationAlpha": 0.5, "ExposePosition": true}
    },
    "Solvers": [
      {"Type": "FullBodyIK", "StartBone": "pelvis",
       "Settings": {"RootBehavior": "Free", "GlobalPullChainAlpha": 0.0},
       "Goals": {"LeftFootIK": {}, "RightFootIK": {}, "LeftHandIK": {"ChainDepth": 2, "PullChainAlpha": 0.5}},
       "Bones": {"spine_01": {"RotationStiffness": 0.8},
                 "calf_l": {"X": "Locked", "Z": "Limited", "MinZ": -5, "MaxZ": 150, "bUsePreferredAngles": true,
                            "PreferredAngles": {"X": 0, "Y": 0, "Z": 90}}}},
      {"Type": "LimbIK", "Enabled": false, "StartBone": "upperarm_r",
       "Goals": {"RightHandIK": {}}, "Settings": {"MaxIterations": 20}}
    ],
    "RetargetRoot": "pelvis",
    "Chains": {
      "Spine":    {"Start": "spine_01", "End": "spine_05"},
      "LeftLeg":  {"Start": "thigh_l", "End": "ball_l", "Goal": "LeftFootIK"},
      "RightLeg": {"Start": "thigh_r", "End": "ball_r", "Goal": "RightFootIK"},
      "LeftArm":  {"Start": "upperarm_l", "End": "hand_l", "Goal": "LeftHandIK", "RenamedFrom": "ArmL"}
    }
  }
}
```

(`RightHandIK` would have to be a goal too; the example keeps it short, so this document as written fails with
`IKRig.GoalNotFound` at `/Body/Solvers/1/Goals/RightHandIK`, which is the behaviour we want.)

### 3.2 Example: IK Retargeter

```jsonc
{
  "SchemaVersion": 1, "Target": "/Game/Characters/Rigs/RTG_Manny_To_Hero", "Class": "/Script/IKRig.IKRetargeter",
  "Action": "CreateOrUpdate",
  "Properties": {"TargetMeshOffset": {"X": 150, "Y": 0, "Z": 0}},
  "Body": {
    "Source": {"IKRig": "/Game/Characters/Rigs/IK_Manny.IK_Manny"},
    "Target": {"IKRig": "/Game/Characters/Rigs/IK_Hero.IK_Hero",
               "PreviewMesh": "/Game/Characters/Meshes/SKM_Hero_LOD.SKM_Hero_LOD"},
    "Ops": [
      {"Name": "Pelvis Motion", "Type": "PelvisMotion", "Settings": {"TranslationAlpha": 0.9}},
      {"Name": "FK Chains", "Type": "FKChains",
       "ChainMapping": {"Spine": "Spine", "LeftLeg": "LeftLeg", "RightLeg": "RightLeg", "LeftArm": "LeftArm"},
       "Chains": {"Spine": {"RotationMode": "MatchChain"}, "LeftArm": {"TranslationMode": "GloballyScaled"}}},
      {"Name": "Run IK Rig", "Type": "RunIKRig",
       "ChainMapping": {"LeftLeg": "LeftLeg", "RightLeg": "RightLeg", "LeftArm": "LeftArm"},
       "Children": [
         {"Name": "Retarget IK Goals", "Type": "IKChains", "Chains": {"LeftArm": {"BlendToSource": 0.3}}},
         {"Name": "Stride Warp IK Goals", "Type": "StrideWarping", "Enabled": false}
       ]},
      {"Name": "Root Motion", "Type": "RootMotion", "Settings": {"RootMotionSource": "GenerateFromTargetPelvis"}},
      {"Name": "Remap Curves", "Type": "CurveRemap"}
    ],
    "Poses": {
      "Source": {"Current": "Default Pose", "Poses": {"Default Pose": {}}},
      "Target": {"Current": "A-Pose",
                 "Poses": {"Default Pose": {},
                           "A-Pose": {"RootOffset": 2.5,
                                      "Bones": {"upperarm_l": {"X": 0, "Y": 0, "Z": -0.3420201, "W": 0.9396926},
                                                "upperarm_r": {"X": 0, "Y": 0, "Z": 0.3420201, "W": 0.9396926}}}}}
    }
  }
}
```

### 3.3 Regions

All regions follow the core rule: **omitted = keep, present = complete state**. Deviations are listed.

**IK Rig Body**

| Region | Shape | Notes |
|---|---|---|
| `PreviewMesh` | object path or `null` | Body-owned (`SetSkeletalMesh`). Required to create anything that names a bone (§4.3). Must be compatible: every bone the rig uses exists (`IKRig.IncompatibleMesh` lists the missing bones, from `FIKRigProcessor::IsIKRigCompatibleWithSkeleton` with a logger). |
| `ExcludedBones` | array of bone names | Order has no meaning (extract writes skeleton order). |
| `Goals` | `{"<GoalName>": {Bone, PositionAlpha?, RotationAlpha?, Offset?, PreviewMode?, ExposePosition?, ExposeRotation?, SizeMultiplier?, ThicknessMultiplier?, RenamedFrom?}}` | Keyed by goal name (the editor's identity; `SanitizeGoalName` rules: letters, digits (not first), `_`, `-`, `.`, ≤ 100 chars, else `IKRig.InvalidName`). Order has no meaning. `Offset` is `CurrentTransform` relative to the bone's ref pose, extracted only when not identity. |
| `Solvers` | ordered array of `{Type, Enabled?, StartBone?, EndBone?, Settings?, Goals?, Bones?}` | Stack order = execution order. `Type` is a solver struct: short name without the `IKRig` prefix and `Solver` suffix for the six engine solvers (`FullBodyIK`, `LimbIK`, `Pole`, `BodyMover`, `SetTransform`, `StretchLimb`), otherwise `/Script/Module.Struct`. `Settings` is the solver's settings struct (sparse, editable fields only). `Goals` is the complete set of connected goals, each with that solver's per-goal settings (sparse; `{}` when the solver has none). `Bones` is the complete set of bones with per-bone settings (sparse). Single-goal solvers (Limb, Pole, Set Transform, Stretch Limb) accept at most one goal (`IKRig.SolverGoalLimit`). Solvers without bone settings refuse `Bones` (`IKRig.SolverHasNoBoneSettings`). |
| `RetargetRoot` | bone name or `""` | The "Pelvis" (`SetRetargetRoot`). |
| `Chains` | `{"<ChainName>": {Start, End, Goal?, RenamedFrom?}}` | Keyed by name; order is derived (sorted by start bone). `Goal` names a goal of `Goals` or is omitted. |

**IK Retargeter Body**

| Region | Shape | Notes |
|---|---|---|
| `Source`, `Target` | `{IKRig, PreviewMesh?}` | `IKRig`: object path or `null`. `PreviewMesh` omitted = "use the IK Rig's mesh" (stored empty). |
| `Ops` | ordered array of Op, nested by `Children` | The op stack panel as shown: top-level ops in stack order, child ops under their parent in their order. |
| Op | `{Name, Type, Enabled?, RenamedFrom?, Settings?, ChainMapping?, Chains?, TargetIKRig?, Children?}` | `Name` unique in the asset (the editor's identity, `GetCleanAndUniqueOpName`). `Type`: short name of the 16 IKRig ops (struct name without `IKRetarget` prefix and `Op` suffix), else `/Script/Module.Struct`. `Settings` sparse over the op's settings struct, excluding the fields the next three keys own. `ChainMapping` `{"<target chain>": "<source chain>" \| null}` (complete over the target rig's chains when present; omitted target chains map to none). `Chains` `{"<target chain>": {sparse per-chain settings}}` (see below). `TargetIKRig` only for an op whose rig differs from the default target rig. `Children` only on ops that can have children (`CanHaveChildOps`), and only child types whose `GetParentOpType` is that op's type (`IKRetargeter.InvalidParentOp`). |
| `Poses` | `{"Source": PoseSet, "Target": PoseSet}` | PoseSet = `{Current, Poses: {"<name>": {RootOffset?, Bones?, RenamedFrom?}}}`. `"Default Pose"` always exists and cannot be renamed or removed (`IKRetargeter.DefaultPoseRequired`). `RootOffset` is the vertical root offset (Z, a number; X/Y are always zero in the editor). `Bones` `{"<bone>": {X,Y,Z,W}}` local rotation offsets (quaternions as stored; normalised on write; extract omits identity). |

Per-chain settings (`Chains`): every op settings array whose element struct has a `TargetChainName` field (FK
`ChainsToRetarget`, IK `ChainsToRetarget`, `ChainsToAlign`, `ChainsToStretch`, Stride Warp `ChainSettings`, Speed
Planting `ChainsToSpeedPlant`, Floor `ChainsToAffect`, and the same shape in plugin ops) is shown as a map keyed by
target chain, values sparse over the element's defaults. Rows are derived (one per target chain); a chain not listed
has default settings (`ResetChainSettingsToDefault`); an unknown chain fails `IKRetargeter.UnknownTargetChain`. This
one rule covers all ops without per-op code.

`Properties` (top-level, sparse, generic admission) carry the viewport / preview Details. Body-owned and refused
there (`IKRig.NotAuthoredProperty` / `IKRetargeter.NotAuthoredProperty`, with the region to use):
`PreviewSkeletalMesh`, `Skeleton`, `Goals`, `SolverStack`, `RetargetDefinition`, `SourceIKRigAsset`,
`TargetIKRigAsset`, `SourcePreviewMesh`, `TargetPreviewMesh`, `RetargetOps`, `*RetargetPoses`, `Current*Pose`,
`Profiles`, `CurrentProfile` and every deprecated property.

### 3.4 Values

Settings structs go through the Editor typed value codec (same forms as everywhere: numbers, enum names, `{X,Y,Z}`,
`{Pitch,Yaw,Roll}`, object paths). Two local forms:

- `FBoneReference` (op settings, chain endpoints): a bone name string (`""` = none); a bone not in the relevant
  skeleton fails `IKRig.BoneNotFound` / `IKRetargeter.BoneNotFound` with close names. The relevant skeleton is the
  op's: source rig's mesh for source-side fields, target otherwise (`FIKRetargetOpSettingsBase::GetSkeleton`).
- Read-only / transient fields of the structs are never emitted or accepted (`IKRig.FieldNotEditable`): the fields
  the editor sets through controller calls are their own keys (`StartBone`, `EndBone`, `Goals`, `Bones`,
  `ChainMapping`, `Chains`, `TargetIKRig`).

`diff_only` extraction omits values equal to the struct default (a new solver / op of that type after
`OnAddedToStack`, so an op's auto-filled `SourcePelvisBone` is not shown when it equals what a new op would get).

### 3.5 Identity and free edits

- **IK Rig goals and chains, retarget poses**: by name. `RenamedFrom` is the editor's rename (`RenameGoal`,
  `RenameRetargetChain`, `RenameRetargetPose`); without it a new name is remove + add. Renaming a goal updates the
  solvers and chains that use it, as `RenameGoal` does (the document must use the new name there too; a stale
  reference fails `IKRig.GoalNotFound`).
- **Solvers** have no name or GUID in 5.7 (index only; the editor labels them "N - Full Body IK"). Apply aligns the
  document list with the live stack by type: longest common subsequence of `Type`s keeps matched solvers (their
  settings are then rewritten completely), unmatched live solvers are removed, unmatched document entries are added
  and moved into place. A reorder of same-type solvers therefore rewrites settings instead of moving; the result is
  the same rig (§3 of the reset: identity need not be preserved). Extract → apply is a no-op because the alignment is
  the identity map.
- **Ops**: by `Name` (unique; `RenamedFrom` = `SetOpName`). A different `Type` under the same name is remove + add.
  Moving an op = its position in the list (`MoveRetargetOpInStack`) or under another parent (`SetParentOpByName`).
- **Bones** (excluded bones, per-bone settings, pose bones) by bone name.
- **Across save / reload / duplicate**: all keys are names stored in the asset; Content Browser duplication keeps
  them. Nothing in either asset is keyed by object path except the rig/mesh references.

| Free edit | Document change | Operation |
|---|---|---|
| Add a goal on a bone and use it in FBIK | new `Goals` entry + key in the solver's `Goals` | `AddNewGoal`, `ConnectGoalToSolver` |
| Move a goal to another bone | `Goals.X.Bone` | `SetGoalBone` |
| Delete a goal | remove the key (and its uses) | `RemoveGoal` (also disconnects it from solvers and chains, as the editor) |
| Rename a chain / goal | new key + `RenamedFrom` | `RenameRetargetChain` / `RenameGoal` |
| Insert / delete / reorder / disable a solver | edit `Solvers` | `AddSolver`, `RemoveSolver`, `MoveSolverInStack`, `SetSolverEnabled` |
| Change FBIK bone limits | `Solvers[i].Bones.<bone>` | `AddBoneSetting` + struct write |
| Exclude a bone | `ExcludedBones` | `SetBoneExcluded` |
| Swap the preview mesh | `PreviewMesh` | `SetSkeletalMesh` (+ re-apply exclusions) |
| Add a retarget op / child op | new entry (under `Children`) | `AddRetargetOp(Type, ParentName)` (runs `OnAddedToStack`), `SetOpName` |
| Re-map a chain | `ChainMapping.<target>` | `SetSourceChain(source, target, op)` |
| Tune a chain | `Chains.<target>.<field>` | struct write + `OnOpPropertyChanged` |
| Point the retargeter at another target rig | `Target.IKRig` | `SetIKRig(Target)` + `AssignIKRigToAllOps(Target)` (what the editor's "replace rig" dialog does) |
| New target pose, rotate bones | `Poses.Target.Poses.<new>` | `CreateRetargetPose`, then per bone `SetRotationOffsetForRetargetPoseBone` on that (current) pose |

### 3.6 Discovery (read-only additions)

AI-from-scratch needs the editor's automatic tools, which are commands, not state. They are offered as **read-only
suggestions**, never as document directives (a directive would make re-apply non-idempotent):

- `inspect_asset_document_target` on a **SkeletalMesh** or an IK Rig adds `IKRigSuggestions`: the
  auto-characterized `RetargetRoot` + `Chains` (`UIKRigController::AutoGenerateRetargetDefinition`, with the
  matched template name and score) and the auto-FBIK `Goals` + `Solvers` + `ExcludedBones`
  (`AutoGenerateFBIK`), already in document form. They are computed on a transient `UIKRigDefinition` in the
  transient package (as `SRetargetAnimAssetsWindow` does for batch retargeting), never on an asset.
- On an IK Retargeter, `RetargeterSuggestions.AutoAlign` returns `Bones` for each `ERetargetAutoAlignMethod`
  (`ChainToChain`, `MeshToMesh`, `LocalRotationAxes`, `GlobalRotationAxes`) for the target (and source) current pose,
  computed by `FRetargetAutoPoseGenerator` on a transient duplicate; `AutoMap` returns `ChainMapping` per chain op for
  `Exact` and `Fuzzy`.
- `list_types` gains `kind: "ikrig_solver"` and `kind: "ikretarget_op"` (struct path, short name, display name,
  settings / goal / bone settings field lists with defaults, `IsSingleton`, `CanHaveChildOps`, `GetParentOpType`).
- Inspect on an IK Rig: bone hierarchy (from the stored skeleton), goals with their bones, which solvers use each
  goal, chain bone lists (`ValidateChainAndGetBones`), status (§5). On a retargeter: chains of both rigs, mapping
  readiness per op, pose bone lists, `Profiles` (read only), status.

## 4. Editor-equivalent operations

### 4.1 Mapping

The editor's pattern is: controller call → `FScopedTransaction` → `Asset->Modify()` → edit →
`FScopedReinitializeIKRig` / `FScopedReinitializeIKRetargeter` broadcast on scope exit. Details edits of a solver /
op struct go through `UIKRigStructViewer` (`IKRigStructViewer.cpp`): `GEditor->BeginTransaction` + `Owner->Modify()`,
write into the struct memory of the wrapped `FInstancedStruct`, `EndTransaction`, then, for
`ReinitializeOnEdit` / `BoneName` properties, `OnStructNeedsReinit` → `BroadcastNeedsReinitialized` (rig) or
`UIKRetargeterController::OnOpPropertyChanged` (retargeter: the op's `OnReinitPropertyEdited`, its children's
`OnParentReinitPropertyEdited`, then reinit).

The profile calls the same controller methods (all public; most are `BlueprintCallable`) and reproduces the struct
viewer write for Details values. One edit primitive call = one batch of ops; the controller transactions are left as
they are and the Undo history rule of §3.1 applies (a successful apply clears history that references the asset; a
failed apply after controller calls also clears it, as with StateTree).

**IK Rig**

| Editor action | Call | Notes |
|---|---|---|
| New IK Rig (Content Browser "IK Rig" on a skeletal mesh: `FAssetTypeActions_IKRigDefinition::CreateNewIKRigFromSkeletalMesh`) | generic factory lifecycle (`UIKRigDefinitionFactory`, no dialog), then `SetSkeletalMesh(Body.PreviewMesh)` as the first op | §4.3 |
| Preview mesh in Details | `SetSkeletalMesh` then re-apply `ExcludedBones` | refused when incompatible (§3.3) |
| Exclude / include bone (hierarchy context menu) | `SetBoneExcluded` | |
| New goal (hierarchy "New IK Goal") | `AddNewGoal(Name, Bone)` | returns `NAME_None` on a duplicate / missing bone → `IKRig.GoalNotCreated` |
| Rename / delete goal, move to bone | `RenameGoal`, `RemoveGoal`, `SetGoalBone` | `RenameGoal` returns the uniquified name; a different name than requested fails `IKRig.NameTaken` |
| Goal Details (alphas, exposure, preview mode, size) | struct-viewer-style write on the `UIKRigEffectorGoal` (Modify, write, `PostEditChangeProperty`) | the goal is a UObject, so this is an ordinary Details edit |
| Goal gizmo offset | `SetGoalCurrentTransform` | |
| Add / remove / move / enable solver (solver stack panel) | `AddSolver(UScriptStruct*)`, `RemoveSolver`, `MoveSolverInStack`, `SetSolverEnabled` | `AddSolver` refuses abstract types → `IKRig.UnknownSolverType` |
| Set root / end bone (hierarchy "Set Root Bone on Solver") | `SetStartBone`, `SetEndBone` | `SetEndBone` requires `UsesEndBone` (`IKRig.SolverHasNoEndBone`); `SetStartBone` is checked by us with `UsesStartBone` because the controller does not |
| Connect / disconnect goal (hierarchy "Connect Goal to Solver") | `ConnectGoalToSolver`, `DisconnectGoalFromSolver` | the controller does not `Modify()` the asset; harmless (the package snapshot covers it), noted in the behaviour test |
| Add / remove bone settings | `AddBoneSetting`, `RemoveBoneSetting` (`CanAddBoneSetting` gives the reason → `IKRig.BoneSettingRefused`) | |
| Solver / goal / bone settings Details | struct-viewer write on `GetSolverStructAtIndex(i)` memory (`GetSolverSettings`, `GetGoalSettings(goal)`, `GetBoneSettings(bone)`), editable fields only (`CopyAllEditableProperties` rules), then `BroadcastNeedsReinitialized` | one raw file `AssetDocumentIKRigStructWrites.cpp` + behaviour test against the real `UIKRigStructViewer` path |
| Retarget root, chains (chain panel) | `SetRetargetRoot`, `AddRetargetChain`, `RemoveRetargetChain`, `RenameRetargetChain`, `SetRetargetChainStartBone/EndBone/Goal` | `AddRetargetChain` uniquifies; a changed name fails `IKRig.NameTaken` |
| Auto Characterize / Auto FBIK (toolbar) | not an apply; §3.6 suggestions | |

**IK Retargeter**

| Editor action | Call | Notes |
|---|---|---|
| New retargeter (Content Browser on an IK Rig: `AssetTypeActions_IKRetargeter`) | generic factory lifecycle (`UIKRetargetFactory::FactoryCreateNew` → `AddDefaultOps`), then `SetIKRig(Source)`, `SetIKRig(Target)` | then the Body regions reconcile the default ops |
| Source / target IK Rig (Details) | `SetIKRig(side, rig)`; for a replaced rig also `AssignIKRigToAllOps(side, rig)` (the editor's `HandleIKRigReplaced` after its confirmation dialog; the document is the confirmation) | `SetIKRig` neither transacts nor modifies: the primitive calls `Asset->Modify()` first |
| Preview mesh (Details) | `SetPreviewMesh(side, mesh)` | |
| Add op (op stack "+ Add New") | `AddRetargetOp(UScriptStruct*, ParentName)` | singletons (`IsSingleton`) refuse a second instance → `IKRetargeter.SingletonOp` |
| Remove / move / enable / rename op, change parent | `RemoveRetargetOp` (removes its children, like the editor), `MoveRetargetOpInStack`, `SetRetargetOpEnabled`, `SetOpName`, `SetParentOpByName` | after each structural call `CleanOpStack` has re-sorted; the primitive re-resolves indices by name |
| Op settings (op Details) | struct-viewer write into `GetRetargetOpStructAtIndex(i)`'s settings, then `OnOpPropertyChanged(OpName, event)` for reinit properties | same raw file as the rig; note the engine's child loop in `OnOpPropertyChanged` iterates the wrong index (child notifications go to ops 0..n-1); we call the method, so we reproduce the editor exactly |
| Per-op target rig (`Settings.IKRigAsset`) | struct write + `OnOpPropertyChanged` (regenerates chain rows) | written before `Chains` |
| Chain mapping (chain map panel, "Auto-Map") | `SetSourceChain(source, target, op)`; `AutoMapChains` is a suggestion only | |
| Reset chain settings | `ResetChainSettingsToDefault(target, op)` | used for chains not listed in `Chains` |
| Retarget poses (pose panel: new, duplicate, rename, delete, set current) | `CreateRetargetPose`, `DuplicateRetargetPose`, `RenameRetargetPose`, `RemoveRetargetPose`, `SetCurrentRetargetPose` | |
| Edit pose (viewport rotate, "Reset", "Snap to ground") | `SetCurrentRetargetPose(pose)` → `ResetRetargetPose(pose, bones)` / `SetRotationOffsetForRetargetPoseBone` (current pose only) / root: `SetRootOffsetInRetargetPose(target − current)` (the method adds) | these two setters do not transact or reinit: the primitive Modifies the asset and calls `BroadcastNeedsReinitialized` as the edit mode does on gizmo release; `Current` is set last |
| Auto Align / Auto Map buttons | suggestions (§3.6) | |

Not reproduced, by design: preview / playback, batch export (`IKRetargetBatchOperation` writes other assets), pose
export to PoseAssets, the "fix root height" prompt (`SetAskedToFixRootHeightForMesh`), editor-only selection.

### 4.2 Op planning order (Python)

IK Rig: `PreviewMesh` → `ExcludedBones` → goal renames → goal adds / bone moves → chain renames → `RetargetRoot` →
chain adds / endpoint changes → solver stack alignment (removes, adds, moves, enable) → per solver start/end bone →
goal connections → bone settings add/remove → settings writes → goal Details → removals of goals and chains not
listed → `Properties`.

Retargeter: `Properties` → `Source` / `Target` (rig, then `AssignIKRigToAllOps` if replaced, then preview mesh) → op
renames → op removals → op adds (parents before children) → moves / parents (top-level order, then children) →
enable → per op: `TargetIKRig`, other reinit settings, `ChainMapping`, `Chains`, remaining settings → poses (add /
rename / remove, bones, root offset) → `Current` poses.

### 4.3 Create

- **IK Rig.** The factory makes an empty rig; the editor's create action then sets the mesh. `Action: Create` with a
  Body that names any bone requires `PreviewMesh` (`IKRig.PreviewMeshRequired`); an empty rig (Body `{}` or only
  `PreviewMesh`) is allowed, like the Content Browser's plain "New IK Rig". No `FactoryContext` is involved (the
  factory has no context field).
- **IK Retargeter.** The factory adds the default ops (Pelvis Motion, FK Chains, Run IK Rig with Retarget IK Goals,
  Root Motion, Remap Curves) with no rigs; the create action then sets both rigs. A `Create` document with `Ops`
  reconciles from that default stack (unlisted default ops are removed), so the extract of the result equals the
  document. Without `Ops` the default stack stays, and after `SetIKRig` the profile calls `AssignIKRigToAllOps` for
  both sides so the default ops are configured as the editor shows them when first opened (`CleanAsset`). Whether
  the real editor's first open leaves exactly that state is checked by a behaviour test.

### 4.4 Primitives and module

Generic property admission covers only the viewport `Properties`. Everything in the Body needs a provider:

- New module **`AssetDocumentRig`** (Editor, PostDefault), owner `EAssetDocumentPrimitiveOwner::Rig` (new value 11),
  provider id `rig`, table-driven through `CreateAssetDocumentAssetTablePrimitiveProvider` like `audio`. Depends on
  `IKRig`, `IKRigEditor`, `PBIK` (enum types), `AssetDocumentEditor` (typed value codec, non-interactive scope),
  `UnrealEd`, `Json`. `UEAssetDoc.uplugin` gains the `IKRig` plugin (enabled by default; it pulls ControlRig and
  FullBodyIK, which are enabled by default too). Keeping it in its own module mirrors `AssetDocumentAudio` /
  `AssetDocumentNiagara`.
- `rig.ikrig.read.v1`: authored state (§2.1) in document-ready records: mesh, excluded bones, goals (all fields), per
  solver: struct path, enabled, start / end bone, settings / goal settings / bone settings as typed JSON plus the
  new-solver defaults of its type, chains, root. Also the skeleton facts for Inspect.
- `rig.ikrig.edit.v1`: op list (`set_mesh`, `exclude`, `goal.add/remove/rename/set_bone/set`, `solver.add/remove/move/
  enable/start_bone/end_bone/connect/disconnect/bone_setting.add/remove/settings`, `chain.add/remove/rename/set`,
  `root`). Op errors carry `/ops/<n>` paths that Python maps back (as Material).
- `rig.ikretargeter.read.v1` / `rig.ikretargeter.edit.v1`: same pattern (`rig.set`, `preview_mesh`, `op.add/remove/
  rename/move/parent/enable/settings/target_rig/map/chain_settings/reset_chain`, `pose.add/duplicate/rename/remove/
  bones/root/current`).
- `rig.status.v1`: processor initialization status (§5).
- `rig.suggest.v1`: §3.6 suggestions (read only, transient objects only).
- `rig.catalog.v1`: the `list_types` kinds.

Python: `profiles/ik_rig.py`, `profiles/ik_retargeter.py`, shared `profiles/ik_common.py` (struct short names, bone
reference codec, chain-row map rule, solver alignment). Reused: `data_table_common.PropertiesRegion`,
`generic_factory_lifecycle`, the `SNAPSHOT_COVERED` preflight.

## 5. Validation, status and rollback

### 5.1 What "compile" is here

Nothing is compiled. The editor's equivalent of a status is the processor log shown in the output panel after every
reinit. `rig.status.v1` runs the same initializations headlessly:

- IK Rig: `FIKRigProcessor::Initialize(Rig, PreviewMesh, Goals)` with an `FIKRigLogger`: errors for no skeleton,
  incompatible mesh (missing required bones / parents), a goal whose bone does not resolve, two goals on different
  bones with the same name; warnings for a goal not used by any solver, an invalid chain ("end bone is not a child
  of the start bone"), solver `GetWarningMessage`.
- IK Retargeter: `FIKRetargetProcessor::Initialize(SourceMesh, TargetMesh, Retargeter, Profile, bSuppressWarnings=false)`:
  errors for missing asset / meshes; warnings per chain (missing start / end bone, end not a child, chain too short),
  per op (`Initialize` false: e.g. Run IK Rig without an IK Rig, FK op with no mapped chain), op
  `GetWarningMessage`.

Messages are attributed to document paths by the names they carry (goal, chain, solver index, op name).

### 5.2 Failure policy and status shape

As Material: read the before-status first; **new errors** fail the apply with `IKRig.ValidationFailed` /
`IKRetargeter.ValidationFailed` at the path of the goal / chain / solver / op and roll back; errors the asset
already had come back as `*.ValidationError` warnings; warnings come back with the successful result
(`*.ValidationWarning`). A retargeter whose meshes are not set reports `Status: Unknown` (nothing can be
initialized, as in the editor) and that is not a failure.

`Compile` on Extract / Apply / Inspect, through a compile-status describer:

```json
{"Status": "UpToDateWithWarnings",
 "Messages": [{"Severity": "Warning", "Message": "IK Goal, 'LeftHandIK' is not connected to any solvers ...",
               "Path": "/Body/Goals/LeftHandIK"}]}
```

`Status`: `UpToDate`, `UpToDateWithWarnings`, `Error`, `Unknown` (no preview mesh / meshes).

### 5.3 Rollback

- Existing asset: Core package snapshot. A derived-state rebuilder (`AssetDocumentRig.IKRig` /
  `AssetDocumentRig.IKRetargeter`) then does what the editor's undo does (`PostEditUndo` on the asset, then the
  controller's `BroadcastNeedsReinitialized` so open processors and anim nodes re-init). Goal subobjects created by the
  apply are new authored objects the snapshot removes.
- Created asset: factory rollback deletes the package.
- Dependents (design rules §3.2). Retargeters reference IK Rigs; anim graphs reference both. The editor propagates
  chain add / rename / remove from an IK Rig only to **retargeters open in an editor**
  (`FIKRetargetEditorController` binds `OnRetargetChainRenamed` ...); closed retargeters re-sync on next load
  (`CleanOpStack` → `ReinitializeWithIKRigs`: a renamed target chain loses its mapping and settings, a renamed source
  chain's mapping becomes none). The profile follows the editor and reports it: an IK Rig apply that renames or
  removes chains lists the retargeters that reference the rig (asset registry referencers) and the affected op
  mappings as `IKRig.DependentsAffected` warnings, so the AI can apply those retargeters' documents next. Decision
  §8.2: follow the editor, no further edits.
- Open editors: an IK Rig or retargeter open in its editor is closed for the apply and reopened on the
  result (Info `IKRig.EditorReopened`, `IKRetargeter.EditorReopened`). The editors hold processors, struct wrappers
  that copy struct memory (written back whole by the next Details edit, not refreshed on Undo) and delegate bindings;
  their refresh calls are not exported, so a reopen rebuilds them from the applied asset.

## 6. Fidelity acceptance plan

### 6.1 Content

The install ships **4 IK Rigs and 4 retargeters**, all in plugins that are off by default, and none under
`Engine/Content` or the IKRig plugin's own `Content`:

| # | Asset | Plugin (default) | Why |
|---|---|---|---|
| 1 | `/MetaHumanCharacter/Animation/Retargeting/IK_MH_IKRig` (mesh `/MetaHumanCharacter/Body/IdentityTemplate/SKM_Body`) | MetaHumanCharacter (beta, off) | current format: FBIK with goal and bone settings, chains with goals; largest (279 KB) |
| 2 | `/MetaHumanCharacter/Animation/Retargeting/RTG_MH_IKRig` | same | current op stack: FK, Run IK Rig, IK Chains, Pelvis, Pole Vector, Curve Remap, Speed Planting, Stride Warping |
| 3 | `/MoverExamples/Characters/Mannequins/Rigs/IK_Mannequin` (mesh `SKM_Manny_Simple`) | MoverExamples (experimental, off) | legacy `IKRigPBIKSolver` UObjects → `PostLoad` conversion |
| 4 | `/MoverExamples/Characters/Mannequins/Rigs/RTG_Mannequin` | same | oldest retargeter format (pre-op-stack conversion: `PostLoadConvertEverythingToOps`); references `/Game/Characters/Mannequins/Rigs/IK_Mannequin` |
| 5 | `/MoverTests/Characters/Mannequins/Rigs/IK_Mannequin` | MoverTests (experimental) | same family, saved in Main |
| 6 | `/MoverTests/Characters/Mannequins/Rigs/RTG_Mannequin` | same | |

Test project recipe: enable `MoverExamples` (and `MetaHumanCharacter` when the decision §8.1 allows), copy
`Templates/TemplateResources/High/Characters/Content` to `/Game/Characters` (UE5 mannequin `SKM_Manny_Simple`,
`SKM_Quinn_Simple`, `SK_Mannequin`), and duplicate #3 to `/Game/Characters/Mannequins/Rigs/IK_Mannequin` so #4's
references resolve. Tests skip with a note when a plugin is not enabled.

Built by the test with the editor's own calls (then saved, reloaded and used as input), so every solver and op type
is covered even though the shipped content does not use them:

- `IK_Tutorial`: on the always-mounted `/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP` (UE4
  skeleton) via `AutoGenerateRetargetDefinition` + `AutoGenerateFBIK` (what the batch retarget window does);
- `IK_AllSolvers`: one rig with all six solver types, bone settings on FBIK and Stretch Limb, excluded bones;
- `RTG_Tutorial_To_Manny`: `AddDefaultOps`, then one op of every remaining type (Pin Bones with pairs, Filter Bones,
  Copy Base Pose with excludes, Scale Source, Additive Pose, Floor Constraint, Stretch Chains, Pole Vector), a
  custom pose made with `AutoAlignAllBones`, a renamed pose, a disabled op.

### 6.2 What is compared

For each asset: duplicate into `/Game` and save → Extract → apply the extract as a new asset (`Create`) → compare:

- **document**: the two extracts are equal;
- **asset state**, normalised: IK Rig `PreviewSkeletalMesh`, `Skeleton` (bone names, parents, excluded bones),
  goals (all UPROPERTYs, `CurrentTransform` within 1e-4), `SolverStack` (type, enabled, settings structs compared
  with `UScriptStruct::CompareScriptStruct` after zeroing transient fields), `RetargetDefinition`; retargeter rigs,
  meshes, flat `RetargetOps` (type, name, parent, enabled, settings, chain maps), poses (quaternions within 1e-6
  after normalisation, current names);
- **status**: equal status and messages (§5);
- **runtime output** (cheap, deterministic): for each rig, `FIKRigProcessor` solves the ref pose with goals at their
  initial transforms, both rigs give the same output pose (1e-3 cm / 1e-4 rad); for each retargeter,
  `FIKRetargetProcessor` retargets the source ref pose plus one frame of a fixed animation, same target pose.

Then re-apply → no-op, and reload from disk → extract equal.

### 6.3 Free edits (document vs the editor's calls on two duplicates)

The same edit is made once through a document and once through the controller calls the editor's UI makes (§4.1); the
normalised asset states and statuses must be equal:

- add a goal on `hand_r`, connect it to FBIK, set its `PullChainAlpha`;
- rename a goal used by a chain and by FBIK;
- delete a solver in the middle of the stack; move a solver up; disable one;
- add FBIK bone limits on `calf_l`, remove another bone's settings;
- exclude / include a bone;
- add, rename, re-point and delete a chain; change the retarget root;
- swap the preview mesh to `SKM_Quinn_Simple` (exclusions kept);
- retargeter: add a Pin Bones op, rename it, move it, disable it, delete Stride Warp (child of Run IK Rig), add a
  Floor Constraint child;
- re-map two chains, reset one chain's settings, change FK `RotationMode` on a chain;
- replace the target IK Rig (`AssignIKRigToAllOps`);
- new target pose from a suggestion, rename it, make it current; reset one bone; set the root offset.

Undo test: human edit in the IK Rig editor, save, AI apply, Ctrl+Z behaviour per §3.1.

### 6.4 AI from scratch

- **Python test:** given only `/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple` and
  `/Engine/Tutorial/.../TutorialTPP`: inspect both meshes, take `IKRigSuggestions`, write `IK_Manny` and `IK_Tutorial`
  (chains + FBIK with foot goals, one hand goal with a limb solver), then `RTG_Tutorial_To_Manny` with the default op
  stack, a Fuzzy `AutoMap` suggestion as `ChainMapping`, an A-pose target pose from the `AutoAlign` suggestion and
  root motion from the pelvis. Status: no errors; the retargeter retargets one frame. Then error cases at their
  document paths with rollback: unknown bone, unknown solver type, second goal on a Limb solver, singleton op twice,
  child op under the wrong parent, unknown chain in `ChainMapping`, incompatible preview mesh.
- **AI black-box** (MCP only + schema section): "make an IK Rig for this mesh with foot IK", "retarget the
  mannequin to this character and fix the arms in an A-pose", "add a pin-bone op that copies `ik_hand_gun` from
  `hand_r`".

## 7. Risks and unknowns

| Risk | Effect | Handling |
|---|---|---|
| Solvers have no identity | reorders of same-type solvers become settings rewrites | functional equivalence (§3.5); documented; tests compare state, not identity |
| Struct-viewer writes are done directly (the viewer itself is a transient UI object) | subtle differences (reinit triggers, hidden fields) | one raw file; behaviour test drives the real `UIKRigStructViewer` with `SetStructToView` + property handles and compares |
| `SetSkeletalMesh` may drop `ExcludedBones` | silent loss | always re-apply after a mesh change; behaviour test records the engine behaviour |
| Controller quirks: `SetIKRig` / pose setters without transaction or `Modify`; `ConnectGoalToSolver` without `Modify`; `SetRootOffsetInRetargetPose` adds; Floor Constraint op controller casts the wrong settings type; Stretch Limb has no solver controller | wrong undo, wrong values | we never use the op / solver scripting controllers (struct writes instead); we `Modify` before calls that do not; root offset written as a delta |
| Chain renames propagate only to open retargeter editors | closed retargeters lose mappings on next load | report `DependentsAffected` (§5.3); decision §8.2 |
| `CleanOpStack` re-sorts and renames ops after structural calls | stale indices in the op list | the edit primitive resolves ops by name after every structural call |
| Retargeter `OnAddedToStack` auto-fills (pelvis bones, root bones from preview mesh bone 0, auto-map) | create from a document must not depend on what the auto-fill chose | Settings written after add are complete over what the document lists; `diff_only` defaults are computed after `OnAddedToStack` on a transient copy |
| 5.7-specific (struct stacks since 5.6; op chain mappings since `OpsOwnChainMapping`) | will change in 5.8 | version-pinned matrix; the generic `TargetChainName` row rule and struct codec keep per-type code to zero |
| Experimental dependencies (FullBodyIK plugin is "Experimental" by location; PBIK enums) | header changes | only enum types from `PBIK`; values through reflection |
| Corpus plugins are off by default (MetaHuman is heavy) | thin real-content fidelity | builder-made fixtures cover every type; decision §8.1 |
| Performance | rigs ≤ 300 KB; processors are cheap | status runs once per apply; suggestions only on request |

## 8. Decisions

1. **Corpus.** MoverExamples is enabled in the test sandboxes only; MetaHumanCharacter is optional (its corpus tests
   skip with a note). The UEAssetDoc plugin itself depends only on IKRig.
2. **Chain rename / remove.** Follow the editor: warn with `IKRig.DependentsAffected` (the retargeters that reference
   the rig and their affected op mappings); never edit other packages.
3. **Automatic tools** (auto-characterize, auto-FBIK, auto-align, auto-map): read-only suggestions in Inspect only,
   never document keys.
4. **Module / owner.** New module `AssetDocumentRig`, owner `Rig` (11), `IKRig` plugin dependency in
   `UEAssetDoc.uplugin`.
5. **Retarget profiles** are read only (Inspect lists them; documents cannot change them,
   `IKRetargeter.ProfilesNotAuthored`).
