# Generic Reflected Profile

Classes: any class without an exact profile (DataAsset subclasses, CurveVector, InputAction, InputMappingContext, Material Function Instances, ...)

Any class without an exact profile (for example `UDataAsset`/`UPrimaryDataAsset` subclasses, `CurveVector`, input actions, Material Function Instances, ...) uses the generic profile. It authors reflected `Properties` only:

- `Body` must be absent or `{}` (`Generic.GenericBodyNotEmpty` otherwise).
- Every `Properties` key must be an admitted, writable reflected property of `Class` (`Generic.UnknownProperty`, `Generic.NonWritableProperty`). Use `inspect_asset_document_target` to list them; it also lists non-admitted properties under `skipped` with a reason (`saved-by-asset`: the asset saves the value in its own data, not as a property, e.g. `SkeletalMesh.Materials`, whose slots the facts' `MaterialSlots` list).
- Properties are sparse: only listed properties are written; omitted properties keep their current value. Diff compares only the properties you list.
- `extract_asset_document` with `diff_only: true` (the default) returns only the properties whose value differs from the class default (a struct, also an array's struct elements, with only its differing members: an apply keeps the members a value omits); `diff_only: false` or `include_all_writable: true` returns every writable property. Extract output includes `"AssetType": "GenericAsset"` and no `Body`.
- Admitted value types: everything the Details panel edits, written as the editor would store it:
  - bool, integer and floating-point numbers, `FString`, `FName`, enums (enumerator name string).
  - `FText`: the source string (`"Jump"`). Texts that are culture-invariant, come from a string table or look like a text macro use UE's text form (`INVTEXT("...")`, `LOCTABLE("Table","Key")`, `NSLOCTEXT("Ns","Key","Source")`). An edited text keeps its localization namespace and key, as the Details text box does; an unchanged text is left as it is, and `""` clears it.
  - Object references (hard `UObject*`/`TObjectPtr`, weak, lazy, `TScriptInterface`): the exact object path string (`"/Game/Dir/Asset.Asset"`, subobjects `"/Game/Dir/Asset.Asset:Sub"`), or `null` to clear. Soft references use the same path string; an empty soft reference reads `"None"`. Hard references are resolved and loaded on apply and must match the property class and its `AllowedClasses`/`DisallowedClasses`/`ExactClass` metadata. A plain (non-instanced) reference may not name an object inside the edited asset; see editor-derived references below.
  - Class references (`TSubclassOf`, `UClass*`; `TSoftClassPtr` as a soft path): the class path (`"/Script/Engine.Texture2D"`, `"/Game/Dir/BP.BP_C"`) or `null`; checked against the meta class, `MustImplement` and the class filters.
  - Instanced subobjects (`Instanced`/`EditInlineNew` properties, also inside arrays, maps and structs): `{"Class": "<class path>", "Properties": {...}}` or `null`. `Properties` is partial: fields it omits keep their current value, and a listed field is written even when it equals the default. Extract lists only the fields that differ from the class defaults. A new class must be concrete and a subclass of the property class, and must pass the class filters. Arrays, sets and map values of instanced subobjects may not gain `null` entries (many owners iterate them without null checks); maps of plain references keep `null` values. If the class stays the same, the existing subobject is edited in place. A different class replaces it with a new subobject that starts from that class's defaults, the way the class picker does. A `VisibleAnywhere` instanced subobject (for example `HLODLayer.HLODBuilderSettings`) is edited in place only, as in the Details panel: `Class` must be its current class, and `null` or another class is rejected.
  - Editor-derived references: a plain hard reference holds an object inside the edited asset, at the top level or inside a struct, array, set or map value. Such a value was created by the asset's own code. Examples are `PaperSprite.BodySetup`, which every sprite edit recreates and renames, and default subobjects such as `ConstantQNRT.Settings`. Plain means not instanced and without `EditInline`/`ShowInnerProperties` metadata. The Details panel asset picker lists assets only, so no one can author such a value. Extract omits the whole property, so extracts and diffs stay stable. `inspect_asset_document_target` lists it under `skipped` with reason `editor-derived-reference`. A value that names an object inside the asset through a plain reference is rejected with `Property.EditorDerivedReference`, but a reference the property already holds may stay. As with the picker, you may replace the stored object with an asset or `null`, or remove the array element. If the editor rebuilds the object, as `BodySetup` does, preflight rejects the change with `Generic.PropertyValueNotStoredExactly`. Author the properties that drive it instead, such as the sprite's collision settings. The rule depends on the current value: while the property holds `null` or an asset, it is an ordinary reference. References inside instanced subobjects keep their object path; edit-inline references are described below.
  - Edit-inline references (a plain object reference with `EditInline` metadata, for example `AnimCurveCompressionSettings.Codec`): the Details panel has no asset picker for them, only a class picker that creates the object inside the asset and the object's own fields inline. The asset's own object (a subobject) reads and writes like an instanced subobject: `{"Class", "Properties"}` with the same class edits it in place, another class creates a new one inside the asset (only `EditInlineNew` classes, as in the picker). An object of another asset reads as its object path; the Details panel would edit that object's fields in place and change that asset, so an edit of its fields is refused (the message names the asset: author that asset's own document). A path string is still accepted as a reference.
  - `FInstancedStruct`: `{"Struct": "<struct path>", "Properties": {...}}` or `null`; partial like instanced subobjects (the same struct keeps omitted members, another struct starts from its defaults); `BaseStruct` metadata is enforced.
  - Structs with a native text form (`FGuid`, `FKey`, `FDateTime`, `FTimespan`, gameplay tags, `FPrimaryAssetId`, ...): their export-text string (`"SpaceBar"`, `"0123456789ABCDEF0123456789ABCDEF"`). All other structs are recursively admitted objects. A row handle (`{"DataTable", "RowName"}`) must name a row of its table and an `FKey` a registered key, wherever they are written (Properties, DataTable cells, Blueprint defaults, `edit_level`): otherwise validate fails with `Property.RowNotFound` / `Property.KeyNotFound`, naming the close names.
  - Arrays, sets, fixed-size C arrays (JSON arrays of exactly that length), and maps keyed by `FString`/`FName`/enum/object path/text-form struct/integer (the key is the JSON object key in that form; an integer key is its canonical decimal text inside the key type's range: `"0"`, `"3"`, `"-1"`, not `"03"` or `"+3"`; e.g. `PerQualityLevelInt.PerQuality`).
  - Still not admitted, and reported under `skipped` by `inspect_asset_document_target`: delegates, maps with plain-struct keys, field paths, and instanced subobjects whose class requires a special outer.
- Value rules: a value is an edit of the current value, with the same rule as `Properties`: **omitted = keep the current value, listed = set it**. Struct members a value omits keep their current value (`{"Mappings": [{"Key": "A"}, {}]}` changes only the first row's key), and so do the `Properties` of an instanced subobject or `FInstancedStruct` whose class or struct stays the same. Arrays, sets and maps are whole values: their length and keys are what you write, and each element or map value that is a struct or instanced value edits the current element at that index or key (new elements start from defaults). Extract output re-applies unchanged and Diff reports nothing for it; Diff of a partial value compares only what the value lists. `null` is a value only for object, class and interface references, instanced subobjects and `FInstancedStruct` (soft references use `"None"`, an empty `FGuid` is `"00000000000000000000000000000000"`); anywhere else it is rejected (`null is not a <type> value`): omit the member to keep it. An integral JSON number (`100`) is accepted for a float or double property (extract writes integral floats as integers; otherwise keep the JSON number kind matching the property). Float precision: a `float` property stores the nearest float32 of any JSON number, as the Details panel does, so write decimals as you mean them (`0.3`, `0.1`). Extract writes the shortest decimal that reads back as the stored float (`0.3`, not `0.30000001192092896`), and Diff and the no-op check compare at float precision, so `0.3`, `0.30000001192092896` and `0.300000001` are the same value. `double` properties keep the number exactly. A value the editor rewrites when it applies it is rejected before anything changes: a `PostEditChange` normalisation (a clamp, an unnamed socket getting a name, a sprite's pivot), an integer out of range, or a non-canonical name or enum spelling. `Generic.PropertyValueNotStoredExactly` points at the first differing path inside the property (`/Properties/Sockets/0/SocketName`), shows the value the asset would hold, and says how many values differ. Author that value instead. Value errors point inside the property (`/Properties/Triggers/0/Class`) and say what the editor expects. Picker rules apply to a changed value, as in the Details panel: abstract classes only for `AllowAbstract` class properties, the `GetAllowedClasses`/`GetDisallowedClasses` functions, and `NoClear` (the `UPROPERTY(NoClear)` specifier, also `NonNullable`: `null` is rejected; a stored `null` re-applies unchanged).

### FactoryContext

`FactoryContext` (generic documents, new assets only): optional top-level path of what the class's New-asset dialog asks for. Some Content Browser factories ask a dialog for an asset before they can create anything (Pose Search Database a schema, Pose Search Schema a skeleton, Geometry Collection Cache a geometry collection, ...): give the object path of an existing asset, e.g. `"FactoryContext": "/Game/Characters/SK_Hero.SK_Hero"`. A factory whose dialog is a class picker (Slate Widget Style: the style type) takes a class path instead, e.g. `"FactoryContext": "/Script/Slate.ButtonWidgetStyle"` (or a Blueprint class `/Game/BP_Style.BP_Style_C`); the class must be one the picker lists: a concrete, non-deprecated subclass of the field's base class (`FactoryCreate.ContextUnsupported` otherwise). `FactoryContext` is set on the factory field that accepts it, exactly as the dialog would. It is not a property: it does not appear in Extract or Diff and is ignored when the asset already exists. A path that names no asset or class fails with `Generic.FactoryContextMissing`. When the factory needs it and it is missing, creation fails with `FactoryCreate.MissingFactoryContext` naming the missing fields and what to give, for example `TargetSkeleton (/Script/Engine.Skeleton)`.

### Creating generic assets

A new generic asset is created with the factory the Content Browser uses. The editor is never blocked: factories run non-interactively, and a factory that opens a modal dialog is cancelled and its products discarded (`FactoryCreate.InteractiveFactory`, nothing is left behind). Any other editor dialog opened during a request is answered with its default (Cancel / No) and reported as the warning `GenericRuntime.EditorDialogAnswered`.

- Factories with a single "which class to create" property get the document `Class` for it (what the class picker would do). The Interchange Pipeline factory's "Pick a Pipeline Class" dialog likewise creates the document `Class` (e.g. `/Script/InterchangePipelines.InterchangeGenericAssetsPipeline`); classes that picker hides (sub-pipelines such as `InterchangeGenericMeshPipeline`) fail with `FactoryCreate.DialogFactoryInitFailed`. If the factory cannot create the requested subclass: `FactoryCreate.SubclassUnsupported`.
- Factories that need a dialog-chosen asset or class take it from top-level `FactoryContext` (above); missing: `FactoryCreate.MissingFactoryContext`, whose message lists the unset fields and what to give for them.
- Actor classes (`LightWeightInstanceManager`, ...) and classes whose factory makes a Blueprint of them (`GlobalNamingTokens`, ...) are not standalone assets: `FactoryCreate.BlueprintClassUnsupported`; create a Blueprint with that parent class through the Blueprint profile.
- A class whose own factory is switched off in this editor (for example `ExternalDataLayerAsset` behind an experimental setting) fails with `FactoryCreate.FactoryDisabled`, as the Content Browser does not offer it.
- A factory whose dialog state cannot be reflected at all fails with `FactoryCreate.IncompleteCreation`.

Example (complete generic document):

```json
{
  "SchemaVersion": 1,
  "Target": "/Game/Input/IA_Jump",
  "Class": "/Script/EnhancedInput.InputAction",
  "Action": "Create",
  "Definitions": {},
  "Properties": {
    "ValueType": "Boolean",
    "ActionDescription": "Jump",
    "bTriggerWhenPaused": true
  }
}
```

### Worked generic examples

Shapes below were applied successfully through the generic route (`Action: CreateOrUpdate` or `Create` for a new asset). Struct members a value omits keep their current value (new array elements start from defaults), so listing every member is optional; the IMC example lists them as extract returns them. These show `Properties` only: a whole document also needs the header, with `"AssetType": "GenericAsset"` (or `"Definitions": {}`) since it has no `Body` (`Meta.AssetTypeRequired`).

InputAction (`/Script/EnhancedInput.InputAction`): `{"Properties": {"ValueType": "Boolean", "ActionDescription": "Jump"}}` (`ValueType`: `Boolean`, `Axis1D`, `Axis2D`, `Axis3D`).

InputMappingContext (`/Script/EnhancedInput.InputMappingContext`), SpaceBar mapped to an action with a Pressed trigger (`Action`, `Key` and `Triggers` are enough; omitted members such as `Modifiers` take their defaults; `Key` is the key name, `Action` the exact asset path of an existing InputAction, `Triggers` are instanced subobjects):

```json
{"Properties": {"DefaultKeyMappings": {"Mappings": [{
  "Action": "/Game/Input/IA_Jump.IA_Jump", "Key": "SpaceBar",
  "Triggers": [{"Class": "/Script/EnhancedInput.InputTriggerPressed", "Properties": {}}],
  "Modifiers": [], "PlayerMappableKeySettings": null, "SettingBehavior": "InheritSettingsFromAction"}]}}}
```

Validate and Apply of Input Actions and Mapping Contexts warn (never refuse) where the editor would accept a mistake: `EnhancedInput.ModifierDropsValue` (a Swizzle on a Boolean / Axis1D action: it reads 0), `EnhancedInput.MappingWithoutAction`, `EnhancedInput.BlueprintValueTypeMismatch` (Blueprint nodes bind the action with another value type: update those graphs and compile them, or keep the ValueType).

MaterialInstanceConstant (`/Script/Engine.MaterialInstanceConstant`) has its own profile (`get_asset_document_schema` with `profile: "MaterialInstance"`): parameters are set by name in `Body.Parameters`, no parameter structs.

```json
{"Body": {"Parent": "/Engine/EngineMaterials/FlattenMaterial.FlattenMaterial",
          "Parameters": {"RoughnessConst": 0.3, "BaseColorConst": [1, 0.25, 0.125, 1]}}}
```

Assets under `/Engine/...` and other plugin mounts are read-only: they can be referenced, inspected (`inspect_asset_document_target` returns their facts, marked `ReadOnly`) and extracted (`extract_asset_document` returns the same document a `/Game` asset of the class gives, with their own `Target`; the result says `read_only: true`), not authored (`Meta.ReadOnlyTarget`). To make an editable copy, apply the extract with `Target` set to a new `/Game` path (replace every occurrence of the source's own path, e.g. a Blueprint's `..._C` class) and `Action: Create`, or call `duplicate_asset` (the Content Browser's Duplicate: one asset, saved; what it references is not copied). For a Blueprint prefer `duplicate_asset`: an engine Blueprint's graphs may use actions (deprecated or context-only) that a document cannot re-create.

Open editor: an apply works while the asset is open in its details-panel editor; the panel edits the asset itself and shows the applied values, also after a failed apply.

### Properties

`Properties` values are plain JSON in the canonical form: `{ "TestString": "plain value", "TestInt": 12 }`. The older typed wrapper `{ "type": "Name", "value": "CreatedName" }` is not unwrapped by the Python runtime and fails type checks; do not use it. Do not invent `type` subtype strings such as `Array<int>` (`UnsupportedTypedSubtype`).

Profiles that own regions of an asset in `Body` (Montage, AnimSequence, Blueprint, ...) reject those same fields in `Properties` (for example `AnimSequence.ManagedPropertyOverlap`); the Body region is the one place to author them. For Blueprint-family assets, top-level `Properties` are the Class Settings (Description, Category, Namespace, ... and for AnimBlueprint the optimization flags and `DefaultBindingClass`).

### Definitions

`Definitions` is a document-local map of reusable fragments keyed by a non-empty author-chosen id. A `DefinitionRef` fragment points at another entry by `Id`; chains are resolved and cycles are rejected (`Generic.DefinitionCycle`). The generic profile validates every Definition even if nothing references it. `Properties` values are written as plain JSON (see "Properties"); Definitions are not substituted into them. BlackboardData and AnimMontage require `Definitions` to be empty.

```json
{
  "Definitions": {
    "EnemyBase": { "Kind": "ClassRef", "Class": "/Script/Engine.Character" },
    "Alias": { "Kind": "DefinitionRef", "Id": "EnemyBase" },
    "Spawn": {
      "Kind": "StructValue",
      "Struct": "/Script/CoreUObject.Vector",
      "Properties": { "X": 1.0, "Y": 2.0, "Z": 3.0 }
    },
    "Icon": { "Kind": "AssetRef", "Path": "/Game/UI/T_Icon.T_Icon" }
  }
}
```

### Fragment Kinds

- `AssetRef`: `{Kind, Path, Class?}`. `Path` must be an asset object path `/Game/.../Name.Name`.
- `ClassRef`: `{Kind, Class, ExpectedBase?}`. `Class` must be `/Script/Module.Class` or `/Game/.../BP_X.BP_X_C`.
- `StructValue`: `{Kind, Struct, Properties}`. `Struct` must be `/Script/Module.Struct`; `Properties` is an object.
- `EmbeddedObject`: recognized but blocked (`Generic.EmbeddedObjectBlocked`).
- `DefinitionRef`: `{Kind, Id}`.

Exact profiles use their own reference shapes inside `Body` (documented in each profile section).
