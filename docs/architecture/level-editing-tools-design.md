# Structured level and actor editing tools for the MCP: design

Status: implemented. Decisions in §8.

World/Level is **not** an asset document. The MCP has a separate, thin,
structured tool set for level and actor editing, so the MCP covers what a person does in the Level Editor. Otherwise the
only way an AI can do this is `execute_python` (EditorActorSubsystem and similar). That path has no preflight, no
atomic rollback, no change list, no read-back and no stable diagnostic codes.

Governing rules: [design rules](asset-document-design-rules.md).
- §2: no Engine changes; versioned DTOs; stable diagnostics.
- §3: rollback means functional equivalence. §3.1 (Undo history) is discussed in §3.6 below.
- §4: editor-equivalent code. Raw paths are allowed when the editor itself works that way; they go in their own file
  with one behaviour test. No private headers and no unexported symbols.
- §4.1: no modal windows.

From the [core guide](../../MCP/schemas/AssetDocument.md), these principles carry over:
- validate before change;
- all or nothing;
- read-back verification;
- a change list in the shape of `diff_asset_document`;
- diagnostics `{path, code, message, severity, details}` with the root cause first;
- `path` is a JSON Pointer into what the AI wrote.

Engine paths below are relative to `<UE_ROOT>/Engine/Source/` unless they say otherwise.

## 1. Scope

### 1.1 Worlds

| World | Scope | Reason |
|---|---|---|
| The editor world (`GEditor->GetEditorWorldContext().World()`): its persistent level | **In** | This is what the Level Editor edits. |
| Sublevels of that world (`UWorld::GetStreamingLevels()` that are loaded) | **In**: query, plus editing actors in them. A spawn can name a target level. | The editor edits loaded sublevels. It refuses locked levels (`FLevelUtils::IsLevelLocked`, checked in `ActorPlacementUtils::IsLevelValidForActorPlacement`, `UnrealEd/Private/AssetSelection.cpp`). |
| World Partition worlds (OFPA: one file per actor) | **In**. Unloaded actors are queryable through actor descriptors. Editing needs the actor to be loaded (§3.7). | Most UE5 maps are partitioned. |
| PIE / SIE worlds | **Never**. Edits are refused while a play session runs (`Level.PlaySessionActive`). | Changes there are discarded. The editor's own scripting refuses too (`EditorScriptingHelpers::CheckIfInEditorAndPIE`). |
| Other worlds: asset editor preview scenes, Level Instance sub-worlds being edited, worlds of unopened maps | Out (v1) | They are not "the level" a person edits in the main viewport. Level Instance editing (`ULevelInstanceSubsystem::EditLevelInstance`) switches the editing context; see §1.3. |

### 1.2 Actor operations

| Operation | Scope | Editor equivalent |
|---|---|---|
| Query actors: filters, paging, components, properties | In | World Outliner plus Details panel |
| Spawn from a class (native or Blueprint `_C`) | In | Place Actors panel / drag a class |
| Spawn from an asset (StaticMesh, SkeletalMesh, Blueprint, NiagaraSystem, SoundBase, ...) | In | Drag from the Content Browser (`UPlacementSubsystem` asset factories) |
| Delete | In | Delete key (`UUnrealEdEngine::DeleteActors`) |
| Duplicate | In | Ctrl+W (`UUnrealEdEngine::DuplicateActors`) |
| Transform (location / rotation / scale) | In | Details Transform rows (root component relative transform) |
| Actor and component property writes | In | Details panel (CPF_Edit, editable on instances) |
| Add, remove and reparent instance components | In | Details "Add Component", delete, drag in the component tree (`USubobjectDataSubsystem`) |
| Attach / detach actors | In | Outliner drag-to-parent (`UEditorEngine::ParentActors`), Detach |
| Label | In | Outliner rename (`FActorLabelUtilities::RenameExistingActor`) |
| Tags | In | `AActor::Tags` property (Details) |
| Outliner folders | In: set an actor's folder; create, rename and delete folders | Outliner folders (`AActor::SetFolderPath`, `FActorFolders`) |
| Layers (non-partitioned worlds) | In | Layers panel (`ULayersSubsystem`) |
| Data Layers (partitioned worlds) | In: add or remove an actor's data layer instances | Data Layers outliner (`UDataLayerEditorSubsystem`) |
| Selection, viewport camera focus | In, as post-commit helpers | Click-select; F (focus) (`UEditorEngine::MoveViewportCamerasToActor`) |
| Pin / load World Partition actors | In, as a helper | Outliner "Pin" (`UWorldPartition::PinActors`) |

### 1.3 Out of scope, and why

| Item | Why |
|---|---|
| Landscape sculpt/paint, foliage painting, mesh/geometry modeling, BSP brush editing | These are tool modes with their own data models (heightmaps, instance buffers, brush CSG), not actor property edits. An AI can still *place* and *configure* such actors through properties. |
| Creating levels, adding/removing sublevels, streaming settings, level visibility | This is world-structure management, and opening or creating a map discards or prompts for unsaved work. Opening and creating maps are the separate `open_level` / `new_level` helpers, which refuse to lose unsaved work (D10). |
| Lighting / navigation / HLOD / texture streaming builds | These are long jobs, not edits. |
| Level Blueprint (`ALevelScriptActor` graph) | It is a Blueprint graph. The Blueprint profile could cover it later through `ULevelScriptBlueprint`; the coverage audit lists it as Blueprint-family. |
| Level Sequences | The Sequencer profile ([asset-document-sequencer-design.md](asset-document-sequencer-design.md)): asset documents that *bind* to level actors (§6). |
| Construction-script (UCS) components | They are rebuilt on every construction pass, and the editor does not let you keep edits to them. Writes are refused with `Level.ComponentNotEditable`. |
| Editing actors inside a Level Instance from the outer world | Their actors belong to another world package, and the editor requires "Edit" mode on the instance. v1 shows them read-only in query (`container` field) and refuses edits (`Level.ActorInLevelInstance`). |
| PCG graphs, Water / Spline point editing beyond reflected properties | They have specialised data models. Reflected properties work through the generic path; spline points are `FInterpCurve` structs and may already round-trip through the codec, but this is unverified (§8). |

## 2. Tool surface

The design adds two editor tools and one static guide tool. The MCP keeps `get_editor_context` and
`get_viewport_screenshot`. `execute_python` stays as the escape hatch; its description will point at `edit_level`.

| Tool | HTTP | Kind |
|---|---|---|
| `query_level` | `POST /assetdoc/level/query` | read-only |
| `edit_level` | `POST /assetdoc/level/edit` | mutating, batched, atomic (`dry_run` = validate + planned changes) |
| `get_level_editing_guide` | none (MCP-side markdown `MCP/schemas/LevelEditing.md`) | static docs (decision D8) |

