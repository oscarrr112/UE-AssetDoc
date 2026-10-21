# DataTable family support matrix

Status: 2026-10-04 (large tables, composite
non-interactive scope). Governing rules:
[design rules](asset-document-design-rules.md) §3 (rollback = functional
equivalence), §4 (raw editor-equivalent code + behaviour test) and §5 (support matrix).

Classes and Profiles (route `data_profile`, Python in `Content/Python/asset_document/profiles/`):

| Class | Profile | Python |
|---|---|---|
| `/Script/Engine.DataTable` | `profile.data_table.v1` | `data_table.py` |
| `/Script/Engine.CompositeDataTable` | `profile.composite_data_table.v1` | `data_table.py` |
| `/Script/Engine.MirrorDataTable` | `profile.mirror_data_table.v1` | `data_table.py` |
| `/Script/Engine.CurveTable` | `profile.curve_table.v1` | `curve_table.py` |
| `/Script/Engine.CompositeCurveTable` | `profile.composite_curve_table.v1` | `curve_table.py` |
| `/Script/Engine.StringTable` | `profile.string_table.v1` | `string_table.py` |

Shared pieces are in `data_table_common.py`. The C++ provider `data` (module
`Source/AssetDocumentData/`, owner `EAssetDocumentPrimitiveOwner::Data`) has six primitives:
`data.data_table.read/edit.v1`, `data.curve_table.read/edit.v1` and `data.string_table.read/edit.v1`.
It uses the graph module's table-driven provider without graphs
(`CreateAssetDocumentAssetTablePrimitiveProvider`). Row and curve values go through the generic
route's typed value codec (`AssetDocumentEditorPropertyValueCodec.h`). Writes are covered by
Core's package snapshot (existing asset) or the factory rollback (created asset). There is no
inverse code.

