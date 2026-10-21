# DataTable Family Profiles

Classes: /Script/Engine.DataTable, /Script/Engine.CompositeDataTable, /Script/Engine.MirrorDataTable, /Script/Engine.CurveTable, /Script/Engine.CompositeCurveTable, /Script/Engine.StringTable

Six exact profiles for the table assets (`DataTable`, `CompositeDataTable`, `MirrorDataTable`, `CurveTable`, `CompositeCurveTable`, `StringTable`). `Properties` are the Details-panel properties (`bStripFromClientBuilds`, `bIgnoreExtraFields`, ...; names owned by the Body are rejected with `DataTable.BodyOwnedProperty`; a StringTable has none: `StringTable.UnsupportedProperties`). Omitted Body key = keep; a present `Rows` / `Entries` is the complete list. Shapes:

```jsonc
// DataTable / MirrorDataTable
"Body": {"RowStruct": "/Script/MyGame.ItemRow",
         "Rows": [{"Name": "Sword", "Values": {"Damage": 12, "Label": "Sword"}},
                  {"Name": "Bow", "RenamedFrom": "Bow_Old", "Values": {"Range": 900.5}}]}
// CompositeDataTable
"Body": {"RowStruct": "/Script/MyGame.ItemRow", "ParentTables": ["/Game/Data/DT_Base.DT_Base"], "Rows": [/* derived */]}
// CurveTable (CompositeCurveTable: + "ParentTables", Rows derived)
"Body": {"CurveTableMode": "SimpleCurves",
         "Rows": [{"Name": "Damage", "InterpMode": "Linear", "Keys": [{"Time": 1, "Value": 10}]}]}
// StringTable
"Body": {"Namespace": "UI", "Entries": {"Menu.Start": "Start Game"},
         "MetaData": {"Menu.Start": {"Comment": "Main menu button"}}}
```

DataTable / MirrorDataTable:

- `RowStruct`: object path of a native `FTableRowBase` struct or a User Defined Struct; required to create (`DataTable.RowStructRequired`, `DataTable.RowStructNotFound`, `DataTable.InvalidRowStruct`). It cannot change on an existing table (`DataTable.RowStructImmutable`); create a new table instead.
- `Rows[]`: `{"Name", "Values", "RenamedFrom"?}`. Rows not listed are removed, the order of the list is the row order. `Values` is the complete row, keyed by field name (User Defined Struct fields by their friendly name): a field it leaves out is the row struct's default (the value a new row gets), so a row without `Values` is a row at the defaults. Deleting a field from a row resets it. To keep a field's current value, extract the table and keep the field in the row (Extract writes every field). `RenamedFrom` renames the row (Rename Row) instead of removing and adding it; its `Values` are still the complete row. It is ignored once the rename happened, so re-applying is a no-op. Errors point at `/Body/Rows/<i>/Values/<Field>`: `DataTable.UnknownField` (with suggestions), `DataTable.InvalidValue`, `DataTable.UnsupportedField`.
- Supported field kinds: the generic value forms (see Generic Reflected Profile): bool, integers, float/double, enums, `FString`, `FName`, `FText` (source string), structs, arrays, sets, string-keyed maps, hard and soft object/class references (exact path or `null`; an unresolved path fails with `DataTable.InvalidValue` at the field) and `FInstancedStruct` (`{"Struct", "Properties"}`). A field the codec cannot represent is left out of Extract, `Supported: false` in Inspect's `RowSchema`, and keeps its value.
- `inspect_asset_document_target` on an existing table asset returns `RowSchema` (`{RowStruct, Fields[{Name, Type, Codec, Supported, EnumValues?}]}`; the profile inspection does not): read it before writing `Values`. `list_types` with `kind: "struct"` and `base: "/Script/Engine.TableRowBase"` lists the structs a table can use (native `FTableRowBase` structs and User Defined Structs); `inspect_asset_document_target` on a native struct path (`/Script/Module.Struct`) returns its `Fields`. For a User Defined Struct, extract or inspect the struct asset itself (`MemberDetails.UseAs.RowStruct` is the path to use); a new row struct is authored with the UserDefinedStruct profile (create it before the table).
- MirrorDataTable create = the New Mirror Data Table dialog: top-level `"FactoryContext": "/Game/Characters/SK_Hero.SK_Hero"` is the Skeleton it asks for (required: `MirrorDataTable.SkeletonRequired`; not a Skeleton: `MirrorDataTable.SkeletonNotFound`, at `/FactoryContext`), `Body.RowStruct` is its "Row Structure" (optional, default `/Script/Engine.MirrorTableRow`, the dialog's default; it must have a `MirroredName` field like the dialog's struct filter: `MirrorDataTable.InvalidRowStruct`). The factory then fills the rows from the skeleton's bone, socket, curve and notify names with the project's Animation settings find/replace expressions, as in the editor; a `Rows` list in the create document then edits those rows. Example: `{"Target": "/Game/Anim/MDT_Hero", "Class": "/Script/Engine.MirrorDataTable", "Action": "Create", "FactoryContext": "/Game/Characters/SK_Hero.SK_Hero", "Body": {}}`. `FactoryContext` is ignored once the table exists; `Skeleton`, `MirrorAxis` and `MirrorFindReplaceExpressions` are ordinary `Properties`.
- CompositeDataTable: `ParentTables` is the ordered parent list (later parents override earlier ones); missing, mismatched and cyclic parents fail with `Composite.ParentNotFound`, `Composite.ParentRowStructMismatch`, `Composite.ParentCycle` at `/Body/ParentTables/<i>`. If the engine still tries to open a dialog while the parents are set, the edit fails with `Composite.InteractiveEditRefused` and is rolled back (also for CompositeCurveTable). `Rows` are derived and read-only: given rows must equal the derived rows (`CompositeDataTable.RowsReadOnly`).
- Open editor: an apply works while the table is open in the Data Table editor; the row list and the row editor show the result, also after a failed apply.