Responses use the AssetDocument result shape `{success, message, diagnostics[], payload}` with HTTP 200 / 207 / 400.
Because of this, failures are formatted as they are for asset documents, with no level-specific response handling. Field names in requests are snake_case, like other MCP
arguments. Reflected property names and values are exactly the AssetDocument forms, for example
`{"X":..,"Y":..,"Z":..}` and `"/Game/A.A"`.

### 2.1 Actor identity

| Field | Stable? | Use |
|---|---|---|
| `path`: soft object path `/Game/Maps/Room.Room:PersistentLevel.PointLight_2` | **Stable.** The actor's object name never changes in the editor; Outliner "rename" changes the label. Unique per level. OFPA actors still live in the map's namespace (`...:PersistentLevel.StaticMeshActor_UAID_0A1B...`). Sublevel actors use the sublevel package. | **Canonical identity.** It is the same string Sequencer stores (`FActorLocatorFragment::Path`, `Runtime/Engine/Public/UniversalObjectLocators/ActorLocatorFragment.h`), see §6. |
| `guid`: `AActor::GetActorGuid()` | Stable and persisted (editor-only data). WP uses it for actor descriptors, pinning and the external package name. Duplicates get a new one. | Accepted as input. It is the only identity for **unloaded** WP actors besides `path`. |
| `label`: `GetActorLabel()` | Not unique; users change it | Accepted as input when unique in the searched scope; otherwise `Level.ActorRefAmbiguous` with the candidates. Returned for display. |
| `external_package`: OFPA package `/Game/__ExternalActors__/Maps/Room/8/2K/...` | Stable while the actor stays in that level | Output only: what gets dirtied and saved, and what source control sees. |
| Component: object name within the actor (`LightComponent0`, a Blueprint SCS variable name `DoorMesh`, an instance component name) | Stable. Native and SCS names come from the class; instance component names are chosen at add time. | Key under `components`. |

**ActorRef input** can be any of the following:
- a string: a path, or `"@id"` for an actor created earlier in the same batch;
- an object `{"path"}`, `{"guid"}`, `{"label", "level"?}` or `{"id"}`.

**ActorRef output** (query and edit results):
`{"path", "guid", "label", "class", "level", "external_package"|null, "loaded"}`.

### 2.2 `query_level` request and response

Request fields (all optional):

| Field | Meaning |
|---|---|
| `what` | `"actors"` (default) or `"world"`. `"world"` returns the summary only: levels with loaded/visible/locked/current state, partitioned flag, data layers, layers, folders, actor counts, play-session flag. |
| `actors` | Explicit ActorRef list. |
| `class` | Class path, subclasses included. `exact_class` matches the class only. |
| `name` | Wildcard over label and object name. |
| `folder` | Folder prefix. |
| `tag`, `layer`, `data_layer` | Exact match. |
| `level` | Level package. |
| `near` | `{"location", "radius"}`, or `bounds` `{"min","max"}`. |
| `selected_only` | Selected actors only. |
| `include_unloaded` | WP only: also list unloaded actors from their descriptors. |
| `detail` | `"summary"` (default), `"components"` or `"properties"`. |
| `diff_only` | Default true: properties that differ from the archetype, i.e. the instance overrides. |
| `limit` | Default 100, max 1000. |
| `offset` | Paging offset. |

Results are ordered by level, then object name, so paging is stable.

```json
{"class": "/Script/Engine.Light", "folder": "Room", "detail": "properties", "limit": 50}
```

```json
{"success": true, "message": "2 of 2 actors",
 "payload": {
  "world": "/Game/Maps/Room.Room", "partitioned": false, "total": 2, "offset": 0,
  "actors": [{
    "path": "/Game/Maps/Room.Room:PersistentLevel.PointLight_0", "guid": "6E1D0C1B4F0A9C2E8B7D11A2C3D4E5F6",
    "label": "KeyLight", "class": "/Script/Engine.PointLight", "level": "/Game/Maps/Room",
    "external_package": null, "loaded": true,
    "folder": "Room", "parent": null, "tags": ["Lighting"], "layers": [], "data_layers": [],
    "transform": {"location": {"X": 0, "Y": 0, "Z": 280}, "rotation": {"Pitch": 0, "Yaw": 0, "Roll": 0},
                  "scale": {"X": 1, "Y": 1, "Z": 1}},
    "components": [
      {"name": "LightComponent0", "class": "/Script/Engine.PointLightComponent", "creation": "Native",
       "editable": true, "parent": null, "socket": null, "root": true,
       "properties": {"Intensity": 8000, "LightColor": {"R": 255, "G": 214, "B": 170, "A": 255}, "AttenuationRadius": 1200}}
    ],
    "properties": {"Tags": ["Lighting"]}
  }, {"...": "..."}]
 }}
```

`creation` is one of:
- `Native`: a default subobject;
- `Blueprint`: SCS, inherited from the Blueprint class;
- `Instance`: added on this actor;
- `ConstructionScript`: UCS; read-only, `editable: false`.

`editable` follows the Details panel rule: `FComponentEditorUtils::CanEditComponentInstance` and, for native
components, `GetPropertyForEditableNativeComponent`. Unloaded WP actors carry only the descriptor fields: `path`,
`guid`, `label`, `class`, `bounds`, `data_layers` and `loaded: false`.

### 2.3 `edit_level` request

```text
{"description": "<undo entry title>", "dry_run": false, "save": false,
 "ops": [ <op>, ... ],
 "after": {"select": [ActorRef...], "focus": [ActorRef...]}}
```

| Op | Fields (`?` = optional) | Notes |
|---|---|---|
| `spawn` | `id?`, `class` \| `asset`, `level?`, `transform?`, `label?`, `folder?`, `tags?`, `properties?`, `components?`, `attach_to?` | One op covers a full Place + Details setup, so an AI needs few ops. |
| `delete` | `actors`, `allow_referenced?` | §3.2 reference policy |
| `duplicate` | `id?`, `actor`, `offset?`, `level?` + the `set` fields | |
| `set` | `actor` + any of `transform`, `label`, `folder`, `tags`, `properties`, `components`, `layers`, `data_layers` | Sparse. Omitted means unchanged, as with AssetDocument `Properties`. `layers` / `data_layers` / `tags` are whole lists. |
| `add_component` | `actor`, `class`, `name`, `parent?`, `socket?`, `asset?`, `properties?` | Instance component |
| `remove_component` | `actor`, `component` | Instance components only (as Details) |
| `reparent_component` | `actor`, `component`, `parent`, `socket?` | |
| `attach` / `detach` | `actor`, `parent`, `socket?`, `component?` / `actor` | |
| `create_folder` / `rename_folder` / `delete_folder` | `path` / `from`, `to` / `path` | |
| `pin` / `unpin` | `actors` (WP) | Helper; not transacted (§3.7) |

- `transform` is the root component's **relative** transform, as the Details panel shows it; it equals the world
  transform when the actor is not attached. `{"location", "rotation", "scale", "space"?: "world"}`, with any member
  omitted.
- `components` is `{"<ComponentName>": {"properties": {...}}}`.
- Batch ids (`id`) are unique per request. Later ops and property values refer to them as `"@id"`.

