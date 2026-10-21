# How UE-AssetDoc thinks

[中文](INTRO.zh-CN.md)

UE-AssetDoc turns the Unreal Editor into an MCP server for AI clients. Many editor MCP servers expose one tool per
editor action: add a node, set a pin, connect two pins, compile. UE-AssetDoc works on whole assets instead: an AI reads
an asset as one JSON document, edits the document, checks what would change, and hands it back to the editor, which
applies it with its own editing operations. This page explains that idea and what you and your AI can rely on. For
installation see the [README](../README.md); for client setup and the tool list see [MCP/README.md](../MCP/README.md).

## 1. An asset is one document

Every asset an AI edits is an **AssetDocument**: one JSON object that describes the whole authored state of the asset.

```jsonc
{"SchemaVersion": 1, "Target": "/Game/AI/BT_Enemy", "Class": "/Script/AIModule.BehaviorTree",
 "Action": "CreateOrUpdate", "Definitions": {}, "Properties": {}, "Body": {"BlackboardAsset": ..., "Tree": ...}}
```

- `Target` is the asset's package path, `Class` its exact class path, `Action` is `Create`, `Update` or
  `CreateOrUpdate`.
- `Properties` holds reflected properties, as in the Details panel. `Body` holds what the Details panel does not
  show: a Behavior Tree's nodes, a Data Table's rows, a Blueprint's graphs, a Niagara system's emitters.

The work is a loop of five steps, each a tool:

| Step | Tool | Changes anything? |
| --- | --- | --- |
| extract | `extract_asset_document` | no |
| edit | (the AI edits the JSON) | no |
| validate | `validate_asset_document` | no |
| diff | `diff_asset_document` | no |
| apply | write the sidecar file, `apply_asset_document_file` | yes |

The document is declarative. It says what the asset should look like, not which clicks lead there. Two rules make that
precise:

- An **omitted** `Body` key keeps that part of the asset as it is, so a document may list only what it changes.
- A **present** key is the complete desired state of that part: entries it does not list are removed. A Data Table
  `Rows` list is the whole table; a Behavior Tree `Tree` is the whole tree.

Top-level `Properties` are always sparse: only the listed properties are written. Applying an unchanged extract
changes nothing.

