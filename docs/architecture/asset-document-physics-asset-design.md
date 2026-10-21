# AssetDocument Physics Asset profile: design

Status: design 2026-10-06; P0-P2 implemented (results in [asset-document-physics-asset-matrix.md](asset-document-physics-asset-matrix.md)). P3 open-editor apply done 2026-10-18: close, apply, reopen (test `PhysicsAsset.OpenEditorApply`).
Governing rules: [design rules](asset-document-design-rules.md) §3 (rollback = functional
equivalence), §3.1 (Undo history), §3.2 (dependent assets), §4 (editor-equivalent code; raw paths only where the
editor does the same, in their own file with a behaviour test; no private headers, no unexported symbols) and §5
(support matrix). The reference is the **Physics Asset Editor** of UE 5.7 (`Engine/Source/Editor/PhysicsAssetEditor`,
mostly `FPhysicsAssetEditorSharedData` and `FPhysicsAssetDetailsCustomization`), the Content Browser's
"Create > Physics Asset" on a skeletal mesh, and `FPhysicsAssetUtils` (`Engine/Source/Developer/PhysicsUtilities`,
public `PHYSICSUTILITIES_API`). Legacy generators are not a reference. Acceptance is editor fidelity on real content,
free edits, and an AI writing physics assets from scratch.

Paths: `RT` = `Engine/Source/Runtime/Engine`, `PAE` = `Engine/Source/Editor/PhysicsAssetEditor/Private`,
`PU` = `Engine/Source/Developer/PhysicsUtilities`.

## 1. Classes in scope

| Class | Scope | Reason |
|---|---|---|
| `/Script/Engine.PhysicsAsset` | **Exact profile** `profile.physics_asset.v1` | Bodies and constraints are instanced subobject arrays whose identity is a bone; the collision table is not a UPROPERTY; constraint profiles live in a non-editable `ProfileHandles` array; creation needs a mesh and opens a modal dialog. The generic route cannot express or create any of it (coverage audit row 4). |
| `USkeletalBodySetup` (in `SkeletalBodySetups`) | Same profile, `Bodies` | One per simulated bone. |
| `UPhysicsConstraintTemplate` (in `ConstraintSetup`) | Same profile, `Constraints` | One per joint. |
| Shape elements: sphere, box, capsule (sphyl), tapered capsule, convex | Same profile, body `Shapes` | All reflected (`FKAggregateGeom`); convex by its `VertexData`. |
| Level set, skinned level set, ML level set, skinned triangle mesh shapes | Read only (listed in Inspect, kept on apply); a document that changes them fails `PhysicsAsset.ShapeTypeNotAuthored` | Experimental, cloth-only, binary grid / mesh data; the editor only creates them through the fit tools. |
| `UPhysicsAsset` subclasses (none in Engine), `USkeletalBodySetup` subclasses | Generic (subclasses) | Exact-class profile. |
| `UPhysicalAnimationComponent` profiles at runtime, Chaos cloth / Chaos flesh / ragdoll anim nodes | Out | Not part of the asset. |
| Skeletal mesh ↔ physics asset assignment (`USkeletalMesh::PhysicsAsset`) | Out of the Body; create-time option only (§4.3) | It is the mesh's property, a separate package. |

## 2. Editor data model: authored vs derived

### 2.1 Authored (Extract emits, Apply writes)

On `UPhysicsAsset` (`RT/Classes/PhysicsEngine/PhysicsAsset.h`):

- `SkeletalBodySetups` (instanced): per body `BoneName` (identity), `PhysicsType` (Default / Kinematic / Simulated),
  `CollisionTraceFlag`, `CollisionReponse` (sic), `bConsiderForBounds`, `bSkipScaleFromAnimation`, `PhysMaterial`,
  `DefaultInstance` (`FBodyInstance`: collision profile / responses, mass override, damping, CoM nudge, solver
  iterations, sleep, ...), `AggGeom` (shapes), and the private `PhysicalAnimationData` (one
  `FPhysicalAnimationProfile` per profile name the body is in, reachable through the public
  `FindPhysicalAnimationProfile` / `AddPhysicalAnimationProfile` / `RemovePhysicalAnimationProfile`).
- `ConstraintSetup` (instanced): per constraint `DefaultInstance` (`FConstraintInstance`: `JointName`,
  `ConstraintBone1` = child, `ConstraintBone2` = parent, child and parent frames `Pos1/PriAxis1/SecAxis1`,
  `Pos2/PriAxis2/SecAxis2`, `AngularRotationOffset`, `bScaleLinearLimits`, `ProfileInstance`: linear / cone / twist
  limits, linear and angular drives, projection, break and plasticity, `bDisableCollision`, parent dominates) and
  `ProfileHandles` (named constraint profiles: `ProfileName` + full `FConstraintProfileProperties`, `UPROPERTY()`
  without `EditAnywhere`).
- `CollisionDisableTable`: `TMap<FRigidBodyIndexPair, bool>`, **not a UPROPERTY**, custom serialized, keyed by body
  **indices**; written by `DisableCollision` / `EnableCollision`.
- Asset Details: `SolverSettings`, `SolverType` (RBAN / World), `bNotForDedicatedServer`, `PhysicalAnimationProfiles`
  and `ConstraintProfiles` (name lists), `AssetUserData`, editor-only `PreviewSkeletalMesh`.