Example (a slice of "build a small lit room with a door blueprint and a camera"):

```json
{"description": "AI: build lit room", "ops": [
  {"op": "spawn", "id": "floor", "asset": "/Engine/BasicShapes/Cube.Cube", "label": "Floor", "folder": "Room",
   "transform": {"location": {"X": 0, "Y": 0, "Z": -10}, "scale": {"X": 8, "Y": 8, "Z": 0.2}},
   "components": {"StaticMeshComponent0": {"properties": {"OverrideMaterials": ["/Game/Materials/M_Floor.M_Floor"]}}}},
  {"op": "spawn", "id": "key", "class": "/Script/Engine.PointLight", "label": "KeyLight", "folder": "Room",
   "transform": {"location": {"X": 0, "Y": 0, "Z": 280}},
   "components": {"LightComponent0": {"properties": {"Intensity": 8000, "AttenuationRadius": 1200,
                                                      "LightColor": {"R": 255, "G": 214, "B": 170, "A": 255}}}}},
  {"op": "spawn", "id": "door", "asset": "/Game/Blueprints/BP_Door", "label": "FrontDoor", "folder": "Room",
   "transform": {"location": {"X": 400, "Y": 0, "Z": 0}, "rotation": {"Yaw": 90}},
   "properties": {"bStartsOpen": false, "LinkedLight": "@key"}},
  {"op": "spawn", "id": "cam", "class": "/Script/Engine.CameraActor", "label": "RoomCam", "folder": "Room",
   "transform": {"location": {"X": -600, "Y": -600, "Z": 300}, "rotation": {"Pitch": -20, "Yaw": 45}},
   "components": {"CameraComponent": {"properties": {"FieldOfView": 70}}}},
  {"op": "attach", "actor": "@key", "parent": "@floor"}
 ],
 "after": {"select": ["@door"], "focus": ["@floor"]}}
```

### 2.4 `edit_level` response, change list and diagnostics

```json
{"success": true, "message": "Level edit applied: 4 actors spawned, 1 attached (undo: 'AI: build lit room')",
 "payload": {
  "transaction": {"title": "AI: build lit room", "undoable": true},
  "actors": {"floor": {"path": "/Game/Maps/Room.Room:PersistentLevel.StaticMeshActor_3", "guid": "...", "label": "Floor", "class": "/Script/Engine.StaticMeshActor", "level": "/Game/Maps/Room", "external_package": null, "loaded": true},
             "key": {"...": "..."}, "door": {"...": "..."}, "cam": {"...": "..."}},
  "changes": [
    {"op": "/ops/0", "kind": "Add", "object": "actor", "actor": "/Game/Maps/Room.Room:PersistentLevel.StaticMeshActor_3"},
    {"op": "/ops/1/components/LightComponent0/properties/Intensity", "kind": "Replace", "object": "property",
     "actor": "/Game/Maps/Room.Room:PersistentLevel.PointLight_4", "component": "LightComponent0",
     "property": "Intensity", "before": 5000, "after": 8000},
    {"op": "/ops/4", "kind": "Replace", "object": "attachment", "actor": "/Game/Maps/Room.Room:PersistentLevel.PointLight_4",
     "before": null, "after": "/Game/Maps/Room.Room:PersistentLevel.StaticMeshActor_3"}
  ],
  "dirty_packages": ["/Game/Maps/Room"], "saved_packages": [],
  "selection": ["/Game/Maps/Room.Room:PersistentLevel.BP_Door_C_1"]},
 "diagnostics": []}
```

- `kind` is `Add`, `Remove` or `Replace`, as in `diff_asset_document`.
- `object` is `actor`, `component`, `property`, `transform`, `label`, `folder`, `attachment`, `tags`, `layers` or
  `data_layers`.
- The `before` of a spawned actor's property is the archetype value.
- `dry_run: true` returns the same `changes` (planned, from preflight), `would_spawn` counts, and no `dirty_packages`.

A failure returns HTTP 400, `success: false`, and the root cause first:

```json
{"success": false,
 "message": "Generic.UnknownProperty: 'Intensty' is not a writable property of PointLightComponent (at /ops/1/components/LightComponent0/properties/Intensty)",
 "diagnostics": [{"path": "/ops/1/components/LightComponent0/properties/Intensty", "code": "Generic.UnknownProperty",
                  "severity": "error", "message": "...", "details": {"class": "/Script/Engine.PointLightComponent"}}],
 "payload": {"rolled_back": false, "phase": "preflight"}}
```

`phase` is `preflight` (nothing touched), `apply` or `verify` (rolled back: `rolled_back: true`, and a rollback
check result).

**Stable codes.** Prefix `Level.*` for the tool's own rules; value and property errors reuse the AssetDocument codes
(`Generic.UnknownProperty`, `Generic.NonWritableProperty`, `TypedValueCodec.*`, `PropertyWrite.*`,
`Property.EditorDerivedReference`).

`Level.*` set:

- Request shape: `InvalidRequest`, `UnknownOp`, `DuplicateId`, `UnknownId`, `TooManyOps`.
- Resolution:
  - `ActorNotFound`, `ActorRefAmbiguous`, `ActorDeletedEarlierInBatch`;
  - `ActorNotLoaded`, `ActorInLevelInstance`;
  - `LevelNotFound`, `LevelNotLoaded`, `LevelLocked`.
- Spawn: `ClassNotPlaceable`, `NoFactoryForAsset`, `SpawnFailed`.
- Properties: `PropertyNotEditableOnInstance`, `CrossLevelHardReference`, `PropertyValueNotStoredExactly`.
- Components:
  - `ComponentNotFound`, `ComponentNotEditable`, `ComponentNotRemovable`;
  - `ComponentNameTaken`, `ComponentClassInvalid`.