The document lives next to the asset as a sidecar file, `<Content dir>/AI/BT_Enemy.assetdoc.json` for
`/Game/AI/BT_Enemy`. Writing that file is the apply: the editor watches sidecar files and applies one about half a
second after it is written. `apply_asset_document_file` returns the result (or the result of the watcher's apply).

## 2. Editor fidelity

An apply goes through the editor's own editing operations: the factories the Content Browser uses, the graph schema
actions, the same property write path as the Details panel, the editor's own struct and enum utilities. The result is
the asset a person would get by making the same edit by hand. Some examples:

- A Data Table row with `RenamedFrom` is renamed with Rename Row, not removed and added again.
- A new Mirror Data Table takes the Skeleton the New Mirror Data Table dialog would ask for (`FactoryContext`), and
  the factory fills its rows as it does in the editor.
- A `float` property stores the nearest 32-bit float, as the Details panel does.
- When a user-defined struct changes, the editor updates the Data Tables and Blueprints that use it, as it does when
  you edit the struct by hand.

What the editor cannot make is refused with a reason, never approximated:

- A factory that would open a modal dialog is cancelled and nothing is left behind
  (`FactoryCreate.InteractiveFactory`).
- A value the editor would rewrite (a clamp, a renamed socket) is refused before anything changes
  (`Generic.PropertyValueNotStoredExactly`), and the diagnostic shows the value the asset would hold.
- A Data Table's row struct cannot change after creation (`DataTable.RowStructImmutable`); the message says to create
  a new table.

The engine itself is never modified, and the plugin does not use private engine headers.

## 3. Check before you change

`validate_asset_document` runs every check apply runs and changes nothing. On success its message says what apply would
do: `would create '<target>'`, `would change N value(s)` (and which entries it would remove), or `would change nothing`.

`diff_asset_document` lists each change as `{path, kind, before, after}`: `path` is a JSON Pointer into the document,
`kind` is `Add`, `Remove` or `Replace`, `before` is the asset and `after` the document. The payload sorts the answer
into containers: the changes are in `changed`; `unchanged`, `skipped` (regions not compared) and `failed` are normally
empty, and the Behavior Tree profile also has `added` and `removed`. An empty `changed` (all containers empty) means
the asset already matches.

An extract carries a `BaseRevision`. If someone changed the asset after the extract, validate and apply refuse the
document (`Document.StaleBase`), so an AI does not overwrite edits it has not seen.

## 4. All or nothing

An apply either succeeds as a whole or leaves the asset as it was:

- A failed apply is rolled back: the asset extracts the same as before, its compile state is no worse, the files on
  disk are not rewritten, and no new package is left behind.
- A Blueprint whose compile has errors after the apply fails with `Graph.FinalizeFailed` and is rolled back. Compiler
  warnings come back as warnings on the successful result.
- After a timeout or a lost connection the sidecar may still have been applied. A `diff_asset_document` that comes
  back empty tells you it was.

An AI's apply is not an entry in the editor's Undo history; Ctrl+Z does not undo it. To go back, apply the earlier
document or use version control. So that a later Ctrl+Z cannot leave an asset half changed, an apply removes the Undo
entries that touch that asset and says so in the result (`Undo.HistoryTrimmed`). Level edits are different (section 8).

## 5. Built for an AI reader

Every result has the same shape: `{success, message, target, asset_path, sidecar_file_path, saved_asset,
wrote_sidecar, diagnostics[], compile?, payload?}`. A diagnostic is `{path, code, message, severity?, details?}`:

- `path` is a JSON Pointer to the field in the document the AI wrote, so it knows what to fix.
- `code` is stable, and its prefix names the layer that refused: `Meta.*` the header, `Generic.*` the generic profile,
  `DataTable.*` or `Widget.*` an exact profile, `Graph.*` a Blueprint graph, `PropertyWrite.*` a value the editor
  refused.
- A failure leads with its root cause: `diagnostics[0]`. Many not-found messages name the close matches.

Tool descriptions are short. The details are in guides the AI reads through the tools: `get_asset_document_schema`
returns the core guide and one section per profile (the `Body` shapes it accepts, its diagnostics, its pitfalls), and
`get_level_editing_guide` covers level editing, playtesting, import and settings. `inspect_asset_document_target`
lists the writable properties of a class or what an existing asset offers to build on; `find_assets` and `list_types`
find assets, classes, structs and enums.

## 6. Profiles

The document `Class` picks a profile, which owns the meaning of `Body` for that asset family. Exact profiles cover
Blueprints and Widget Blueprints, Animation Blueprints and animation assets, Behavior Trees and Blackboards,
StateTrees, EQS queries, Data Tables and user-defined structs and enums, curves, Materials and Material Instances,
Niagara, MetaSounds and Sound Cues, IK Rigs and retargeters, Control Rigs, Physics Assets, Level Sequences and more.
`inspect_asset_document_profile` tells which profile owns a class.

Any other class uses the **generic profile**: its reflected properties, written as the Details panel writes them,
created with the factory the Content Browser uses. Data Assets, Input Actions and Input Mapping Contexts, for
example, use this profile.

The support matrices in [`docs/architecture/`](architecture/) record, per asset family, what can be read, created
and changed. Anything a person commonly changes in the editor should be in it; a rarer form that is not supported is
refused with a diagnostic that says what to do instead.

## 7. The editor is the server

There is no separate process between the AI and the editor. The plugin serves MCP inside the editor at
`http://127.0.0.1:8559/mcp`; a small stdio bridge (`ue-assetdoc`, Node.js) relays to it for clients that only start
stdio servers. The plugin's modules are editor modules only.

The endpoint can drive the editor as the user who runs it, including `execute_python` and `delete_assets`, so it is
local by default:

- It listens on the loopback interface only and refuses a request whose `Host` is not `localhost`, `127.0.0.1` or
  `[::1]`, or whose `Origin` is not a page served from localhost.
- Requests that send data must be `application/json`, which a web page cannot send to another origin without a CORS
  preflight; the server answers none.
- A bearer token can be required, and `execute_python` and `delete_assets` can be switched off in Project Settings.
  Both are on by default.
- Remote connections are off; turning them on always requires the token.

Treat access to the endpoint like a shell session of the editor's user. [SECURITY.md](../SECURITY.md) says what is
exposed and how to report a vulnerability.

## 8. Beyond documents

Levels are not asset documents. `query_level` reads the open level and `edit_level` changes it in batches: a batch is
checked first (`dry_run: true` shows the plan), applied as a whole or rolled back, and is one Edit > Undo entry.
`play` and `query_play` run Play In Editor, inject input, wait on conditions and read the running game;
`get_viewport_screenshot` shows what the player sees. Editor utilities import and export files, save, reload and
delete assets, open and close asset editors, change project settings, and run Python in the editor for the rest.

## 9. A walkthrough: changing a Behavior Tree

The asset is the guide's example tree `/Game/AI/BT_Enemy`: a Selector whose first child chases `TargetActor` and whose
second child waits. The AI wants the wait to be 3.5 seconds instead of 2.

**Extract.** `extract_asset_document {"asset_path": "/Game/AI/BT_Enemy"}` returns the document.
The part that matters (abridged):

```jsonc
"Children": [
  {"Id": "00000000000000000000000000000003", "Class": "/Script/AIModule.BTComposite_Sequence", ...},
  {"Id": "00000000000000000000000000000006", "Class": "/Script/AIModule.BTTask_Wait", "Editor": {...},
   "Properties": {"WaitTime": {"DefaultValue": 2.0, "Key": "None"}}}
]
```

**Edit.** The AI changes `"DefaultValue": 2.0` to `3.5` and keeps everything else, including the node `Id`s, which
keep the nodes' identity.

**Validate.** `validate_asset_document` with the edited document answers
`AssetDocument valid; Apply would change 1 value(s)`, with `change_count: 1` in its payload.

**Diff.** `diff_asset_document` returns one change:

```json
{"path": "/Body/Tree/Root/Children/1/Properties/WaitTime/DefaultValue",
 "kind": "Replace", "before": 2.0, "after": 3.5,
 "status": "changed", "current": 2.0, "desired": 3.5}
```

(`status`, `current` and `desired` are the Behavior Tree profile's own names for the same change.)

**Apply.** The AI writes the document to `<Content dir>/AI/BT_Enemy.assetdoc.json` and calls
`apply_asset_document_file` with that path. The result is `BehaviorTree document applied` (if the file watcher
applied it first, the call returns that result with `applied_by: "file_watcher"`). The asset is saved, and an open
Behavior Tree editor is closed and reopened on the result (`BehaviorTree.EditorReopened`). A diff of the same document
is now empty.

**A mistake.** Had the AI written `"Key": "TargetActr"` on the Move To node, validate would refuse it before anything
changed:

```
[UnknownBlackboardKey] at /Body/Tree/Root/Children/0/Children/0/Properties/BlackboardKey/Key:
The key 'TargetActr' does not exist in the effective BlackboardAsset. Close: TargetActor.
```

One remark outside the asset: Move To needs a navigation mesh. A level with no `NavMeshBoundsVolume` has none, and the
enemy would stand still in Play In Editor (`query_play` shows `move_status` `Idle`). `edit_level` can spawn
`/Script/NavigationSystem.NavMeshBoundsVolume`, scaled over the floor; the engine then builds the mesh.

## 10. What it is not, and its limits

- It is not a runtime or gameplay plugin; it works in the editor.
- It does not replace the editor. It edits the same assets the same way, so a person can open and change anything the
  AI made, and the next extract shows that change.
- Some things have no document: levels (use the level tools), GameplayTags (edit their ini or use the
  GameplayTagsManager editor API through `execute_python`), and CSV or JSON import into Data Tables (author the rows).
- An asset with unsaved changes made outside the document is refused (`DirtyExistingAssetTransactionUnsupported`):
  save it (`save_assets`) and extract again, or reload it to drop those changes.
- Assets outside `/Game` (engine and plugin content) can be read and referenced, not authored; extract them and apply
  a copy under `/Game`, or use `duplicate_asset`.
- Large assets make large documents: a Control Rig extract can run to hundreds of kilobytes.
- Known open issues, with their workarounds, are in
  [docs/architecture/asset-document-known-issues.md](architecture/asset-document-known-issues.md).

## Further reading

- [MCP/schemas/AssetDocument.md](../MCP/schemas/AssetDocument.md): the core guide the AI reads (workflow, header,
  omitted versus empty, validate and diff, diagnostics).
- [MCP/schemas/assetdocument/](../MCP/schemas/assetdocument/): one section per profile.
- [docs/architecture/asset-document-design-rules.md](architecture/asset-document-design-rules.md): the rules the
  profiles follow (rollback, engine APIs, support matrices).
