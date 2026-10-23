# Import and Settings Guide

## import_assets

The Content Browser's Import and Reimport, without dialogs. Sources are
local files the user named, as absolute paths; no URLs or network shares.

| Kind | Formats | Assets |
| --- | --- | --- |
| Meshes | `.fbx`, `.gltf` / `.glb`, `.obj` | StaticMesh, or SkeletalMesh + Skeleton (+ PhysicsAsset), AnimSequence, materials, textures |
| Textures | `.png`, `.jpg`, `.tga`, `.exr` (and the editor's other image formats) | Texture2D |
| Sounds | `.wav` | SoundWave |

`{"files": ["C:/Art/Hero.fbx"], "destination": "/Game/Characters/Hero"}` imports with the
project's default Interchange pipelines (Project Settings > Interchange), as the import dialog
opens them (the editor's factory where Interchange does not import the format). `name` renames the asset of one file. The result lists every created or updated
asset (`assets[]`: `path`, `class`, `status`, and `facts`, e.g. `Bounds`,
`PlayLength`, `Duration`, `Bones`, `Triangles`, `CollisionShapes`, texture `Size` / `SRGB` /
`Compression` and the `Source` file), the importer of each file and its warnings. Imported
packages are saved (`save: false` leaves them dirty). Asset paths follow the pipelines (glTF imports into
`<destination>/<scene>/<Type>` subfolders by default): use the result's paths.

`options` sets the common dialog settings:

| Option | Effect |
| --- | --- |
| `skeleton` | target Skeleton asset for skeletal meshes and animations |
| `animations_only` | import only the animations (with `skeleton`) |
| `mesh_type` | `"auto"`, `"static"` or `"skeletal"` |
| `combine_meshes` | one static mesh for the whole file |
| `collision`, `collision_type` | generate collision; `Box`, `Sphere`, `Capsule`, `Convex18DOP`, ... |
| `create_physics_asset`, `nanite` | skeletal physics asset; Nanite for static meshes |
| `import_materials`, `import_textures`, `import_animations` | what else the file creates |
| `uniform_scale` | import scale (FBX, glTF; Interchange's OBJ import ignores it) |
| `normal_map`, `srgb`, `compression`, `flip_normal_green` | texture settings (`compression`: `Default`, `Normalmap`, `Masks`, `HDR`, ...) |

Any other dialog setting goes in `pipeline`, by property name, nested by sub-pipeline:
`{"pipeline": {"MeshPipeline": {"bImportLods": false}}}`. `{"action": "options", "files": [f]}`
lists every setting of the file's pipelines with its current value. A setting no pipeline of the
file has is `Import.UnknownPipelineProperty`; a valid option that does not apply to some file
(mesh options on a texture) is a warning, `Import.OptionNotApplicable`.

An existing asset is `Import.AssetExists` unless `replace_existing: true` (the editor's
"overwrite?" yes: the asset is updated in place, references stay).

`{"action": "reimport", "assets": ["/Game/Props/SM_Crate"]}` reimports from the recorded source
file with the settings stored in the asset (as the editor's Reimport); `source` reimports one
asset from another file. Only the texture options apply to a reimport.

| Code | Meaning |
| --- | --- |
| `Import.SourceNotLocal`, `SourceNotFound`, `SourceNotFile` | not a local file; missing; a folder |
| `Import.FormatNotSupported` | no importer for the extension (`details.supported` lists them) |
| `Import.InvalidDestination` | not a `/Game` folder |
| `Import.UnknownOption`, `InvalidOption`, `SkeletonNotFound` | options |
| `Import.NothingImported`, `ReimportFailed` | the importer failed; `details.errors`. A file of bones without a mesh (skeleton-only or animation-only FBX) imports nothing alone: pass `skeleton` + `animations_only: true` for its animations |
| `Import.NoSourceFile` | reimport of an asset not imported from a file |

## settings

Reads and changes the World Settings panel of the open level and the Project Settings sections.
Reading is free; `action: "write"` changes.

- `{"section": "world"}` lists the World Settings that differ from the defaults (a struct, also in
  an array, with its differing members only; a written struct keeps the members it omits);
  `"properties": ["KillZ", "DefaultGameMode"]` reads those whole (any property the panel shows).
- `{"action": "write", "section": "world", "properties": {"DefaultGameMode":
  "/Game/Core/BP_ArenaMode.BP_ArenaMode_C", "KillZ": -5000}, "description": "AI: arena rules"}`
  writes like the panel: one Undo entry (`transaction.title` / `id` for `undo_editor`); the map
  is left dirty unless `save: true`.
- `{"action": "list"}` lists the Project Settings sections (`Project/Maps` is Maps & Modes,
  `Engine/Input`, plugin and game settings) with their class and config file.
- `{"section": "Maps & Modes", "properties": ["GameDefaultMap", "GlobalDefaultGameMode"]}` reads a
  section; sections are named by display name, `Category/Section`, or settings class.
- `{"action": "write", "section": "Maps & Modes", "properties": {"GameDefaultMap":
  "/Game/Maps/Arena.Arena", "EditorStartupMap": "/Game/Maps/Arena.Arena", "GlobalDefaultGameMode":
  "/Game/Core/BP_ArenaMode.BP_ArenaMode_C"}}` writes the section's default config file in the
  project's `Config/` (`config_file`, e.g. `Config/DefaultEngine.ini`; checked out or made
  writable) as the Project Settings editor does; `written_keys` are read back from the file.
  Project Settings writes are not in the Undo history.
- `"dry_run": true` on a write checks it and returns the planned `changes` (`before` / `after`)
  without changing anything.

| Code | Meaning |
| --- | --- |
| `Settings.SectionNotFound`, `SectionAmbiguous` | close / candidate sections are listed |
| `Settings.ConfigFileNotInProject`, `Protected` | saved to engine, plugin or per-user config; UE-AssetDoc's own, or settings that run code (Python startup scripts): not written |
| `Settings.ConfigFileReadOnly` | the file could not be checked out or made writable |
| `Settings.RestartRequired` | warning: takes effect after an editor restart |
| `Generic.UnknownProperty` | no such property in the section |

## Asset files: save_assets, reload_assets, asset_editor, export_assets

The editor's File > Save, Reload, asset editor windows and Export, without dialogs. Assets are package or object paths; each is answered on its own
(`refused[]` with `Assets.NotFound`, `Assets.SaveFailed`, ...; 207 when others succeeded). Refused during Play In Editor.

- `save_assets {"assets": [...]}` saves those with unsaved changes (`clean[]` lists the others;
  `force: true` re-saves them). `{"all_dirty": true, "only": "maps" | "content"}` is Save All; a
  never-saved (Untitled) map is listed in `not_saved`. `edit_level` / `settings` take `save: true`
  for the level they change.
- `reload_assets {"assets": [...]}` reloads from disk after a file changed outside the editor.
  Unsaved changes are refused (`Assets.UnsavedChanges`) unless `force: true` discards them
  (`discarded_changes`); the open map is refused (`Assets.OpenMap`: use `open_level`); an asset
  not in memory needs nothing (`not_loaded`).
- `asset_editor {"action": "open" | "close" | "apply_editor" | "list", "assets": [...]}`: close an
  editor before a tool that refuses open ones. An editor with edits not applied to its asset yet
  (Material, Niagara: its Apply is enabled) does not close (`Assets.EditorNotClosed`): pass
  `discard_unapplied: true` to drop them, or `apply_editor` to press its Apply (then `save_assets`
  and extract again: the asset changed). Every result lists `open`: the assets with an editor open
  (not the editors' working copies). Without an audio device (`-NoSound`) a MetaSound's editor is
  refused (`Assets.EditorNeedsAudio`: the engine would crash).
- `export_assets {"assets": [...], "folder": "Saved/Exports/Audio"}` writes
  `<folder>/<AssetName>.<format>`: `wav` for sounds, `png` for textures by default; other classes
  pass `format` (`Assets.NoExporter` lists formats). Folder and every file written stay in the
  project (no `..`, no link out: `Assets.FileOutsideProject`); existing files need `overwrite: true` (`Assets.FileExists`).
