# Material / MaterialFunction graph support matrix

Governing rules:
[design rules](asset-document-design-rules.md) §3 (rollback = functional
equivalence), §3.1 (Undo history), §4 (raw editor-equivalent code + behaviour test) and §5 (support
matrix). Reference editor: the Material Editor (`FMaterialEditor`, `UMaterialGraphSchema`,
`UMaterialEditingLibrary`). The legacy `MaterialGenerator` is not a reference.

| Class | Profile | Python |
|---|---|---|
| `/Script/Engine.Material` | `profile.material.v1` (route `data_profile`) | `profiles/material.py` |
| `/Script/Engine.MaterialFunction` | `profile.material_function.v1` | same |
| `/Script/Engine.MaterialFunctionMaterialLayer` | `profile.material_function_material_layer.v1` | same |
| `/Script/Engine.MaterialFunctionMaterialLayerBlend` | `profile.material_function_material_layer_blend.v1` | same |

C++: three primitives of the existing `data` provider (owner `Data`, no new provider), file
`Source/AssetDocumentData/Private/AssetDocumentDataMaterialPrimitives.cpp`:
`data.material.read.v1` (paged graph read, pins, per-class new-node values),
`data.material.edit.v1` (ops `create` / `delete` / `set` / `connect` / `output` /
`parameter_groups`) and `data.material.compile.v1` (the editor's Apply). They exist because no
generic primitive can run the Material Editor's graph operations: expression creation through the
editor's factory path, `FExpressionInput` wiring with the schema's type check, the Details-edit
follow-ups (`UpdateExpressionParameterName`, `PropagateExpressionParameterChanges`, attribute pin
rebuilds) and the recompile. Writes are covered by Core's package snapshot (existing asset) or the
factory rollback (created asset); there is no inverse code. Create uses the generic factory
lifecycle (`MaterialFactoryNew`, `MaterialFunctionFactoryNew`, layer / blend factories).

Classification: **Supported** (Extract, Apply, Diff), **Read-only**, **Unsupported** (reason given).

## Document shape

```jsonc
"Body": {
  "Expressions": {
    "Albedo": {"Class": "TextureSampleParameter2D", "Position": [-600, 0],
               "Properties": {"ParameterName": "Albedo", "Group": "Surface"},   // differences from a new node
               "Inputs": {"Coordinates": "Tiling"}},                           // pin -> "<key>" | "<key>.<output>"
    "Tiling": {"Class": "TextureCoordinate", "Position": [-900, 0], "Properties": {"UTiling": 4, "VTiling": 4}}
  },
  "Outputs": {"BaseColor": "Albedo.RGB"},     // materials only: the main node's pins
  "ParameterGroups": {"Surface": 0}            // materials only: Parameter Group Data
},
"Properties": {"MaterialDomain": "MD_Surface", "BlendMode": "BLEND_Opaque", ...}
```

`Expressions`, every expression's `Properties` / `Inputs`, `Outputs` and `ParameterGroups` are
complete when present; an omitted section is left as it is. The agent-facing description is
`MCP/schemas/assetdocument/Material.md`.

## Identity

- Expression key = the expression's object name inside the material / function (`GetName()`). It is
  stable across save, reload and Content Browser duplication, so extract → apply to a new asset
  keeps it. A new expression is created through the editor's path and renamed to its key; a
  deleted one is renamed out of the way (`Deleted_<key>`) before the editor delete so the key can
  be reused for another class in the same apply. A different `Class` on a key = delete + create.
- Comments are expressions (`Class: "Comment"`, held in `EditorComments`).
- Expression references inside properties (named reroute `Declaration`, composite pin bases) are
  keys.
- Sources: `"<key>"` = output 0 (also equivalent to `"<key>.<output-0 name>"`: older assets save
  output 0 unnamed where a new node names it, e.g. `RGB`); `"<key>.<name>"`; `"<key>.#<n>"` for
  unnamed / duplicate outputs. Inputs by `GetInputName` (function calls: the input name without the
  type suffix); `#<n>` when not unique.
- The order of the expression array is not carried (the core sorts JSON keys). Existing nodes keep
  their array position; new nodes are appended in key order.

## Matrix

| Editor surface | Body location | Class | Mechanism / reason |
|---|---|---|---|
| New Material / Function / Layer / Layer Blend (Content Browser) | `Action: Create` | Supported | generic factory lifecycle; graph + Details in one apply; a failed create leaves nothing |
| Palette: place a node | `Expressions.<key>` (new key) | Supported | `UMaterialEditingLibrary::CreateMaterialExpressionEx` (= `FMaterialEditor::CreateNewMaterialExpression`); `IsAllowedIn` as the palette filters (`Material.ExpressionNotAllowed`); abstract / deprecated classes refused |
| Comment box (C) | `Class: "Comment"` | Supported | the editor's comment command (default size and colour); `Text`, `SizeX/SizeY`, `CommentColor`, `FontSize`, bubble flags |
| Delete node | key removed | Supported | `DeleteMaterialExpression`; links to it break as in the editor |
| Move node | `Position` | Supported | `MaterialExpressionEditorX/Y`; integers |
| Wire / unwire a pin | `Inputs.<pin>` | Supported | `UMaterialGraphSchema::CanCreateConnection` type check (`Material.IncompatibleConnection`) + `FExpressionInput::Connect`; cycles refused (`Material.ConnectionCycle`); wiring runs sources-forward so dynamic pin types resolve as in a human session |
| Main material node pins | `Outputs.<attribute>` | Supported | `GetExpressionInputForProperty`; names from `FMaterialAttributeDefinitionMap` (incl. `MaterialAttributes`, Substrate `FrontMaterial`) |
| Details panel of a node | `Properties` | Supported | Modify, `PreEditChange`, write, `PostEditChangeProperty`, then `FMaterialEditor::NotifyPostChange` (parameter rename across same-named nodes, `PropagateExpressionParameterChanges`, dynamic parameter update) |
| Parameters (scalar, vector, texture, static switch / mask, RVT, collection, ...) | node `Properties` | Supported | `ParameterName`, `Group`, `SortPriority`, defaults, slider ranges as Details values |
| Material Function Call (pick a function) | `Properties.MaterialFunction` | Supported | Details edit (the call's `PostEditChangeProperty` rebuilds its pins from the function); a function edit updates callers (`UpdateMaterialFunction`) |
| Function inputs / outputs (functions, layers, blends) | `FunctionInput` / `FunctionOutput` nodes | Supported | incl. `Preview` pins, `bLastPreviewed` (previewed output) |
| Set / Get Material Attributes pin lists | `Properties.AttributeSetTypes` / `AttributeGetTypes` | Supported | the graph node's `PostEditChangeProperty` pin rebuild, same result without a graph: inputs / outputs rebuilt by attribute id, downstream output indices remapped, links to removed pins break |
| Named reroutes | `NamedRerouteDeclaration` / `NamedRerouteUsage` + `Declaration` key | Supported | `NodeColor` is random per new node: always extracted, omitted = keep |
| Custom (HLSL) node | `Properties.Code`, `Inputs`, `AdditionalOutputs`, ... | Supported | Details values; dynamic pins follow |
| Composites, pin bases, reroutes | node `Properties` / `Inputs` | Supported | references by key |
| Pins hidden by a node setting (e.g. `LegacyBlendMaterialAttributes` Use A/B) | `Inputs.<member name>` | Supported | reflected `FExpressionInput` members that `GetInput` does not return keep their links |
| Links from nodes without output pins (legacy) | `"<key>"` | Supported | link assigned as the asset stores it (index 0, no masks) |
| Collapse, comment bubble | `bCollapsed`, `bCommentBubbleVisible` | Supported | node state as properties |
| Parameter Group Data (Details) | `ParameterGroups` | Supported | first entry wins on duplicates, as `GetGroupSortPriority` reads them |
| Material Details (domain, blend, shading model, usage, Substrate, ...) | `Properties` | Supported | generic property admission |
| Apply (compile) | — | Supported | one `RecompileMaterial` / `UpdateMaterialFunction` per apply; new errors fail (`Material.CompileFailed`, rolled back); pre-existing errors are warnings; shaders not awaited |
| Asset open in the Material Editor | — | Supported (close / apply / reopen) | the editor edits a preview copy and writes it back on its Apply, Save and Undo, with no path that reads the asset again: it is closed for the apply and reopened (`Material.EditorReopened`); pending edits in the copy (its Apply enabled) refuse before any change (`Material.OpenInEditorUnsavedEdits`). An apply is not an undo step. `UEAssetDoc.Material.OpenEditorApply.Material` / `.Function` |
| Thumbnail (preview mesh / camera) | — | Unsupported | `ThumbnailInfo`: editor state, not material content (`Material.NotAuthoredProperty`) |
| Legacy pin constant (`UseConstant` / `Constant` on `FExpressionInput`) | — | Unsupported | not editable in the Material Editor; stays as stored on existing nodes; new nodes use their `Const*` properties |
| Expression array order | — | Unsupported | not a graph property; it only renumbers texture references (`MaterialStoreTexSample` index, preshader `TEX[n]`) and custom vertex interpolators in the generated HLSL |
| Selection, viewport, node search, preview mesh | — | Unsupported | editor UI state |
| Material Instances | — | separate profile | [MI matrix](asset-document-material-instance-matrix.md) |
| Material Function Instances, Landscape materials | — | generic profile | — |

## Compile and shader behaviour

The edit primitive marks the material as the editor's preview copy (`bIsPreviewMaterial`) for the
duration of the edit, so node edits do not recompile one by one; the compile primitive then runs
the editor's Apply once. The before-state's errors are read without recompiling (current shader
platform resource); only errors the material did not have fail the apply. Function applies call
`UpdateMaterialFunction`, which recompiles the materials that use the function.

## Acceptance evidence

`UEAssetDoc.Material.*` (5 tests, all green):

- **Fidelity: 33/33** real engine assets (21 materials, 12 functions / layers / blends): duplicate
  → Extract → apply as a new asset → node graph, links, Details values, parameters, compile errors
  and translated HLSL equal (HLSL normalized for texture / interpolator numbering) → re-apply is a
  no-op. Includes DefaultMaterial, WorldGridPreviewMaterial (178 nodes), m_SimpleVolumetricCloud
  (297), PivotPainter2FoliageShader, ParallaxOcclusionMapping_BoundedUVs, MF_Substrate_OpenPBR_Opaque
  (258), Substrate-StandardSurface-Translucent, MLB_Vertical, MatLayerBlend_Standard, post process,
  light function and decal materials. Two pass with notes: SelectedGeometryMaterial (same HLSL lines
  in another interpolator order) and m_SimpleVolumetricCloud (extracts `"K"` where the original
  spells `"K.RGB"`; the original document applies to the new asset as a no-op).
- **FreeEdit**: on two duplicates of WorldGridMaterial, the same edits (insert a node into a
  connection, delete a node, reconnect, change and rename a parameter, move nodes) through the
  document and through the editor API give the same graph, HLSL, parameters and compile errors.
- **Python**: an AI-from-scratch PBR material (parameters, a WorldAlignedTexture function call,
  Fresnel emissive, comment, groups) creates and compiles in one apply; a Material Instance of it
  sees the parameters; free edits; refusals at document paths; rollback after a delete and on a new
  compile error; reload from disk.
- **Function**: function + caller; editing the function updates the caller; layer and layer blend.
- **Undo**: human edit, AI apply, Ctrl+Z behaviour (§3.1).

Function fidelity compares graphs and documents; there is no HLSL comparison for functions (they
have no shader of their own).