Create uses the generic factory lifecycle (`editor.scratch.create_by_factory` /
`editor.asset.create_by_factory`; the editor picks the class's factory). Details-panel
properties go through the generic Editor property admission (`Properties`).

Classification:
- **Supported**: Extract, Apply and Diff.
- **Read-only**: Extract and Diff only; authoring a different value fails with a stable diagnostic.
- **Unsupported**: not in the document; the reason is given.

Every Apply runs editor-equivalent operations inside transactions (one editor transaction per
edit primitive call; the per-operation transactions of `FDataTableEditorUtils` nest into it). A successful apply that
recorded transactions clears the undo history that references the asset
(`AssetDocumentUndoHistory.h`); a failed apply restores the asset and keeps the human's history.

## Document shapes

```jsonc
// DataTable / MirrorDataTable
"Body": {"RowStruct": "/Script/MyGame.ItemRow",
         "Rows": [{"Name": "Sword", "Values": {"Damage": 12, "Label": "Sword"}},
                  {"Name": "Bow", "RenamedFrom": "Bow_Old", "Values": {"Range": 900.5}}]}
// CompositeDataTable
"Body": {"RowStruct": "/Script/MyGame.ItemRow", "ParentTables": ["/Game/Data/DT_Base.DT_Base"],
         "Rows": [/* derived, read-only */]}
// CurveTable (CompositeCurveTable: + "ParentTables", Rows derived)
"Body": {"CurveTableMode": "SimpleCurves",
         "Rows": [{"Name": "Damage", "InterpMode": "Linear", "Keys": [{"Time": 1, "Value": 10}]}]}
// StringTable
"Body": {"Namespace": "UI", "Entries": {"Menu.Start": "Start Game"},
         "MetaData": {"Menu.Start": {"Comment": "Main menu button"}}}
```

## 1. DataTable / CompositeDataTable / MirrorDataTable

| Editor surface | Body location | Class | Mechanism / reason |
|---|---|---|---|
| New Data Table (pick Row Structure: native `FTableRowBase` struct or User Defined Struct) | `RowStruct` + `Action: Create` | Supported | resolved with `editor.reference.resolve_asset` (now allowed in Apply) and handed to `UDataTableFactory::Struct` as `factory_context`. A missing struct gives `DataTable.RowStructRequired`, an unknown one `DataTable.RowStructNotFound`, a non-row struct `DataTable.InvalidRowStruct` (`FDataTableEditorUtils::IsValidTableStruct`). A failed create leaves nothing behind |
| Change Row Structure of an existing table | `RowStruct` | Read-only | the Data Table editor offers no row struct change. A different value fails with `DataTable.RowStructImmutable`; create a new table instead |
| Add row / Remove row | `Rows[]` entry added / omitted | Supported | `FDataTableEditorUtils::AddRow` / `RemoveRow` |
| Rename row | `Rows[i].RenamedFrom` | Supported | `FDataTableEditorUtils::RenameRow` keeps the row data. `RenamedFrom` is ignored once the rename happened, so a re-apply is a no-op |
| Reorder rows (drag, Move Up/Down) | order of `Rows[]` | Supported | `FDataTableEditorUtils::MoveRow`, only for rows that are out of place. The order is sent after adds, removes and renames, or when the authored order differs, as `{"op": "order", "rows", "at"}` segments of 1000 names (no single op outgrows the per-call budget); a value-only edit sends none |
| Edit a row's values (row editor) | `Rows[i].Values` (sparse: omitted fields are kept) | Supported | the row editor's sequence (`SRowEditor`): `Modify`, `BroadcastPreChange(RowData)`, copy, `HandleDataTableChanged(row)`, `MarkPackageDirty`, `BroadcastPostChange`. Values are decoded into a staged copy first, and unchanged rows are not touched. Diagnostics: `DataTable.UnknownField` (with suggestions), `DataTable.InvalidValue`, `DataTable.UnsupportedField`, each at `/Body/Rows/<i>/Values/<Field>` |
| Field kinds: bool, ints, float/double, enum, FString, FName, structs (FVector…), arrays, sets, string-keyed maps, soft object/class refs | `Values` | Supported | the shared typed value codec. float32 values extract as the shortest decimal that round-trips |
| Field kinds: hard object/class refs, FText, FInstancedStruct | `Rows[i].Values.<Field>` | Supported | the generic route's value forms (`object_path.v1`/`class_path.v1` path or `null`, `text.v1` source string, `instanced_struct.v1`); hard references resolve, load and obey the class filters; an unresolved path fails with `DataTable.InvalidValue` at the field |
| Field kinds: instanced subobjects (`Instanced` UObject members, at any depth) | — | Unsupported | the row editor shows the row as a struct with no owning object, so its class picker cannot create one (`SPropertyEditorEditInline::OnClassPicked` needs an object parent), and a decoded subobject would have no owner to be saved under. Left out of Extract, `Supported: false` in `RowSchema`, refused when authored (`DataTable.UnsupportedField`); an `FInstancedStruct` value whose struct holds one fails with `DataTable.InvalidValue` (`TypedValueCodec.InstancedObjectNeedsOwner`) |
| User Defined Struct fields | `Values` by friendly name | Supported | members are matched by `GetAuthoredName` (what the editor shows). Internal `MemberVar_N_<GUID>` names are unknown fields |
| Inspect the row schema | Inspect → `RowSchema {RowStruct, Fields[{Name, Type, Codec, Supported, EnumValues?}]}` | Supported | `data.data_table.read.v1` |
| Details panel (`bStripFromClientBuilds`, `bIgnoreExtraFields`, `bIgnoreMissingFields`, `ImportKeyField`, …) | `Properties` | Supported | generic Editor property admission. `RowStruct` / `ParentTables` are Body-owned (`DataTable.BodyOwnedProperty`) |
| CompositeDataTable parent tables (add, remove, reorder) | `ParentTables` (ordered; later parents override earlier ones) | Supported | the Details panel's sequence: `Modify`, `PreEditChange`, set, `PostEditChangeProperty(ValueSet)`, run inside `FAssetDocumentEditorNonInteractiveScope` (public header `AssetDocumentEditorNonInteractiveScope.h`; `WriteParentTables` in `AssetDocumentDataPrimitives.cpp`). Missing, mismatched and cyclic parents are still checked first, so the engine's dialogs (`UCompositeDataTable::UpdateCachedRowMap`) have no reason to open: `Composite.ParentNotFound`, `Composite.ParentRowStructMismatch`, `Composite.ParentCycle` at `/Body/ParentTables/<i>`. If one still tries, the scope suppresses it, and a refused modal fails the op with `Composite.InteractiveEditRefused` (rolled back) |
| CompositeDataTable rows | `Rows` | Read-only | derived from the parents; the editor does not edit them. Given Rows must equal the derived rows (`CompositeDataTable.RowsReadOnly`, checked after the parent edit and rolled back) |
| New Mirror Data Table (pick Skeleton, find/replace expressions) | `Action: Create` + top-level `FactoryContext` (Skeleton), `Body.RowStruct` (default `/Script/Engine.MirrorTableRow`) | Supported | `UMirrorDataTableFactory::FactoryCreateNew` creates nothing unless both `Skeleton` and `Struct` are set, which only its dialog (`SMirrorDataTableFactoryWindow`) writes; the dialog defaults the struct to `FMirrorTableRow` and filters structs to table structs with a `MirroredName` field. The profile passes the Skeleton and the row struct as the two factory context objects of `editor.asset.create_by_factory.v2` (`factory_context`, `factory_context_2`); the generic factory context fills each into the one factory field whose class accepts it (`ConfigureAssetDocumentEditorGenericFactoryContexts`; two objects for one field: `FactoryCreate.ContextAmbiguous`). The factory then runs `FindReplaceMirroredNames()` with the Animation settings expressions, as in the editor. Missing skeleton `MirrorDataTable.SkeletonRequired`, not a skeleton `MirrorDataTable.SkeletonNotFound`, a struct without `MirroredName` `MirrorDataTable.InvalidRowStruct`. Test `UEAssetDoc.DataTable.Mirror` (create equals a dialog-configured factory twin, round trip, update, failed creates leave nothing) |
| Mirror Data Table rows and Details | `Rows`, `Properties` | Supported | same as a DataTable (row struct `FMirrorTableRow`) |
| Import / reimport from CSV / JSON | — | Unsupported | a file import, not an authoring edit; author the rows instead |
| Copy / paste / duplicate row | — | Supported (as the result) | expressed as the resulting rows |
| Apply while the table is open in the Data Table editor | any | Supported | edited in place; the row primitives' change notifications refresh the row list and row editor, and after a rollback the open-editor handler (`AssetDocumentDataOpenEditorHandler.cpp`, `RefreshInPlace` in `Runtime/AssetDocumentOpenEditor.h`) sends the Data Table editors the row-list change their Undo ends in. Test `DataTable.OpenEditorApply` |

## 2. CurveTable / CompositeCurveTable

| Editor surface | Body location | Class | Mechanism / reason |
|---|---|---|---|
| New Curve Table (Linear/Constant → simple curves, Cubic → rich curves) | `CurveTableMode` + `Action: Create` | Supported | generic factory. The mode is set by the first curve, as the editor's first add does |
| Add / remove / rename curve | `Rows[]` / omit / `RenamedFrom` | Supported | `UCurveTable::AddSimpleCurve` / `AddRichCurve` / `DeleteRow` / `RenameRow`, wrapped in `Modify` and `PostEditChangeProperty` like `FCurveTableEditor` |
| Edit keys (time, value, interpolation, tangents, extrapolation, default value) | `Rows[i]` curve fields | Supported | simple curves: `InterpMode` (Linear/Constant), `Keys [{Time, Value}]`, extrapolation, `DefaultValue`. Rich curves: the CurveFloat curve form (`curve_float.RichCurveCodec`). Native curves go through the shared codec over `FSimpleCurve` / `FRichCurve` |
| Change the curve type of a table with curves | `CurveTableMode` + `Rows` | Supported | rebuilds the table from Rows (`EmptyTable`, then add), as a reimport with the other type does. Without Rows it fails with `CurveTable.ModeChangeNeedsRows` |
| Curve order | — | Unsupported | the Curve Table editor has no reorder; the row map order is not authored (Diff ignores it) |
| Invalid keys / types | — | — | `CurveTable.KeysNotAscending`, `CurveTable.InvalidKey`, `CurveTable.InvalidInterpMode` (cubic in a simple table), `CurveTable.ModeRequired`, each with the document path |
| CompositeCurveTable parent tables | `ParentTables` | Supported | same mechanism and checks as CompositeDataTable |
| CompositeCurveTable curves | `Rows` | Read-only | derived (`CompositeCurveTable.RowsReadOnly`) |
| Details panel | `Properties` | Supported | generic admission (CurveTable has no Details-visible authorable property besides import data) |
| Import from CSV / JSON | — | Unsupported | file import |

## 3. StringTable

| Editor surface | Body location | Class | Mechanism / reason |
|---|---|---|---|
| New String Table | `Action: Create` | Supported | generic factory. The namespace defaults to the asset name (the template sets it); `Namespace` sets it in the same apply |
| Namespace | `Namespace` | Supported | `Modify`, then `FStringTable::SetNamespace` (as the editor's namespace box). Identity rule as the editor: no `"' ,\n\r\t` and no `[` `]` (`StringTable.InvalidNamespace`) |
| Add / edit / delete entry | `Entries {key: source}` (full map; omit = keep) | Supported | `Modify`, then `SetSourceString` / `RemoveSourceString`. Empty source strings are refused as in the editor (`StringTable.EmptySourceString`). A rename is remove + add, as in the editor |
| Entry metadata (e.g. `Comment`) | `MetaData {key: {id: value}}` | Supported | `FStringTable::SetMetaData` / `RemoveMetaData`. In the editor, metadata is set through CSV import only; the Profile uses the same API |
| Details panel | `Properties` | Unsupported | a StringTable has no Details-panel properties (`StringTable.UnsupportedProperties`) |
| Import from CSV | — | Unsupported | file import |

### StringTable cleanup (coverage audit "cleanup failed")

`FStringTableEngineBridge` (Engine/Private/Internationalization/StringTable.cpp) keeps every
loaded `UStringTable` in `KeepAliveStringTables`. It reports them to GC as an in-memory
reference, and its GC callback drops only tables flagged `RF_MirroredGarbage` /
`RF_NewerVersionExists`.

So, for a loaded string table:
- `UPackageTools::UnloadPackages` + GC never frees it (this is how the audit cleaned up);
- the editor's plain delete (`ObjectTools::DeleteObjects`) reports the bridge's in-memory
  reference and fails ("Could not delete"), just as the Content Browser's delete dialog lists it;
- the dialog's **Force Delete** (`ObjectTools::ForceDeleteObjects`) and the AssetDocument
  created-asset rollback (`OrphanPackageCleanup` → `MarkAsGarbage`) both mark it garbage, and
  both release it.

This is engine behaviour, not an AssetDocument leak. The coverage audit's cleanup now
force-deletes what an unload leaves alive. `UEAssetDoc.StringTable.Python`
checks that a force-deleted table leaves no object, package file or registry entry, and that a
failed create leaves nothing.

## 4. Limits

- **Two budgets.** A whole document (request `CanonicalDocument`, result `CanonicalValue`, Python
  `DocumentCodec`) is bounded by `RuntimeDocumentMaximumNodes` = `DocumentCodec.MAXIMUM_NODES` =
  2^20 nodes, a memory bound. Every primitive call keeps the 4096-node per-call budget
  (`RuntimeValueMaximumNodes`: invocation values, outcomes, changed sets, diagnostics). Reads are
  paged by that budget (`AppendPaged`), and the Profile chunks edits below it (`chunk_ops`,
  3000 nodes per call; the row order goes as segments). A 20-field row costs about 23 nodes.
- **Before (811a1681).** The document cap was 4096 nodes, so Extract and Apply both stopped at about
  177 rows × 20 fields (`Codec.NodeLimitExceeded` at `/Body/Rows/177/...`). An edit of a table over
  about 4,000 rows would also have failed: it always sent one `order` op naming every row, larger
  than the per-call budget. A create recorded the whole table once per row
  (`FDataTableEditorUtils` opens a transaction per row), so 5,000 rows took 417 s.
- **Then (ec7eecbf).** The binding limit was an 8 MiB host transport limit on the result's
  canonical bytes (`MaximumBytesLeaf`): about 20,000 rows × 20 scalar fields (18,000 passed,
  22,000 failed) and about 13,000 rows of the FText and object-reference row. Every table
  primitive call also recorded the whole table into the transaction buffer twice (pre-image and
  the finalize diff), so a chunked create cost O(rows²): 10,000 rows took 46.8 s on the PC.
- **Now.** Table primitives do not record undo (`FTableUndoRecordingSuppression`: the table's
  `RF_Transactional` flag is cleared for the call). An AI apply is not part of the user's Undo stack
  (direction reset §3.1) and its rollback restores the package snapshot, so those records were never
  used; with no record the transaction buffer drops the empty transaction. The transport limit is
  64 MiB, so the binding limit is the 2^20-node document budget: about 45,500 rows × 20 scalar
  fields (Extract of 44,000 rows passes in 11.7 s; 47,000 rows fails with
  `Codec.NodeLimitExceeded` at `/Body/Rows/45589`, about 18 MiB of JSON). Rows whose values are
  nested (FText, struct or reference objects) cost more nodes per row and stop earlier.
  Create from the full document, PC sandbox editor (`DataTable.LargeTableScaling` asserts the
  per-row time of 25,000 rows stays under twice that of 1,000 rows):

  | Rows | Before | After |
  |---|---|---|
  | 1,000 | 2.5 s | 1.5–1.9 s |
  | 5,000 | 18.0 s | 8.3 s |
  | 10,000 | 46.8 s | 17.5 s |
  | 20,000 | not measured (near the 8 MiB limit) | 35.3 s |
  | 30,000 (12 MiB JSON) | (over the transport limit) | 48.6 s |

  At 10,000 rows the editor primitive calls went from 26.4 s to 0.67 s; what is left is the
  linear Python document work and the save/verify.
- **Measured before the undo change** (`UEAssetDoc.DataTable.LargeTable` /
  `LargeRichTable`, 5,000 rows × 20 fields, sandbox editor on macOS). Resident memory is the process delta; it is noisy and is
  only a rough guide.

  | Stage | Scalar row (2.0 MiB JSON) | FText + hard/soft refs (3.1 MiB JSON) | Scalar, 18,000 rows (7.3 MiB) |
  |---|---|---|---|
  | Extract | 0.48 s, +47 MiB | 0.51 s, +56 MiB | 2.3 s |
  | Diff | 0.87 s, +18 MiB | 0.92 s, +27 MiB | 4.8 s |
  | Unchanged re-apply (no-op, no save) | 1.31 s, +27 MiB | 1.37 s, +33 MiB | 5.7 s |
  | One-row edit (save included) | 4.46 s, +73 MiB | 4.81 s, +98 MiB | 18.2 s |
  | Create from the full document | 5.83 s, +197 MiB | 6.98 s, +145 MiB | 46.7 s |

  The 18,000-row column is the ec7eecbf build (the create figure includes the quadratic undo
  recording described above). A full reversal of a large table moves almost every row, and each
  `MoveRow` re-sorts the row map, so it is O(rows² log rows).
- **One row or curve per op.** A single row (or curve with its keys) larger than the per-call
  budget cannot be sent: about 4,000 value nodes in one row.
- **Integral floats.** A float or double field accepts `1` as well as `1.0`: integers are coerced
  on decode, and Diff compares numbers numerically.