- Hierarchy and grouping: `AttachRejected` (with the editor's `CanParentActors` reason), `FolderNotFound`,
  `DataLayerNotFound`, `DataLayersUnavailable`, `LayersUnavailable`.
- Delete: `DeleteReferenced` (error), `DeletedActorWasReferenced` (warning).
- Editor state: `PlaySessionActive`, `EditorBusy`.
- Outcome:
  - `VerificationMismatch`;
  - `RollbackIncomplete`, `TransactionLost`;
  - `SaveFailed` (HTTP 207, edits kept).

## 3. Semantics

### 3.1 Request lifecycle (game thread, one request at a time)

1. **Admission.**
   - Refuse during PIE/SIE (`Level.PlaySessionActive`).
   - Refuse while a transaction is open or an undo/redo is running (`GEditor->IsTransactionActive()`,
     `GIsTransacting`), for example while the user is dragging a gizmo: `Level.EditorBusy`, "retry shortly".
   - Refuse while a modal window is up.
2. **Parse and resolve**, purely from the request: shape, op kinds, unique ids, JSON pointers.
3. **Preflight**: read-only against the world. See 3.2.
4. **Pre-image digest** of everything the batch can touch:
   - for every resolved existing actor: class, label, folder, parent and socket, transform, tags, layers, data
     layers, component set (name, class, parent), and the codec encoding of every property the batch writes;
   - the actor list of every target level (to detect leftovers);
   - the dirty flag of every package involved.
5. **Apply**, inside one editor transaction:
   - `GEditor->BeginTransaction(description)`; record the new transaction's `TransactionId`.
   - Run each op through the editor API (§5.2). Their own `FScopedTransaction`s nest into ours, because
     `UTransBuffer::Begin` only increments `ActiveCount`.
   - After each op, check that our transaction is still the active one. If not: `Level.TransactionLost`, stop.
6. **Verify**, still inside the transaction: read back every planned change with the same codec and compare with
   `EncodeAsAuthored` semantics (only what the request listed). Check that spawned actors exist with the requested
   class/label/folder, attachments are as planned, and deleted actors are gone. A difference is
   `Level.VerificationMismatch`, or `Level.PropertyValueNotStoredExactly` with the stored value.
7. **Commit**: `GEditor->EndTransaction()`. This is one Undo entry, titled `description` (default
   `"AI level edit (N ops)"`).
8. **Failure in 5 or 6.**
   - `EndTransaction()`.
   - **Only if** the newest transaction in the buffer is ours (same `TransactionId`), call
     `GEditor->UndoTransaction(/*bCanRedo*/false)`. That applies the undo and removes the entry.
     Why the check matters: `UTransBuffer::End` pops a transaction that recorded nothing (transient). A blind Undo
     would then undo the **user's** previous edit.
   - Then compare the post-rollback state with the step-4 digest. Equal: `rolled_back: true`. Different:
     `Level.RollbackIncomplete` with the differences. No further automatic action: reloading the map would destroy
     the user's unsaved work.
9. **Post-commit helpers** (not part of the transaction, never rolled back): `after.select`, `after.focus`, `pin`.
10. **Save** if `save: true` (§3.4).

`dry_run: true` stops after 3 and returns planned changes.

### 3.2 Preflight (nothing in the world changes)

Preflight runs on a **symbolic plan**: a model of actors (existing, to-be-spawned with their class, deleted), each
with its component set, labels and attachment graph. Each op validates against the plan state after the previous
ops. Checks:

- **Reference resolution.**
  - ActorRef → loaded actor, unique label, or batch id spawned *earlier*.
  - Using an actor deleted earlier in the batch: `Level.ActorDeletedEarlierInBatch`.
  - Component names: from the class (native default subobjects; SCS nodes along the Blueprint chain), from the actor
    instance (instance components), or from earlier `add_component` ops.
- **Spawn.**
  - Class: concrete, not `CLASS_NotPlaceable`/`Abstract`, a subclass of `AActor`.
  - Blueprint: has a `GeneratedClass` (same check as `TryPlacingAssetObject`).
  - Asset: `UPlacementSubsystem::FindAssetFactoryFromAssetData(...)` and `CanPlaceElementsFromAssetData`
    (`Level.NoFactoryForAsset` names the asset class).
  - Target level: loaded, not locked.
- **Properties**: AssetDocument admission and codec (§4). That covers decode into scratch memory, type checks,
  picker class filters, `NoClear`, editor-derived references, instance editability and cross-level hard references.
- **Components.**
  - `add_component`: the class is a non-abstract `UActorComponent` that the Add Component menu accepts
    (`BlueprintSpawnableComponent` / `FComponentTypeRegistry` rules). Scene components need a scene parent. The name
    is free.
  - `remove_component`: only `Instance`-created components (Details refuses the others).
  - UCS components: `Level.ComponentNotEditable`.
- **Attach**: `UEditorEngine::CanParentActors` on the plan graph (cycles, self-attach, missing root). The editor's
  reason text goes into the message.
- **Delete.** Compute what the Delete key's dialog would show (`UUnrealEdEngine::DeleteActors`,
  `UnrealEd/Private/EditorActor.cpp`):
  - hard referencers: `FBlueprintEditorUtils::GetActorReferenceMap`;
  - Level Blueprint references: `FindReferencesToActorFromLevelScript`;
  - soft referencers: `IAssetTools::FindSoftReferencesToObjects`, for example Level Sequences that bind the actor.

  Without `allow_referenced: true` this is `Level.DeleteReferenced`, listing referencers: it is the AI form of the
  dialog's "No". With it, the delete goes ahead and each referencer comes back as warning
  `Level.DeletedActorWasReferenced`.
- **Folders / layers / data layers.**
  - Data layers: the data layer instance must exist (by asset path).
  - Layers: missing ones are created, as `ULayersSubsystem::EnsureLayerExists` does.
  - A data layer op on a non-partitioned world: `Level.DataLayersUnavailable`.

What preflight **cannot** see: `PostEditChange` normalisation, construction-script results, and factory-specific
placement adjustments. Step 6 catches them after the write; the batch then rolls back with a precise diagnostic. This
differs from AssetDocument, which preflights on a transient clone. The reason: an actor clone needs a world, which
means spawning; and rollback here is cheap and exact (an editor Undo), whereas an asset save/install is not. The
`dry_run` message says so.

### 3.3 One transaction per batch, and why it differs from AssetDocument

The [design rules](asset-document-design-rules.md) (§3) reject `FScopedTransaction` as the rollback mechanism *for asset apply*, for three reasons: it
records only objects that call `Modify()`; it would have to stay open across save/install; and it pollutes the
user's Undo stack.

None of these apply here:
- every editor operation used is itself transactional (`RF_Transactional` spawn, `Modify()` in every editor path
  listed in §5.2);
- the batch never saves inside the transaction;
- AI level edits are **undoable like the user's own** (Ctrl+Z undoes the whole batch; Ctrl+Y
  redoes it). The editor's own multi-step commands work this way, for example `ParentActors` and `DestroyActors`.

What the transaction does not cover, and how each is handled:

| Not covered | Handling |
|---|---|
| Assets loaded to resolve references | Stay loaded. They are not modified, so this is functionally equivalent. |
| World Partition actor descriptors, external package objects of spawned-then-undone actors | Undo deletes the actor; the editor updates descriptors on undo. The new external package was never saved. Risk R3 checks that "Save All" afterwards writes nothing for it. |
| Selection, pins, viewport camera | Changed only by post-commit helpers. Placement runs with selection off; `DestroyActors` deselects what it deletes, and Undo restores that. |
| Package dirty flags | Undo leaves packages dirty. Design rules §3 item 5 accepts this. |
| Editor operations that reset the transaction buffer mid-batch (map change, `ReloadPackages`) | None of the §5.2 paths do this. If it happens: `Level.TransactionLost`, no rollback possible, reported. |

