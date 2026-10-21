# AnimSequence Profile

Classes: /Script/Engine.AnimSequence

Class `/Script/Engine.AnimSequence`: authored state of a sequence (post-import). It does not author import settings or compressed output, and it creates a new sequence from `References.Skeleton`. `Body` keys (unknown keys fail with `AnimSequence.UnknownBodyKey`; `_Skipped` is extract-only):

Apply works with the sequence open in its Animation editor: the timeline, curve and notify panels and the preview show the applied (or rolled-back) sequence.

- `References` (`Skeleton`, `RetargetSource`, `RetargetSourceAsset`), `Preview` (`PreviewMesh`, `PreviewPoseAsset`), `Playback` (`RateScale`, `bLoop`, `Interpolation`, `NumberOfFrames`, `FrameRate`), `Additive` (`AdditiveAnimType`, `RefPoseType`, `RefFrameIndex`, `RefPoseSeq`), `RootMotion`, `Compression`: objects. The existing sequence's skeleton is read-only (must equal the live one).
- `Curves` (float curves), `NotifyTracks`, `Notifies`, `NotifyStates`, `SyncMarkers`, `Metadata` (instanced `UAnimMetaData`), `AssetUserData`, `BoneTracks`: arrays.
- Omitted region = keep, a present region is the complete state, `[]` clears. Other plain editable fields go in `Properties` (names owned by a Body region are rejected with `AnimSequence.ManagedPropertyOverlap`).

Extract a real sequence for the exact shapes.

## Reading animation data

- `inspect_asset_document_target` on a sequence gives `Animation` (PlayLength, Frames, FrameRate, `RateScale`, `PlayDuration` = PlayLength / |RateScale| when it is not 1, RootMotion, Notifies) and `Motion`: `RootDisplacement` / `RootDistance` / `RootYawChange` of the root bone from the first to the last frame and the pelvis (`Pelvis.First`, `Last`, `Displacement`), component space, root as authored. A zero `RootDisplacement` with a moving pelvis is an in-place animation; a travelling root is root motion (`bEnableRootMotion` decides whether the capsule takes it).
- Any bone at any time: `sample_animation_bones` (an AnimSequence, AnimComposite or AnimMontage; read-only; the engine's pose evaluation of the raw data, an additive animation on its base pose). Times are on the asset's timeline (where notifies are); each sample adds `PlayTime` = Time / RateScale when RateScale is not 1. Bone keys are not written by documents (`BoneTracks` is read-only): bones the retarget chains do not map come from the IK Retargeter's ops (IKRig section, `PinBone`).

### sample_animation_bones

- `bones`: bone or socket names (skeleton or mesh sockets). `times` (seconds), or `start` / `end` / `step` (default 0, the length, one frame; `end` included).
- `space`: `component` (default, the skeletal mesh component's space), `local` (relative to the parent bone) or `root` (relative to the root bone: root motion removed). `root`: `pose` (default, the root as authored: the character's travel) or `extracted` (the root reset as root motion extraction does: `bEnableRootMotion` with `RootMotionRootLock`, or `bForceRootLock`).
- `speed: true` adds each bone's `Speed`, cm per second of play since the previous sample (weapon-tip speed for hit windows). `mesh`: evaluate with that skeletal mesh's proportions and sockets (same or compatible skeleton).
- A Montage is sampled on one slot (`slot`, default its first slot track) through its segments: each sample names `Segment`, `Anim` and `AnimTime`; `Gap: true` between segments.
- Result payload: `{Asset, PlayLength, RateScale, Space, Root {Mode, EnableRootMotion, RootMotionRootLock, ForceRootLock, RootLocked}, Applied[] (what applied, in words), Bones, Sockets?, Samples[{Time, PlayTime?, <bone>: {Location [X,Y,Z] cm, Rotation [Pitch,Yaw,Roll] deg, Scale?, Speed?}}]}`.
- At most 64 bones, 1000 times and 2000 transforms per call. Errors: `AnimationSample.BoneNotFound` (close names), `TimeOutOfRange` (names the length), `TooManySamples`, `SlotNotFound`, `NotAnimation`, `InvalidArgument`.

