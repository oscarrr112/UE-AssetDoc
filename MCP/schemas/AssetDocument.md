# AssetDocument Guide

AssetDocument is the JSON format for authoring Unreal assets. One document (`SchemaVersion`, `Target`, `Class`, `Action`, `Definitions`, `Properties`, `Body`) describes one asset.

The document `Class` picks a **profile**: an exact one (its section, see "Profile Sections", has its Body shapes) or the generic reflected profile.

## Workflow

1. Find what exists: `find_assets` (path prefix, class, name; `uses`: what its graphs use, e.g. `K2:CallArrayFunction`), `list_types` (classes, structs, enums; `kind: "struct", base: "/Script/Engine.TableRowBase"` for row structs). `inspect_asset_document_target` on a `/Script/...` struct or enum lists its fields or values; on an asset, what to build on (slots, sockets, bones, parameters, compile status), summarized unless `detail: true`.
2. `inspect_asset_document_profile` shows which profile owns a class and its `BodySections`; `inspect_asset_document_target` lists the writable reflected `Properties`.
3. Existing asset: `extract_asset_document` and edit the result (extract emits exactly the shapes the profile accepts; applying an unchanged extract is a no-op). Keep its `BaseRevision`: apply and validate refuse it if the asset changed since (`Document.StaleBase`); after an apply, use its `base_revision`. New asset: `create_asset_document_template` with `class` and `target`, or `duplicate_asset`.
4. `validate_asset_document` and/or `diff_asset_document` (inline `document` or `file_path`; neither changes anything).
5. Write the document to its sidecar file and call `apply_asset_document_file` with that path.
6. Confirm with `extract_asset_document` (or a now-empty `diff_asset_document`).

## Sidecar Files And The File Watcher

A sidecar is `<Content dir>/<path>/<AssetName>.assetdoc.json`: `/Game/AI/BB_Enemy` is `<Content dir>/AI/BB_Enemy.assetdoc.json`. `health_check` gives `contentDir`. Its `Target` must equal the path-derived `/Game` target; a file outside `Content` (or linked out) is refused. There is no inline apply.

**The editor applies a sidecar as soon as it is written** (about 0.5 s after the file changes, and saves), once per file content. If the watcher already applied this content successfully and the asset is unchanged since, `apply_asset_document_file` returns its result (`applied_by: "file_watcher"`; a `Create` reports the creation, not `TargetExists`); otherwise it applies itself. A failed create leaves no asset and keeps the sidecar (`sidecar_kept: true`): fix it and apply again, or delete it. An apply that changes the asset rewrites the sidecar (`Create` as `CreateOrUpdate`): edit and save it again.

