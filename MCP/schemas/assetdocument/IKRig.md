# IK Rig Profiles

Classes: /Script/IKRig.IKRigDefinition, /Script/IKRig.IKRetargeter

Exact profiles for IK Rigs and IK Retargeters (Python runtime, `rig_profile`). `extract_asset_document`, `inspect_asset_document_target`, `diff_asset_document` and `apply_asset_document_file` work on both classes: every Body region is created and edited.

Edits are the IK Rig / IK Retarget editors' own (their controller calls and Details edits), so the result is what a person would get in the editor. Apply works with the asset open in the IK Rig / IK Retarget editor: the editor is closed for the apply and reopened on the result (Info `IKRig.EditorReopened`, `IKRetargeter.EditorReopened`), because its Details panel holds copies of solver, goal and op settings.

## IK Rig Body

```jsonc
"Body": {
  "PreviewMesh": "/Game/Characters/Meshes/SKM_Hero.SKM_Hero",      // the skeleton the bones come from
  "ExcludedBones": ["ik_foot_root"],
  "Goals": {
    "LeftFootIK": {"Bone": "foot_l"},
    "RightHandIK": {"Bone": "hand_r", "RotationAlpha": 0.5, "Offset": {"Location": {"X": 0, "Y": 0, "Z": 10}}}
  },
  "Solvers": [
    {"Type": "FullBodyIK", "StartBone": "pelvis", "Settings": {"Iterations": 30, "RootBehavior": "Free"},
     "Goals": {"LeftFootIK": {}, "RightHandIK": {"PullChainAlpha": 0.5}},
     "Bones": {"calf_l": {"X": "Limited", "MinX": -10, "MaxX": 10}}},
    {"Type": "LimbIK", "StartBone": "upperarm_r", "Goals": {"RightHandIK": {}}, "Enabled": false}
  ],
  "RetargetRoot": "pelvis",
  "Chains": {"LeftLeg": {"Start": "thigh_l", "End": "foot_l", "Goal": "LeftFootIK"},
             "Spine": {"Start": "spine_01", "End": "spine_03"}}
}
```

- Every region is the complete state when present and kept as it is when omitted. `Goals` and `Chains` are keyed by name; `Solvers` is the ordered solver stack.
- `PreviewMesh` must be set before anything names a bone (`IKRig.PreviewMeshRequired`); it can be changed to another compatible mesh but not cleared. A mesh that lacks bones the rig uses is refused (`IKRig.IncompatibleMesh`).
- Goals: `Bone` plus the goal Details `PositionAlpha`, `RotationAlpha`, `PreviewMode`, `SizeMultiplier`, `ThicknessMultiplier`, `ExposePosition`, `ExposeRotation` (omitted = the new-goal default). `Offset` is the viewport gizmo offset from the bone's reference pose (`Location` cm, `Rotation` degrees Pitch/Yaw/Roll). Goal names are letters, digits and `_`, unique ignoring case.
- Solvers: `Type` is a `list_types` kind `ikrig_solver` name (`FullBodyIK`, `LimbIK`, `Pole`, `BodyMover`, `SetTransform`, `StretchLimb`). `StartBone` / `EndBone` only for solver types that use them (list_types `UsesStartBone` / `UsesEndBone`); they can be changed, not cleared. `Goals` lists the connected goals with their per-goal settings; `Bones` the bones with per-bone settings. `Settings`, goal settings and bone settings are sparse: an omitted field has the value of a new solver; list_types shows the fields and defaults. Solvers are matched to the live stack by `Type` in order: a solver whose type moved is moved; a removed type is removed.
- One-goal solvers (`LimbIK`, `Pole`, `SetTransform`, `StretchLimb`) take one goal (`IKRig.SolverGoalLimit`).
- Chains: `Start`, `End` (bones) and optional `Goal`. `RetargetRoot` is the retarget root bone (`""` clears it).
- Renames: `"RenamedFrom": "OldName"` on a goal or chain renames it (keeps its connections and settings) instead of removing the old entry and adding a new one. Renaming or removing a chain does not change the IK Retargeters that use the rig: the apply warns `IKRig.DependentsAffected` and names them; apply their documents next.
- Not authored: the skeleton itself (from `PreviewMesh`), editor state (bone draw size, controller). Automatic setup is not a document key: Inspect's `Suggestions.Body` is what Auto Characterize + Auto FBIK would make (apply it as the Body, or edit it first).

## IK Retargeter Body

