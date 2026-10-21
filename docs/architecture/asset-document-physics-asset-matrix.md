# Physics Asset support matrix and corpus results (P0 + P1 + P2)

Status: 2026-10-08, P0 + P1 merged, P2 create implemented; ready for approval. P3 open-editor apply done 2026-10-18 (test `PhysicsAsset.OpenEditorApply`). Design: [asset-document-physics-asset-design.md](asset-document-physics-asset-design.md)
(decisions in §8). Rules: [design rules](asset-document-design-rules.md) §3 (rollback = functional
equivalence), §4 (editor-equivalent code + behaviour test), §5 (support matrix). Reference editor: the Physics Asset
Editor (`FPhysicsAssetEditorSharedData`, `FPhysicsAssetDetailsCustomization`, the asset / body / constraint Details).
The agent-facing description is `MCP/schemas/assetdocument/PhysicsAsset.md`.

| Class | Profile | Phase | State |
|---|---|---|---|
| `PhysicsAsset` | `profile.physics_asset.v1` (`physics_profile`, provider `physics`, owner `Physics` = 13) | P0, P1, P2 | Extract / Inspect / Diff / Apply; Create from a skeletal mesh (document bodies or the editor's generation) |

Classification: **Supported**, **Read only**, **Refused** (an error code), **Later** (phase named).

## Editor surfaces

| Editor surface | Body location | Class | Mechanism |
|---|---|---|---|
| Preview mesh picker | `PreviewMesh` | Supported | `UPhysicsAsset::SetPreviewMesh` after a bone check (its "missing bone" dialog never opens; `PhysicsAsset.IncompatibleMesh`) |
| Add Body (shapes given) | `Bodies.<bone>` new key | Supported | `FPhysicsAssetEditorSharedData::MakeNewBody` transcribed (`AssetDocumentPhysicsEditorEquivalent.cpp`), generation-settings flags, parents first |
| Add Body with fit (the Tools tab's body creation settings) | `Bodies.<bone>.Fit` (new body, no `Shapes`; create-only, never extracted) | Supported | transcribed `MakeNewBody` fit: `FMeshUtilitiesEngine::CalcBoneVertInfos` (dominant / any weight, LOD) + `FPhysicsAssetUtils::CreateCollisionFromBone`; failed fit destroys the body as the editor does (`PhysicsAsset.FitFailed`); `AutoNameAllPrimitives`. On an existing body: `PhysicsAsset.FitIgnored` warning |
| Add / Replace Bodies on a selection, Generate All Bodies on an existing asset | — | Not authored | the document states the bodies (or `Fit` per new body); regenerating an existing asset = remove its `Bodies` entries and add them with `Fit` |
| Delete Body (constraint fix-up to the nearest bodies below) | key removed | Supported | `SharedData::DeleteBody` transcribed, `FPhysicsAssetUtils::DestroyBody` |
| Body Details (physics type, mass, damping, collision profile / responses, phys material, ...) | `Bodies.<bone>.Properties` | Supported | editor typed value codec, sparse over a new body; `FBodyInstance::FixupData` + `PostEditChangeProperty` |
| Shapes: add / remove / Details of spheres, boxes, capsules, tapered capsules | `Shapes.<type>[i]` | Supported | `AggGeom` element write, auto name `<bone>_<type>` as `AutoNamePrimitive` |
| Convex shapes (points, transform) | `Shapes.Convexes[i].VertexData`, `Transform` | Supported | `UpdateElemBox`; hull rebuilt (`InvalidatePhysicsData` + `CreatePhysicsMeshes`) as the editor's convex copy; `IndexData` kept when the points are unchanged |
| Level sets, skinned level sets, ML level sets, skinned triangle meshes | — | Read only | Inspect `BodyFacts.ReadOnlyShapes`; kept on apply; `PhysicsAsset.ShapeTypeNotAuthored` |
| Physical animation profile membership + data | `Bodies.<bone>.PhysicalAnimation.<profile>` | Supported | `Add/RemovePhysicalAnimationProfile`, data written as the `CurrentPhysicalAnimationProfile` dummy's `PostEditChangeProperty` copies it |
| Create Constraint between two bodies | `Constraints.<joint>` new key | Supported | `SharedData::MakeNewConstraint` with the key as joint name (`CreateNewConstraint` + `InitConstraintSetup`: snap, default profile, collision off) |
| Delete Constraint | key removed | Supported | `FPhysicsAssetUtils::DestroyConstraint` |
| Constraint bodies (Details `ConstraintBone1/2`) | `Child`, `Parent` | Supported | bone write + Snap when `Frames` are omitted |
| Constraint frames (viewport gizmo, Snap) | `Frames` | Supported | `Pos1/PriAxis1/SecAxis1`, `Pos2/...` written as the Details do, `PostEditChangeProperty`; omitted = `SnapTransformsToDefault` (the editor's Snap) |
| Constraint Details (limits, drives, projection, ...) | `Constraints.<joint>.Properties` | Supported | codec write of the instance, `PostEditChangeProperty` → `UpdateProfileInstance` with no current profile (the default profile) |
| Constraint profile membership + data | `Constraints.<joint>.Profiles.<profile>` | Supported | `Add/RemoveConstraintProfile`, data written into the profile handle (no profile switching) |
| Constraint rename | `RenamedFrom` | Supported (document only) | `JointName` write; the editor has no rename command (`JointName` is `VisibleAnywhere`) |
| Enable / Disable Collision | `CollisionDisabled` | Supported | `EnableCollision` / `DisableCollision` (as `SharedData::SetCollisionBetween`) |
| Profile lists: add / rename / remove entries | `Properties.ConstraintProfiles`, `PhysicalAnimationProfiles` | Supported | `PostEditChangeProperty` with the Details panel's event (`SanitizeProfilesHelper` renames / removes everywhere) |
| Other asset Details (`SolverType`, `SolverSettings`, ...) | `Properties` | Supported | generic Details admission |
| Profile picker (current profile preview) | — | Not authored | transient; reset to None before every edit |
| New Physics Asset (factory + mesh picker) | `Action: Create`, `FactoryContext` (else `Body.PreviewMesh`) | Supported | generic factory route with the dialog-factory seam: `IAssetTools::CreateAsset` + the factory's pre-dialog step (`PreviewSkeletalMesh = TargetSkeletalMesh`); no mesh `PhysicsAsset.MeshRequired`, not a mesh `PhysicsAsset.UnknownMesh`; never assigned to the mesh (decision §8.2) |
| New Physics Asset generation dialog (body / constraint creation settings, Create Asset) | `Body.Generation` (create without `Bodies`; create-only, never extracted) | Supported | provider op `generate`: `FPhysicsAssetUtils::CreateFromSkeletalMesh(bSetToMesh=false, bShowProgress=false)` with the project's `UPhysicsAssetGenerationSettings` overlaid by `Generation`; with `Bodies` `PhysicsAsset.GenerationWithBodies`; on an existing asset `PhysicsAsset.GenerationIgnored` warning |
| (suggestion) what the dialog would generate | Inspect `Generation` `{Settings, Body}` | Read only | the same generation on a transient asset, read as document regions |
| Asset open in the Physics Asset Editor | — | Supported (P3): close, apply, reopen (Info `PhysicsAsset.EditorReopened`) | test `PhysicsAsset.OpenEditorApply` |

## Corpus results

Mac (M-series, Development editor, 2026-10-07). Read = `Corpus` / `Read` tests (extract and Inspect of the asset as
stored); Fidelity = `Fidelity` test (a duplicate is emptied, rebuilt from the source's extract, re-applied unchanged,
saved and reloaded; the rebuilt and reloaded extracts equal the source's). PC numbers come from the gate suite.

| Asset | Content | Extract / Inspect | Doc size | Status | Fidelity (empty / rebuild / unchanged re-apply) |
|---|---|---|---|---|---|
| generated (capsules, `CreateFromSkeletalMesh` defaults) | 12 bodies, 11 constraints, 66 pairs | 11 / 13 ms | 7.3k | UpToDate | PASS 126 / 104 / 20 ms |
| generated (single convex hulls) | 12 convex bodies | 15 / 18 ms | 18.6k | UpToDate | PASS 83 / 138 / 29 ms |
| Engine TutorialTPP_PhysicsAsset (UE4 ragdoll, old save) | 23 bodies (capsules), 22 constraints, 22 pairs | 12 / 15 ms | 11.3k | UpToDate | PASS 74 / 139 / 23 ms |
| Template Characters PA_Mannequin (UE5 Manny) | 22 bodies (4 box, 18 capsule), 23 constraints, 92 pairs | 24 / 28 ms | 45.8k | UpToDate | PASS 106 / 262 / 53 ms |
| Template Weapons PA_Rifle | 1 body (capsule) | 9 / 9 ms | 0.9k | UpToDate | PASS 60 / 66 / 14 ms |
| Template Weapons PA_Pistol | 1 body (2 boxes) | 9 / 9 ms | 1.0k | UpToDate | PASS 61 / 65 / 14 ms |
| Template Weapons PA_GrenadeLauncher | 1 body (3 boxes, 2 capsules) | 9 / 10 ms | 1.6k | UpToDate | PASS 62 / 71 / 14 ms |
| Template Vehicles PA_SportsCar | 5 bodies (convex chassis, 4 wheel spheres), 4 constraints | 13 / 12 ms | 6.2k | UpToDate | PASS 65 / 88 / 18 ms |
| Template Vehicles SKM_Offroad_PhysicsAsset | 5 bodies (convex, boxes, spheres), 4 constraints | 15 / 13 ms | 4.7k | UpToDate | PASS 67 / 85 / 17 ms |
| MoverExamples PA_Mannequin | 22 bodies, 23 constraints, 227 pairs | 22 / 26 ms | 34.0k | Unknown (preview mesh `/Game/Characters/Mannequins/Meshes/SKM_Manny` missing in the plugin) | read only: no preview mesh, so no rebuild (`PhysicsAsset.PreviewMeshRequired` for new bodies) |
| ChaosModularVehicleExamples SportsCar_PhysicsAsset | - | - | - | - | skipped: plugin not enabled in the test sandbox |
| MetaHuman PA_MetaHuman_ARKit_Mapping | - | - | - | - | skipped: plugin not enabled in the test sandbox |

Fixes the corpus drove: `-0.0` components read as `0` (GrenadeLauncher box `Rotation.Yaw`, SportsCar / Manny
constraint axes); Snap / frame checks only when both bones are in the preview mesh (the engine asserts otherwise).

### Create from extract (§6.2 as written)

`CreateFidelity`: each source's extract (with `FactoryContext` = its preview mesh, `Action: Create`) creates a new
asset; its extract, status and runtime facts (a `USkeletalMeshComponent` instancing it: per-body mass, constraint
count, bounds bodies) equal the source's, the mesh is neither assigned nor dirtied, an unchanged re-apply is a no-op,
save + reload keeps the extract. Create time = the whole Create apply, PC (Windows) / Mac (M-series), Development
editor, 2026-10-08:

| Asset | Create PC / Mac | Doc size | Result |
|---|---|---|---|
| generated (capsules) | 295 / 144 ms | 7.5k | PASS, UpToDate |
| generated (single convex hulls) | 321 / 184 ms | 18.9k | PASS, UpToDate |
| Engine TutorialTPP_PhysicsAsset | 357 / 183 ms | 11.6k | PASS, UpToDate |
| Template Characters PA_Mannequin | 571 / 317 ms | 45.0k | PASS, UpToDate |
| Template Weapons PA_Rifle / PA_Pistol / PA_GrenadeLauncher | 168 / 206 / 172 ms; Mac 95 / 89 / 94 ms | 0.9k / 0.9k / 1.6k | PASS, UpToDate |
| Template Vehicles PA_SportsCar / SKM_Offroad_PhysicsAsset | 293 / 289 ms; Mac 119 / 120 ms | 6.1k / 4.6k | PASS, UpToDate |
| MoverExamples PA_Mannequin | - | - | skipped: no preview mesh in the plugin (a physics asset is created for a mesh) |
| ChaosModularVehicleExamples, MetaHuman | - | - | skipped: plugins not enabled in the test sandbox |

## Behaviour tests (`UEAssetDoc.PhysicsAsset.*`)

| Test | Covers |
|---|---|
| `Read` | tutorial asset + generated fixtures (capsules, single convex hulls): extract stable twice and after reload, Diff empty, no side effects, Inspect facts (`Skeleton`, `BodyFacts`, `MeshUsers`) |
| `Corpus` | design §6.1 corpus duplicated into `/Game`: the same read checks, timings |
| `Status` | `physics.status.v1`: errors / warnings at document paths, `Unknown` without a preview mesh |
| `CreateFidelity` | §6.2 as written: create from each source's extract (table above) |
| `FromScratch` | §6.4 AI-from-scratch flow through the document service: Validate makes nothing; an 11-body ragdoll (shapes, cone / twist limits, hinges, collision pairs) created UpToDate, Diff empty, simulated 60 physics steps without NaN (and it falls); `Generation` (boxes) then two bodies edited; `Fit` bodies named as the editor names them with its constraints; Inspect `Generation` = the extract of a default create; typical errors at their paths (unknown bone, constraint to a missing body, empty `Shapes` on a simulated body, pair with a missing body, unlisted profile, bad `Generation.LodIndex`, no mesh, unknown mesh) |
| `Fidelity` | §6.2 without Create: every source emptied by a document and rebuilt from its extraction; equal extract and status, unchanged re-apply no-op, reload stable |
| `FreeEdits` | §6.3: the editor's calls (Delete Body, Add Body + fit, Add Box, body / shape / constraint Details, profile entry + picker + Assign + edit, Disable Collision, convex copy) vs a document on two duplicates |
| `Authoring` | AI-written edit (new body, box-hull convex, explicit frames, constraint rename with profile data, an explicitly written default), `Fit` errors / `FitIgnored`, and typical errors at paths |
| `SaveReloadProfile` | `DefaultProfile` / `ProfileInstance` save swap: values across save + reload, after a human save while previewing a profile, profile renames |
| `Rollback` | failures after earlier phases changed the asset (late bad value, new status error) restore the saved state |
| `Undo` | undo history around a human edit, an AI apply and a failed apply |
| `OpenEditorApply` | P3: with the asset open in the Physics Asset Editor and not saved, an apply (a leaf body deleted with its constraint and pairs, a mass override, a profile) reopens the editor on the result (its preview shows the applied bodies); a failed apply rolls back and reopens it on the rolled-back asset; a human Details edit is saved, undone and redone; the editor closes without a prompt |

## Known limits

- The editor side of `FreeEdits` uses the transcribed `MakeNewBody` / `DeleteBody` (the same file the provider uses)
  for Add / Delete Body: `FPhysicsAssetEditorSharedData` is private and its commands act on the editor's selection, so
  a test cannot run the real command; every other editor-side edit is the engine / Details call the editor makes.
- The §6.4 flow is a C++ automation test driving documents through the document service (as the IK Rig `FromScratch`),
  not a Python test; the AI-facing black-box trial is P3 and scheduled separately.
- Apply to an asset open in the Physics Asset Editor stays refused (P3 decision).
- `Generation` / `Fit` with level-set geometry types make read-only shapes (Inspect `ReadOnlyShapes`).
