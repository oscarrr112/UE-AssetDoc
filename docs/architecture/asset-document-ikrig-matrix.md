# IK Rig / IK Retargeter support matrix and corpus results

Design: [asset-document-ikrig-design.md](asset-document-ikrig-design.md).
Rules: [design rules](asset-document-design-rules.md) §3 (rollback = functional equivalence), §4
(editor-equivalent code + behaviour test), §5 (support matrix). Reference editors: the IK Rig editor
(`UIKRigController`, the solver stack, the struct viewer) and the IK Retarget editor (`UIKRetargeterController`, the
op stack, edit-pose mode, the toolbar's Auto Align / Auto Map). The agent-facing description is
`MCP/schemas/assetdocument/IKRig.md`.

| Class | Profile | State |
|---|---|---|
| `IKRigDefinition` | `profile.ik_rig.v1` | Extract / Inspect (+ `Suggestions`) / Apply / Diff |
| `IKRetargeter` | `profile.ik_retargeter.v1` | Extract / Inspect (+ `Suggestions`) / Apply / Diff |

Classification: **Supported**, **Read only**, **Refused** (an error code; the editor cannot do it either).

## Editor surfaces

| Editor surface | Body location | Class | Mechanism |
|---|---|---|---|
| New IK Rig on a mesh; preview mesh swap | `Action: Create`, `PreviewMesh` | Supported | `UIKRigDefinitionFactory`, `SetSkeletalMesh` (exclusions re-applied) |
| Goals, solvers (all six types), solver goal / bone settings, excluded bones, chains, retarget root | `Goals`, `Solvers`, `ExcludedBones`, `Chains`, `RetargetRoot` | Supported | `rig.ikrig.edit.v1` through `UIKRigController` and struct-viewer writes |
| Auto Characterize, Auto FBIK | Inspect `Suggestions.Body` | Supported, read-only suggestion | `rig.suggest.v1` on a transient copy |
| New IK Retargeter; pick source / target rig ("assign to all ops") | `Action: Create`, `Source` / `Target.IKRig` | Supported | `UIKRetargetFactory`, `SetIKRig` + `AssignIKRigToAllOps` |
| Source / target preview mesh | `Source` / `Target.PreviewMesh` | Supported | `SetPreviewMesh`; omitted = the rig's mesh, `null` = none |
| Op stack: add (every op type, child ops under Run IK Rig), remove, rename, move, re-parent, enable | `Ops`, `Children`, `Name`, `RenamedFrom`, `Enabled` | Supported | `rig.ikretargeter.edit.v1`: `AddRetargetOp`, `RemoveRetargetOp`, `SetOpName`, `MoveRetargetOpInStack`, `SetParentOpByName`, `SetRetargetOpEnabled` |
| Op Details (settings), per-chain op settings | `Settings`, `Chains` | Supported | struct-viewer writes over a new op's defaults; re-initialized like `UIKRigStructViewer::TriggerReinitIfNeeded` |
| Op chain mapping, Auto Map Chains | `ChainMapping`; Inspect `Suggestions.AutoMap` | Supported | `SetSourceChain`; `AutoMapChains` on a transient copy |
| Op target rig | `TargetIKRig` | Supported | op `IKRigAsset` property + `OnOpPropertyChanged` |
| Retarget poses: new, delete, rename, current; edit-pose rotations, root offset, reset | `Poses.<Side>` | Supported | `CreateRetargetPose`, `RemoveRetargetPose`, `RenameRetargetPose`, `SetCurrentRetargetPose`; edit-pose mode's `SetDeltaRotationForBone` / `SetRootTranslationDelta` / `ResetRetargetPose` |
| Auto Align All Bones | Inspect `Suggestions.AutoAlign` | Supported, read-only suggestion | `AutoAlignAllBones` (ChainToChain) on a transient copy |
| Retarget profiles | Inspect `Profiles` | Read only | `IKRetargeter.ProfilesNotAuthored` (no editor UI in 5.7) |
| Edit an asset open in its editor | — | Supported: close, apply, reopen (Info `IKRig.EditorReopened`, `IKRetargeter.EditorReopened`) | tests `IKRig.OpenEditorApply.Rig`, `IKRig.OpenEditorApply.Retargeter` |
| Interactive editor prompts during an apply | — | Refused | `IKRetargeter.InteractiveEditRefused` (non-interactive scope) |

## Corpus and fixture results

UE 5.7, MoverExamples enabled; MetaHumanCharacter and MoverTests not enabled: those rows skip
with a note), `UEAssetDoc.IKRig.*` 17/17. Times are one run, in ms.

### Read (design §6.1 #1–6)