```jsonc
"Body": {
  "Source": {"IKRig": "/Game/Rigs/IK_Mannequin.IK_Mannequin"},
  "Target": {"IKRig": "/Game/Rigs/IK_Hero.IK_Hero", "PreviewMesh": "/Game/Meshes/SKM_Hero_Alt.SKM_Hero_Alt"},
  "Ops": [
    {"Name": "Pelvis Motion", "Type": "PelvisMotion", "Settings": {"TranslationAlpha": 0.9}},
    {"Name": "FK Chains", "Type": "FKChains", "ChainMapping": {"Spine": "Spine", "LeftArm": "LeftArm", "Tail": null},
     "Chains": {"LeftArm": {"RotationMode": "Interpolated", "RotationAlpha": 0.8}}},
    {"Name": "Legs IK", "Type": "RunIKRig", "RenamedFrom": "Run IK Rig", "Children": [
      {"Name": "IK Chains", "Type": "IKChains"},
      {"Name": "Stride Warping", "Type": "StrideWarping", "Enabled": false}]},
    {"Name": "Root Motion", "Type": "RootMotion"},
    {"Name": "Curve Remap", "Type": "CurveRemap"}
  ],
  "Poses": {"Target": {"Current": "A-Pose", "Poses": {"Default Pose": {},
    "A-Pose": {"RootOffset": 2.5, "Bones": {"upperarm_l": {"Roll": -45}, "upperarm_r": {"X": 0, "Y": 0, "Z": 0.38, "W": 0.92}}}}}}
}
```

- `Source` / `Target`: `IKRig` (picking a rig points every op at it, as the editor does) and `PreviewMesh` only when it differs from the rig's mesh (`null` = none). A new retargeter with both rigs gets the editor's default op stack when `Ops` is omitted.
- `Ops` is the complete op stack in order (child ops under `Children`); ops are matched by `Name` (ignoring case); `RenamedFrom` renames. `Type` is a `list_types` kind `ikretarget_op` name; singletons (`CurveRemap`, `CopyBasePose`, `ScaleSource`) once; child types (`IKChains`, `SpeedPlanting`, `FloorConstraint`, `StrideWarping`) only under `RunIKRig`. `Enabled` defaults to true.
- `Settings` is sparse over a new op of that type; `Chains` holds per-target-chain settings that differ from the defaults; `TargetIKRig` only when the op uses another rig than the target rig. `ChainMapping` (target chain -> source chain or `null`) is complete when present and kept when omitted.
- Poses per side: `Current` and the named poses (`Default Pose` always listed when `Poses` is). `RootOffset` is a vertical offset (cm); `Bones` are local rotation offsets from the reference pose, as quaternions `{X, Y, Z, W}` (normalized on write) or `{Pitch, Yaw, Roll}` degrees; unlisted bones keep the reference pose. `RenamedFrom` renames a pose. Poses need the side's IK Rig.
- Retarget profiles are not authored (Inspect `Profiles`; `IKRetargeter.ProfilesNotAuthored`).
- Bones no chain maps (e.g. `ik_hand_gun`, `ik_hand_root`, `ik_foot_*`): a `PinBone` op after the chain ops copies one bone's transform to another, as the editor's Pin Bones op: `{"Name": "Pin Gun", "Type": "PinBone", "Settings": {"BonesToPin": [{"BoneToCopyFrom": {"BoneName": "hand_r"}, "BoneToCopyTo": {"BoneName": "ik_hand_gun"}}], "SkeletonToCopyFrom": "Target"}}` (`"Source"`: copy the source skeleton's bone, e.g. an animated `ik_hand_gun` to the target's). `CopyBasePose` copies every same-named source bone as the base pose. list_types `ikretarget_op` shows the other settings (TranslationMode, RotationMode, offsets).
- Known issue: writing a `RootMotion` op's `SourceRoot` / `TargetRoot` / `TargetPelvis` fails the post-save check (`GenericRuntime.VerificationMismatch`) and is rolled back; the op keeps its defaults (its warnings come back as `ValidationWarning`).

## Exporting animations

`retarget_animations` runs the editor's Export Selected Animations / the Content Browser's Retarget Animations (`UIKRetargetBatchOperation`) with this retargeter and saves the new assets (`save: false` leaves them unsaved). Check the result with `sample_animation_bones` (AnimSequence section).