The `execute_python` route (`AssetDocumentHttpServer.cpp` `HandleExecute`) uses the same sequence. A naive failure
path would call `GEditor->UndoTransaction()` **while the transaction is still active**. `UTransBuffer::CanUndo`
returns false while `ActiveCount > 0`, so nothing would be undone and the transaction would never end:
- partial changes stay;
- every later editor edit merges into that open transaction;
- Undo stays disabled ("Can't undo while ... is in progress").

The route therefore ends the transaction first and undoes only if the newest transaction is ours (the step-8
sequence), so a failing script leaves no change and no open transaction.

### 3.4 Dirty packages and saving

**Recommendation: leave packages dirty by default.**
- This is what a person's edit does. The user saves when they choose to, and Undo stays meaningful.
- The result lists `dirty_packages`:
  - non-partitioned: the map or sublevel package;
  - OFPA: each touched actor's external package, plus actor-folder packages.
- `save: true` saves exactly those packages after commit. It uses the editor's save path for level packages
  (`FEditorFileUtils::SaveDirtyPackages`-family with the explicit package list, inside the non-interactive scope).
  That path also handles OFPA packages of deleted actors (delete or mark-for-delete). Packages that were dirty before
  the batch are **not** saved unless the batch touched them; the result names them as `already_dirty`.
- A save failure does not roll back: the edit is committed and in Undo. It returns 207 with `Level.SaveFailed` and
  `saved_packages`.

Unlike AssetDocument (`DirtyExistingAssetTransactionUnsupported`), a level with unsaved changes is **not**
rejected. Rollback uses Undo rather than disk, so the user's unsaved work survives a failed batch.

### 3.5 Concurrency with the user and open editors

- Requests run on the game thread (the same `RunOnGameThread` as AssetDocument routes), so they never interleave
  with a user action. A user mid-interaction (open transaction) gets `Level.EditorBusy`.
- Selection: unchanged unless `after.select` is given. Deleted actors are deselected (the editor does this).
- Open Details panels and the Outliner refresh through the normal `PostEditChange` / `OnLevelActorAdded` broadcasts.
- An open Blueprint editor for a spawned Blueprint class is unaffected.
- A Level Sequence open in Sequencer re-resolves its bindings on change, as it does for user edits.
- The file watcher and AssetDocument applies are serialized on the game thread with level edits. §3.6 covers their
  Undo interaction.

### 3.6 Interaction with the AssetDocument Undo-history rule (§3.1)

AssetDocument applies stay out of Undo. When a successful apply changes an asset that the transaction buffer
references, the **whole** Undo history is cleared (`AssetDocumentUndoHistory.cpp` `FinishApply`, the editor's
`ReloadPackages` behaviour).

A level batch that spawned `BP_Door` references the `BP_Door` package, through the actor's class and archetype in
the recorded state. A later AssetDocument apply to `BP_Door` therefore clears Undo, including the AI's and the
user's level edits.

This is correct and wanted. Undoing a door spawn after the Blueprint was rebuilt and its instances reinstanced is
exactly the mixed-state hazard the rule prevents. The level guide states it. Decision D4.

Level batches themselves never clear Undo history. They are ordinary editor transactions.

### 3.7 World Partition specifics

- Query with `include_unloaded` reads descriptors (`UWorldPartitionBlueprintLibrary::GetActorDescs` /
  `FWorldPartitionActorDescInstance`: guid, class, name, label, bounds, actor package, data layers). No load happens.
- Editing needs a loaded actor. An unloaded target gets `Level.ActorNotLoaded`, with the hint "pin it first".
- `pin` is the Outliner's Pin (`UWorldPartition::PinActors`): it loads the actor and keeps it loaded across region
  changes. It is a view-state change outside the transaction. `unpin` reverses it, and refuses dirty actors because
  unloading them would lose changes.
- Spawn goes to the persistent level (WP); the external package is created automatically. `UPlacementSubsystem`'s
  `PreferredLevel` is advisory and WP can override it, as noted in `FAssetPlacementInfo`. The result reports the
  actual level and package.
- Data layers are identified by `UDataLayerAsset` path; an instance name is accepted when unique. Assignment uses
  `UDataLayerEditorSubsystem::AddActorToDataLayer` / `RemoveActorFromDataLayer`. Spawn respects the editor's
  "current data layers" the same way placement does.

## 4. Property values

The same typed value codec and admission are used as for the generic AssetDocument profile, so an AI writes actor
properties exactly as it writes asset `Properties`:

- **Admission.** `AssetDocumentEditorPropertyAdmission.cpp` `TryAdmitProperty`:
  - CPF_Edit, not transient, deprecated or skip-serialized;
  - the codec admits the type;
  - writability through `PropertyAccessUtil::CanSetPropertyValue(Property, EditorReadOnlyFlags, bOwnerIsTemplate=false)`,
    which already applies **instance** semantics (`CPF_DisableEditOnInstance` refuses). A property that is
    `EditDefaultsOnly` is reported as `Level.PropertyNotEditableOnInstance` rather than the generic code, so the
    message says "edit the Blueprint's defaults (AssetDocument) instead".
  - Blueprint "Instance Editable" variables are CPF_Edit without `DisableEditOnInstance`, so they are admitted.
- **Decode and write.** `DecodeAssetDocumentEditorTypedValue` with `Base = {current value, Owner = actor or
  component}`: omitted struct members stay, text keys are kept, picker filters and `NoClear` apply. The write is
  Details-equivalent: `PropertyAccessUtil::SetPropertyValue` with `EPropertyAccessChangeNotifyMode::Default`, which
  calls `PreEditChange`/`Modify`/`PostEditChangeProperty`. On a Blueprint instance, `PostEditChangeProperty` reruns
  the construction script. Component instance data keeps overrides across the rerun, as in the editor.
- **Float precision**: the same normalisation (`NormalizeDocumentProperties`) and shortest-decimal read-back.
- **Asset references**: object paths, as in documents.
- **Actor and component references.** AssetDocument's editor-derived-reference rule would refuse a hard reference to
  an actor in the same world, because its outer chain reaches the world package. For level edits the reference
  policy is replaced by the Details actor picker's rules:
  - a hard reference (`AActor*`, `TObjectPtr<AActor>`, component pointers) may name an actor, or a component of an
    actor, in the **same `ULevel`**: a path, or `"@id"` of an actor spawned earlier in the batch;
  - another level is `Level.CrossLevelHardReference` (the package could not save it). Soft references
    (`TSoftObjectPtr<AActor>`) may name any level of the world;
  - `DisallowLevelActorReference` metadata (`SPropertyEditorAsset.cpp`) refuses level actors;
  - `GetAllowedActors`-style filter functions run on the owner, as for asset pickers;
  - `"@id"` is only recognised in object/soft-object positions whose class is an Actor or ActorComponent. An FString
    value `"@x"` stays a string.
- **Instanced subobjects** on components (for example instanced settings objects) use `{"Class", "Properties"}` with
  the owning component as outer, the same as for assets.

**Object-level API.** The asset-side admission functions are `Private` to `AssetDocumentEditor`, and the public
`EditorPropertyValueCodec` has no owner and no instanced subobjects. `AssetDocumentEditor` therefore exports a small
public object-level API:

