# PoseAsset Profile

Classes: /Script/Engine.PoseAsset

Exact profile for Pose Assets: the Pose Asset editor's poses, curves and additive settings. Omitted Body key = keep.

Apply works with the asset open in its Pose Asset editor: the pose list and the preview show the applied (or rolled-back) poses.

```jsonc
"Body": {
  "SourceAnimation": "/Game/Anims/Face_Poses.Face_Poses",
  "Skeleton": "/Game/Characters/SK_Face_Skeleton.SK_Face_Skeleton",
  "PreviewMesh": "",
  "Poses": [{"Name": "Neutral", "RenamedFrom": "Pose_0"},
            {"Name": "Smile", "Curves": {"CTRL_Smile": 1.0}, "Bones": {"jaw": {"Rotation": {"Pitch": 0, "Yaw": 0, "Roll": -12}}}}],
  "Curves": ["CTRL_Smile", "CTRL_Blink"],
  "Additive": true,
  "BasePose": "Neutral"
}
```

- Create needs `SourceAnimation` (an Animation Sequence; `PoseAsset.SourceAnimationRequired`), as the editor's Create Pose Asset: one pose per frame, named `Pose_0`, `Pose_1`, ... `Skeleton` is the source's and read only (`PoseAsset.SkeletonImmutable`).
- `Poses` is the full list in the asset's order: list a pose to keep it, `RenamedFrom` to rename it, leave it out to delete it. The editor cannot reorder poses (`PoseAsset.PoseOrderUnsupported`).
- A pose's `Curves` (curve -> value; curves at 0 left out) and `Bones` (bone -> offset from the Skeleton's reference pose: `Location` added, `Rotation` applied on top, `Scale` factor; identity parts left out) are read only (`PoseAsset.PoseDataReadOnly`). A pose without them is written as its name; leaving them out of a pose row skips the check.
- Body `Curves` lists the asset's curves: leave one out to delete it from every pose (the pose list's Delete Curves). Curves cannot be added (`PoseAsset.CurveAddUnsupported`).
- New poses: a Pose Asset with a source animation gets its poses from it (`PoseAsset.PoseAddUnsupported`). With `"SourceAnimation": ""` a pose not in the asset is added from the reference pose (Add Pose from Reference), after all existing poses.
- `"SourceAnimation": ""` clears the Source Animation row (the poses stay). Setting an animation runs the editor's Update Source: the poses and curves are re-read from it (leave `Curves` and the poses' `Curves` / `Bones` out then; `PoseAsset.PoseDataReadOnly`).
- `Additive` / `BasePose` are applied together (the Details panel's Apply). `BasePose` is a pose name or `""` (the reference pose); renaming the base pose keeps it.
- Other Details settings (`RetargetSource`, ...) are top-level `Properties`.