### 2.2 Derived (never emitted)

| State | Recomputed by |
|---|---|
| `BodySetupIndexMap` (bone → body index) | `UpdateBodySetupIndexMap` (after add / remove, `PostLoad`) |
| `BoundsBodies` | `UpdateBoundsBodiesArray` |
| Body `BodySetupGuid`, cooked convex / triangle data, `ElemBox` of convexes, `bCreatedPhysicsMeshes` | `InvalidatePhysicsData` + `CreatePhysicsMeshes` after shape edits (`UBodySetup::PostEditChangeProperty`) |
| Constraint `DefaultProfile` (transient) | `SetDefaultProfile` / `UpdateProfileInstance` (`PostEditChangeProperty`), load. **On save, `Serialize` writes `DefaultProfile` into `DefaultInstance.ProfileInstance`** (`RT/Private/PhysicsEngine/PhysicsConstraintTemplate.cpp:32`): a direct write of `ProfileInstance` that skips the update is lost on save. |
| `CurrentPhysicalAnimationProfileName`, `CurrentConstraintProfileName`, body `CurrentPhysicalAnimationProfile` (editor dummies for the Details panel) | transient UI state |
| `PrePhysicalAnimationProfiles` / `PreConstraintProfiles` (rename / duplicate detection in `PostEditChangeProperty`) | transient |
| `ThumbnailInfo`, old solver settings | not authored |

There is no compile. The editor shows no error list; `IsDataValid` (§5) and the runtime's failure to initialize
bodies are the only "status".

## 3. Document shape

### 3.1 Example (small, complete: three-body chain on the tutorial mannequin)

```jsonc
{
  "SchemaVersion": 1, "Target": "/Game/Physics/PA_TutorialArm", "Class": "/Script/Engine.PhysicsAsset",
  "Action": "CreateOrUpdate",
  "FactoryContext": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP",
  "Properties": {"SolverType": "RBAN", "ConstraintProfiles": ["Ragdoll"],
                 "PhysicalAnimationProfiles": ["HitReact"]},
  "Body": {
    "PreviewMesh": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP",
    "Bodies": {
      "upperarm_l": {
        "Properties": {"PhysicsType": "Default", "DefaultInstance": {"MassInKgOverride": 4.0, "bOverrideMass": true}},
        "Shapes": {"Capsules": [{"Name": "upperarm_l_capsule", "Center": [14, 0, 0], "Rotation": [0, 0, 90],
                                 "Radius": 6.5, "Length": 18}]},
        "PhysicalAnimation": {"HitReact": {"OrientationStrength": 500, "AngularVelocityStrength": 50}}
      },
      "lowerarm_l": {
        "Shapes": {"Capsules": [{"Name": "lowerarm_l_capsule", "Center": [12, 0, 0], "Rotation": [0, 0, 90],
                                 "Radius": 5.5, "Length": 16}]}
      },
      "hand_l": {
        "Properties": {"PhysicsType": "Kinematic"},
        "Shapes": {"Boxes": [{"Name": "hand_l_box", "Center": [7, 0, 0], "X": 12, "Y": 4, "Z": 8}]}
      }
    },
    "Constraints": {
      "lowerarm_l": {"Child": "lowerarm_l", "Parent": "upperarm_l",
                     "Properties": {"ProfileInstance": {"ConeLimit": {"Swing1Motion": "Locked", "Swing2Motion": "Limited",
                                                                      "Swing2LimitDegrees": 70},
                                                        "TwistLimit": {"TwistMotion": "Locked"}}},
                     "Profiles": {"Ragdoll": {"ConeLimit": {"Swing2LimitDegrees": 120}}}},
      "hand_l":     {"Child": "hand_l", "Parent": "lowerarm_l"}
    },
    "CollisionDisabled": [["upperarm_l", "lowerarm_l"], ["lowerarm_l", "hand_l"], ["upperarm_l", "hand_l"]]
  }
}
```

### 3.2 Regions

All regions: **omitted = keep, present = complete state**.