```cpp
// Source/AssetDocumentEditor/Public/AssetDocumentEditorObjectProperties.h  (sketch)
namespace UE::AssetDocument::EditorObjectProperties
{
struct FReferencePolicy { const ULevel* SameLevel = nullptr; TFunction<UObject*(FStringView Id)> ResolveBatchId; };
ASSETDOCUMENTEDITOR_API TArray<FAdmittedField> ListWritable(UObject& Object);             // instance semantics
ASSETDOCUMENTEDITOR_API bool Read(UObject& Object, FName Property, FAssetDocumentRuntimeValueDto& Out);
ASSETDOCUMENTEDITOR_API bool Decode(UObject& Owner, FName Property, const FAssetDocumentRuntimeValueDto& Value,
                                    const FReferencePolicy&, TUniquePtr<FPreparedWrite>& Out, TArray<FAssetDocumentRuntimeDiagnostic>&);
ASSETDOCUMENTEDITOR_API bool Commit(FPreparedWrite&, TArray<FAssetDocumentRuntimeDiagnostic>&);  // Details-equivalent write
ASSETDOCUMENTEDITOR_API bool EncodeAsAuthored(UObject& Object, FName Property, const FAssetDocumentRuntimeValueDto& Authored, FAssetDocumentRuntimeValueDto& Out);
}
```

These wrap the existing `FAssetDocumentEditorPreparedPropertyWrite`, admission and codec. The reference policy is an
option of the existing derived-reference check; there is no second codec.

## 5. Implementation

### 5.1 Where the code lives

**New editor module `Source/AssetDocumentLevel`** (Type Editor, PostEngineInit). Decision D7.
- It is not an AssetDocument profile, so it is not named `AssetDocument*`. It does reuse AssetDocument's result,
  diagnostic, DTO and codec types.
- Dependencies:
  - plugin: `AssetDocument` (result/DTO types), `AssetDocumentEditor` (object-property API, non-interactive scope),
    `UEAssetDoc` (HTTP route registration);
  - engine: `UnrealEd`, `LevelEditor`, `EditorFramework` (`UPlacementSubsystem`), `SubobjectDataInterface`,
    `DataLayerEditor`, `AssetTools`, `Json`.

Files:

```text
Private/
  LevelHttpRoutes.cpp            # /assetdoc/level/{query,edit}; RunOnGameThread; FAssetDocumentHttpServer::RegisterExternalRoute
  LevelActorRef.{h,cpp}          # ActorRef parse/resolve/encode (path, guid, label, @id); shared format (§6)
  LevelQuery.{h,cpp}             # filters, paging, detail levels, WP descriptors
  LevelEditRequest.{h,cpp}       # parse + op table + JSON pointers
  LevelEditPlan.{h,cpp}          # symbolic plan + preflight
  LevelEditTransaction.{h,cpp}   # Begin/verify/End/Undo-if-ours, digest compare, TransactionLost guard
  LevelEditDigest.{h,cpp}        # pre/post state digest
  Ops/LevelOpSpawn.cpp, LevelOpDelete.cpp, LevelOpDuplicate.cpp, LevelOpSet.cpp, LevelOpComponents.cpp,
      LevelOpAttach.cpp, LevelOpFolders.cpp, LevelOpLayers.cpp, LevelOpHelpers.cpp   # one handler per op kind
  Raw/LevelPlacement.cpp         # B1 (§5.3)
  Raw/LevelDetach.cpp            # B1 (§5.3)
  Tests/...
```

Each op is a handler `{Parse, Preflight(plan), Apply(world), Verify}` registered in one table. A new op means a new
file and no changes elsewhere.

**C++ / Python split: C++ only for v1.** The design rules give Python the business semantics of *asset
profiles*: Meta model, regions, canonicalization of documents and sidecar bytes. None of these exist here:
- there are no per-class profiles;
- there is no document to canonicalize or persist;
- every op maps one-to-one onto an editor API;
- transaction authority is C++ by rule ("Python does not own ... transaction commit/rollback").

Routing a batch through the Python host would add a DTO round trip per op and an opaque-handle scheme for actors,
and gain no reuse. The reuse is the shared C++ pieces: codec, admission, non-interactive scope, diagnostics, result
shape. If class-specific helpers appear later (spline points, landscape materials), they can become Python
components over the same op handlers. Decision D6.

### 5.2 Editor API per op

| Op | Editor path | Notes |
|---|---|---|
| spawn (asset) | `UPlacementSubsystem::PlaceAsset({AssetToPlace, PreferredLevel, FinalizedTransform, NameOverride}, {})` followed by the post-steps of `PlaceAssetUsingFactory` (`UnrealEd/Private/AssetSelection.cpp`) | This is the Content Browser drag path minus viewport snapping (B1, §5.3). Factory: `FindAssetFactoryFromAssetData`. Blueprints place through `UActorFactoryBlueprint`. |
| spawn (class) | `GEditor->FindActorFactoryForActorClass(Class)`, then the same placement with `FactoryOverride`. Without a factory: `UEditorEngine::AddActor(Level, Class, Transform, bSilent, RF_Transactional, bSelectActor=false)` | This is what `FLevelEditorViewportClient::TryPlacingAssetObject` does for a class (`LevelEditorViewport.cpp`). |
| delete | `GUnrealEd->DeleteActors(Actors, World, SelectionSet, /*bVerifyDeletionCanHappen*/true, /*bWarnAboutReferences*/false, /*bWarnAboutSoftReferences*/false)` | References are checked in preflight (§3.2) instead of the dialogs. The verify step keeps the `CanDeleteActor` / `ShouldAbortActorDeletion` refusals: builder brush, world settings, locked level. |
| duplicate | `UEditorActorSubsystem::DuplicateActors(Actors, World, Offset, {LevelOverride, bTransact})` → `UUnrealEdEngine::DuplicateActors` | Ctrl+W path. |
| transform | Root component `RelativeLocation` / `RelativeRotation` / `RelativeScale3D` through the object-property write (Details Transform rows write these properties with `PostEditChange`). `space: "world"` converts with the parent's world transform first. | Same as typing in Details; mobility is not changed. |
| properties | §4 object-property API on the actor or the named component | |
| label | `FActorLabelUtilities::RenameExistingActor(Actor, Label, /*bMakeUnique*/false)` | Outliner rename; it fixes soft references. Labels are not made unique, as in the Outliner. |
| folder | `AActor::SetFolderPath_Recursively` (Outliner drag also moves attached children); create/rename/delete via `FActorFolders` | |
| tags | `Tags` property write (§4) | |
| layers | `ULayersSubsystem::AddActorsToLayers` / `RemoveActorFromLayers` | Creates missing layers like the editor. |
| data layers | `UDataLayerEditorSubsystem::AddActorToDataLayer` / `RemoveActorFromDataLayer` | |
| add_component | `USubobjectDataSubsystem::AddNewSubobject({ParentHandle, NewClass, AssetOverride, BlueprintContext=nullptr})`, then `RenameSubobject` to the requested name | The Details "Add Component" path for an instance. Instance components get `CreationMethod::Instance` and `AActor::AddInstanceComponent`. |
| remove_component | `USubobjectDataSubsystem::DeleteSubobject(ContextHandle, Handle)` | |
| reparent_component | `USubobjectDataSubsystem::ReparentSubobject({NewParentHandle}, Handle)` + socket property | |
| attach | `GEditor->CanParentActors` then `GEditor->ParentActors(Parent, Child, Socket, Component)` (`UnrealEd/Private/EditorEngine.cpp`) | Outliner drag-to-parent. Keeps the world transform and moves the child to the parent's folder, as the editor does. |
| detach | Body of `UEditorEngine::DetachSelectedActors` for one actor (B1, §5.3) | |
| select (helper) | `UEditorActorSubsystem::SetSelectedLevelActors` | |
| focus (helper) | `GEditor->MoveViewportCamerasToActor(Actors, /*bActiveViewportOnly*/true)` | |
| pin / unpin (helper) | `UWorldPartition::PinActors` / `UnpinActors` | |