**Mirroring** (`health_check` `sidecar_sync.enabled`): every `/Game` asset keeps its sidecar as its mirror, so read assets from their files and edit by writing them (applied, then rewritten; an omitted key keeps its value). Files are current when `sidecar_sync.state` is `idle` and show the assets as last saved. Edits made while the editor is closed are applied at its next start. A change on both sides is a conflict (`Sync.Conflict` and a notice; the asset's side is in `<AssetName>.assetdoc.asset.json`): edit the sidecar to the result, then `sync_asset_document` `accept_sidecar` (or `accept_asset`).

## Document Header

```json
{"SchemaVersion": 1, "Target": "/Game/Data/DA_Test", "Class": "/Script/MyGame.MyDataAsset",
 "Action": "CreateOrUpdate", "Definitions": {}, "Properties": {}, "Body": {}}
```

- `SchemaVersion`: the integer `1`.
- `Target`: canonical `/Game/...` package path (letters, digits, `_`, `-`, `/`; no `.ObjectName` suffix). Assets outside `/Game` (engine and plugin content) can be referenced and inspected, not authored.
- `Class`: exact class path, `/Script/Module.ClassName` or `/Game/Path/BP_Name.BP_Name_C`. Short class names are rejected.
- `Action`: `Create`, `Update` or `CreateOrUpdate`. `Create` of an existing asset (`CutoverPreparation.TargetExists`) and `Update` of a missing one fail; templates and extracts carry `CreateOrUpdate`.
- `Definitions`, `Properties`: objects, optional. `Body`: profile-specific; absent or `{}` for the generic profile.
- `AssetType`: legacy; only `"GenericAsset"` (`Meta.InvalidAssetType`). A generic document without `Body` and `Definitions` needs it (`Meta.AssetTypeRequired`); an exact profile needs its `Body` (`Meta.BodyRequired`).
- `FactoryContext` (generic profile, new assets only): see the Generic section.

Field names are case-sensitive; unknown keys and the extract-only `_Skipped` are rejected. A top-level `_meta` object (written by mirroring) is ignored; leave it as it is.

## Omitted Versus Empty

- An **omitted** Body key (region) keeps the current state, so a document may list only the regions it changes.
- A **present** region is the complete desired state of that region: entries not listed are removed (or reset to the default or parent value); `[]` or `{}` clears it.
- Inside a listed entry, omitted optional fields take their defaults (extract, edit, apply is stable). Deviations are stated per profile.
- Top-level `Properties` are sparse on every profile: only listed properties are written.

## Validate And Diff

- `validate_asset_document` runs apply's checks, changing nothing. On success it says what apply would do: `would create '<target>'`, `would change N value(s)` (payload `change_count`, `changes`, `removals`) or `would change nothing`. Only apply reports save failures and Blueprint graph edits beyond keys, pins, links.
- `diff_asset_document` lists what apply would change. Entries: `path` (JSON Pointer of the smallest differing value), `kind` (`Add`, `Remove`, `Replace`), `before` (asset), `after` (document). The payload holds them in `changed` (empty: nothing), beside `unchanged`, `skipped`, `failed` (normally empty; Behavior Tree also `added`, `removed`). A missing asset gives `would_create: true` and a `/` Add; an `Update` fails with `GenericRuntime.UpdateTargetMissing`. Some regions diff as one `Replace` (see the profile sections).
- An inline `document` is read like the same sidecar text (`1` stays an integer, `1.0` a float).
- `extract_asset_document` with `diff_only: true` (default) omits class-default values (`Properties` and profile node values); `diff_only: false` lists every writable value.

## Results And Diagnostics

Every result is `{success, message, target, asset_path, sidecar_file_path, saved_asset, wrote_sidecar, diagnostics[], compile?, payload?}`. HTTP 400, 207 (partial) and 200 with `success: false` are failures. A failed result leads with its root cause: `diagnostics[0]`, and `message` is `<Code>: <Message> (at <Path>) (+N more)` of it; diagnostics reporting its consequences follow.

A diagnostic is `{path, code, message, severity?, details?}`:

- `path` is a JSON Pointer into the authored document (`/Body/Keys/2/Name`): the field to fix.
- `severity` is `error`, `warning` or `info`; a successful result can carry warnings and notes.
- `details`: internal context, not something to edit.

Code prefixes name the rejecting layer: `Meta.*` header; `Generic.*` generic profile; `<Profile>.*` an exact profile; `Graph.*` Blueprint graphs; `FactoryCreate.*` creation; `ClassReference.*`, `TypedValueCodec.*`, `PropertyWrite.*` a refused value; `GenericRuntime.*` runtime or routing (`VerificationMismatch`: read back differently, rolled back); `RequestCoordinator.*`, `Persistence.*` save and install.

A failed apply is rolled back: same extract, compile state not worse, no new package left behind.

**No result is not a failure:** after a connection error or timeout, `health_check`, then `diff_asset_document` (empty: applied) before retrying.

## Blueprint Compile Status

Extract and apply results of Blueprint-family assets (Blueprint, WidgetBlueprint, AnimBlueprint) carry `compile` (inspect: payload `Compile`):

```json
{"Status": "UpToDateWithWarnings",
 "Messages": [{"Severity": "Warning", "Message": "...", "Graph": "EventGraph", "Node": "Print String"}]}
```

`Status` is the editor's: `UpToDate`, `UpToDateWithWarnings`, `Error`, `Dirty` (not compiled since the last change), `Unknown` or `BeingCreated`. `Messages` are the compiler messages on the graph nodes. An apply whose compile has errors fails with `Graph.FinalizeFailed` and is rolled back; warnings and notes come back as diagnostics of the successful apply (`Graph.FinalizeMessage`, `Graph.OrphanedPinDefault`). Other compiled assets report it too (see their sections).

## Editor Undo History

AI applies do not enter the user's Undo stack. An apply removes the Undo entries that referenced the asset and older ones that would undo across them (`Undo.HistoryTrimmed`); other history stays undoable with `undo_editor`, or is cleared if it cannot be split (`Undo.HistoryCleared`). An asset with unsaved changes is rejected (`DirtyExistingAssetTransactionUnsupported`): `save_assets` and extract again, or `reload_assets` (force) to drop them.

## Pitfalls

- `Generic.UnknownProperty` carries no suggestion: check the name against `inspect_asset_document_target`.
- `float` properties store the nearest float32, as the Details panel does; `Generic.PropertyValueNotStoredExactly` is only for values the editor rewrites (clamps, normalised names).
- GameplayTags have no document: `execute_python` on the GameplayTags ini (`Config/DefaultGameplayTags.ini`) or the GameplayTagsManager editor API.

## Profile Sections

Read the section of your class before writing its `Body`: `get_asset_document_schema` with `profile` set to the class path (`/Script/Engine.CurveTable` finds the DataTable section; any other class, Generic) or a section name below; `"all"` returns every section.
