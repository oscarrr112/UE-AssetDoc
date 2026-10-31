# AnimSequence Profile

Classes: /Script/Engine.AnimSequence

Class `/Script/Engine.AnimSequence`: authored state of a sequence (post-import). It does not author import settings or compressed output, and it creates a new sequence from `References.Skeleton`. `Body` keys (unknown keys fail with `AnimSequence.UnknownBodyKey`):

Apply works with the sequence open in its Animation editor: the timeline, curve and notify panels and the preview show the applied (or rolled-back) sequence.

- `References` (`Skeleton`, `RetargetSource`, `RetargetSourceAsset`), `Preview` (`PreviewMesh`, `PreviewPoseAsset`), `Playback` (`RateScale`, `bLoop`, `Interpolation`, `NumberOfFrames`, `FrameRate`), `Additive` (`AdditiveAnimType`, `RefPoseType`, `RefFrameIndex`, `RefPoseSeq`), `RootMotion`, `Compression`: objects. The existing sequence's skeleton is read-only (must equal the live one).
- `Curves` (float curves), `NotifyTracks`, `Notifies`, `NotifyStates`, `SyncMarkers`, `Metadata` (instanced `UAnimMetaData`), `AssetUserData`: arrays.
- Read-only arrays (give them as extracted or omit them, else `AnimSequence.ReadOnlyRegion`; apply keeps them): `BoneTracks` (animated bone names), `TransformCurves` (`{Name, KeyCount, Start, End}`: bone offsets keyed on the additive layer tracks) and `Attributes` (`{Bone, Name, Type, KeyCount, Start, End}`: animated bone attributes such as imported timecode). The last two are listed when the sequence has any; `Start` / `End` are the first and last key times.
- Not in the document, and kept by apply: bone, transform-curve and attribute key values, import settings, compressed data, and Details values that are only shown (`ParentAsset`, `AssetMappingTable`, `PlatformTargetFrameRate`).
- Two notify tracks with one name (a track renamed in the Notifies panel) read with `AnimSequence.DuplicateNotifyTrack`; rows name the second such track `"<name>#2"`. Two notifies on one track and time do not stay in a sequence: UE moves one to another track when it loads, so they read on two tracks; a document that puts two there is refused with `AnimSequence.OverlappingNotifies`. A `RefPoseSeq` the additive settings hide (an older asset) reads with `AnimSequence.RefPoseSeqHidden`. These warnings mean the asset keeps the value; a document that adds or changes such a value is refused with the same code.
- A notify, notify state, metadata or user-data row keeps the existing object it stands for, so inserting, removing or moving rows keeps each row's object. Extract writes a notify's `Guid` (32 hex digits) where it names that notify alone; a row with that `Guid` keeps that notify (of its class or name), wherever it moves. A new row has no `Guid` (two rows with one `Guid` keep nothing by it). Other notify rows keep a notify of their class or name, nearest first: at the same time and track, else the nearest in time. `Guid` is never compared or written. A metadata or user-data row keeps an equal object of its class, else the next one. A Details value with no document form reads in the row's `Properties` as `{"Unsupported": reason, "Text": export text}` with `AnimSequence.Unrepresented`: leave it as extracted (the kept object keeps it) or omit it; an edited marker, or one on a new row, is refused with `AnimSequence.UnrepresentedValue`.
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