| Asset | Extract | Inspect | Status | Content |
|---|---|---|---|---|
| MoverExamples `IK_Mannequin` (legacy UObject solvers, PostLoad conversion) | 42 | 43 | UpToDate | 4 goals, 1 solver, 50 chains |
| MoverExamples `RTG_Mannequin` (oldest format; rigs at a missing `/Game` path) | 19 | 43 | Unknown (no rigs resolve) | 4 ops |
| fixture `IK_Tutorial` / `RTG_Tutorial` | 26 / 37 | 38 / 59 | UpToDate / UpToDateWithWarnings | Auto Characterize + Auto FBIK; default op stack |
| MetaHumanCharacter #1–2, MoverTests #5–6 | — | — | skipped (plugin not enabled) | |

Extract is stable (twice equal, equal after reload) and reading dirties no package.

### IK Rig apply (§6.2: extract → Create → equal extract and status → unchanged re-apply no-op → reload)

| Source | Create | Unchanged re-apply | Status |
|---|---|---|---|
| fixture `IK_Tutorial` (Auto Characterize + Auto FBIK) | 251 | 44 | UpToDate |
| fixture `IK_Plain` (New IK Rig on a mesh) | 195 | 38 | UpToDate |
| MoverExamples `IK_Mannequin` | 271 | 42 | UpToDate |

§6.3 rig free edits (document side 280 ms), §6.4 rig from scratch, Undo, StructWrites, OpenEditorApply.Rig: pass.

### IK Retargeter apply (§6.2)

| Source | Ops | Create | Unchanged re-apply | Status (equal on both) |
|---|---|---|---|---|
| fixture `RTG_Plain` (factory default ops) | 5 | 218 | 55 | UpToDateWithWarnings (tutorial `Root` chain too short; Root Motion bones unset) |
| fixture `RTG_Rich` (every op type incl. the 4 child types, a disabled op, an auto-aligned target pose, a renamed source pose with a rotation and a root offset; Tutorial → Manny) | 12 | 275 | 66 | UpToDateWithWarnings |
| MoverExamples `RTG_Mannequin`, re-pointed at a `/Game` copy of its `IK_Mannequin` (Details pick of both rigs + "assign to all ops") | 4 | 299 | 79 | UpToDateWithWarnings (Pole Vector op not initialized, as in the source) |
| MetaHumanCharacter `RTG_MH_IKRig` | — | — | — | skipped (plugin not enabled) |

- §6.3 retargeter free edits (`RetargeterFreeEdits`): on duplicate A through the controller calls the editor makes
  (add + name a Pin Bone op, remove Filter Bone, move Root Motion to the top, enable / disable ops, rename Run IK Rig,
  Pelvis Motion Details, FK unmap of a chain, FK chain row Details, new target pose with a bone rotation and a root
  offset, rename the source pose, remove the aligned pose, set the current pose); A's extract (with `RenamedFrom`)
  applied to duplicate B (358 ms): equal extracts, equal status, empty Diff, the renamed Run IK Rig keeps its 4
  children.
- `RetargeterPoses`: a rotator, an approximate quaternion (normalized on write) and an exact one read back as stored,
  stable after reload; re-applying the extract is a no-op; removing a pose falls back to Default Pose.
- `RetargeterUndo`: an AI apply clears undo history referencing the retargeter and keeps the human's saved edit; a
  failed apply rolls back to the human state; undo / redo stay whole.
- `OpenEditorApply.Rig` / `OpenEditorApply.Retargeter`: with the asset open in its editor and not saved, an apply
  (retarget root; an op disabled) closes the editor and reopens it on the result (a new editor whose controller reads
  the applied asset); a failed apply rolls back and the editor reads the rolled-back asset; a human edit through the
  reopened editor's controller is saved, undone and redone; the editor closes without a prompt.

### AI surface

| Check | Result |
|---|---|
| IK Rig `Suggestions.Body` = what Auto Characterize + Auto FBIK make (`Suggest`) | equal; Inspect with suggestions 23 ms (Tutorial, template "UE4 Mannequin", score 1.0, 53 bones) |
| Retargeter `Suggestions.AutoAlign.Target.ChainToChain` = Auto Align All Bones on a duplicate | equal (35 bones); Inspect with suggestions 64 ms |
| Reading suggestions changes nothing | no dirty package, no undo history, the asset extracts as before |
| §6.4 from scratch (`RetargeterFromScratch`): two rigs from a mesh-only document + their suggestions, a retargeter with an AI-written op stack, the auto-aligned target pose made current | status UpToDateWithWarnings (Head chain too short), FK Chains auto-mapped ≥ 8 chains, Diff empty, re-apply no-op |
| Authoring errors at their document paths, nothing changed | unknown op type, second Curve Remap, child op at the top level, unknown op setting, unknown target chain, pose bone not in the skeleton, Default Pose removed, a non-IK-Rig as `IKRig` |
| A failed Create leaves nothing | no object, no file |

## Known limits

- Retarget profiles are read only (no editor UI in 5.7).
- Only `ChainToChain` auto-align is computed by Inspect (`rig.suggest.v1` takes `methods` for the others).
- The processor's own log sometimes reports a too-short chain once per side and sometimes once; status equality is
  checked within one run, where it is stable.
- MetaHuman and MoverTests content is not covered (plugins not enabled).
