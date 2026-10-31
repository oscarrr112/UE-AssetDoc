# UserDefinedStruct / UserDefinedEnum Profiles

Classes: /Script/CoreUObject.UserDefinedStruct, /Script/Engine.UserDefinedEnum

Exact profiles for User Defined Structs and Enums. Create uses the stock factories. `Properties` is not accepted (`UserDefinedStruct.UnsupportedProperties` / `UserDefinedEnum.UnsupportedProperties`). A present `Members` / `Enumerators` is the complete ordered list; inside a listed entry, omitted settings mean their defaults (`InstanceEditable` true, the rest false / empty) and an omitted `DefaultValue` keeps the current default. Shapes:

```jsonc
// UserDefinedStruct
"Body": {"Tooltip": "One inventory item.",
         "Members": [
           {"Name": "Label", "Type": "text", "DefaultValue": "Sword"},
           {"Name": "Damage", "Type": "float", "DefaultValue": 12.5, "ClampMin": "0", "UIMax": "100"},
           {"Name": "Kind", "Type": "byte:/Game/Data/E_Kind.E_Kind", "DefaultValue": "Melee"},
           {"Name": "Offset", "Type": "struct:/Script/CoreUObject.Vector", "DefaultValue": {"X": 1, "Y": 2, "Z": 3}, "Widget3D": true},
           {"Name": "Lookup", "Type": {"Category": "name", "Container": "map", "ValueType": "int"}},
           {"Name": "Power", "RenamedFrom": "Strength", "Type": "int", "SaveGame": true, "InstanceEditable": false}]}
// UserDefinedEnum
"Body": {"Tooltip": "How an item is used.", "Bitflags": false,
         "Enumerators": [{"Name": "Melee", "Tooltip": "Close range"}, {"Name": "Ranged", "RenamedFrom": "Bow"}]}
```

UserDefinedStruct:

- `Members[].Name`: the friendly name shown in the Struct editor, unique ignoring case (`UserDefinedStruct.DuplicateMember`); a new name is 1 to 100 characters on one line without leading or trailing spaces (`UserDefinedStruct.InvalidMemberName`). A name the struct already has that breaks this reads with a warning and re-applies unchanged. Identity is the member GUID: `RenamedFrom` renames a member and keeps its data in data tables, Blueprints and containing structs; it is ignored once the rename happened. A member not listed is removed; at least one member is required.
- `Type`: the Blueprint variable type form (same as UBlueprint `Variables[].Type`; `float` = double). Inspect's `MemberDetails.TypeForm` lists one example per kind. Changing the type resets the default (as in the editor); values in data tables are converted where they fit. Exec, wildcard, delegate, reference, const, unresolved objects and recursive structs fail with `UserDefinedStruct.InvalidMemberType` at `/Body/Members/<i>/Type`.
- `DefaultValue`: the generic value form for the member type (a struct value may name a subset of its fields); `null` resets. `UserDefinedStruct.InvalidDefault` at the member; a type the codec cannot represent is `DefaultSupported: false` in Inspect (`UserDefinedStruct.UnsupportedDefault`); a default the struct editor set for such a member reads as a marker (core guide) with a warning `UserDefinedStruct.Unrepresented`, kept when left or omitted (an edited marker: `UserDefinedStruct.UnrepresentedValue`).
- `Tooltip`, `InstanceEditable`, `SaveGame`, `MultiLine` (string / text only), `Widget3D` (Vector / Transform only), `ClampMin` / `ClampMax` / `UIMin` / `UIMax` (numeric members; `""` clears).

UserDefinedEnum:

- `Enumerators[].Name`: the display name, unique ignoring case (`UserDefinedEnum.DuplicateEnumerator`); a new name is 1 to 100 characters on one line without leading or trailing spaces (`UserDefinedEnum.InvalidEnumeratorName`; a name the enum already has reads with a warning and re-applies unchanged); the internal `NewEnumeratorN` names and values (0..N-1) are not authored (Inspect shows them). A value of the enum elsewhere (a property, DataTable cell, notify field) is written as the display name, as extract reads it back; another spelling fails with `PropertyWrite.DecodeFailed` naming the enumerator to write (Blueprint variable and pin defaults also take `NewEnumeratorN`). `RenamedFrom` renames and keeps stored values; an enumerator not listed is removed (values that used it stop naming an enumerator: warning `UserDefinedEnum.EnumeratorValuesInvalidated`).
- `Tooltip` (enum and per enumerator), `Bitflags`.

While Play-In-Editor runs, an apply is refused before any change (`Graph.PieActive` at `/Target`). Dependents: editing a type updates the loaded assets that use it the same way the editor does (data table rows, Blueprints, containing structs). If one of them has unsaved changes, the apply is refused before any change (`UserDefinedStruct.DependentHasUnsavedChanges` / `UserDefinedEnum.DependentHasUnsavedChanges`, naming the packages): save or revert them first. On success every updated package is reported (warning `…DependentsUpdated`); dependents that were saved stay saved. Blueprints the change left needing a compile are compiled; one that does not compile is named (warning `…DependentCompileFailed`): fix it (e.g. nodes that used a removed member) and apply it. A failed apply restores the type and reloads its dependents from disk. Inspect (`MemberDetails` / `EnumeratorDetails`) is compact: per member `CppType`, `Codec`, `Allows` (settings it accepts), `UseAs` (the path to use as a DataTable `RowStruct` or as a `Type`: `struct:<path>` / `byte:<path>`) and `Dependents` (loaded users, when any).

An apply works while the struct or enum is open in its editor, which shows the result.