| Region | Shape | Notes |
|---|---|---|
| `PreviewMesh` | skeletal mesh path or `null` | `PreviewSkeletalMesh`. Required when the asset has none and the document names bones it must resolve (§4.3). Changing it never re-fits bodies. |
| `Bodies` | `{"<bone>": Body}` | Complete: a bone not listed loses its body (editor's Delete Body, with its constraint fix-up: §4.1). |
| `Constraints` | `{"<joint name>": Constraint}` | Complete. |
| `CollisionDisabled` | `[[boneA, boneB], ...]` unordered pairs | The complete set of body pairs whose collision is disabled (the editor's "Disable Collision" pairs). Omitted: kept for existing pairs; new bodies follow the editor default (§3.4). |

Top-level `Properties` (generic admission, sparse) carry the asset Details: `SolverSettings`, `SolverType`,
`bNotForDedicatedServer`, `PhysicalAnimationProfiles`, `ConstraintProfiles`, `AssetUserData`. Refused there
(`PhysicsAsset.NotAuthoredProperty`): `SkeletalBodySetups`, `ConstraintSetup`, `PreviewSkeletalMesh` (Body-owned),
`BoundsBodies`, `ThumbnailInfo`, the old solver settings. The two profile name lists are written through the asset's
`PostEditChangeProperty` so the editor's `SanitizeProfilesHelper` renames / duplicates / removes the per-body and
per-constraint entries exactly as the Details panel does (a removed name removes the profile from every body and
constraint; a renamed entry renames it everywhere).

Body:

| Field | Meaning | Omitted |
|---|---|---|
| `Properties` | `USkeletalBodySetup` Details, sparse over editable properties (`PhysicsType`, `CollisionTraceFlag`, `CollisionReponse`, `bConsiderForBounds`, `bSkipScaleFromAnimation`, `PhysMaterial`, `DefaultInstance` sub-fields, ...); `AggGeom`, `BoneName`, `CurrentPhysicalAnimationProfile` refused | keep / new-body defaults |
| `Shapes` | `{Spheres, Boxes, Capsules, TaperedCapsules, Convexes}`: each an ordered array of elements; per element its reflected fields (`Name`, `Center`, `Rotation`, sizes, `bContributeToMass`, `CollisionEnabled`, `RestOffset`; convex `VertexData` and `Transform`) | keep. A present type array is complete; a type not named in a present `Shapes` is emptied, except the read-only experimental types, which are kept. |
| `PhysicalAnimation` | `{"<profile>": FPhysicalAnimationData (sparse)}`: the body's membership and data per physical animation profile; profile must be in `Properties.PhysicalAnimationProfiles` | keep |
| `Fit` | create-only helper: `{GeomType, VertWeight, ...FPhysAssetCreateParams}` asks the editor to fit the body's shapes from the mesh, like "Add / Replace Bodies" (§4.1). Not emitted. Ignored when `Shapes` is present. | — |

Constraint:

| Field | Meaning | Omitted |
|---|---|---|
| `Child`, `Parent` | `ConstraintBone1` / `ConstraintBone2`; both must be bodies | required on create; keep |
| `Frames` | `{Child: {Position, PriAxis, SecAxis}, Parent: {...}}` (`Pos1/PriAxis1/SecAxis1`, `Pos2/...`) | new constraint: snapped to the child bone (`SnapTransformsToDefault`, the editor's default); existing: keep |
| `Properties` | `FConstraintInstance` Details below the joint (sparse: `ProfileInstance.*`, `AngularRotationOffset`, `bScaleLinearLimits`); `JointName`, `ConstraintBone1/2`, frames refused here | keep / new-constraint defaults |
| `Profiles` | `{"<profile>": FConstraintProfileProperties (sparse against this constraint's default profile)}`: membership and data per constraint profile; complete when present | keep |

### 3.3 Identity and keys

- **Bodies are keyed by bone name** (`BoneName`). That is the engine's own identity (`FindBodyIndex`,
  `BodySetupIndexMap`), unique per asset (the editor deletes the old body before making a new one on the same bone),
  stable across save, reload and duplicate. A body cannot be moved to another bone (the editor has no such command;
  `BoneName` is `VisibleAnywhere`): changing the bone is delete + create.
- **Constraints are keyed by `JointName`.** The editor names a generated constraint after its child bone and a user
  constraint `UserConstraint`, `UserConstraint_0`, ...; `FindConstraintIndex(JointName)` is the engine lookup. If an
  existing asset has duplicate joint names (possible through old content), extract keys the later ones `<name>~2`,
  `~3` and warns `PhysicsAsset.DuplicateJointName`; apply refuses to create duplicates. A key that is not the joint
  name of an existing constraint creates one with that joint name. Renaming a joint (`JointName` is editable in the
  constraint Details) is expressed as `"RenamedFrom": "<old>"` on the new key (as IK Rig goals).
- **Shapes are positional** inside their type array (the engine and the editor address them by type + index; nothing
  references a shape). `Name` is a field, defaulting to the editor's auto name `<bone>_<type>` (`AutoNamePrimitive`).
- **Collision pairs are bone-name pairs.** Extract converts the index-keyed table to bone names; apply converts back
  after bodies are final. Because the table is keyed by indices, every body delete shifts it: we only ever change it
  through `DestroyBody` (which fixes the table) and `DisableCollision` / `EnableCollision`, never by writing the map.
- **Profiles are names**; membership is a key under `PhysicalAnimation` / `Profiles`.

### 3.4 Free edits

| Free edit | Document change | Operation (§4) |
|---|---|---|
| Add a body | new bone key with `Shapes` (or `Fit`) | create body, shapes / fit, editor's constraint fix-up for child bodies |
| Delete a body | remove the key | editor's Delete Body: constraints of nearest child bodies re-parented, constraints removed, table fixed |
| Add / remove / resize a shape | edit `Shapes` arrays | shape write, physics meshes rebuilt |
| Change body physics (simulated / kinematic, mass, damping, collision profile) | `Properties` | Details write |
| Add a constraint between any two bodies | new key with `Child` / `Parent` | `CreateNewConstraint` + the editor's `InitConstraintSetup` |
| Reparent a constraint | change `Child` / `Parent` | bone write + re-snap if `Frames` omitted (as "Snap" in the editor) |
| Edit limits / drives | constraint `Properties` | Details write + `UpdateProfileInstance` |
| Enable / disable collision between bodies | add / remove pairs in `CollisionDisabled` | `EnableCollision` / `DisableCollision` |
| New / renamed / removed profile | `Properties.ConstraintProfiles` / `PhysicalAnimationProfiles` (a rename is the same list position with a new name, as in the Details panel) and the per-body / per-constraint entries | `PostEditChangeProperty` sanitize, then per-item add / remove / data writes |
| Reorder | (none: bodies and constraints are maps; their array order is not authored) | — |

New bodies without a present `CollisionDisabled` get the editor default (`bDisableCollisionsByDefault`, true in the
editor's generation settings): collision disabled against every other body. With `CollisionDisabled` present, the
list is the truth.

`Bodies` and `Constraints` array order (`SkeletalBodySetups`, `ConstraintSetup`) is not authored: the engine resolves
by name and the table by index pairs, so any order is functionally equivalent. Extract sorts keys by bone hierarchy
order of the preview mesh (stable diffs); apply appends new items as the editor does.

### 3.5 Discovery (read only)

`inspect_asset_document_target` adds: `Skeleton` (preview mesh bones with parents and whether each has a body),
`Generation` (what "Generate All Bodies" with the project's `UPhysicsAssetGenerationSettings` or given params
**would** produce, computed on a transient asset: a suggestion the AI can copy into a document, as IK Rig's
auto-characterize suggestion), `MeshCompatibility` (bodies whose bone the preview mesh lacks), `Mass` (per body mass
as the editor's Details show it), the read-only experimental shapes, and `Status` (§5).

## 4. Editor-equivalent operations

### 4.1 Mapping

The editor's operations live in `FPhysicsAssetEditorSharedData` (private, `PAE`). The engine and `FPhysicsAssetUtils`
calls are public, so each structural edit is written from the editor's observable result (bodies, joints, frames,
profiles, collision table) in one isolated raw file (`PhysicsAssetEditorEquivalent.cpp`) with a behaviour test that runs the real editor operation on a duplicate
(through an open Physics Asset Editor in automation) and compares the asset.

| Edit | Editor path | Our path |
|---|---|---|
| Add body on bone, shapes given | `MakeNewBody(params, bone)`: `Modify`, `FPhysicsAssetUtils::CreateNewBody`, fit shapes, `AutoNameAllPrimitives`, constraint fix-up for physical child bones (create missing child constraints, re-point an existing child constraint whose parent was the new body's controlling body and re-enable that collision), constraint to the controlling parent body, `RefreshPhysicsAssetChange` | same result; the "fit" step is replaced by writing the document's shapes (the editor itself writes shapes through Details afterwards); `CreateNewBody` with `bDisableCollisionsByDefault` per §3.4 |
| Add body, `Fit` | same, fit included: `FPhysicsAssetUtils::CreateCollisionFromBone` with the mesh's dominant / any-weight bone infos (`IMeshUtilities::CalcBoneVertInfos`, public module interface, as the editor's `Initialize` computes them) | same calls |
| Delete body | `DeleteBody`: transaction, `BodyFindConstraints`, for each nearest physical child: copy the deleted constraint to child → grandparent (`CreateNewConstraint(..., Constraint)` + `InitConstraintSetup`), `FPhysicsAssetUtils::DestroyBody` (table fix, constraints removed, maps updated) | same result; then the document's `Constraints` decides what survives (a fix-up constraint the document does not list is deleted, one it lists is updated) |
| Shapes | Details on `AggGeom` + viewport gizmo; `UBodySetup::PostEditChangeProperty` → `InvalidatePhysicsData`, `CreatePhysicsMeshes` | typed codec writes into `AggGeom` arrays inside `PreEditChange` / `PostEditChangeProperty(AggGeom)`; convex: also `UpdateElemBox` per element (what the convex import paths do) |
| Body Details | Details panel | typed codec with `PreEditChange` / `PostEditChangeChainProperty` per changed property |
| Physical animation profile data | Details customization: add body to current profile (`AddPhysicalAnimationProfile`), edit through the `CurrentPhysicalAnimationProfile` dummy, remove (`RemovePhysicalAnimationProfile`) | `AddPhysicalAnimationProfile` / `RemovePhysicalAnimationProfile` / `FindPhysicalAnimationProfile` (public), codec write into the returned struct |
| Add constraint | `MakeNewConstraint(parent, child)`: unique `UserConstraint[_N]` name, `FPhysicsAssetUtils::CreateNewConstraint`, `InitConstraintSetup` (`ConstraintBone1/2`, `SnapTransformsToDefault`, `SetDefaultProfile`, disable collision between the pair) | same result with the key as joint name; frames from `Frames` when given (`SetRefFrame` per frame, then `SetDefaultProfile`) |
| Delete constraint | `DeleteCurrentConstraint`: `FPhysicsAssetUtils::DestroyConstraint` | same |
| Constraint Details | Details (customized); `UPhysicsConstraintTemplate::PostEditChangeProperty` → `UpdateProfileInstance` | codec write, then `PostEditChangeProperty` with `CurrentConstraintProfileName == NAME_None` (we never select a profile; §2.2 save gotcha) |
| Constraint profile membership / data | Details: "Add to current profile" (`AddConstraintProfile`), edit while that profile is current (writes the handle through `UpdateProfileInstance`), remove (`RemoveConstraintProfile`) | `AddConstraintProfile` / `RemoveConstraintProfile` (public inline, `WITH_EDITOR`), codec write into `ProfileHandles[i].ProfileProperties` directly (no "current profile" switching, which would also touch every constraint's `DefaultInstance` through `ApplyConstraintProfile`) |
| Collision on / off between bodies | `SetCollisionBetween(a, b, enable)`: `Modify`, `EnableCollision` / `DisableCollision`, refresh | same engine calls (`ENGINE_API`) |
| Profile name lists | asset Details → `PostEditChangeProperty` (`SanitizeProfilesHelper`) | property write with the real `FPropertyChangedEvent` (array change type) |
| Preview mesh | Persona preview-mesh picker → `SetPreviewMesh` | `UPhysicsAsset::SetPreviewMesh` |
| Every structural edit | `RefreshPhysicsAssetChange` (recreate physics state of components that use the asset) | once at the end (§5) |

All operations run inside the provider's non-interactive scope and one transaction per apply; `FPhysicsAssetUtils`
calls do not transact, so every touched object is `Modify`'d first, as the editor does.

### 4.2 Plan order

1. `PreviewMesh` (bones must resolve before anything else).
2. Top-level `Properties` (profile name lists first, so membership writes find the names).
3. Constraint deletes (keys gone or whose `Child`/`Parent` change to a pair handled by delete + create).
4. Body deletes (editor's fix-up runs; fix-up constraints are reconciled in step 6).
5. Body creates (parents before children in bone order, so the editor's fix-up sees the same state it sees when a
   user adds bodies root-down), shapes, body `Properties`, physical animation entries.
6. Constraints: renames (`RenamedFrom`), creates, bone changes, frames, `Properties`, `Profiles`; delete constraints
   the fix-up created that the document does not list.
7. `CollisionDisabled` (diff against the table after steps 4–6, by bone pair).
8. Finalize: `UpdateBodySetupIndexMap`, `UpdateBoundsBodiesArray`, `RefreshPhysicsAssetChange`, validation (§5).

### 4.3 Create

The editor's create path is `UPhysicsAssetFactory` (`TargetSkeletalMesh`) → `CreatePhysicsAssetFromMesh`, which opens
the modal "New Physics Asset" dialog (`OpenNewBodyDlg`) and then runs
`FPhysicsAssetUtils::CreateFromSkeletalMesh(NewAsset, Mesh, UPhysicsAssetGenerationSettings::CreateParams, Error,
bSetToMesh=true)` (generate bodies for every bone above `MinBoneSize`, constraints, collision defaults, set the
preview mesh, assign the asset to the mesh). `USkeletalMeshEditorSubsystem::CreatePhysicsAsset` is the scripted
equivalent without the dialog.

Profile create, editor-equivalent without the dialog:

- The mesh comes from `FactoryContext` (a skeletal mesh path; it fills `TargetSkeletalMesh`, as the generic
  FactoryContext route already does for this factory) or `Body.PreviewMesh`; neither → `PhysicsAsset.MeshRequired`.
- `NewObject<UPhysicsAsset>` in the target package, `SetPreviewMesh(Mesh)`.
- If the document has a `Bodies` region: no generation; bodies are built from the document (§4.1). This is the
  normal AI and round-trip path (a round-tripped document always has `Bodies`).
- If `Bodies` is omitted: the editor's generation runs (`CreateFromSkeletalMesh`, `bShowProgress=false`) with the
  project's `UPhysicsAssetGenerationSettings` defaults or a create-only `Generation` key holding
  `FPhysAssetCreateParams` overrides (`GeomType`, `MinBoneSize`, `bBodyForAll`, `AngularConstraintMode`, ...). The
  result then gets the document's other regions. `Generation` is never emitted.
- Mesh assignment (`bSetToMesh`): decision §8.2. Recommendation: **do not** assign (the mesh is another package the
  document did not name); Inspect reports "mesh X uses physics asset Y", and the AI can set the mesh's
  `PhysicsAsset` through the mesh's own document.
- `FAssetRegistryModule::AssetCreated`; failure → factory rollback deletes the package.

## 5. Validation, status and rollback

### 5.1 Status

No compile exists. After every apply (and on Extract / Inspect) the provider computes what the editor would show
or what breaks at runtime:

- **Errors** (`IsDataValid`, which data validation and cook run): a body without shapes ("Bone X requires at least
  one collision shape"); a simulated body where no shape contributes to mass.
- **Errors** (structural): a constraint whose `Child` or `Parent` is not a body; `Child == Parent`; two constraints
  on the same child / parent pair with the same joint name; a collision pair naming a missing body; a profile entry
  for a profile not in the asset's list.
- **Warnings**: bodies whose bone the preview mesh lacks (`IsPhysicsAssetCompatible`-style check; the editor shows
  them greyed out); a body without a constraint that is not a root (it will fall off); a convex element whose hull
  fails to cook (`CreatePhysicsMeshes` leaves no Chaos convex); constraints whose child body is the parent of their
  parent body (inverted); no preview mesh.

Messages carry document paths (`/Body/Bodies/hand_l/Shapes`, `/Body/Constraints/hand_l`).

### 5.2 Failure policy and status shape

As Material: before-status first; **new errors** fail the apply with `PhysicsAsset.ValidationFailed` at the path and
roll back; pre-existing errors return as `PhysicsAsset.ValidationError` warnings; warnings return as
`PhysicsAsset.ValidationWarning`. `Compile: {Status, Messages}` through a compile-status describer; `Status`:
`UpToDate`, `UpToDateWithWarnings`, `Error`, `Unknown` (no preview mesh: bone checks skipped).

### 5.3 Rollback

- Existing asset: Core package snapshot. The snapshot serializes with `FObjectWriter`, so the custom-serialized
  `CollisionDisableTable` is included, and `UPhysicsConstraintTemplate::Serialize` sees a saving, non-transacting
  archive: it writes `DefaultProfile` into `ProfileInstance` and restores it afterwards. Because every constraint
  write keeps `DefaultProfile` in sync (`UpdateProfileInstance`), that is the authored value; a behaviour test
  covers it. Bodies and constraints created by the apply are removed by the snapshot.
- Rebuilder `AssetDocumentPhysics.PhysicsAsset` (what the editor's `PostUndo` does): `UpdateBodySetupIndexMap`,
  `UpdateBoundsBodiesArray`, per body `InvalidatePhysicsData` + `CreatePhysicsMeshes`, per constraint
  `SetDefaultProfile(DefaultInstance)` (load does the same), `RefreshPhysicsAssetChange`.
- Created asset: factory rollback deletes the package.
- Dependents (§3.2): skeletal meshes, skeletal mesh components and anim graphs (RigidBody node) reference the asset by
  pointer and resolve bodies by bone name at runtime; nothing is compiled from it. `RefreshPhysicsAssetChange`
  recreates physics state of live components, as the editor does. No dependent package changes.
- Open editor (P3, done): an asset open in the Physics Asset Editor is closed for the apply and reopened on the result
  (Info `PhysicsAsset.EditorReopened`): the editor keeps selection by index, a running simulation and preview
  component state that index shifts would break, and its refresh (`FPhysicsAssetEditor`, SharedData) is private.

## 6. Fidelity acceptance plan

### 6.1 Content

| # | Asset | Mounted by default | Content |
|---|---|---|---|
| 1 | `/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_PhysicsAsset` | yes | full mannequin, capsules, constraints, collision table |
| 2 | Template `High/Characters/Content/Mannequins/Rigs/PA_Mannequin` (copied to `/Game/Characters/Mannequins/Rigs`, with `SKM_Manny_Simple`) | template copy | UE5 mannequin, drives, projection |
| 3 | Template `Standard/Weapons` `PA_Rifle`, `PA_Pistol` (copied) | template copy | small rigid props |
| 4 | Template `Standard/Vehicles` `PA_SportsCar`, `SKM_Offroad_PhysicsAsset` (copied) | template copy | boxes / convex, wheels without constraints |
| 5 | `/ChaosModularVehicleExamples/Models/SportsCar/SportsCar_PhysicsAsset` | ChaosModularVehicle examples (off) | convex hulls |
| 6 | `/MoverExamples/Characters/Mannequins/Rigs/PA_Mannequin` | MoverExamples (experimental, off) | drives, projection |
| 7 | `/MetaHumanCharacter/Body/IdentityTemplate/PHYS_Body`, `/MetaHumanCharacter/Face/PHYS_Face` | MetaHumanCharacter (beta, off) | constraint profiles, drives, mass overrides |
| 8 | `/ProceduralVegetationEditor/SampleAssets/...` tree physics assets | ProceduralVegetation (off) | many bodies, no constraints |

No shipped asset has physical animation profiles, tapered capsules or level sets. Fixtures built in the test with
the editor's own calls (generation, `MakeNewBody`, `MakeNewConstraint`, profile Details) on a duplicate of
`TutorialTPP`: `PA_Fixture_Profiles` (two constraint profiles, two physical animation profiles, partial membership),
`PA_Fixture_Shapes` (one body per shape type incl. tapered capsule and a convex), `PA_Fixture_UserConstraints`
(`UserConstraint`, `UserConstraint_0` between non-adjacent bodies), `PA_Generated` (the editor's generation with
default settings).

### 6.2 What is compared

For each asset: duplicate into `/Game`, save → Extract → apply as a new asset with the same preview mesh → compare:

- **document**: the two extracts are equal;
- **bodies**: same bone set; per body every reflected property (`AggGeom` element-wise with float tolerance,
  `DefaultInstance`, physics type, collision settings), physical animation data per profile;
- **constraints**: same joint names; `DefaultInstance` reflected compare (frames, limits, drives), `ProfileHandles`
  by name;
- **collision table**: the same set of bone-name pairs;
- **runtime**: a transient `USkeletalMeshComponent` with the preview mesh and each asset: same body count, same
  per-body mass (`CalculateMass`), same constraint count, same bounds bodies;
- **status**: equal.

Then re-apply → no-op; reload → extract equal; save → reload → constraint `ProfileInstance` unchanged (§2.2 gotcha).

### 6.3 Free edits (document vs the editor's operations on duplicates of #1 and `PA_Fixture_Profiles`)

Add a body on `head`; delete `lowerarm_l` (fix-up re-parents `hand_l` to `upperarm_l`); replace a capsule by a box;
make `hand_r` kinematic with a mass override; add a user constraint between `hand_l` and `spine_03`; loosen a twist
limit; add a constraint to the `Ragdoll` profile with different limits; rename a constraint profile (all members
follow); enable collision between `thigh_l` and `thigh_r`; change the preview mesh to `SKM_Quinn_Simple`. The
editor-side edits call the same SharedData operations the UI commands call (in automation with an open editor);
assets must compare equal (§6.2).

Undo test: human edit in the Physics Asset Editor, save, close, AI apply, Ctrl+Z per §3.1.

### 6.4 AI from scratch

- **Python test:** one `Create` on `TutorialTPP` with a ragdoll document written by hand (pelvis, spine, head, arms,
  legs: 11 bodies, 10 constraints with cone / twist limits, collision disabled between neighbours), status
  `UpToDate`, a component with the asset simulates for 60 frames without NaNs. Second document: omit `Bodies`, give
  `Generation: {"GeomType": "Box", "MinBoneSize": 10}`, then edit two bodies. Error cases at paths with rollback:
  unknown bone, constraint to a missing body, empty `Shapes` on a simulated body, collision pair with a missing body,
  profile entry for an undeclared profile, no mesh on create.
- **Black-box trial** (MCP tools + schema section only): "make a ragdoll for TutorialTPP with limited elbows and
  knees", "add a hit-react physical animation profile to the upper body", "make the ponytail bones simulated with a
  soft drive".

## 7. Risks, unknowns and phases

### 7.1 Risks

| Risk | Effect | Answer |
|---|---|---|
| Editor operations live in private `FPhysicsAssetEditorSharedData` | same result needed | one raw file using the public calls (`FPhysicsAssetUtils`, `UPhysicsAsset`, `FConstraintInstance`); behaviour test against the real SharedData operation in an automation editor session |
| Index-keyed, non-reflected `CollisionDisableTable` | index drift on delete; generic tools blind to it | only `DestroyBody` / `EnableCollision` / `DisableCollision` touch it; document uses bone pairs; snapshot covers it (custom `Serialize`) |
| `DefaultProfile` / `ProfileInstance` save swap | silent loss of limit edits on save | every constraint write ends in `UpdateProfileInstance` with no current profile; save-reload test |
| "Current profile" editor state (`CurrentConstraintProfileName`) | edits land in a profile instead of the default | we never set it; if an asset is saved with a non-None transient value it does not persist (transient) |
| Generation depends on mesh skin weights and project generation settings | `Generation` / `Fit` results differ across meshes and projects | those keys are create-time helpers, never emitted; round trip always uses explicit `Shapes` |
| Experimental shape types (level sets, skinned triangle meshes) | not authorable | read only; kept on apply |
| Convex cooking | hull may fail for degenerate `VertexData` | warning with path; behaviour mirrors the editor (no Chaos convex, body still exists) |
| Joint names are not guaranteed unique in old content | key collision | `~N` suffix on extract + warning |
| Thin default-mounted corpus (one engine asset) | coverage | template copies and fixtures; optional plugins (§8.3) |
| `PhysicsUtilities` is a Developer module | editor-only dependency | fine for an Editor module (UnrealEd already depends on it) |

### 7.2 Phases (each independently mergeable)

| Phase | Content | Acceptance |
|---|---|---|
| **P0 read** | provider (module per §8.1), `physics.asset.read.v1`, `physics.status.v1`, Python Extract / Inspect / Diff (Apply refused `PhysicsAsset.ReadOnly`), compile-status describer | extract stable (twice equal, equal after reload) on #1–#4 and the fixtures; collision pairs match the editor's "No Collision" display |
| **P1 apply on existing assets** | `physics.asset.edit.v1` ops (body add / remove / shapes / settings / physical animation, constraint add / remove / rename / bones / frames / settings / profiles, collision pairs, preview mesh), raw editor-equivalent file + behaviour tests, rebuilder, open-editor refusal | §6.2 on #1–#4 and fixtures (apply onto an emptied duplicate); §6.3 edits; rollback after a failed validation; save-reload gotcha test; Undo test |
| **P2 create** | create with mesh (`FactoryContext` / `PreviewMesh`), document bodies or `Generation`, `Fit`, `Inspect.Generation` suggestion | §6.2 as written (create from extract); §6.4 Python test |
| **P3 AI surface** (open-editor apply done 2026-10-18) | `MCP/schemas/assetdocument/PhysicsAsset.md`, open-editor apply if decided, black-box trial | §6.4 trial; matrix approved |

## 8. Decisions needed (with recommendations)

1. **Module / owner.** New module `AssetDocumentPhysics` (owner `Physics`, new value 12, provider `physics`,
   depends on `PhysicsUtilities`, `PhysicsCore`, `UnrealEd`, `AssetDocumentEditor`) versus folding into
   `AssetDocumentAnimation`. Recommendation: own module; physics assets are used by meshes, vehicles and props, not
   only animation, and the dependency set is different.
2. **Assign the new asset to the mesh on create** (the editor's factory does, `bSetToMesh = true`). Recommendation:
   no; the mesh is a different package the document did not name (§3.2 of the reset). Inspect shows the
   assignment; the AI sets it through the mesh's document. Alternative: follow the editor and report the mesh as an
   updated dependent.
3. **Corpus plugins.** Only one physics asset is mounted by default. Recommendation: use the template copies (#2–#4)
   as the main corpus, enable `MoverExamples` (already proposed for IK Rig) in the test sandbox, keep MetaHuman,
   ChaosModularVehicle examples and ProceduralVegetation optional.
4. **Generation as document keys** (`Generation` on create, `Fit` per body) versus suggestion-only (as IK Rig's
   auto-characterize). Recommendation: allow both create-time keys, never emitted, because "Create Physics Asset"
   without generation is not an editor path at all (the editor always generates), and per-body fit is the editor's
   normal "Add Body" path; also offer the `Generation` suggestion in Inspect.
5. **Constraint identity = `JointName`** with `RenamedFrom` for renames (recommended) versus the child / parent bone
   pair (`FindConstraintIndex(Bone1, Bone2)`), which breaks for user constraints between the same pair and for
   reparenting.

### Decisions (Root, 2026-10-07; recorded with P0 + P1)

1. **Module / owner:** own module `AssetDocumentPhysics`, owner `Physics` = **13** (12 is reserved for Cinematics),
   provider `physics`.
2. **Mesh assignment on create:** no. Create never assigns the new asset to the mesh; Inspect `MeshUsers` shows which
   meshes use the asset (asset registry referencers, and whether a loaded mesh's `PhysicsAsset` is this asset).
3. **Corpus:** the template copies (#2–#4, copied into `/Game/<pack>` by the tests as project creation does) are the
   main corpus; `MoverExamples` is enabled in the test sandboxes; MetaHuman, ChaosModularVehicle examples and
   ProceduralVegetation are optional (the tests skip them with a note).
4. **Generation keys:** `Generation` (create) and `Fit` (per body) are allowed create-time keys, never emitted;
   `Generation` is also an Inspect suggestion. Both arrived with P2 (see "P2 create" below).
5. **Constraint identity:** `JointName`, renames with `RenamedFrom`.

Semantics settled while implementing P0/P1 (they refine §3.2 / §3.4 / §4):

- **Inside a body / constraint entry every key is sparse with defaults**: omitted `Properties`, `PhysicalAnimation`,
  `Profiles` mean "a new body's / constraint's values" (not "keep"); extract omits them when they equal the
  defaults, so a round trip stays small. `Shapes` omitted keeps the body's shapes (a new body needs them); a present
  `Shapes` is complete over the authored types. An explicitly written default is equal to an omitted one: the read
  header carries the complete default values (`defaults: {body, constraint, physical_animation}`; a profile entry's
  default is the constraint's own default profile) and Diff / apply planning / post-apply verification compare
  against them at every depth.
- **-0.0 reads as 0** (rotator / axis components stored negative-signed), so documents and their re-extracts are
  textually stable.
- **`Frames` omitted = the editor's Snap** (`SnapTransformsToDefault`), for existing constraints too; extract emits
  `Frames` only when they differ from the snapped frames (1e-4).
- **Shape `Name` equal to the editor's auto name** (`<bone>_<type>`) is the default and dropped in canonical form.
- **Collision fields derived from a named collision profile** (`ObjectType`, `CollisionEnabled`,
  `CollisionResponses` when `CollisionProfileName` is not `Custom` / none) are neither emitted nor written
  (`FBodyInstance::FixupData` derives them, as load does).
- **Constraint `ProfileInstance` is read from `DefaultProfile`** (what a save writes), and every edit first resets the
  transient profile previews to None (`ApplyConstraintProfile(None)`, as choosing None in the editor's picker), so a
  human who saved while previewing a profile never leaks that profile into the default values.
- **`JointName` is `VisibleAnywhere`** (not editable in the constraint Details, contrary to §3.3): `RenamedFrom` is a
  document-only rename (`constraint.rename`); the editor has no rename command.
- New bodies use the project's generation settings flags (`bDisableCollisionsByDefault`, `bCreateConstraints`), as
  the editor's Add Body does.
- **P2 create (2026-10-08):** the generic factory route creates the asset.
  `UPhysicsAssetFactory` always opens its modal dialog, so the integration's dialog-factory seam
  (`AssetDocumentEditorDialogFactory.h`) replaces `FactoryCreateNew` by `IAssetTools::CreateAsset` without a factory
  plus a registered initializer that does what the factory does before its dialog (`PreviewSkeletalMesh =
  TargetSkeletalMesh`); scratch / Validate creates a transient asset with the same initializer. The profile's
  `create_target_context` resolves the mesh (`FactoryContext`, else `Body.PreviewMesh`) into the factory context.
  The first apply step of a created asset without `Bodies` is the provider op `generate {params}` =
  `FPhysicsAssetUtils::CreateFromSkeletalMesh(..., bSetToMesh=false, bShowProgress=false)` with the project's
  `UPhysicsAssetGenerationSettings::CreateParams` overlaid by `Generation` (sparse `FPhysAssetCreateParams`,
  decoded by the editor value codec; `LodIndex` checked against the mesh). `Fit` on a new body is the Add Body fit:
  `bodies.add {bones, fit}` runs the transcribed `MakeNewBody` with `FMeshUtilitiesEngine::CalcBoneVertInfos`
  (dominant / any weight, LOD) and `FPhysicsAssetUtils::CreateCollisionFromBone`, destroying the body when the fit
  fails as the editor does (`PhysicsAsset.FitFailed`), then `AutoNameAllPrimitives`. `Generation` / `Fit` on an
  existing asset / body are warnings (`PhysicsAsset.GenerationIgnored`, `PhysicsAsset.FitIgnored`), not errors, so
  a create document stays re-appliable. Inspect `Generation` = `{Settings, Body}`: a transient asset generated with
  the project settings for the preview mesh, read as document regions (provider `read` detail `generation`).