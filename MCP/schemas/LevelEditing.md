# Level Editing Guide

Levels are not asset documents: the open level (persistent level, loaded sublevels, World Partition actors) is read
with `query_level`, changed with `edit_level`; `open_level` / `new_level` change the map; `undo_editor` undoes a batch.

Workflow: `query_level` `{"what": "world"}`, then actors with filters and `detail`; `edit_level` with `"dry_run":
true`, then the same batch; confirm with `query_level` / `get_viewport_screenshot`. All level tools are refused
during Play In Editor (`Level.PlaySessionActive`).

## Actor references

Each result actor has `path` (the stable identity, `/Game/Maps/Room.Room:PersistentLevel.PointLight_0`), `guid`
(stable; also names unloaded World Partition actors), `label` (the Outliner's, not unique), `class`, `level`,
`external_package` (the actor's own package on World Partition maps), `loaded`.

References (query_level `actors`, edit_level `actor`/`actors`/`after`): a path string, `{"path"}`, `{"guid"}`,
`{"label", "level"?}`, a bare label or object name; edit_level also `"@id"`. Labels and names must match one actor,
else `Level.ActorRefAmbiguous` with `details.candidates`. edit_level resolves references before the batch: a label an earlier op sets does not
redirect a later op.

## query_level request

All optional; unknown fields are refused (`Level.InvalidRequest`).
- `what`: `"actors"` (default) or `"world"` (summary, with unsaved `dirty_packages`). `actors`: references (filters still
  apply).
- `class` (`/Script/Engine.Light`, Blueprint `/Game/BP/BP_Door`; `exact_class`: no subclasses); `name`: wildcard
  (`*`, `?`) over label and object name; `folder`: prefix by whole segments (`Room` matches `Room/Walls`); `tag`,
  `layer`, `data_layer` (asset path or name): exact; `level`: level package (`/Game/Maps/Room_Sub`).
- `near` `{"location": {X,Y,Z}, "radius": cm}`, `bounds` `{"min", "max"}`: by actor location; `selected_only`;
  `include_unloaded` (World Partition).
- `detail`: `"summary"` (default), `"components"`, `"properties"` (= `"full"`); `diff_only` (with `"properties"`:
  only overrides, structs with only their changed members; default `true`); `properties`: value paths (`"PointLightComponent.Intensity"`) read into
  `values` / `value_errors`.
- `limit` (default 100, at most 1000), `offset`.

Actors are the Outliner's, in a stable order: `{world, partitioned, total, offset, limit, actors}`.

## Actor fields

- Summary: the reference fields, `folder`, `parent` / `parent_socket`, `tags`, `layers`, `data_layers`, `transform`
  (`location`, `rotation` `{Pitch,Yaw,Roll}` degrees, `scale`; relative, as the Details panel shows).
- `"components"` adds `components`, root first: `name` (the key edits use), `class`, `creation` (`Native`,
  `Blueprint`, `Instance`, `ConstructionScript`), `editable` (as the Details panel), `parent`, `socket`, `root`.
- `"properties"` adds `properties` to each component and the actor.

## Property values

AssetDocument value forms: structs as objects (`{"X":1,"Y":2}`), enums by name, assets as object paths, actors as
actor paths (a hard pointer: same level only, as the actor picker). Only properties the Details panel edits on a
placed actor are listed and writable; EditDefaultsOnly ones belong to the Blueprint (its asset document).

## edit_level request

```json
{"description": "AI: door", "after": {"select": ["@door"]}, "ops": [
 {"op": "spawn", "id": "door", "asset": "/Game/BP/BP_Door", "folder": "Room", "transform": {"location": {"X": 390}}},
 {"op": "set", "actor": {"label": "Lamp"}, "tags": ["lit"], "properties": {"Intensity": 500}}]}
```

Ops (`[optional]`; edits below):

- `spawn` `class` (actor class or Blueprint) or `asset` (a Content Browser drop) [`id`, `level`, `attach_to`,
  `transform`, edits]; `duplicate` `actor` [`id`, `offset`, `level`, `transform`, edits]; `set` `actor`
  [`transform`, edits]; `delete` `actors` [`allow_referenced`].
- `add_component` `actor`, `class`, `name` [`parent`, `socket`, `asset` (a mesh for a mesh component),
  `properties`]; `remove_component` `actor`, `component` (instance components only; its children move to its
  parent); `reparent_component` `actor`, `component`, `parent` [`socket`].
- `attach` `actor`, `parent` [`socket`, `component`]; `detach` `actor`.
- `create_folder` / `delete_folder` `path` (delete moves its actors and sub-folders up, as the Outliner);
  `rename_folder` `from`, `to` (not onto an existing folder).
- `pin` / `unpin` `actors`: load / release World Partition actors; after the batch, not in Undo.

Edits: `label`, `folder`, `tags`, `layers` (not World Partition), `data_layers` (World Partition), `properties`,
`components`. Lists replace the whole list.

- `transform`: `location`, `rotation` (degrees), `scale`; omitted members keep their value. `space`: `"local"`
  (default, relative to the attach parent, as the Details panel) or `"world"`.
- `folder` `""` is the root. Attach/detach keep the world transform and move the actor to its topmost parent's
  folder (as the Outliner); a later `transform` is parent-relative.
- `components`: `{"<ComponentName>": {"properties": {...}}}`, names from `detail: "components"`.
- `"id"` names a spawned/duplicated actor; later ops use `"@id"` as a reference or property value. At most 500 ops.
- `after.select` replaces the selection, `after.focus` moves the camera; only on success.

## edit_level result

Payload: `transaction` `{title, id, undoable}`, `actors` (batch id -> reference), `ops`, `changes`,
`dirty_packages`, `already_dirty`, `saved_packages`, `selection`, `pinned` / `unpinned`. A change is `{op, kind
(Add, Remove, Replace), object (actor, property, transform, component, attachment, label, folder, ...), actor,
component?, property?, before?, after?}`. A dry run returns `dry_run: true` and the planned `changes`.

## Transactions and Undo

- Everything (actors, assets, components, properties, delete referencers) is checked first; a refused batch changes
  nothing (`rolled_back: false`, `phase: "preflight"`). A batch that changes nothing records no Undo.
- One batch is one Edit > Undo entry titled by `description` (`transaction.title` / `id`; `execute_python`:
  `undo_title` / `undo_id`). `undo_editor` (`expect_title` / `expect_id`, `direction`) undoes (redoes) it only while
  it is the newest Undo (next Redo) entry; else `Undo.EntryMismatch` names the entry on top (e.g. the user's) and
  nothing changes.
- Written values are read back; a failure while applying or verifying undoes the whole batch (`rolled_back: true`,
  `phase`, `rollback_check: "equal"`), the user's earlier Undo entries untouched.

An AssetDocument apply to an asset level actors use (a placed Blueprint) removes the Undo entries recording or
referencing it and older ones that would undo across them (`Undo.HistoryTrimmed`); other batches stay undoable.

## Saving

Nothing is saved unless `save: true`, which saves `dirty_packages`, the packages the batch dirtied (map, sublevel,
World Partition actor and folder packages; a deleted actor's file is removed). `already_dirty`: those unsaved before
the request; saving writes them too (without World Partition the map holds every actor). A save failure keeps the
edit: HTTP 207, `Level.SaveFailed` (`details.not_saved`).

## Maps

`open_level` `{path, save_first?}` opens a map; `new_level` `{path, template?, save_first?}` creates one from a New
Level template (`empty` default, `empty_open_world`, `basic`, `open_world`), saves it at `path`, opens it. Unsaved
changes are never lost: `Level.UnsavedChangesWouldBeLost` lists the dirty packages (maybe the user's); `save_first:
true` saves exactly those first; a never-saved level is refused. A map change clears Undo; the result is the
`what: "world"` summary. Errors: `Level.MapNotFound` (close maps named), `MapExists`, `TemplateNotFound`,
`OpenFailed`, `CreateFailed`.

## World Partition

`include_unloaded: true` also lists unloaded actors from their descriptors (`loaded: false`, reference fields,
`folder`, `tags`, `data_layers`, `bounds`). Nothing loads implicitly; edit_level refuses unloaded actors
(`Level.ActorNotLoaded`): `pin` them (`payload.pinned`), edit them in the next request, save, then `unpin` (refused
while unsaved). Unloading clears the whole Undo history: warning `Level.UndoHistoryCleared`, `undoable: false`.

## Diagnostics

Results: `{success, message, diagnostics[], payload}`; a diagnostic has `path` (JSON Pointer into the request),
`code`, `message`, `severity`, `details`.

- `Level.InvalidRequest`: unknown field, wrong type or range. `Level.EditorBusy`: no world, or a transaction or
  dialog open; retry. Batch: `Level.UnknownOp`, `DuplicateId`, `UnknownId`, `TooManyOps`, `ActorDeletedEarlierInBatch`.
- `Level.ActorNotFound` (close ones named), `Level.ActorRefAmbiguous`; `Level.ActorNotLoaded`,
  `ActorInLevelInstance`; `Level.LevelNotFound`, `LevelNotLoaded`, `LevelLocked`.
- Spawn: `Level.AssetNotFound`, `ClassNotPlaceable`, `NoFactoryForAsset`.
- `Level.ComponentNotFound` (close ones named), `ComponentNotEditable` (read-only), `ComponentNotRemovable` (made by
  the class, Blueprint or construction script, `details.creation`), `ComponentNameTaken` (also BP variables),
  `ComponentClassInvalid`.
- `Level.AttachRejected` (`details.reason`: cycle, mobility, level, socket); `Level.FolderNotFound`
  (`details.folders`); `Level.LayersUnavailable`, `DataLayersUnavailable`, `DataLayerNotFound`.
- `Level.PropertyNotEditableOnInstance` (EditDefaultsOnly: change it in the Blueprint),
  `Level.CrossLevelHardReference`, `NotALevelActor` (a class default), `ActorReferenceDisallowed`,
  `Level.PropertyValueNotStoredExactly` (`details.expected` / `stored`); other property errors use AssetDocument
  codes (`Generic.UnknownProperty`, value type errors) at the value's path.
- `Level.DeleteReferenced` (`details.referencers`; `allow_referenced: true` deletes, warning
  `Level.DeletedActorWasReferenced`), `Level.DeleteRefused`.
- Rolled back: `Level.VerificationMismatch`, `SpawnFailed`, `TransactionLost`, `EditorDialogRefused`.
  `Level.RollbackIncomplete`: `details.differences` remain, check the level. `Level.SaveFailed`: applied, some
  packages not saved.
- undo_editor: `Undo.EntryMismatch` (another entry on top), `Undo.NothingToUndo`, `NothingToRedo` (empty history).

## Pitfalls

- A `float` keeps ~7 digits; clamped or construction-script values fail with `Level.PropertyValueNotStoredExactly`.
- Construction-script components are rebuilt on each change: read-only, absent for an actor the batch creates.
- Imports, World / Project Settings: topic `import_settings`.
- No World Partition: an empty folder is not saved (`Level.FolderNotSaved`).
