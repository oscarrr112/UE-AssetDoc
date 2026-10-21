# Material Profile

Classes: /Script/Engine.Material, /Script/Engine.MaterialFunction, /Script/Engine.MaterialFunctionMaterialLayer, /Script/Engine.MaterialFunctionMaterialLayerBlend

Exact profiles for materials and material functions (including Material Layer and Material Layer Blend functions). The Body is the expression graph the Material Editor shows: nodes, comments, connections, the main material node's pins and the parameter group order. Material-level settings in the Details panel (`MaterialDomain`, `BlendMode`, `ShadingModel`, `TwoSided`, usage flags, ...; for functions `Description`, `bExposeToLibrary`, `LibraryCategoriesText`, ...) are generic `Properties`. The raw graph containers (`Expressions`, `EditorOnlyData`, `ExpressionCollection`, `FunctionExpressions`, `ParameterGroupData`, ...) are Body-owned; `ThumbnailInfo` is not authored.

```jsonc
"Body": {
  "Expressions": {                                      // every node and comment, by key
    "UV":     {"Class": "TextureCoordinate", "Position": [-1200, -200], "Properties": {"UTiling": 2, "VTiling": 2}},
    "Albedo": {"Class": "TextureSampleParameter2D", "Position": [-900, -200],
               "Properties": {"ParameterName": "Albedo", "Group": "Surface", "Texture": "/Game/T/T_Rock.T_Rock"},
               "Inputs": {"Coordinates": "UV"}},
    "Tint":   {"Class": "VectorParameter", "Position": [-900, 50],
               "Properties": {"ParameterName": "Tint", "Group": "Surface", "DefaultValue": {"R": 1, "G": 0.5, "B": 0.25, "A": 1}}},
    "Mul":    {"Class": "Multiply", "Position": [-600, -100], "Inputs": {"A": "Albedo.RGB", "B": "Tint.RGB"}},
    "Wat":    {"Class": "MaterialFunctionCall", "Position": [-900, 350],
               "Properties": {"MaterialFunction": "/Engine/Functions/Engine_MaterialFunctions01/Texturing/WorldAlignedTexture.WorldAlignedTexture"},
               "Inputs": {"TextureObject": "Detail", "TextureSize": "Size"}},
    "Detail": {"Class": "TextureObjectParameter", "Position": [-1200, 300], "Properties": {"ParameterName": "Detail"}},
    "Size":   {"Class": "Constant3Vector", "Position": [-1200, 500], "Properties": {"Constant": {"R": 256, "G": 256, "B": 256, "A": 0}}},
    "Blend":  {"Class": "LinearInterpolate", "Position": [-400, 0], "Properties": {"ConstAlpha": 0.3},
               "Inputs": {"A": "Mul", "B": "Wat.XYZ Texture"}},
    "Fres":   {"Class": "Fresnel", "Position": [-700, 600], "Properties": {"Exponent": 3}},
    "Emis":   {"Class": "Multiply", "Position": [-400, 650], "Inputs": {"A": "Fres"}, "Properties": {"ConstB": 2}},
    "Note":   {"Class": "Comment", "Position": [-1250, -300], "Properties": {"Text": "Albedo", "SizeX": 800, "SizeY": 450}}
  },
  "Outputs": {"BaseColor": "Blend", "EmissiveColor": "Emis"},                      // materials only
  "ParameterGroups": {"Surface": 0, "Detail": 1}                                    // materials only
},
"Properties": {"MaterialDomain": "MD_Surface", "BlendMode": "BLEND_Opaque", "TwoSided": true}
```

- Start from `extract_asset_document` of an existing graph, or `create_asset_document_template`. `inspect_asset_document_target` on a `/Game` material or function returns the document (an outline unless `detail: true`) plus `Pins` (each node's `Inputs` / `Outputs` as `"Name: type"`), `MaterialInputs` (the main node's pins with their types) and `CompileErrors` (the current errors, if any).
- `Expressions` is complete: a key not listed is deleted (its links are broken, as the editor's delete does), a new key creates a node, a different `Class` on a key replaces the node (its links, other nodes' inputs and `Outputs` naming the key, are made again to the new node). The key is the node's object name (letters, digits, `_`, `-`; no spaces, `.`, `/`, `:`, `#`); it stays the same across save, reload and duplication. Extract keeps the editor's names (`MaterialExpressionMultiply_3`); new keys can be anything readable.
- `Class`: native expression classes by short name without the `MaterialExpression` prefix (`Multiply`, `TextureSampleParameter2D`, `MaterialFunctionCall`, `Custom`, `NamedRerouteDeclaration`, `Comment`, ...); other classes by class path. A class the editor's palette does not offer in this asset fails with `Material.ExpressionNotAllowed` (for example `FunctionInput` in a material). Unknown classes fail with `Material.UnknownExpressionClass` and close names.
- `Position`: `[x, y]` integers. Omitted keeps the node where it is; a new node without it is placed in columns left of the material node.
- `Properties`: the node's Details panel values, complete: an omitted property has the value of a new node of that class, and extract lists only the values that differ. Values use the AssetDocument value forms (numbers, `{"R","G","B","A"}` colors, enum names, object paths for textures and functions). Expression references (a named reroute usage's `Declaration`, ...) are keys. Node state is a property too: `bCollapsed`, `bCommentBubbleVisible`, comment `SizeX`/`SizeY`/`Text`/`CommentColor`, `bLastPreviewed` on a function output.
- `Inputs`: `{"<input pin>": "<source>"}`, complete (an omitted input is not connected). A source is `"<key>"` (output 0) or `"<key>.<output name>"` (`"Albedo.RGB"`, `"Wat.XYZ Texture"`); `"<key>.#<n>"` names an unnamed or duplicate output by index, and an input pin without a unique name is `"#<n>"`. Pin names are the ones on the node (a function call's inputs are the function's input names); a pin a node setting hides keeps its link under its member name. Read pins with `inspect_asset_document_target`.
- `Outputs` (materials only, complete): the main material node's pins by attribute name (`BaseColor`, `Metallic`, `Specular`, `Roughness`, `Anisotropy`, `EmissiveColor`, `Opacity`, `OpacityMask`, `Normal`, `Tangent`, `WorldPositionOffset`, `Displacement`, `SubsurfaceColor`, `CustomData0`, `CustomData1`, `AmbientOcclusion`, `Refraction`, `PixelDepthOffset`, `ShadingModel`, `SurfaceThickness`, `FrontMaterial` (Substrate), `MaterialAttributes` (with `bUseMaterialAttributes`), ...). Which pins are active depends on the domain, blend mode and shading model, as in the editor. On a function, `Outputs` fails with `Material.OutputsOnFunction`; a function's outputs are `FunctionOutput` nodes. A number is a constant for that pin (`"Roughness": 0.15`): the apply makes the Constant node a person would wire there (key `Constant_<pin>`, `R` = the number) and extract writes it back as the number; naming another source in its place deletes that node.
- `ParameterGroups` (materials only, complete): the Details "Parameter Group Data" list, `{"<group>": <sort priority>}`.
- A Body section that is omitted is left as it is.
- Apply edits the graph the editor's way and then recompiles once (the editor's Apply). It fails with `Material.CompileFailed` (and is rolled back) when the recompile has errors the material did not have before; errors it already had come back as `Material.CompileError` warnings. Shader compilation is not awaited. A function apply updates the materials that use it, as the Function Editor's Apply does.
- Material Instances have their own section (`profile: "MaterialInstance"`); Material Function Instances use the generic profile.
