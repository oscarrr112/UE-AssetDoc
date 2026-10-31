# AnimComposite Profile

Classes: /Script/Engine.AnimComposite

Exact profile for Anim Composites: the segment track, curves and notifies of the Anim Composite editor. Omitted Body key = keep.

Apply works with the composite open in its Animation editor: the segment track and the preview show the applied (or rolled-back) segments.

```jsonc
"Body": {
  "Skeleton": "/Game/Characters/SK_Hero_Skeleton.SK_Hero_Skeleton",
  "PreviewMesh": "",
  "Segments": [{"Animation": "/Game/Anims/Jump_Start.Jump_Start"},
               {"Animation": "/Game/Anims/Jump_Loop.Jump_Loop", "StartTime": 0.1, "EndTime": 0.6, "PlayRate": 1.5, "LoopCount": 3}],
  "Curves": [], "NotifyTracks": [{"Name": "1"}], "Notifies": [{"Time": 0.2, "Track": "1", "Name": "Jump"}], "NotifyStates": []
},
"Properties": {"bLoop": false}
```

- `Skeleton` is required to create (`AnimComposite.SkeletonRequired`) and cannot change afterwards (`AnimComposite.SkeletonImmutable`).
- `Segments` is the ordered track; segments play back to back (their timeline positions follow and are read only; Inspect `CompositeDetails` lists them with the length). `StartTime` / `EndTime` are times in the segment's animation (default: all of it), `PlayRate` (default 1, not 0), `LoopCount` (default 1). Omitted on a segment whose index and animation are unchanged = keep. A segment whose animation was deleted reads `"Animation": null` with `AnimComposite.MissingReference`, and a saved segment value the Details rows would refuse (`LoopCount` 0) reads with `AnimComposite.InvalidSegment`; both are kept while the document keeps them, and a new one is refused.
- Animations are Animation Sequences or other Anim Composites with a compatible skeleton and one additive type; a composite cannot play itself, directly or through another composite (`AnimComposite.IncompatibleAnimation`).
- `Curves`, `NotifyTracks`, `Notifies` and `NotifyStates` take the AnimSequence profile's forms, rules and codes (`AnimSequence.*`); times are within the composite's length after the segments apply. Other Details settings are top-level `Properties`.
