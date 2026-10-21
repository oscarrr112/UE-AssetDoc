# Niagara reference: Emitter assets

The emitter editor without its UI: the edits are made on its working copy (a placeholder system holding a copy of the asset's exposed version), which its Apply writes back over the asset (same objects), then the asset compiles.

```jsonc
{"SchemaVersion": 1, "Target": "/Game/FX/NE_Sparks", "Class": "/Script/Niagara.NiagaraEmitter", "Action": "Create", "Definitions": {},
 "Properties": {"bIsInheritable": true},                         // asset settings (Asset Options)
 "Body": {"Parent": {"Emitter": "/Game/FX/NE_Base.NE_Base"},     // or "From": "/Niagara/DefaultAssets/Templates/Emitters/Fountain"
          "Properties": {"bDeterminism": true, "RandomSeed": 77},
          "EmitterUpdate": [{"Name": "SpawnRate", "Script": "/Niagara/Modules/Emitter/SpawnRate.SpawnRate", "Inputs": {"SpawnRate": 90.0}}],
          "Renderers": [{"Name": "NiagaraSpriteRendererProperties_0", "Class": "SpriteRenderer", "Properties": {"SortOrderHint": 3}}],
          "ScratchPads": [],
          "Versions": [{"Number": "1.0", "Exposed": true}]}}
```

- The Body is a system `Emitters` entry (emitter `Properties`, module stacks, event handlers, simulation stages, `Renderers`, `ScratchPads`, `Parent` version), applied as in a system; the asset's own settings (`bIsInheritable`, `TemplateAssetDescription`, `Category`, ...) are top-level `Properties`. An emitter asset is a standard emitter (`Stateless`: `Niagara.StatelessEmitter`).
- Create: `From` makes the new asset a copy of another emitter (no parent), `Parent` a child of an inheritable one (`Niagara.NotInheritable`), as the New Niagara Emitter dialog; `{"Emitter": path, "Version": guid}` picks a version. The rest of the Body then edits the new asset. A parent cannot be set or changed later (`Niagara.ParentImmutable`); `Parent: null` removes it.
- `ScratchPads` is complete: add, `RenamedFrom`, delete, as the system's list.
- `Versions` works as for scripts (the version menu, at most one `Exposed`); editing the exposed version of a versioned emitter is refused (`Niagara.ExposedVersionEdit`) unless `"EditExposedVersion": true`. The Body describes the exposed version; a new version's renderers are copies under new object names, so a document that adds a version omits `Renderers` (kept) and lists them as extracted afterwards.
- Before any change, a loaded asset that inherits from the emitter (a child emitter, a system using it or a child) with unsaved changes refuses the apply (`Niagara.DependentHasUnsavedChanges`). After it, as the editor's Apply, they merge the changes and recompile, open editors included (`Niagara.DependentsUpdated` lists them; they stay unsaved).
- Compile errors the emitter did not have before fail the apply (`Niagara.CompileFailed`) and roll it back. An emitter open in its editor is closed for the apply and reopened (`Niagara.EditorReopened`); edits there not applied yet (Apply or Apply Scratch enabled) refuse the apply before any change (`Niagara.OpenInEditorUnsavedEdits`; `asset_editor` `close` with `discard_unapplied: true` drops them, `apply_editor` applies them). Re-applying an unchanged document is a no-op.
