# AnimMontage capability matrix (legacy C++ profile vs Python route)

Status: 2026-10-03. This is the §5 "no capability regression" check
for `/Script/Engine.AnimMontage`. It compares:

- **Legacy**: the retired C++ `FAnimMontageAssetDocumentCapability` / `AnimMontageAssetDocumentProfile`.
- **Python before**: `profile.anim_montage.v1` at integration b1554d87.
- **Python now**: the profile after this slice.

Every Python-now row has an in-engine round-trip test in
`UEAssetDoc.AnimMontage.*`, and the template only emits keys that apply accepts.

## Surfaces

| Document key / field | Legacy | Python before | Python now | Primitive (Python now) |
|---|---|---|---|---|
| `Skeleton`, `References.Skeleton` | yes | yes | yes | `animation.asset.skeleton.set.v1` |
| `PreviewMesh`, `Preview.PreviewMesh` | yes | yes | yes | `animation.asset.preview_mesh.set.v1` |
| `Curves` | yes | yes | yes | `animation.curve.float.replace.v2` |
| `SlotAnimTracks` | yes | yes | yes | `animation.montage.slot_tracks.replace.v1` |
| `CompositeSections` | yes | yes | yes | `animation.montage.sections.mutate.v1` |
| `Notifies`, `NotifyStates` | yes | yes | yes | `animation.montage.notifies/notify_states.replace.v1` |
| `Preview.PreviewBasePose` | yes | blocked | **restored** | `animation.montage.settings.set.v1` |
| `Blend.BlendInTime`, `BlendOutTime` | yes | blocked | **restored** | settings (`FAlphaBlend::SetBlendTime`) |
| `Blend.BlendModeIn/Out`, `BlendOutTriggerTime`, `bEnableAutoBlendOut` | yes | blocked | **restored** | `editor.property.write_admitted_batch.v1` |
| `Blend.BlendInOption/BlendOutOption` | no | no | **added** | settings (`SetBlendOption`) |
| `Blend.BlendInCustomCurve/BlendOutCustomCurve` (CurveFloat asset) | no | no | **added** | settings (`SetCustomCurve`) |
| `Blend.BlendProfileIn/BlendProfileOut` (Skeleton profile name) | no | no | **added** | settings (`USkeleton::GetBlendProfile`) |
| `Sync.SyncGroup`, `Sync.SyncSlotIndex` | yes | blocked | **restored** | Editor batch, then the settings derived refresh (`PostEditChangeProperty(SyncGroup)` collects markers) |
| `RootMotion.bEnableRootMotionTranslation/Rotation`, `RootMotionRootLock` | yes | blocked | **restored** | settings |
| `TimeStretch.TimeStretchCurveName` | yes | blocked | **restored** | Editor batch, then bake (`FTimeStretchCurve::BakeFromFloatCurve`) |
| `TimeStretch.SamplingRate`, `CurveValueMinPrecision` | yes | blocked | **restored** | settings (raw reflection on private EditAnywhere fields, `MontageSettingsRawEdits.cpp`) |
| `Metadata` (`[{Class, Properties}]`) | yes | blocked | **restored** | settings (instanced `UAnimMetaData`, same-class instances reused) |
| `SectionMetadata` (`{Section: [{Class, Properties}]}`) | yes | blocked | **restored** | settings |
| `Properties` (plain reflected Montage fields, e.g. `RateScale`, `bLoop`) | yes | blocked | **restored** | `editor.property.write_admitted_batch.v1` |
| `Definitions` / `DefinitionRef` fragments | yes | blocked | not supported (see below) | n/a |
| Create with any of the above in one apply | yes | blocked (Skeleton only, then Update) | **restored** | `editor.asset.create_by_factory.v2` (Skeleton as `factory_context`), then every row above on the new asset |
| Apply into an open Animation (Montage) editor | — | — | **added** | in place, then the editor's refresh: document tab (timeline, notify, sections) rebuilt, Montage Sections tab, preview montage instance restarted; test `AnimMontage.OpenEditorApply` |

Plain reflected fields always go through the generic Editor batch. Settings with a specific UE
API go through the one new primitive. Rollback is Core's whole-package snapshot. The settings
primitive declares `Object:` and `Package:` footprints and `bCoveredByPackageSnapshot`. No inverse
code is written. Reads all come from the single profile snapshot, which now has an eighth key,
`settings`.

## Shape changes against legacy

- Legacy wrapped references in `{Kind: AssetRef, Path}` and metadata objects in
  `{Kind: EmbeddedObject, Class, Properties}`. The Python route uses plain object paths and
  `{Class, Properties}`. Old fragment-shaped documents fail validation. Extract always emits the
  new shape.
- `Properties` never repeats a field that a Body region owns. These are `BlendModeIn/Out`,
  `BlendOutTriggerTime`, `bEnableAutoBlendOut`, `SyncGroup`, `SyncSlotIndex` and
  `TimeStretchCurveName`. Authoring one of them there fails with `AnimMontage.ManagedPropertyOverlap`.
- Floats compare at stored float32 precision. Settings regions and Properties are sparse: an
  omitted field is unchanged.
