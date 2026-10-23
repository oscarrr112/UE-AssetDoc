# User Defined Struct / Enum support matrix

Governing rules:
[design rules](asset-document-design-rules.md) §3 (rollback = functional
equivalence), §3.2 (dependents of a changed type), §4 (editor-equivalent code) and §5 (support matrix).

| Class | Profile | Python |
|---|---|---|
| `/Script/CoreUObject.UserDefinedStruct` | `profile.user_defined_struct.v1` | `user_defined_struct.py` |
| `/Script/Engine.UserDefinedEnum` | `profile.user_defined_enum.v1` | `user_defined_enum.py` |

Shared pieces: `user_defined_types_common.py` (and `data_table_common.py` for the header and
transport). Route `data_profile`, C++ provider `data` (module `Source/AssetDocumentData/`), four
primitives: `data.user_struct.read/edit.v1` (`AssetDocumentDataUserStructPrimitives.cpp`),
`data.user_enum.read/edit.v1` (`AssetDocumentDataUserEnumPrimitives.cpp`). The dependents search,
the dependents rule and the two package-snapshot rebuilders are in
`AssetDocumentDataUserTypeDependents.cpp`.

Why the `data` provider: these types exist mainly as data table row structs and Blueprint data
types; the module already holds the value codec bridge (`EncodeMember` / `DecodeMember`) used for
member defaults and the data table code that reads rows by authored member name. A Blueprint
module would add nothing (no graphs).

Every edit is the Struct / Enum editor's own operation (`FStructureEditorUtils`,
`FEnumEditorUtils`) inside one editor transaction per primitive call. Writes to the type are
covered by Core's package snapshot (existing asset) or the factory rollback (created asset).
Create uses the generic factory lifecycle (`StructureFactory`, `EnumFactory`, non-interactive scope).

## Document shapes

```jsonc
// UserDefinedStruct
"Body": {"Tooltip": "One inventory item.",
         "Members": [
           {"Name": "Label", "Type": "text", "DefaultValue": "Sword", "Tooltip": "Shown in the UI", "MultiLine": true},
           {"Name": "Damage", "Type": "float", "DefaultValue": 12.5, "ClampMin": "0", "UIMax": "100"},
           {"Name": "Kind", "Type": "byte:/Game/Data/E_Kind.E_Kind", "DefaultValue": "Melee"},
           {"Name": "Offset", "Type": "struct:/Script/CoreUObject.Vector", "DefaultValue": {"X": 1, "Y": 2, "Z": 3}, "Widget3D": true},
           {"Name": "Lookup", "Type": {"Category": "name", "Container": "map", "ValueType": "int"}},
           {"Name": "Power", "RenamedFrom": "Strength", "Type": "int", "SaveGame": true, "InstanceEditable": false}]}
// UserDefinedEnum
"Body": {"Tooltip": "How an item is used.", "Bitflags": false,
         "Enumerators": [{"Name": "Melee", "Tooltip": "Close range"}, {"Name": "Ranged", "RenamedFrom": "Bow"}]}
```

Omitted Body key = keep. A present `Members` / `Enumerators` is the complete ordered list. In a
listed member or enumerator, omitted settings mean their defaults (`InstanceEditable` true, the
rest false / empty); an omitted `DefaultValue` keeps the current default (a new member gets the
type default). Extract omits default settings.

## 1. UserDefinedStruct

