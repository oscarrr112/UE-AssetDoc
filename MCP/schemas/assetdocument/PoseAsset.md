# PoseAsset Profile

Classes: /Script/Engine.PoseAsset

Exact profile for Pose Assets: the Pose Asset editor's poses and additive settings. Omitted Body key = keep.

Apply works with the asset open in its Pose Asset editor: the pose list and the preview show the applied (or rolled-back) poses.

```jsonc
"Body": {
  "SourceAnimation": "/Game/Anims/Face_Poses.Face_Poses",
  "Skeleton": "/Game/Characters/SK_Face_Skeleton.SK_Face_Skeleton",
  "PreviewMesh": "",
  "Poses": [{"Name": "Neutral", "RenamedFrom": "Pose_0"}, "Pose_1", "Smile"],
  "Additive": true,
  "BasePose": "Neutral"
}
```

- Create needs `SourceAnimation` (an Animation Sequence; `PoseAsset.SourceAnimationRequired`), as the editor's Create Pose Asset: one pose per frame, named `Pose_0`, `Pose_1`, ... `Skeleton` is the source's and read only (`PoseAsset.SkeletonImmutable`).
- `Poses` is the full list in the asset's order: list a pose to keep it, `RenamedFrom` to rename it, leave it out to delete it. The editor cannot add or reorder poses (`PoseAsset.PoseAddUnsupported` lists the poses; `PoseAsset.PoseOrderUnsupported`).
- `Additive` / `BasePose` are applied together (the Details panel's Apply). `BasePose` is a pose name or `""` (the reference pose); renaming the base pose keeps it.
- Changing `SourceAnimation` runs the editor's Update Source: the poses are re-read from the new animation.
- Pose curves and bone tracks are read only (Inspect: `PoseAssetDetails`). Other Details settings (`RetargetSource`, ...) are top-level `Properties`.
