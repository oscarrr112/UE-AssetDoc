# AnimComposite Profile

Classes: /Script/Engine.AnimComposite

Exact profile for Anim Composites: the segment track of the Anim Composite editor. Omitted Body key = keep.

Apply works with the composite open in its Animation editor: the segment track and the preview show the applied (or rolled-back) segments.

```jsonc
"Body": {
  "Skeleton": "/Game/Characters/SK_Hero_Skeleton.SK_Hero_Skeleton",
  "PreviewMesh": "",
  "Segments": [{"Animation": "/Game/Anims/Jump_Start.Jump_Start"},
               {"Animation": "/Game/Anims/Jump_Loop.Jump_Loop", "StartTime": 0.1, "EndTime": 0.6, "PlayRate": 1.5, "LoopCount": 3}]
},
"Properties": {"bLoop": false}
```

- `Skeleton` is required to create (`AnimComposite.SkeletonRequired`) and cannot change afterwards (`AnimComposite.SkeletonImmutable`).
- `Segments` is the ordered track; segments play back to back (their timeline positions follow and are read only; Inspect `CompositeDetails` lists them with the length). `StartTime` / `EndTime` are times in the segment's animation (default: all of it), `PlayRate` (default 1, not 0), `LoopCount` (default 1). Omitted on a segment whose index and animation are unchanged = keep.
- Animations are Animation Sequences or other Anim Composites with a compatible skeleton and one additive type; a composite cannot play itself, directly or through another composite (`AnimComposite.IncompatibleAnimation`).
- Notifies and curves of a composite are not authored by this profile; other Details settings are top-level `Properties`.