| Editor surface | Body location | Class | Mechanism / reason |
|---|---|---|---|
| New Structure | `Action: Create` | Supported | generic factory (`UStructureFactory`); its starting member `MemberVar_0` (bool) is replaced by the document's members: adds first, then the remove (the editor cannot remove the last member) |
| Add member | `Members[]` entry | Supported | `AddVariable`, then `RenameVariable` to the authored name |
| Remove member | entry omitted | Supported | `RemoveVariable`. The last member cannot be removed (`Members` must be non-empty; `UserDefinedStruct.LastMember` if a plan ever tried) |
| Rename member | `Members[i].RenamedFrom` (or a case-only change of `Name`) | Supported | `RenameVariable(Guid, Name)`. Identity is the member GUID: data tables, Blueprints and containing structs keep the member's data. A case-only rename goes through a stand-in name (the editor compares names ignoring case). Names match first (ignoring case), so `RenamedFrom` is ignored once the rename happened |
| Member name | `Name` | Supported | the friendly name the editor shows; the internal `Name_N_<GUID>` property name never appears in the document. Unique ignoring case (`UserDefinedStruct.DuplicateMember`, `MemberExists`) |
| Member type (type picker: primitives, struct, enum, object/class/soft refs, interface, array/set/map) | `Type` | Supported | the Blueprint variable type form (`ublueprint_members.type_dto`); `ChangeVariableType` (it resets the member's default, as in the editor). Exec / wildcard / delegate / reference / const types, unknown categories, objects that do not resolve, a non-struct for `struct:` and structs that would recurse (`CanHaveAMemberVariableOfType`) fail with `UserDefinedStruct.InvalidMemberType` at `/Body/Members/<i>/Type` |
| Reorder members (drag) | order of `Members[]` | Supported | `MoveVariable(PositionAbove)` for members out of place; sent only when the order differs |
| Default value (Default Values panel) | `DefaultValue` | Supported | the shared typed value codec decodes into a copy of the struct defaults, then `FBlueprintEditorUtils::PropertyValueToString` + `ChangeVariableDefaultValue`, as the panel does. A struct value may name a subset of its fields. Invalid value: `UserDefinedStruct.InvalidDefault` at `/Body/Members/<i>/DefaultValue`; a type the codec cannot represent (e.g. instanced objects): left out of Extract, `DefaultSupported: false` in Inspect, `UserDefinedStruct.UnsupportedDefault` when authored |
| Member tooltip | `Tooltip` | Supported | `ChangeVariableTooltip` |
| Editable (instance) | `InstanceEditable` | Supported | `ChangeEditableOnBPInstance` |
| SaveGame | `SaveGame` | Supported | `ChangeSaveGameEnabled` |
| Multi-line text | `MultiLine` | Supported | `ChangeMultiLineTextEnabled`; string / text (and arrays of them) only, else `UserDefinedStruct.MultiLineNotAllowed`. Extract reports the effective value (`IsMultiLineTextEnabled`) |
| 3D widget | `Widget3D` | Supported | `Change3dWidgetEnabled`; Vector / Transform only (`UserDefinedStruct.Widget3DNotAllowed`) |
| Value range (Clamp / UI min / max) | `ClampMin`, `ClampMax`, `UIMin`, `UIMax` (numbers or numeric strings; `""` clears) | Supported | `FStructureEditorUtils::SetMetaData`; numeric members only (`CanEditValueRange`, else `UserDefinedStruct.ValueRangeNotAllowed`) |
| Struct tooltip | `Body.Tooltip` | Supported | `ChangeTooltip` |
| Inspect | `MemberDetails {Status, ErrorMessage?, Members[{Name, CppType, Codec, Allows?, DefaultSupported?: false, Invalid?: true}], TypeForm, UseAs {RowStruct, Type}, Dependents?}` (compact: optional keys only when they say something) | Supported | `data.user_struct.read.v1 {dependents: true}`; `TypeForm` gives one example per type kind; `UseAs` is the path to put in a DataTable `RowStruct` or a member / variable `Type` |
| Members whose type no longer exists (`bInvalidMember`) | `Members[i]` | Read-only | extracted with the stored type; `Invalid: true` in Inspect. Retype or remove them |
| Details panel (none besides the struct tooltip) | `Properties` | Unsupported | `UserDefinedStruct.UnsupportedProperties` |
| Member category, advanced display | — | Unsupported | the Struct editor does not expose them |

## 2. UserDefinedEnum

| Editor surface | Body location | Class | Mechanism / reason |
|---|---|---|---|
| New Enumeration | `Action: Create` | Supported | generic factory (`UEnumFactory`) |
| Add enumerator | `Enumerators[]` entry | Supported | `AddNewEnumeratorForUserDefinedEnum`, then `SetEnumeratorDisplayName` |
| Remove enumerator | entry omitted | Supported | `RemoveEnumeratorFromUserDefinedEnum`. Values that used it in other assets stop naming an enumerator, as in the editor (warning `UserDefinedEnum.EnumeratorValuesInvalidated`) |
| Rename (display name) | `Enumerators[i].RenamedFrom` (or a case change) | Supported | `SetEnumeratorDisplayName`. The internal name (`NewEnumeratorN`) never changes, so stored values keep their enumerator; data table and struct default values (which use display names) follow the rename. Unique ignoring case (`UserDefinedEnum.DuplicateEnumerator` / `EnumeratorExists`); names the editor refuses (`(INVALID)`) fail with `UserDefinedEnum.InvalidEnumeratorName` |
| Reorder (drag) | order of `Enumerators[]` | Supported | `MoveEnumeratorInUserDefinedEnum`; the editor re-resolves stored values by enumerator, so they keep their meaning |
| Enumerator description | `Enumerators[i].Tooltip` | Supported | the editor's Description box: `Modify` + `SetMetaData("ToolTip", …, index)` |
| Enum description | `Body.Tooltip` | Supported | the Details panel write of `EnumDescription` (`PreEditChange`, set, `PostEditChangeProperty`, which copies it to the enum's tooltip) |
| Bitmask Flags | `Bitflags` | Supported | `SetEnumeratorBitflagsTypeState` |
| Inspect | `EnumeratorDetails {Enumerators[{Name, InternalName, Value}], ValueForm, UseAs {Type}, Dependents?}` | Supported | `data.user_enum.read.v1 {dependents: true}` |
| Enumerator values | — | Read-only | the editor assigns 0..N-1 (reassigned on reorder / remove); not authored |

Undo history (direction doc §3.1): the Struct / Enum editor operations record their own
transactions, so every apply that changed the type, and every failed apply that already ran one,
clears undo history that references the type; a rejected apply keeps it. Core restores a
`UEnum`'s bytes after clearing its name table (`UEnum::Serialize` requires an empty one).

Enum Ctrl+Z in stock UE 5.7: undoing any Enum-editor step asserts (`NumValues == 0`, Enum.cpp:119):
the transaction buffer re-serializes the `UEnum` over its live name table, which is the same check
Core works around. This happens without AssetDocument (a human-only enumerator add followed by
Ctrl+Z hits it). After an apply that changed the enum, or a failed apply that already ran an editor
step, no undo record references the enum, so Ctrl+Z cannot reach that path. A rejected apply, or
one that failed before an editor step, leaves the history alone, so a human's own earlier enum edit
stays undoable and still hits the engine assert. Struct undo is not affected.

## 3. Dependents (assets that use a changed type)

The rule is in the direction doc §3.2. Evidence (UE 5.7 source):

- Struct: every compiling edit runs `FStructureEditorUtils::OnStructureChanged` →
  `FUserDefinedStructureCompilerUtils::CompileStruct`. It calls `BroadcastPreChange` (data tables
  with the struct as `RowStruct`: `UDataTable::CleanBeforeStructChange` serializes the rows with
  tags and calls `Modify`; `RestoreAfterStructChange` reads them back after the compile; a tag whose member name
  is gone is matched by the GUID part of the name (`UUserDefinedStruct::CustomFindProperty`), so a
  renamed member keeps its values), replaces
  the struct with a temporary duplicate in every `FStructProperty` (Blueprint generated classes,
  other User Defined Structs) and marks those Blueprints structurally modified (also Blueprints
  whose `CachedUDSDependencies` contain the struct), recompiles containing structs and marks them
  dirty. Tooltip and 3D-widget edits do not compile.
- Enum: `FEnumEditorUtils::BroadcastChanges` (add / remove / move / bitflags with
  `bResolveData`) reserializes every object of a class with a byte / enum property of the enum
  through an enumerator resolver (in memory, without `Modify`), recompiles containing structs,
  reconstructs nodes that depend on the enum (`INodeDependingOnEnumInterface`), resets pin
  defaults that no longer name an enumerator to `(INVALID)`, updates bitmask int variables, and
  marks those Blueprints modified. A display-name change (internal names unchanged) only
  reconstructs enum nodes and checks pin defaults; data tables and structs are not touched.

What AssetDocument does (`AssetDocumentDataUserTypes.h`):

| Situation | Behaviour |
|---|---|
| Edit that compiles / broadcasts, a loaded dependent has unsaved changes | refused before any change: `UserDefinedStruct.DependentHasUnsavedChanges` / `UserDefinedEnum.DependentHasUnsavedChanges` at `/Body/Members` / `/Body/Enumerators`, naming the packages. Save or revert them, then apply again |
| Edit succeeds | the editor's own updates run unchanged. Every package they marked dirty is reported (warning `…DependentsUpdated`). Predicted dependents that were saved stay saved (`kept_clean`): their saved files load through the same tagged-property migration, so memory equals what reloading them gives. Other packages the editor dirtied stay dirty and are reported |
| Edit leaves a dependent Blueprint needing a compile | the editor only regenerates its skeleton class: its generated class keeps the struct's temporary duplicate (old layout) and has no function bytecode until compiled. Every Blueprint that was compiled before the edit and is not after it is compiled (the editor's Compile), its saved state kept; one that fails to compile is reported (warning `UserDefinedStruct.DependentCompileFailed` / `UserDefinedEnum.DependentCompileFailed`). Test `Blueprint.DependentClasses.UserDefinedTypes` |
| Member retype / remove with dependents | warnings `UserDefinedStruct.MemberValuesConverted` / `MemberValuesRemoved` (values converted where the old value fits, e.g. a data table int 7 becomes 7.0 after int → float, otherwise the type default; removed values dropped) |
| Apply fails after the editor updated dependents | Core restores the type; the rebuilder recompiles it (struct: from the restored editor data, as the editor's Undo does) and restores, from package snapshots taken with the type's, every loaded dependent whose objects the apply changed (dirty or not: a shape change also migrates in-memory data without dirtying), in memory and in place (a dependent Blueprint is recompiled); nothing is reloaded from disk. They end as before the apply (warning `UserDefinedType.DependentsRestored`); a dependent that cannot be snapshot-restored (or was loaded during the apply) keeps the editor's update and is named (`UserDefinedType.DependentNotRestored`) |

Prediction covers loaded data tables, Blueprints (properties, `CachedUDSDependencies`, enum
nodes and pins, bitmask variables) and User Defined Structs (followed recursively). Not
predicted: `FInstancedStruct` / property-bag users (reinstanced by StructUtils) and actors placed
in open levels; their packages, if dirtied, are reported but not kept clean or reloaded.

## 4. Tests

- Python: `tests/test_user_defined_types.py` (document validation, op planning incl. create,
  rename, retype, reorder, removes before renames, case-only renames, no-op, error-path mapping,
  diff, warnings).
- Editor (production Service): `UEAssetDoc.UserDefinedTypes.Struct`, `.Enum`,
  `.Dependents`, `.Undo`, `.EnumUndo` (a human enum edit, then an AI apply and a failed apply:
  no history references the enum, Undo/Redo are safe).
- Open editor: `UserDefinedTypes.OpenEditorApply.Struct` / `.Enum` (in place; the editors refresh from the
  Struct / Enum editor notifications the type operations and the rollback rebuilders send: the Default Values
  panel and the enumerator list show applied and rolled-back members; an unsaved enum edit refuses the apply
  with `DirtyExistingAssetTransactionUnsupported`; enum Ctrl+Z is the stock UE 5.7 assert above, so the test
  saves and applies instead).