- `SyncSlotIndex` accepts -1 (`INDEX_NONE`), which is a valid native value.
- A *changed* `TimeStretchCurveName` must name an authored float curve. Otherwise the error is
  `AnimMontage.MissingTimeStretchCurve`. Keeping the native name is always accepted. The engine
  default `MontageTimeStretchCurve` usually names no curve, and extract → apply must round-trip it.
  A new Montage owns only the curves its document authors, so on create an omitted `Curves` means
  none. Preflight checks this before anything is created, and `Validate` does too when `Action` is `Create`.

## Create (one apply)

AnimMontage uses the shared class-neutral lifecycle `factory_generic.v1`, like DataAsset and
CurveFloat. The Montage-specific editor lifecycle primitives (`editor.animation_montage.*`) and the
`animation_montage_exact.v1` catalog row are gone. One `ApplyFile` creates and populates the asset:

1. Preflight emits step 0, `editor.asset.create_by_factory.v2`, with the Body `Skeleton` as the
   optional `factory_context`. Every Body/Properties step follows it. Editor rows are admitted from
   the class (`editor.property.enumerate_admitted.v1`) because there is no target to enumerate yet.
2. The generic factory create sets the context on the editor factory's single settable object
   property that accepts it (`UAnimMontageFactory::TargetSkeleton`). This is raw editor-equivalent
   code in `AssetDocumentEditorGenericFactoryContext.cpp`, and its behaviour test is
   `MontageLifecycle.GenericFactoryContext`.
3. The Animation request owner defers each family's preparation to invoke time, on the
   just-created asset. Their typed inverses are not armed. Core composition accepts
   snapshot-covered contributions on a create, so created-asset rollback covers every step.
4. On any failure (after create, while populating, or while saving), created-asset rollback deletes
   the object, package, registry entry and file, and the authored sidecar is untouched. Deletion
   unbinds the animation data controller. If the undo buffer is the only remaining reference
   (populate steps transact on the data model), deletion resets it, as the editor's own delete does.

What stays Montage-specific:

- The Python profile requires a Skeleton and supplies it as the factory context. It also knows the
  native default time-stretch curve name of a new Montage.
- The deferred, invoke-time family preparation in `MontageNotifyRequestOwner`. Montage families
  prepare against live native state, which does not exist before the factory runs.
- Notify placement on a zero-length new Montage takes its length from the structure that travels
  with the notify replacement.

Unchanged re-apply of an existing Montage, or of the same create document, is a no-op: no save, and
package and sidecar bytes are unchanged.

## Still unsupported, and why

| Surface | Why not now |
|---|---|
| `Definitions` / `DefinitionRef` | Document-level reuse was a legacy-only feature with no production consumer. The Python codec has no definition resolver. Fails with `AnimMontage.UnsupportedDefinitions`. |
| Branching points (`BranchingPointMarkers`) | Derived from notifies with `MontageTickType=BranchingPoint` and refreshed by the engine. Authored indirectly through `Notifies`. |
| Sync markers (`MarkerData`) | Derived cache. The settings primitive refreshes it the way the details panel does. It is never authored. |
| `AssetUserData`, `ParentAsset` / child-asset overrides, `PreviewPoseAsset` | Not in legacy either. Object-array and ownership semantics would need their own primitive. These are candidates for a later slice. |
| Hard-object Montage fields outside the named regions | The generic Editor batch does not admit hard object pointers. The ones that matter (`PreviewBasePose`, blend custom curves, blend profiles) are covered by the settings primitive. |
| Properties of shared (non-instanced) metadata pointers, e.g. a class default object in `MetaData` | Observed by class only (`Properties: {}`) and never written. Re-authoring them would mutate a shared object. Abstract classes cannot be instanced. |
| Aggregate reflected fields (structs, arrays, maps) in `Properties` and in metadata properties | The Editor batch rejects aggregates by design. Scalar metadata properties round-trip. |

## Tests

- In-engine (`UEAssetDoc.AnimMontage.`):
  - `RegisteredProfileSchema`
  - `CreateTemplate`
  - `Validate`
  - `Extract.Settings`
  - `Settings.RoundTrip`
  - `Settings.Metadata`
  - `Settings.InvalidValueRollsBack`: the Core snapshot restores the earlier Editor-batch writes
    after the settings primitive fails.
  - `Diff`
  - `Apply.CreatePopulated`: one apply creates the fully populated Montage. Extract reads back
    every authored field. Re-applying the same file is a no-op. After unload and reload, Extract
    is unchanged.
  - `Apply.FailureCleansNewAsset`: invalid input is rejected before create. Failures after the
    factory step (slot reference, notify property, settings reference) and a vetoed staging save
    leave no object, package, registry entry or file, and the sidecar bytes are unchanged.
  - `Apply.UnchangedReapplyIsNoOp`
  - `OpenEditorApply`: apply, a rolled-back failed apply and a human edit with Undo / Redo, all with
    the Montage open in its editor, whose panels and preview show each state.
- Lifecycle: `UEAssetDoc.MontageLifecycle.GenericFactoryContext`;
  `ApplyPreflight.AnimMontage.*`.
- Raw edits behavior: `UEAssetDoc.AnimationProvider.MontageSettings.RawEdits`.
- Python: `test_anim_montage.py`, in particular `AnimMontageSettingsTests`.
- Legacy tests that were retired, and the reason for each, are listed at the top of
  `Source/AssetDocument/Private/Tests/AssetDocumentAnimMontageTests.cpp`.
