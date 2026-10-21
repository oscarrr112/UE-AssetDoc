# PhysicsAsset Profile

Classes: /Script/Engine.PhysicsAsset

Exact profile for Physics Assets (Python runtime, `physics_profile`). `extract_asset_document`, `inspect_asset_document_target`, `diff_asset_document` and `apply_asset_document_file` (Create and Update) work.

Edits are the Physics Asset Editor's own (Add Body, Delete Body, its constraint fix-ups, Details edits, Enable / Disable Collision, profile assignment), so the result is what a person gets in the editor. Apply works with the asset open in the Physics Asset Editor: the editor is closed for the apply and reopened on the result (Info `PhysicsAsset.EditorReopened`), because it tracks selection and its preview simulation by body index. Applying never changes the skeletal mesh that uses the asset (Inspect `MeshUsers` lists those meshes).

```jsonc
"Properties": {"ConstraintProfiles": ["Ragdoll"], "PhysicalAnimationProfiles": ["HitReact"]},
"Body": {
  "PreviewMesh": "/Game/Characters/SKM_Hero.SKM_Hero",
  "Bodies": {
    "upperarm_l": {
      "Properties": {"PhysicsType": "PhysType_Default", "DefaultInstance": {"bOverrideMass": true, "MassInKgOverride": 4.0}},
      "Shapes": {"Capsules": [{"Center": {"X": 14, "Y": 0, "Z": 0}, "Rotation": {"Pitch": 0, "Yaw": 0, "Roll": 90}, "Radius": 6.5, "Length": 18}]},
      "PhysicalAnimation": {"HitReact": {"OrientationStrength": 500, "AngularVelocityStrength": 50}}
    },
    "lowerarm_l": {"Shapes": {"Boxes": [{"Center": {"X": 7, "Y": 0, "Z": 0}, "X": 12, "Y": 4, "Z": 8}]}},
    "hand_l": {"Shapes": {"Convexes": [{"VertexData": [{"X": -3, "Y": -3, "Z": -3}, {"X": 3, "Y": -3, "Z": -3}, {"X": 0, "Y": 3, "Z": -3}, {"X": 0, "Y": 0, "Z": 3}]}]}}
  },
  "Constraints": {
    "lowerarm_l": {"Child": "lowerarm_l", "Parent": "upperarm_l",
                   "Properties": {"ProfileInstance": {"ConeLimit": {"Swing1Motion": "ACM_Locked", "Swing2LimitDegrees": 70},
                                                      "TwistLimit": {"TwistMotion": "ACM_Locked"}}},
                   "Profiles": {"Ragdoll": {"ConeLimit": {"Swing2LimitDegrees": 120}}}},
    "hand_l": {"Child": "hand_l", "Parent": "lowerarm_l"}
  },
  "CollisionDisabled": [["upperarm_l", "lowerarm_l"], ["lowerarm_l", "hand_l"]]
}
```

