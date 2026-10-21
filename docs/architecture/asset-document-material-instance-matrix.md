# MaterialInstanceConstant support matrix

Status: 2026-10-04. Governing rules:
[design rules](asset-document-design-rules.md) §3 (rollback = functional
equivalence), §3.1 (Undo history), §4 (raw editor-equivalent code + behaviour test) and §5 (support
matrix). Reference editor: the Material Instance Editor (`FMaterialInstanceEditor` with its
`UMaterialEditorInstanceConstant` proxy, `Editor/UnrealEd/Private/PreviewMaterial.cpp`).

| Class | Profile | Python |
|---|---|---|
| `/Script/Engine.MaterialInstanceConstant` | `profile.material_instance_constant.v1` (route `data_profile`) | `profiles/material_instance.py` |

The C++ side is two primitives of the existing `data` provider (module `Source/AssetDocumentData/`,
file `AssetDocumentDataMaterialInstancePrimitives.cpp`): `data.material_instance.read.v1` and
`data.material_instance.edit.v1`. They were added because no generic primitive can list the
parameters a parent chain exposes (`UMaterialInstance::GetAllParametersOfType` is not reflected) or
run the editor's override update (`FMaterialInstanceParameterUpdateContext` →
`UpdateStaticPermutation`); the static switch / component mask overrides live in the
non-reflected static parameter set. Writes are covered by Core's package snapshot (existing asset)
or the factory rollback (created asset); there is no inverse code. Create uses the generic factory
lifecycle (`MaterialInstanceConstantFactoryNew`, `InitialParent` = factory context).

Classification:
- **Supported**: Extract, Apply and Diff.
- **Read-only**: Extract and Diff only; authoring a different value fails with a stable diagnostic.
- **Unsupported**: not in the document; the reason is given.

## Document shape

```jsonc
"Body": {
  "Parent": "/Game/Materials/M_Base.M_Base",             // null: no parent
  "Parameters": {                                          // overridden global parameters, by name
    "Roughness": 0.4,                                      // scalar
    "BaseColor": [1, 0, 0, 1],                             // vector [R,G,B,A] ({"R","G","B","A"} accepted)
    "Albedo": "/Game/Textures/T_Rock.T_Rock",              // texture ("" = override with None)
    "UseDetail": true,                                     // static switch
    "Channels": {"R": true, "G": false, "B": false, "A": false},  // static component mask
    "Metallic": null                                       // untick the override (parent value)
  },
  "LayerParameters": {"1": {"Tiling": 4}},                 // layered materials, by layer index
  "BlendParameters": {"1": {"Height": 0.5}},
  "RefractionDepthBias": 2.5,                              // Details: Refraction Depth Bias
  "MaterialLayers": {"Layers": [...], "Blends": [...], "Names": [...], "Visible": [...]}  // read-only
},
"Properties": {"PhysMaterial": "/Game/Phys/PM_Rock.PM_Rock", "BasePropertyOverrides": {...}, ...}
```

Parameter maps are sparse: an omitted parameter keeps its state, a value overrides it, `null`
clears the override (a `null` on a parameter that is not overridden is a no-op). Extract lists the
overridden parameters only, sorted by name; the parameter arrays never appear as `Properties`.

## Matrix