Everything runs inside `FAssetDocumentEditorNonInteractiveScope` (design rules §4.1), so a factory or delete path
that tries a dialog is cancelled and reported, never blocking.

### 5.3 Raw paths (B1): each in its own file with one behaviour test

1. **`Raw/LevelPlacement.cpp`**: placement with an explicit transform and target level. The editor's drag path
   (`PlaceAssetUsingFactory`) computes the transform from the viewport cursor (`GEditor->ClickLocation`,
   `GCurrentLevelEditingViewportClient` snapping) and always uses the *current* level. The helper keeps every other
   step of that function, in order:
   - `IsLevelValidForActorPlacement`;
   - `UPlacementSubsystem::PlaceAsset` with `FactoryOverride`;
   - `SetFlags(RF_Transactional)`;
   - legacy `UActorFactory::CreateActor` fallback;
   - `InvalidateLightingCache`, `PostEditChange`, `MarkPackageDirty`;
   - `FEditorDelegates::OnNewActorsPlaced`;
   - `PostEditMove(true)` (asset path).

   Selection is off. Test: place the same asset through `UEditorActorSubsystem::SpawnActorFromObject` (the official
   scripting path) and through the helper; compare class, components, flags, package dirtiness, folder/data-layer
   assignment and the Undo result. The alternative is `SpawnActorFromObject` itself plus a temporary
   `MakeLevelCurrent` and a post-hoc transform. It is simpler, but it changes the user's current level and applies
   surface-snap rotation first. See D12.
2. **`Raw/LevelDetach.cpp`**: `DetachSelectedActors` works only on the selection. The helper does its body for one
   actor: `OldParent->Modify(false)`, `DetachFromComponent(KeepWorldTransform)`,
   `SetFolderPath_Recursively(topmost parent's folder)`. Test: the same result as selecting the actor and calling
   `DetachSelectedActors`.

No other raw paths are needed. Every other op is a public `UNREALED_API` / `UE_API` editor function.

### 5.4 MCP side

- `MCP/tools.json`: `query_level` and `edit_level` are listed with their input schemas and map to the
  `/assetdoc/level/query` and `/assetdoc/level/edit` routes. The editor serves the list, and the stdio bridge
  (`MCP/src/bridge/`) forwards it unchanged. `get_level_editing_guide` returns `MCP/schemas/LevelEditing.md`.
- The `execute_python` description gains "for level/actor edits use `edit_level`".
- `MCP/schemas/LevelEditing.md` covers:
  - workflow: `query_level` (world), `query_level` (actors), `edit_level` with `dry_run`, `edit_level`, then
    `query_level` / `get_viewport_screenshot`;
  - ActorRef and ids;
  - op reference;
  - transform semantics;
  - properties (link to the AssetDocument value rules);
  - transactions and Undo (including §3.6);
  - saving;
  - World Partition (pin);
  - diagnostics list;
  - pitfalls: component names come from `query_level detail: components`; UCS components are read-only;
    EditDefaultsOnly properties belong to the Blueprint.
- `stdioBrokerSmoke.test.ts` / `toolCatalog.test.ts` are extended.

### 5.5 Test plan

Automation tests `UEAssetDoc.Level.*` (Editor, real RHI, as for the other AssetDocument tests; under the
`UEAssetDoc` prefix so the suite tooling, which groups by the third name segment, runs them as the
`Level` group). Each test creates its own map
under `/Game/__AssetDocumentTests__/Level/` with `ULevelEditorSubsystem::NewLevel(path, bIsPartitionedWorld)`, and
deletes it in cleanup. Real engine content is used: `/Engine/BasicShapes/*`, engine light/camera classes, and a test
Blueprint created through AssetDocument.