- Regions (`PreviewMesh`, `Bodies`, `Constraints`, `CollisionDisabled`) are the complete state when present and kept as they are when omitted.
- `PreviewMesh`: the skeletal mesh whose skeleton the bones come from. Adding a body needs it (`PhysicsAsset.PreviewMeshRequired`); a mesh that lacks a bone with a body is refused (`PhysicsAsset.IncompatibleMesh`).
- `Bodies` are keyed by bone. A new key is the editor's Add Body on that bone (constraints to the body above and to physical children are made as the editor makes them; then the document's `Constraints` decides what stays) and needs `Shapes` or `Fit` (`PhysicsAsset.ShapesRequired`; see Create). A removed key is the editor's Delete Body. A body cannot move to another bone (remove it and add one).
- Inside a body every key is sparse. `Properties`: the body Details (`PhysicsType`, `CollisionTraceFlag`, `PhysMaterial`, `DefaultInstance` fields such as `bOverrideMass`, `MassInKgOverride`, `LinearDamping`, `CollisionProfileName`, ...); an omitted field has the value of a new body. With a named collision profile (not `Custom`) the channels and responses come from the profile and are not written. `PhysicalAnimation`: the body's data per physical animation profile (omitted fields: a new profile entry's values); the profile must be listed in `Properties.PhysicalAnimationProfiles`.
- `Shapes`: `Spheres`, `Boxes`, `Capsules`, `TaperedCapsules`, `Convexes`, each an ordered array (shapes are positional). A present `Shapes` is complete: a type it does not name is emptied. Element fields are the shape Details (`Center`, `Rotation`, `Radius`, `Length`, `X`/`Y`/`Z`, `Radius0`/`Radius1`, `RestOffset`, `bContributeToMass`, `CollisionEnabled`); an omitted field is a new shape's value. `Name` defaults to the editor's auto name `<bone>_sphere` / `_box` / `_capsule` / `_tapered_capsule` / `_convex` (extract omits it then). A convex is `VertexData` (its hull points, at least four; the hull is cooked on apply) and an optional `Transform`. Level sets and skinned triangle meshes are read only (Inspect `BodyFacts.ReadOnlyShapes`; kept on apply; writing them: `PhysicsAsset.ShapeTypeNotAuthored`).
- `Constraints` are keyed by joint name (the editor names a constraint after its child bone). `Child` / `Parent` are the two bodies' bones (`PhysicsAsset.BodyNotFound` when one has no body). `Frames` (`{Child: {Position, PriAxis, SecAxis}, Parent: {...}}`) are the joint frames in each bone's space; omitted = the editor's Snap (joint at the child bone), which is what extract omits. `Properties` are the constraint Details below the joint (`ProfileInstance.LinearLimit`, `ConeLimit`, `TwistLimit`, `LinearDrive`, `AngularDrive`, `bDisableCollision`, ..., `AngularRotationOffset`, `bScaleLinearLimits`): the default profile; an omitted field is a new constraint's value. `Profiles`: the constraint's data per constraint profile, sparse against its default values; the profile must be listed in `Properties.ConstraintProfiles`. `"RenamedFrom": "<old joint name>"` renames a constraint (keeps its Details and profile data) instead of removing it and adding a new one. Extract keys repeated joint names of old content `<name>~2` (with a read warning); rename one of them.
- Writing a default value explicitly is the same as leaving it out (Diff and the post-apply check compare it against the default; extract omits it).
- `CollisionDisabled`: the complete set of body pairs whose collision is disabled (the editor's Disable Collision), in any order. New bodies get the editor default (collision disabled against the other bodies) unless the list is present.
- `Properties.ConstraintProfiles` / `PhysicalAnimationProfiles` are the profile lists as the Details panel edits them: a different name at the same list position renames the profile on every body and constraint; a removed name removes it everywhere. Other asset Details (`SolverType`, `SolverSettings`, ...) are top-level `Properties`. Not authored: the editor's current-profile preview, bounds bodies, the thumbnail.

## Create

`"Action": "Create"` makes the asset as the New Physics Asset dialog does, for the Skeletal Mesh in top-level `"FactoryContext": "<mesh object path>"` (else `Body.PreviewMesh`; neither: `PhysicsAsset.MeshRequired`; not a loadable Skeletal Mesh: `PhysicsAsset.UnknownMesh`). The mesh becomes the preview mesh; the new asset is never assigned to the mesh (assign it on the mesh yourself; Inspect `MeshUsers` shows it). Create-only keys, never extracted and ignored with a warning once the asset exists (`PhysicsAsset.GenerationIgnored`, `PhysicsAsset.FitIgnored`):

- `Body.Generation` (only without `Bodies`; with them: `PhysicsAsset.GenerationWithBodies`): the dialog's generation of the bodies and constraints from the mesh. Its fields are the dialog's settings, sparse over the project's Physics Asset generation settings: `GeomType` (`EFG_Sphyl` capsule, `EFG_Box`, `EFG_Sphere`, `EFG_TaperedCapsule`, `EFG_SingleConvexHull`, `EFG_MultiConvexHull`), `MinBoneSize`, `MinWeldSize`, `VertWeight` (`EVW_DominantWeight` / `EVW_AnyWeight`), `bAutoOrientToBone`, `bCreateConstraints`, `AngularConstraintMode` (`ACM_Limited`, `ACM_Free`, `ACM_Locked`), `bWalkPastSmall`, `bBodyForAll`, `bDisableCollisionsByDefault`, `HullCount`, `MaxHullVerts`, `LodIndex`. Other `Body` regions given with it (`CollisionDisabled`, ...) and `Properties` then edit the generated asset. A create without `Bodies` and without `Generation` generates with the project settings, as the dialog's defaults. Mesh without render data / nothing generated: `PhysicsAsset.GenerationFailed`.
- A new body's `Fit` (without `Shapes`): its shapes are fitted from the mesh vertices weighted to the bone, as the editor's Add Body does, with the same settings fields (`GeomType`, `VertWeight`, `bAutoOrientToBone`, `HullCount`, `MaxHullVerts`, `LodIndex`, ...); `{}` = project settings. A bone with too few vertices: `PhysicsAsset.FitFailed`. `Fit` also works when updating an existing asset (on a body it adds).

```jsonc
{"Target": "/Game/Characters/PA_Hero", "Class": "/Script/Engine.PhysicsAsset", "Action": "Create",
 "FactoryContext": "/Game/Characters/SKM_Hero.SKM_Hero", "Body": {"Generation": {"GeomType": "EFG_Box", "MinBoneSize": 10}}}
```

Explicit bodies instead: `"Body": {"Bodies": {"pelvis": {"Fit": {"GeomType": "EFG_Box"}}, "spine_03": {"Shapes": {"Spheres": [{"Radius": 15}]}}}, "Constraints": {"spine_03": {"Child": "spine_03", "Parent": "pelvis"}}}`. Then extract the asset and edit it as an ordinary document. `validate_asset_document` runs the whole create on a scratch asset and makes nothing.

Inspect adds `Generation` (when there is a preview mesh): `{Settings, Body}`, the project's generation settings and the `Bodies` / `Constraints` / `CollisionDisabled` the dialog would make with them, as a starting point for a document; also `Skeleton` (the preview mesh's bones with parents and whether each has a body), `BodyFacts` (mass as the Details show it, whether the bone is in the preview mesh, read-only shapes), `MeshUsers` (meshes that use the asset) and `ReadWarnings`.

## Compile

Extract, Apply and Inspect results carry `Compile: {Status, Messages[{Severity, Message, Path?, Source}]}`. A physics asset has no compile: the status is data validation (a body without shapes; a simulated body whose shapes do not contribute mass) and the checks under which the ragdoll breaks: errors for a constraint whose `Child` / `Parent` has no body, a constraint between a body and itself, profile data for an unlisted profile; warnings for a body whose bone the preview mesh lacks, a body below another body without a constraint (it falls off when simulated), an inverted constraint, a convex that does not cook. `Status` is `UpToDate`, `UpToDateWithWarnings`, `Error`, or `Unknown` (no preview mesh: bone checks skipped). An apply that adds an error fails with `PhysicsAsset.ValidationFailed` at that path and is rolled back; errors the asset already had come back as `PhysicsAsset.ValidationError` warnings and warnings as `PhysicsAsset.ValidationWarning`.
