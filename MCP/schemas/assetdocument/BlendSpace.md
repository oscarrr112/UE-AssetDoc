# BlendSpace Profiles

Classes: /Script/Engine.BlendSpace, /Script/Engine.BlendSpace1D, /Script/Engine.AimOffsetBlendSpace, /Script/Engine.AimOffsetBlendSpace1D

Exact profiles for Blend Spaces and Aim Offsets (2D: axes X and Y; the `1D` classes: X only). Omitted Body key = keep.

Apply works with the asset open in its Blend Space editor: the grid and the preview show the applied (or rolled-back) samples and axes.

```jsonc
"Body": {
  "Skeleton": "/Game/Characters/SK_Hero_Skeleton.SK_Hero_Skeleton",
  "PreviewMesh": "/Game/Characters/SK_Hero.SK_Hero",
  "Axes": {"X": {"Name": "Direction", "Min": -180, "Max": 180, "GridDivisions": 4, "SnapToGrid": true, "WrapInput": false},
           "Y": {"Name": "Speed", "Min": 0, "Max": 600, "GridDivisions": 4}},
  "Samples": [{"Animation": "/Game/Anims/Idle.Idle", "X": 0, "Y": 0},
              {"Animation": "/Game/Anims/Walk.Walk", "X": 0, "Y": 300, "RateScale": 1.2, "SingleFrame": null, "IncludeInAnalyseAll": true}]
},
"Properties": {"TargetWeightInterpolationSpeedPerSec": 2.5, "bLoop": true}
```

- `Skeleton` is required to create (the New Blend Space dialog's skeleton; `BlendSpace.SkeletonRequired`) and cannot change afterwards (`BlendSpace.SkeletonImmutable`).
- `Axes`: omitted fields keep their value; `Min` < `Max`; `GridDivisions` 1..1000.
- `Samples` is the full sample list. A sample is identified by its position (X, Y), which the editor keeps unique (`BlendSpace.DuplicateSample`): moving a sample is removing it and adding a new one. Positions must be inside the axis range (`BlendSpace.SampleOutOfRange`: widen the axis in the same document) and, when every axis snaps to its grid, on a grid point (`BlendSpace.SampleOffGrid`). `RateScale` (0.01..64, default 1), `SingleFrame` (frame index or `null` = whole animation) and `IncludeInAnalyseAll` (default true): omitted on a new sample = default, on an existing sample = keep.
- Animations need a compatible skeleton and the additive type the class takes; Aim Offsets take mesh-space or local-space rotation offset animations only (`BlendSpace.IncompatibleAnimation`).
- Other Details settings (sample smoothing `InterpolationParam`, `TargetWeightInterpolationSpeedPerSec`, notifies mode, ...) are top-level `Properties`; after they change the blend space is re-validated and resampled, as the editor does.
- Inspect: `BlendSpaceDetails` (`SampleCount`, invalid samples).
