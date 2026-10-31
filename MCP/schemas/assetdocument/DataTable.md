# DataTable Family Profiles

Classes: /Script/Engine.DataTable, /Script/Engine.CompositeDataTable, /Script/Engine.MirrorDataTable, /Script/Engine.CurveTable, /Script/Engine.CompositeCurveTable, /Script/Engine.StringTable

Six exact profiles, one per table class above. `Properties` are the Details-panel properties (`bStripFromClientBuilds`, `bIgnoreExtraFields`, ...; names the Body owns: `DataTable.BodyOwnedProperty`; a StringTable has none: `StringTable.UnsupportedProperties`). A present `Rows` / `Entries` is the complete list. Shapes:

```jsonc
// DataTable / MirrorDataTable
"Body": {"RowStruct": "/Script/MyGame.ItemRow",
         "Rows": [{"Name": "Sword", "Values": {"Damage": 12, "Label": "Sword"}},
                  {"Name": "Bow", "Values": {"Range": 900.5}}]}
// CompositeDataTable
"Body": {"RowStruct": "/Script/MyGame.ItemRow", "ParentTables": ["/Game/Data/DT_Base.DT_Base"], "Rows": [/* derived */]}
// CurveTable
"Body": {"CurveTableMode": "SimpleCurves",
         "Rows": [{"Name": "Damage", "InterpMode": "Linear", "Keys": [{"Time": 1, "Value": 10}]}]}
// StringTable
"Body": {"Namespace": "UI", "Entries": {"Menu.Start": "Start Game"},
         "MetaData": {"Menu.Start": {"Comment": "Main menu button"}}}
```

DataTable / MirrorDataTable:

- `RowStruct`: object path of a native `FTableRowBase` struct or a User Defined Struct; required to create (`DataTable.RowStructRequired`, `DataTable.RowStructNotFound`, `DataTable.InvalidRowStruct`). It cannot change on an existing table (`DataTable.RowStructImmutable`).
- `Rows[]`: `{"Name", "Values", "RenamedFrom"?}`. Rows not listed are removed, the order of the list is the row order. `Values` is the complete row, keyed by field name (User Defined Struct fields: friendly names): a field it leaves out is the row struct's default, so a row without `Values` is a row at the defaults; deleting a field resets it, so keep a field to keep its value (Extract writes every field). `RenamedFrom` renames the row (Rename Row) instead of removing and adding it; its `Values` are still the complete row. It is ignored once the rename happened, so re-applying is a no-op. Errors are at `/Body/Rows/<i>/Values/<Field>`: `DataTable.UnknownField` (with suggestions), `DataTable.InvalidValue`, `DataTable.UnsupportedField`.
- Supported field kinds: the generic value forms (see Generic Reflected Profile): scalars, enums, strings and names, `FText` (source string), structs, arrays, sets, string-keyed maps, object/class references (exact path or `null`; unresolved: `DataTable.InvalidValue` at the field) and `FInstancedStruct`. A field the codec cannot represent is `Supported: false` in `RowSchema`; a row holding a value there reads it as a marker (core guide) with a warning `DataTable.Unrepresented` at its first row. Leave the marker or omit the field to keep the value (also in a complete-row edit); an edited marker fails with `DataTable.UnrepresentedValue`, a plain value with `DataTable.UnsupportedField`. A supported field with such content inside (a struct with a field-path member, an array of such) reads with the marker (warned) in place of that member: an edit is applied over the current value, so the marked content stays, an edited marker fails with `DataTable.InvalidValue` at the marker, and leaving the field out keeps its whole value. Array elements carrying markers are matched to the current element with those markers, so removing or moving elements keeps each one's content; an element without its markers where the current element at that index has marked content fails (`DataTable.InvalidValue` at the element): keep the markers as extracted.
- A row struct that no longer loads (deleted, or renamed without a redirector): `RowStruct` is its stored path, `Rows` is a marker and Extract warns `DataTable.RowStructMissing` at `/Body/RowStruct`. The rows cannot be read or edited until it loads again; leave the marker or omit `Rows`, and the rest of the table still applies.
- `inspect_asset_document_target` on an existing table asset returns `RowSchema` (`{RowStruct, Fields[{Name, Type, Codec, Supported, EnumValues?}]}`; the profile inspection does not): read it before writing `Values`. `list_types` with `kind: "struct"` and `base: "/Script/Engine.TableRowBase"` lists the usable structs (native `FTableRowBase` structs and User Defined Structs); `inspect_asset_document_target` on a native struct path returns its `Fields`. For a User Defined Struct, extract or inspect the struct asset (`MemberDetails.UseAs.RowStruct` is the path to use); author a new row struct with the UserDefinedStruct profile before the table.
- MirrorDataTable create = the New Mirror Data Table dialog: top-level `"FactoryContext": "/Game/Characters/SK_Hero.SK_Hero"` is the Skeleton it asks for (required: `MirrorDataTable.SkeletonRequired`; not a Skeleton: `MirrorDataTable.SkeletonNotFound`, at `/FactoryContext`), `Body.RowStruct` is its "Row Structure" (optional, default `/Script/Engine.MirrorTableRow`; it must have a `MirroredName` field: `MirrorDataTable.InvalidRowStruct`). The factory fills the rows from the skeleton's bone, socket, curve and notify names with the project's Animation settings find/replace expressions; a `Rows` list in the create document then edits those rows. `FactoryContext` is ignored once the table exists; `Skeleton`, `MirrorAxis` and `MirrorFindReplaceExpressions` are ordinary `Properties`.
- CompositeDataTable: `ParentTables` is the ordered parent list (later parents override earlier ones); missing, mismatched and cyclic parents fail with `Composite.ParentNotFound`, `Composite.ParentRowStructMismatch`, `Composite.ParentCycle` at `/Body/ParentTables/<i>`. If the engine tries to open a dialog while the parents are set, the edit fails with `Composite.InteractiveEditRefused` and is rolled back (also for CompositeCurveTable). `Rows` are derived and read-only: given rows must equal the derived rows (`CompositeDataTable.RowsReadOnly`).
- An apply works while the table is open in its editor, which shows the result.

