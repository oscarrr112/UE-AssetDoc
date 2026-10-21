# MaterialInstance Profile

Classes: /Script/Engine.MaterialInstanceConstant

Exact profile for `/Script/Engine.MaterialInstanceConstant`. Parameters are set and cleared by name, as in the Material Instance Editor; everything else in its Details panel (`PhysMaterial`, `BasePropertyOverrides`, `LightmassSettings`, `SubsurfaceProfile`, `NaniteOverrideMaterial`, ...) is generic `Properties`. `Parent` and the raw parameter arrays (`ScalarParameterValues`, `StaticParametersRuntime`, ...) are Body-owned (`MaterialInstance.BodyOwnedProperty`). `ThumbnailInfo` is editor state: not extracted and not authored (`MaterialInstance.NotAuthoredProperty`). Extract and Inspect list only `Properties` that differ from the class default, a struct (`BasePropertyOverrides`) with only its differing members (`diff_only: false` on extract: all).

```jsonc
"Body": {
  "Parent": "/Game/Materials/M_Base.M_Base",
  "Parameters": {"Roughness": 0.4, "BaseColor": [1, 0, 0, 1], "Albedo": "/Game/Textures/T_Rock.T_Rock",
                 "UseDetail": true, "Channels": {"R": true, "G": false, "B": false, "A": false},
                 "Metallic": null},
  "LayerParameters": {"1": {"Tiling": 4}}, "BlendParameters": {"1": {"Height": 0.5}},
  "RefractionDepthBias": 2.5
}
```

- Discover the parameters: `inspect_asset_document_target` on an existing instance returns `ParameterSchema`, one compact row per parameter the parent chain exposes: `{"Name", "Kind", "Path" (where to author it), "Overridden", "Value" (when overridden), "Default" (the parent's value), "Group"?, "Description"?, "UiMin"/"UiMax"?}`, plus `BaseMaterial`, `HasStaticPermutation` and `StaleOverrides`. For a new instance, create it with `Parent` and the overrides you know: an unknown name fails with `MaterialInstance.UnknownParameter`, whose message lists the parent's parameters as `Name (kind)`, so one more apply fixes every name. Floats are compared as the editor stores them (`0.3` is fine).
- `Parent`: object path of a Material or Material Instance Constant, or `null`. On create it is the factory's initial parent (a create without `Parent` makes a parentless instance). Changing it behaves like the editor: overrides whose name and type the new parent also exposes are kept, the rest are dropped; parameters in the same document are applied against the new parent. Errors: `MaterialInstance.ParentNotFound`, `MaterialInstance.InvalidParent`, `MaterialInstance.ParentCycle` at `/Body/Parent`.
- `Parameters` (global), `LayerParameters` / `BlendParameters` (layered materials, keyed by layer index `"0"`, `"1"`, ...): sparse. Omitted parameter = keep, a value = override (tick the box), `null` = clear the override (parent value). Extract lists overridden parameters only.
- Values per `Kind`: `scalar` number; `vector` / `double_vector` `[R, G, B, A]` (`{"R","G","B","A"}` accepted); `texture`, `texture_collection`, `runtime_virtual_texture`, `sparse_volume_texture`, `parameter_collection` object path (`""` = override with None); `font` `{"Font": path, "Page": int}`; `static_switch` bool; `static_component_mask` `{"R","G","B","A"}` bools. Errors at `/Body/Parameters/<name>`: `MaterialInstance.UnknownParameter` (with close names), `MaterialInstance.AmbiguousParameter`, `MaterialInstance.InvalidValue` (wrong type, wrong asset class, texture of the wrong sampler type), `MaterialInstance.AssetNotFound`.
- `RefractionDepthBias`: the Details value (number; `null` = parent value).
- `MaterialLayers`: the instance's own layer stack, read-only (`MaterialInstance.MaterialLayersReadOnly`); edit the stack in the editor.
- Static switches / masks change the static permutation; the apply queues the shader compile asynchronously (as the editor does) and does not wait for it.
- Apply works with the instance open in the Material Instance Editor: the editor stays open and shows the applied values.
- Material Function Instances and Landscape material instances use the generic profile.