| Editor surface | Body location | Class | Mechanism / reason |
|---|---|---|---|
| New Material Instance (Content Browser, or "Create Material Instance" on a material) | `Action: Create` + `Parent` | Supported | generic factory lifecycle; `Parent` is resolved (`editor.reference.resolve_asset`, expected `MaterialInterface`) and handed to `UMaterialInstanceConstantFactoryNew::InitialParent` as `factory_context` (optional: a create without `Parent` makes a parentless instance, as the New menu does). Create + overrides + Details in one apply. Unknown parent: `MaterialInstance.ParentNotFound` at `/Body/Parent`; a failed create leaves nothing |
| Parent (Details) | `Parent` | Supported | the proxy's Parent change: `PreEditChange`, `UMaterialInstanceConstant::SetParentEditorOnly` + `PostEditChange` inside an `FMaterialUpdateContext`, then the full override rewrite below. Only a Material or a Material Instance Constant (`MaterialInstance.InvalidParent`); not this instance or one of its children (`MaterialInstance.ParentCycle`) |
| Parent change: overrides of the old parent | — | Supported (editor behaviour) | as in the editor (`RegenerateArrays` + `CopyToSourceInstance(ClearParameterFlag::All)`): an override is kept when the new parent exposes a parameter with the same name, association and type; the others are dropped. Authored parameters in the same document are applied against the new parent |
| Parameter override checkbox + value: scalar, vector, double vector, texture, texture collection, runtime virtual texture, sparse volume texture, font, material parameter collection | `Parameters.<name>` | Supported | `FMaterialInstanceParameterUpdateContext(ClearParameterFlag::All)` + `SetParameterValueEditorOnly(…, SetCurveAtlas)` for every override + `PostEditChange` + `MarkPackageDirty` (= `UMaterialEditorInstanceConstant::CopyToSourceInstance`). Values are decoded against the parameter's kind: `MaterialInstance.InvalidValue` (wrong JSON type, wrong asset class, texture whose sampler type the parameter cannot read: the editor's picker would not offer it), `MaterialInstance.AssetNotFound` (missing asset), both at `/Body/Parameters/<name>` |
| Static switch, static component mask | `Parameters.<name>` | Supported | same update; the context's destructor runs `UMaterialInstance::UpdateStaticPermutation` (see "Shader compilation") |
| Untick an override (revert to parent) | `Parameters.<name>: null` | Supported | the override is left out of the rewrite |
| Unknown parameter | `Parameters.<name>` | — | `MaterialInstance.UnknownParameter` at the parameter path, with close names and the parent's parameter list (`Name (kind)`, first 40), so every name can be fixed in one round; nothing changes. Two parameters of different types with the same name: `MaterialInstance.AmbiguousParameter` |
| Discover parameters (the editor's parameter list: groups, defaults, override state) | Inspect → `ParameterSchema [{Name, Kind, Path, Overridden, Value? (when overridden), Default, Group?, Description?, DeclaredBy? (material function), UiMin/UiMax?, Association/LayerIndex?}]`, `BaseMaterial`, `HasStaticPermutation`, `StaleOverrides?` | Supported | `data.material_instance.read.v1` (`GetAllParametersOfType` per type; `Default` = `GetParameterValue(CheckNonOverrides)`, the value unticking would give). `Path` is the document location to author |
| Parameters of an instance-of-an-instance | `Parameters` | Supported | the parent chain exposes the root material's parameters; `Default` is the parent instance's value |
| Material layer / blend parameter values (layered materials) | `LayerParameters.<index>.<name>`, `BlendParameters.<index>.<name>` | Supported | same mechanism with `FMaterialParameterInfo{Association, Index}` (not covered by an editor E2E test: the test parents are built without a layer stack; covered by Python unit tests) |
| Material layer stack (add / remove / replace layer and blend functions, layer names, visibility) | `MaterialLayers` | Read-only | extracted when the instance has its own stack; a different value fails with `MaterialInstance.MaterialLayersReadOnly`. Authoring the stack needs the Layer Parameters panel's `FMaterialLayersFunctions` edits (layer GUIDs, link state to the parent, restrictions); left for a later step. Every edit re-applies the current stack (`SetMaterialLayers`), as the editor does |
| Refraction Depth Bias (Details) | `RefractionDepthBias` | Supported | the editor stores it as a scalar override entry `RefractionDepthBias` that no parent parameter exposes; every edit keeps it, a number sets it, `null` removes it (parent value) |
| Overrides of parameters the parent no longer exposes (stale entries) | Inspect `StaleOverrides` | Read-only | not extracted. The next edit drops them, as the Material Instance Editor's next edit does |
| Details: PhysMaterial, PhysicalMaterialMap, PhysMaterialMask, BasePropertyOverrides (blend mode, shading model, two sided, opacity mask clip, …), LightmassSettings, SubsurfaceProfile / bOverrideSubsurfaceProfile, NaniteOverrideMaterial, post-process overrides (BlendableLocation/Priority, UserSceneTextureOverrides), AssetUserData | `Properties` | Supported | generic Editor property admission (`Modify`, `PreEditChange`, set, `PostEditChangeProperty`; `UMaterialInstance::PostEditChangeProperty` runs `UpdateStaticPermutation` for base property overrides). `Parent` and the nine raw parameter arrays are Body-owned: `MaterialInstance.BodyOwnedProperty` at `/Properties/<name>` |
| Enumeration objects (scalar parameter enum sources) | — | Unsupported | `UMaterialInstance::EnumerationObjects` is not an editable reflected property on the instance (the editor edits it through its proxy); rare |
| Scalar curve atlas parameters | `Parameters.<name>` (number) | Supported (value) | the position is authored as a number; the curve/atlas binding is kept (`SetCurveAtlas`) but not authored. A parent change does not run the editor's `ClearInvalidParameterOverrides` atlas check |
| Instance open in the Material Instance Editor | — | Supported (in place + refresh) | the editor's proxy (`UMaterialEditorInstanceConstant`) writes its parameter list back on every Details edit and on every undo / redo (`CopyToSourceInstance`): after the apply and after a rollback it reads the instance again (`SetSourceInstance`) and the editor refreshes (`NotifyExternalMaterialChange`). No pending state of its own (its edits dirty the package). An apply is not an undo step. `UEAssetDoc.MaterialInstance.OpenEditorApply` |
| Save / reload | — | Supported | `UEAssetDoc.MaterialInstance.Python` reloads from disk to the same Extract |
| Material Function Instance / Layer Instance / Layer Blend Instance | — | Unsupported (generic route) | the editor edits them through a preview material instance and copies the arrays back (`ApplySourceFunctionChanges` + `UMaterialEditingLibrary::UpdateMaterialFunction`); not the same mechanism. They stay on the generic reflected route |
| LandscapeMaterialInstanceConstant | — | Unsupported (generic route) | a Landscape-module subclass with terrain layer weight static parameters; stays on the generic route |
| Material (expression graph) | — | Supported elsewhere | own profiles (`profile.material.v1`, function / layer / blend): [material matrix](asset-document-material-matrix.md) |

## Shader compilation

Static switches, component masks, the layer stack and base property overrides change the static
permutation: `UpdateStaticPermutation` calls `CacheResourceShadersForRendering(None)`, which queues
an asynchronous shader compile, and an `FMaterialUpdateContext` recreates the render state of
components that use the instance. The apply does not wait for the compile (neither does the
editor); the package is saved right away, which is correct because an editor (non-cook) save of a
material instance does not serialize shader maps. Measured in
`UEAssetDoc.MaterialInstance.Python` (sandbox editor, macOS, M-series): a create
with a static switch and component mask override takes about 0.15 s and a static switch edit about
0.16 s for the whole apply (validate, snapshot, edit, save, fresh-load verification), while the
permutation's shaders keep compiling in the background (the log shows those jobs). A scalar /
vector / texture edit does not change the permutation and only recaches uniform expressions.

## Rollback and Undo

Every apply runs the edit inside one editor transaction (`PreEditChange` records the instance). A
successful apply clears the undo history that references the instance (`AssetDocumentUndoHistory.h`);
a failed apply restores the package snapshot (parent, overrides, static permutation) and keeps the
human's history (`UEAssetDoc.MaterialInstance.Undo`).

## Tests

- Python: `Content/Python/asset_document/tests/test_material_instance_profile.py` (document shape,
  value forms, op planning, Extract/Diff, path mapping, Body-owned properties, read-only layers).
- Editor (production Service, `ApplyFile` / `Extract` / `Diff` / `Inspect`):
  `UEAssetDoc.MaterialInstance.Python` and `.Undo`
  (`Source/AssetDocumentData/Private/Tests/AssetDocumentMaterialInstancePythonTests.cpp`). The parent
  materials are built in the test with `UMaterialEditingLibrary` (there is no Material route yet).