CurveTable / CompositeCurveTable:

- `CurveTableMode`: `SimpleCurves` (Linear/Constant keys) or `RichCurves` (Cubic, tangents, extrapolation; rows use the CurveFloat curve form). It is set by the first curve on a new table. Changing the mode of an existing table needs `Rows` (the table is rebuilt from them): `CurveTable.ModeChangeNeedsRows`; `CurveTable.ModeRequired` when it is missing on create.
- `Rows[]`: `{"Name", "RenamedFrom"?, ...curve}`; a simple curve is `{"InterpMode": "Linear"|"Constant", "Keys": [{"Time", "Value"}], extrapolation, "DefaultValue"}`. Keys must ascend in time: `CurveTable.KeysNotAscending`, `CurveTable.InvalidKey`, `CurveTable.InvalidInterpMode` (cubic in a simple table). Curve order is not authored.
- CompositeCurveTable: `ParentTables` as above; `Rows` derived and read-only (`CompositeCurveTable.RowsReadOnly`).

StringTable:

- `Namespace`: defaults to the asset name; may not contain `"' ,` newlines, tabs or `[` `]` (`StringTable.InvalidNamespace`).
- `Entries`: `{key: source string}`, the complete map. Empty sources are refused (`StringTable.EmptySourceString`); renaming a key is remove + add.
- `MetaData`: `{key: {id: value}}` (e.g. `Comment`).
- A CSV-imported table can hold keys with spaces, empty sources or metadata ids with spaces: they read with a warning and re-apply unchanged (core guide, "Assets That Break The Rules For New Input"); new ones are refused.

Large tables: a whole table goes in one document; edits reach the editor in bounded chunks, a value-only edit touches only the changed rows, and an unchanged re-apply is a no-op. The limit is the document's node budget (`Codec.NodeLimitExceeded`): about 45,000 rows of 20 scalar fields, fewer with text or reference values. CSV/JSON import is not part of the documents: author the rows. A 5,000 × 20 table extracts in about 0.5 s, is created in about 8 s and applies a one-row edit in about 4.5 s. A single row must stay under about 4,000 value nodes.

Diff: `Rows` diffs per row and field: a Remove per removed row (`/Body/Rows`), an Add per new row (`/Body/Rows/<index>`), a Replace of `/Body/Rows/<index>/Name` per rename, a Replace per changed field (`/Body/Rows/<index>/Values/<field>`); a change of row order alone is one Replace of `/Body/Rows`.