CurveTable / CompositeCurveTable:

- `CurveTableMode`: `SimpleCurves` (Linear/Constant keys) or `RichCurves` (Cubic, tangents, extrapolation; rows use the CurveFloat curve form). It is set by the first curve on a new table. Changing the mode of an existing table needs `Rows` (the table is rebuilt from them): `CurveTable.ModeChangeNeedsRows`; `CurveTable.ModeRequired` when it is missing on create.
- `Rows[]`: `{"Name", "RenamedFrom"?, ...curve}`; a simple curve is `{"InterpMode": "Linear"|"Constant", "Keys": [{"Time", "Value"}], extrapolation, "DefaultValue"}`. Keys must ascend in time: `CurveTable.KeysNotAscending`, `CurveTable.InvalidKey`, `CurveTable.InvalidInterpMode` (cubic in a simple table). Curve order is not authored.
- CompositeCurveTable: `ParentTables` as above; `Rows` derived and read-only (`CompositeCurveTable.RowsReadOnly`).

StringTable:

- `Namespace`: defaults to the asset name; may not contain `"' ,` newlines, tabs or `[` `]` (`StringTable.InvalidNamespace`).
- `Entries`: `{key: source string}`, the complete map (omit = keep). Empty sources are refused (`StringTable.EmptySourceString`); renaming a key is remove + add.
- `MetaData`: `{key: {id: value}}`, e.g. `Comment`.

Large tables: a whole table goes in one document; edits are sent to the editor in bounded chunks. A value-only edit touches only the changed rows, and an unchanged re-apply is a no-op. The limit is the document's node budget (`Codec.NodeLimitExceeded`): about 45,000 rows of 20 scalar fields, fewer with text or reference values. A 5,000 × 20 table extracts in about 0.5 s, applies a one-row edit in about 4.5 s, and is created in about 8 s; creation time grows linearly with the row count. A single row must stay under about 4,000 value nodes.

Import from CSV/JSON is not part of the documents; author the rows instead.

Diff: `Rows` diffs per row and per field, naming the row: a Remove per removed row (`/Body/Rows`), an Add per new row (`/Body/Rows/<index>`), a Replace of `/Body/Rows/<index>/Name` per rename, and a Replace per changed field (`/Body/Rows/<index>/Values/<field>`). Only a change of row order alone is one Replace of `/Body/Rows`.
