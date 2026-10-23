# AnimSequence support matrix (legacy C++ profile vs Python route)

Governing rules:
[design rules](asset-document-design-rules.md) §3 (rollback = functional
equivalence), §4 (raw editor-equivalent code in its own file + behaviour test) and §5 (support
matrix, no capability regression).

`/Script/Engine.AnimSequence` (exact class) is read, created and updated by the Python profile
`profile.anim_sequence.v1` (`Content/Python/asset_document/profiles/anim_sequence.py`). The legacy
C++ `FAnimSequenceAssetDocumentProfile` stays in the tree but is no longer registered.

Columns: **Legacy** = the retired `FAnimSequenceAssetDocumentProfile`; **Python** = the current profile.
Classification: **Supported** (Extract + Apply + Diff), **Read-only** (Extract/Diff only; authoring
a different value fails with a stable diagnostic), **Unsupported** (not in the document; reason given).

## 1. What a human edits, and where it lives

### Details panel: asset settings

| Editor surface | Body location | Legacy | Python | Class | Mechanism (Python) |
|---|---|---|---|---|---|
| Skeleton, at creation (New Animation → pick skeleton) | `References.Skeleton` | ➖ (no create) | ✅ | Supported | `editor.asset.create_by_factory.v2`, Skeleton = `factory_context` (`UAnimSequenceFactory::TargetSkeleton`) |
| Skeleton of an existing sequence | `References.Skeleton` | Read-only (mismatch rejected) | Read-only | Read-only | must equal the live skeleton (`AnimationProvider.Authoring.SkeletonMismatch`); see §3 |
| Retarget Source (name) | `References.RetargetSource` | ✅ | ✅ | Supported | Editor property batch (`"None"` = no name; `""` accepted) |
| Retarget Source Asset | `References.RetargetSourceAsset` | ✅ | ✅ | Supported | Editor property batch (soft path; `null` ↔ the codec's `"None"`) |
| Preview mesh | `Preview.PreviewMesh` | ✅ | ✅ | Supported | `animation.asset.authoring.edit.v1` (`UAnimationAsset::SetPreviewMesh`); the mesh must use a compatible skeleton, as the picker filters (`AnimationProvider.Authoring.SkeletonMismatch`) |
| Preview pose asset | `Preview.PreviewPoseAsset` | ➖ | ✅ | Supported | authoring edit, hard object reference |
| Rate Scale | `Playback.RateScale` | ✅ | ✅ | Supported | Editor property batch |
| Loop (`bLoop`) | `Playback.bLoop` | ➖ | ✅ | Supported | Editor property batch |
| Interpolation (Linear / Step) | `Playback.Interpolation` | ➖ | ✅ | Supported | Editor property batch |
| Length (number of frames: timeline insert/remove/resize) | `Playback.NumberOfFrames` | ➖ | ✅ | Supported | authoring edit (`IAnimationDataController::SetNumberOfFrames`) |
| Sampling frame rate | `Playback.FrameRate` | ➖ | ✅ | Supported | authoring edit (`IAnimationDataController::SetFrameRate`) |
| Additive Anim Type | `Additive.AdditiveAnimType` | ✅ | ✅ | Supported | Editor property batch |
| Base Pose Type | `Additive.RefPoseType` | ✅ | ✅ | Supported | Editor property batch |
| Ref Frame Index | `Additive.RefFrameIndex` | ✅ | ✅ | Supported | Editor property batch |
| Base Pose Animation | `Additive.RefPoseSeq` | ✅ | ✅ | Supported | authoring edit, hard object reference; UE keeps it only with an additive type and `RefPoseType` `ABPT_AnimScaled`/`ABPT_AnimFrame` (`AnimSequence.RefPoseSeqHidden` when the document itself hides it) |
| Root motion: Enable / Root Motion Root Lock / Force Root Lock / Normalized scale | `RootMotion.*` | ✅ | ✅ | Supported | Editor property batch |
| Compression error threshold scale | `Compression.CompressionErrorThresholdScale` | ✅ | ✅ | Supported | Editor property batch |
| Do Not Override Compression | `Compression.bDoNotOverrideCompression` | ✅ | ✅ | Supported | Editor property batch |
| Bone / Curve Compression Settings asset | `Compression.BoneCompressionSettings`, `.CurveCompressionSettings` | ✅ | ✅ | Supported | authoring edit, hard object reference |
| Allow Frame Stripping | `Compression.bAllowFrameStripping` | ➖ | ✅ | Supported | Editor property batch |
| Variable Frame Stripping Settings | `Compression.VariableFrameStrippingSettings` | ➖ | ✅ | Supported | authoring edit, hard object reference |
| Any other plain editable field (e.g. `StripAnimDataOnDedicatedServer`) | `Properties` | ➖ | ✅ | Supported | Editor property batch; Body-owned names are rejected there (`AnimSequence.ManagedPropertyOverlap`) |
| Meta Data (instanced `UAnimMetaData`) | `Metadata` | ✅ (managed objects only) | ✅ | Supported | authoring edit (`AddMetaData`/`EmptyMetaData`) |
| Asset User Data (instanced) | `AssetUserData` | ✅ (managed objects only) | ✅ | Supported | authoring edit (`AddAssetUserData`/`RemoveUserDataOfClass`) |
| Import settings: `ImportFileFramerate`, `ImportResampleFramerate`, `AssetImportData` | — | ➖ | ➖ | Unsupported | VisibleAnywhere import provenance; only a reimport changes them |
| Platform Target Frame Rate (`PlatformTargetFrameRate`) | — | ➖ | ➖ | Unsupported | VisibleAnywhere, derived per platform |
| Parent asset / asset mapping table | — | ➖ | ➖ | Unsupported | VisibleAnywhere; the child-asset feature is Montage-oriented and not editable on a sequence |
| Thumbnail info | — | ➖ | ➖ | Unsupported | editor thumbnail state, VisibleAnywhere |

### Timeline (notify / curve / marker panels)

| Editor surface | Body location | Legacy | Python | Class | Mechanism |
|---|---|---|---|---|---|
| Notify tracks: add, remove, rename, reorder | `NotifyTracks` | ✅ | ✅ | Supported | authoring edit (`AnimNotifyTracks`, raw edit like the notify panel) |
| Add/move/delete a notify (class-based `UAnimNotify`) | `Notifies` | ✅ (managed objects only) | ✅ (all notifies) | Supported | authoring edit (raw edit like `SAnimNotifyPanel`) |
| Skeleton (named) notify | `Notifies[]` with `Name`, no `Class` | ➖ | ✅ | Supported | as above; the Skeleton's notify-name list is not edited (it is another package) |
| Notify object properties (e.g. PlaySound `Sound`, `VolumeMultiplier`, the notify's `NotifyColor`) | `Notifies[].Properties` | ✅ | ✅ | Supported | admitted editor codecs + hard object references |
| Notify trigger settings (chance, weight threshold, filter type/LOD, dedicated server, follower, montage tick type, filterable by request) | `Notifies[]` / `NotifyStates[]` fields | ➖ | ✅ | Supported | authoring edit |
| Notify states (`UAnimNotifyState`) with duration | `NotifyStates` | ✅ (managed objects only) | ✅ | Supported | authoring edit |
| Sync markers (add / move / delete / rename, per track) | `SyncMarkers` | ✅ | ✅ | Supported | authoring edit (`AuthoredSyncMarkers` + `RefreshSyncMarkerDataFromAuthored`) |
| Float curves: add / remove / rename (= remove + add), keys, interpolation, tangents | `Curves[].Keys` | ✅ (sparse patch, Linear/Constant/Cubic only) | ✅ (full replace, all key fields) | Supported | authoring edit (`IAnimationDataController` `AddCurve`/`RemoveCurve`/`SetCurveKeys`) |
| Float curve type flags (Editable / Metadata curve) | `Curves[].Flags` | ✅ | ✅ | Supported | `SetCurveFlags` |
| Float curve color / comment | `Curves[].Color`, `.Comment` | ➖ | ✅ | Supported | `SetCurveColor`, `SetCurveComment` |
| Bone tracks present on the sequence | `BoneTracks` | ➖ | ✅ (names) | Read-only | Extract lists the animated bones; authoring a different list fails (`AnimSequence.ReadOnlyRegion`) |
| Raw bone-track keys (position/rotation/scale per frame) | — | ➖ | ➖ | Unsupported | see §3 |
| Transform curves / additive layer tracks (Persona "Key" on a bone) | — | ➖ | ➖ | Unsupported | see §3 |
| Custom attribute curves (`AttributeCurves`) | — | ➖ | ➖ | Unsupported | see §3 |
| Notify link method / slot (`FAnimLinkableElement`) | — | ➖ | ➖ | Unsupported | Montage-only concept; sequence notifies are absolute-time |
| Notify / marker GUIDs, notify track colors | — | ➖ | ➖ | Unsupported | editor identity/display state the panels regenerate (GUIDs are kept for reused rows; new tracks are white like the panel's) |

### Document operations

| Operation | Legacy | Python |
|---|---|---|
| Create + populate in one `ApplyFile` | ➖ | ✅ (needs `References.Skeleton`) |
| Update | ✅ | ✅ |
| Apply into an open Animation editor (timeline, curves, notifies and preview refreshed) | ➖ | ✅ (`AnimSequence.OpenEditorApply`) |
| Extract / Inspect / Diff / Validate / Schema / Template | ✅ | ✅ |
| Unchanged re-apply is a no-op (no save, bytes unchanged) | ✅ | ✅ (exact `diff_exact` feeds the route's no-op probe) |
| `Definitions` / `DefinitionRef` | ✅ | ➖ (`AnimSequence.UnsupportedDefinitions`) |
| Legacy input spellings | — | accepted: `{Kind: AssetRef\|ClassRef, Path}` wrappers, `{Kind: EmbeddedObject, Class, Properties}` rows, `TrackName` for `Track`; Extract writes the plain forms |

Counts (rows of the two surface tables): **Supported 33, Read-only 2, Unsupported 9.**

## 2. Region semantics

- Omitted Body region = keep as is. A given list region is its full desired state: `[]` clears
  (`Curves`, `Notifies`, `NotifyStates`, `SyncMarkers`, `Metadata`, `AssetUserData`). Settings
  regions are sparse per field: `{}` changes nothing, an omitted field keeps its value.
- Inside a row, omitted optional fields keep the reused row's value or take the UE default, and
  are not compared: curve `Flags`/`Color`/`Comment`, notify trigger settings, object `Properties`
  keys. A new curve without `Flags` gets the editor default (`Editable`). Extract always writes
  curve `Color` (UE derives a per-name default), trigger settings only when they differ from the
  UE default, and a notify `Name` only for a named (class-less) notify.
- `NotifyTracks` cannot be `[]` (`AnimSequence.NotifyTrackRequired`): UE recreates track `"1"`
  whenever the list is empty (`UAnimSequenceBase::InitializeNotifyTrack`). Rows whose region is
  not authored keep their track by name; removing a track still used by such a row fails with
  `AnimationProvider.Authoring.NotifyTrackInUse`.
- `Track` is optional and means the first notify track. Two notifies (events or states) on one
  track at the same time are rejected (`AnimSequence.OverlappingNotifies` in the document,
  `AnimationProvider.Authoring.OverlappingNotifies` against kept rows), because UE would silently
  move one to another track (`UAnimSequenceBase::RefreshCacheData`).
- Notify / marker times must lie in `[0, PlayLength]`, a state's `Time + Duration` too
  (`AnimationProvider.Authoring.TimeOutOfRange`). `Playback.NumberOfFrames`/`FrameRate` are
  applied first, so one document can lengthen the sequence and place notifies in the new range.
- Notify, metadata and user-data rows reuse the existing object at the same index when the class
  matches (properties written in place, GUIDs kept), like editing in the details panel.
- Apply order: the Editor property batch (plain fields) first, then one authoring edit — as a
  human sets the additive type before picking the base pose, which UE would otherwise clear.
- Curve key tangents are compared only where they are authored state (`TangentMode` User/Break
  outside Constant segments); Auto tangents are recomputed by UE (`FRichCurve::AutoSetTangents`).
- Floats compare at stored float32 precision. Primitive diagnostics are reported at the document
  path the author wrote (e.g. `/Body/Notifies/0/Properties`).

## 3. Unsupported, and why

| Surface | Why |
|---|---|
| Raw bone-track keys | Dense per-frame transform data produced by import (FBX/USD), Control Rig/Sequencer baking or retargeting, not by hand-editing in the sequence editor. A JSON key dump would be megabytes, useless for an AI author, and unsafe against the compressed-data/DDC pipeline. Reimport or bake tools are the right channel. Bone *names* are exposed read-only. |
| Transform curves / additive layer tracks | Persona's bone "Key" tool writes temporary additive transform curves that are baked into bone tracks; same data class as raw keys. |
| Custom attribute curves | Import-produced typed attribute keys with no editor key UI. |
| Changing the Skeleton of an existing sequence | That is "Replace Skeleton"/retarget, an asset-level operation that remaps every track; not a field edit. Authoring a different skeleton fails with `AnimSequence.SkeletonMismatch`. |
| Import data, platform frame rate, parent asset, thumbnail | VisibleAnywhere (not editable by a human either). |
| Skeleton notify-name registry | Adding a named notify in Persona also adds the name to the Skeleton asset. That is another package; snapshot-covered requests may only mutate their target. The named notify still fires. |
| Properties of shared (non-instanced) metadata / user-data pointers | Observed by class only (`Properties: {}`), never written (it would mutate a shared object). |

## 4. Rollback and new C++

- Existing target: Core package snapshot (`core.package-snapshot.v1`); created target: factory
  rollback deletes the asset. Preflight returns `SnapshotCoveredMutationV1`; no typed inverse code.
- The handler sets `preflight_scratch = False`, so the generic runtime preflights it without a
  scratch preview (`registered_runtime._lifecycle`). The generic `editor.scratch.clone_asset.v2`
  cannot clone a sequence anyway: Control Rig's MovieScene track inside the data model creates
  non-transient easing objects during duplication (`Scratch.CloneFailed`, now naming the object).
- The new primitives are request-bound to the Animation request owner (like
  `animation.montage.settings.set.v1`): `FAnimationRequestScope` now binds AnimSequence requests
  too, and the owner admits them only for the exact `UAnimSequence` target of the request.
- New C++ (Animation provider, class-neutral over `UAnimSequenceBase`, admitted for exact
  `UAnimSequence` targets):
  - `animation.asset.authoring.snapshot.v1` — read-only full observation (Preflight/Verify).
  - `animation.asset.authoring.edit.v1` — one snapshot-covered mutation for the timeline, curves,
    frame data, preview, hard object references, metadata and asset user data.
  - Raw editor-equivalent writes (notify/track/marker arrays, reflected hard object references,
    instanced subobject allocation) live in `AnimationAssetAuthoringRawEdits.cpp`, behaviour test
    `UEAssetDoc.AnimationProvider.AssetAuthoring.RawEdits`.
- Existing Animation sequence primitives (`animation.sequence.*`, `animation.curve.float.*`) need
  prepared typed captures and are not used by this profile; they and their tests are unchanged.

## 5. Behaviour changes against the legacy C++ profile

- New document shapes: plain object paths (legacy `{Kind: AssetRef, Path}` accepted as input),
  `{Class, Properties}` objects, `Track` instead of `TrackName` (alias accepted).
- `Curves` is a full replacement (legacy: sparse patch where `[]` was a no-op).
- `Notifies` / `NotifyStates` / `Metadata` / `AssetUserData` own *all* rows, not only objects
  named `AssetDocumentManaged_*`; Extract shows every notify.
- Create is supported (one apply, `References.Skeleton` required).
- Every reflected editable field is reachable (Body regions or `Properties`), not only the legacy
  scalar list; Extract writes all of them, so it is longer than the legacy extract.
- Diagnostic codes are new (`AnimSequence.*` for the document, `AnimationProvider.Authoring.*`
  from the primitive); the legacy codes are gone with the C++ route.

## 6. Tests

- In-engine, production route (`UEAssetDoc.AnimSequence.Python.`):
  - `CreatePopulated`: one-apply create of every surface; Extract reads back every authored field;
    Diff of the authored and of the extracted document is empty; unchanged re-apply saves nothing
    and keeps package and sidecar bytes; unload/reload keeps Extract equal.
  - `UpdateRollback`: rich update keeps omitted regions; `[]` clears; mid-apply failure (Editor
    batch written, then the authoring edit rejects), vetoed staging save, and an in-primitive
    failure (`NotifyTrackInUse`) each restore Extract and package bytes.
  - `Validation`, `Schema` (InspectProfile, CreateTemplate validates).
- Open editor: `UEAssetDoc.AnimSequence.OpenEditorApply` (apply, rolled-back failure,
  human edit Undo / Redo, all with the sequence open in its editor).
- Raw edits behaviour: `UEAssetDoc.AnimationProvider.AssetAuthoring.RawEdits`.
- Python: `tests/test_anim_sequence.py` (validation, lowering, path mapping, extract projection,
  sparse diff, orchestration order).
- Retired: the six `profiles/anim_sequence_*` candidate modules and their tests (never
  registered), and the C++ `AnimSequence.Roundtrip` / `AnimSequence.RegisteredProfileSchema`
  tests, which asserted the registered C++ route. The C++ profile's direct tests remain.