| Test | Checks |
|---|---|
| `Query.World` / `Query.Filters` / `Query.Paging` | Summary, every filter, stable order across pages, `detail` levels, instance-override `diff_only` |
| `Query.WorldPartitionUnloaded` | WP map with actors saved far apart; unload the region; descriptors listed with `loaded:false`; edit → `ActorNotLoaded`; `pin` → editable |
| `Edit.SpawnClass` / `SpawnAsset` / `SpawnBlueprint` | Factory choice is the same as the editor's (StaticMesh → `AStaticMeshActor`, BP → `_C`); transform exact; spawn into a sublevel; locked level refused |
| `Edit.Properties` | Actor, native component, SCS component (instance override survives the construction rerun), Blueprint instance-editable variable, EditDefaultsOnly refused, float normalisation, clamped value → rollback + `PropertyValueNotStoredExactly` |
| `Edit.ActorReferences` | `"@id"` into an `AActor*` property; same-level path OK; cross-level hard reference refused; soft reference across levels OK |
| `Edit.Components` | add (named, with parent/socket), properties on it, reparent, remove; native removal refused; UCS write refused |
| `Edit.Hierarchy` | attach/detach (folder follows, world transform kept), cycle refused with the editor reason |
| `Edit.Organisation` | label, tags, folders (create/rename/delete), layers (non-WP), data layers (WP) |
| `Edit.Delete` | referenced actor refused, `allow_referenced` warns; Level Sequence soft referencer reported |
| `Transaction.UndoRedo` | One batch = one Undo entry; Ctrl+Z restores the digest; Ctrl+Y re-applies; the user's previous entry untouched |
| `Transaction.RollbackMidBatch` | Failure injected after op k of n (test hook in `LevelEditTransaction`, as AssetDocument's four failure boundaries): digest equal to before, no leftover actors, Undo stack length unchanged, user's previous entry still undoable |
| `Transaction.EmptyBatchFailure` | Failure before any record: **the user's previous edit is not undone** (the §3.1 step 8 guard) |
| `Transaction.Busy` / `PlaySession` | Open user transaction → `EditorBusy`; PIE → `PlaySessionActive`, nothing touched |
| `OFPA.DirtyAndSave` | WP map: `dirty_packages` = external actor packages; `save:true` writes exactly those; delete+save removes the actor package; spawn+undo+Save All writes nothing new (R3) |
| `UndoHistory.AssetApplyClearsLevelUndo` | Spawn BP actor, apply an AssetDocument change to the BP → history cleared (documents §3.6); an apply to an unrelated asset keeps it |
| `Raw.Placement`, `Raw.Detach` | §5.3 behaviour tests |
| `ExecutePython.FailureRollsBack` | failing script leaves no change and no open transaction |

**AI scenario.** A fresh agent using only MCP tools gets the task: *"In
/Game/Maps/Test_Room build a small lit room: floor, four walls, a ceiling light, a door Blueprint
(create it: a frame mesh and a door mesh that can swing), and a camera looking at the door."*

Measured:
- tool calls and failed calls;
- whether diagnostics alone let it recover;
- end state checked by `query_level` and a viewport screenshot;
- one Ctrl+Z undoes the whole AI build.

Run it in both a non-partitioned map and a WP map.

## 6. Relation to Level Sequence

A Level Sequence is an asset document; the actors it binds are level content. To keep the two tools apart while
letting them refer to the same actors, use one reference format:

- **What the sequence stores.** UE 5.7 stores possessable bindings as Universal Object Locators. The actor fragment
  is `FActorLocatorFragment{FSoftObjectPath Path}`, for example `/Game/Maps/Room.Room:PersistentLevel.PointLight_0`.
  It is resolved inside the streaming world, with WP container ids for level instances (`ActorLocatorFragment.cpp`).
- **The shared ActorRef canonical form is exactly that `path` string.** `query_level` returns it; `edit_level`
  returns it for spawned actors (under their batch id); a LevelSequence document writes it as the binding target.
  `guid` and `label` are lookup helpers for the level tools only. A sequence document must store `path`, because
  that is what the engine persists. The Sequencer profile should accept the same ActorRef *input* forms (`path`, `{"guid"}`,
  `{"label"}`) and canonicalize to `path` at apply, so an AI can paste `query_level` output into a binding.
- **Division of labour.**
  - The sequence document never creates, moves or deletes level actors. A missing target is a sequence diagnostic
    ("actor not found in the editor world"); the AI creates it with `edit_level` first.
  - `edit_level` never edits sequence bindings.
  - Spawnables are owned by the sequence and are not level actors.
- **Cross-effects that must be reported, not handled.**
  - Deleting a bound actor: soft-referencer warning or error (§3.2), because the editor's delete dialog finds
    sequence bindings through `FindSoftReferencesToObjects`. This is unverified; risk R5.
  - Duplicate creates new paths, so bindings do not follow, as in the editor.
  - Renaming a label does not change `path`, so bindings survive.
- **Level-instance actors**: path inside the instance's level plus container id. Out of scope v1 on both sides.

## 7. Risks and unknowns

| # | Risk | Mitigation |
|---|---|---|
| R1 | An editor path used inside the batch opens a dialog or resets the transaction buffer | Non-interactive scope (cancels modals, reports); `TransactionLost` guard after each op; tests per op |
| R2 | A Blueprint construction script changes values after a write (override lost or rewritten) | Verify step reads back after the rerun; rollback with `PropertyValueNotStoredExactly`. Component pointers are re-resolved by name in every op (a rerun recreates SCS components), never cached across ops. |
| R3 | OFPA: an actor spawned then undone leaves a dirty external package that "Save All" writes | Test `OFPA.DirtyAndSave`. If the editor leaves one, the rollback step discards the new package (as `AssetDocumentEditorOrphanPackageCleanup` does for assets) |
| R4 | Placement helper drifts from the editor's drag path in a future UE version | `Raw.Placement` compares with `SpawnActorFromObject` every run |
| R5 | Soft-reference search misses Sequencer locator bindings, or is slow on big maps | Verify with a bound actor; restrict the search to loaded LevelSequence assets if needed; measure |
| R6 | Large batches on WP maps (thousands of actors): digest and verification cost | Digest only touched actors plus per-level actor sets; `TooManyOps` cap (default 500) |
| R7 | Floats in transforms (rotator ↔ quaternion) read back differently | Store and verify `RelativeRotation` (FRotator, written as given); compare at the codec's float precision |
| R8 | `AddNewSubobject` naming / `RenameSubobject` refuses a name already used by an SCS variable | Preflight checks names against the plan; `ComponentNameTaken` |
| R9 | Users expect labels to be unique and use them as ids | Ambiguity diagnostic lists candidates with paths; the guide says use `path` |

## 8. Decisions

| # | Question | Decision |
|---|---|---|
| D1 | Do AI level edits go into the user's Undo stack? | **Yes**: one entry per batch, titled by `description`. This is the reverse of the AssetDocument rule, and it is deliberate (§3.3). |
| D2 | Save policy | **Leave dirty by default**; `save: true` saves exactly the packages the batch dirtied; a save failure keeps the edit (207). |
| D3 | Reject levels with unsaved changes (like `DirtyExistingAssetTransactionUnsupported`)? | **No.** Rollback uses Undo, not disk, so unsaved user work is safe. |
| D4 | AssetDocument applies that change an asset referenced by level transactions (e.g. a spawned Blueprint) clear the whole Undo history, including level edits | **Accept and document.** It is the existing rule, it mirrors the editor's `ReloadPackages`, and it avoids mixed states. |
| D5 | Delete of referenced actors | **Refuse** with `Level.DeleteReferenced` (lists hard, level-script and soft referencers) unless `allow_referenced: true`, which deletes and warns. |
| D6 | Python orchestration for level ops? | **No, C++ only in v1** (§5.1); revisit only if class-specific helpers appear. |
| D7 | Module and naming | New editor module **`AssetDocumentLevel`**, plus a small public object-property API exported from `AssetDocumentEditor`. |
| D8 | Number of tools | **`query_level`, `edit_level`, and a static `get_level_editing_guide`**. The guide is not folded into `get_asset_document_schema`, because levels are not asset documents. |
| D9 | Unloaded World Partition actors | **No implicit loading.** `ActorNotLoaded`, plus an explicit `pin` helper (the Outliner's Pin). |
| D10 | Opening and creating maps | Separate helpers `open_level` / `new_level`, refusing when unsaved changes would be lost (`Level.UnsavedChangesWouldBeLost`; `save_first` saves them first). |
| D11 | Fix the `execute_python` open-transaction bug now? | **Yes.** Small; a failing script otherwise leaves the editor's Undo broken. |
| D12 | Spawn path | **`UPlacementSubsystem` with explicit transform and level** (B1 helper, §5.3), rather than `SpawnActorFromObject` + temporary current-level switch. |
| D13 | Shared actor reference with the Sequencer profile | **Canonical = actor soft object path** (`FActorLocatorFragment::Path`); `guid`/`label` are lookup-only inputs. |