- Input: `assets` (sequences, montages, blend spaces, composites on the source mesh's skeleton) or `folder` (every animation asset under a `/Game` folder; `recursive: false` for that folder only; assets on other skeletons are listed in `Skipped`). `source_mesh` / `target_mesh` default to the retargeter's preview meshes.
- Names: `prefix` + name (with `search` replaced by `replace`) + `suffix`, in `target_folder` (a `/Game` folder) or next to each source. Montages bring their segment animations and (`include_referenced`, default true) the assets they reference, such as blend space samples.
- `existing`: `error` (default: refused with `Retarget.TargetExists` listing the outputs that exist, nothing made), `overwrite` (the window's Overwrite: the new asset takes the old one's name and references; the old one is deleted) or `unique` (a numbered name).
- Result payload: `{Created[{Asset, Class, Source}], Replaced?, Skipped?, Saved[], NotSaved?}`. Bones the chains do not map (e.g. `ik_hand_gun`) follow the retargeter's ops: add a `PinBone` op (above).
- Errors: `Retarget.RetargeterNotFound`, `NoIKRig`, `NoMesh`, `MeshNotFound`, `SameMesh`, `RetargeterInvalid` (it does not initialize), `AssetNotFound`, `NotAnimation`, `WrongSkeleton`, `NoAnimations`, `InvalidFolder`, `NameClash`, `TargetExists`, `PieActive`, `InvalidArgument`.

## Compile

Extract, Apply and Inspect results carry `Compile: {Status, Messages[{Severity, Message, Path?, Source}]}`: the editor's processor initialization (its output log). `Status` is `UpToDate`, `UpToDateWithWarnings`, `Error`, or `Unknown` (no preview mesh / meshes: nothing can be initialized, not a failure). `Path` points at the goal, chain, solver or op a message names. An apply that adds an initialization error fails with `IKRig.ValidationFailed` / `IKRetargeter.ValidationFailed` and is rolled back; errors the asset already had come back as `ValidationError` warnings and warnings as `ValidationWarning`.

## Inspect

IK Rig: `Skeleton` (bones with parents and `Excluded`), `GoalUsers` (solvers and chains per goal), `SolverFacts` (display name, start/end bone use, warning), `ChainBones` (each chain's bones root to tip and whether it resolves). IK Retargeter: `Profiles`, `OpFacts` (singleton, children, parent type, mapping readiness), `RigChains` (each side's chains).

`Suggestions` (read-only, computed on a copy): IK Rig `Body` (Auto Characterize + Auto FBIK), `Characterization` (template, score, missing bones), `AutoIK` (outcome, missing chains). IK Retargeter `AutoAlign.<Side>.ChainToChain` (a pose `{RootOffset?, Bones}` = Auto Align All Bones; needs both preview meshes) and `AutoMap.<op>.Exact|Fuzzy` (the op's `ChainMapping` after Auto Map Chains). Copy a suggestion into the document to use it.

## Discovery

`list_types` kind `ikrig_solver`: solver types with `Settings`, `GoalSettings` and `BoneSettings` fields and defaults. Kind `ikretarget_op`: op types with `Singleton`, `CanHaveChildren`, `ParentType`, `ChainMapping`, `Settings` and `ChainSettings`. Filter with `name` (substring or wildcard).

## Errors

`IKRig.BoneNotFound` (with close bone names), `IKRig.GoalNotFound`, `IKRig.ChainNotFound`, `IKRig.NameTaken`, `IKRig.InvalidName`, `IKRig.UnknownSolverType`, `IKRig.SolverHasNoStartBone` / `SolverHasNoEndBone` / `SolverHasNoGoalSettings` / `SolverHasNoBoneSettings`, `IKRig.BoneSettingRefused`, `IKRig.FieldNotEditable` (a read-only or unknown settings field; the message lists the editable ones), `IKRig.InvalidValue`, `IKRig.UnknownMesh`, `IKRig.CannotClear`, `IKRig.UnknownKey`, `IKRig.NotAuthoredProperty` (a Body-owned or editor-state name in `Properties`). Retargeter: `IKRetargeter.UnknownOpType`, `SingletonOp`, `InvalidParentOp`, `DuplicateName`, `UnknownIKRig`, `UnknownMesh`, `NoTargetIKRig`, `NoChainMapping`, `UnknownTargetChain`, `ChainNotFound`, `NoChainSettings`, `DefaultPoseRequired`, `PoseNotFound`, `IKRigRequired`, `BoneNotFound`, `InvalidValue`, `InvalidBody`. Each is reported at the document path it concerns; a failed apply changes nothing.
