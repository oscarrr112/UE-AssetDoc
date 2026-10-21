# Niagara Profile

Classes: /Script/Niagara.NiagaraSystem, /Script/Niagara.NiagaraEmitter, /Script/Niagara.NiagaraScript, /Script/Niagara.NiagaraParameterCollection

Niagara **systems** apply: Details `Properties`, `UserParameters`, the declared `Parameters`, `Emitters` (add, delete, rename, enable, parent removal and version, emitter `Properties`, `Renderers`), every module stack (modules, inputs, Set Parameters, event handlers, simulation stages), lightweight emitters and scratch pads. Niagara **scripts** (module, function and dynamic input assets) apply: usage, settings, parameters, the graph and versions. Niagara **emitter assets** apply as the emitter editor (see "Emitter assets"), **parameter collections** as the collection editor (see "Parameter collections"); collection instance assets are not authored. Engine and plugin assets (`/Niagara/...`) extract read only; re-target the document to a `/Game` path to create a copy, or use `duplicate_asset`.


```jsonc
"Properties": {"WarmupTime": 0.5},                              // System Details (generic property admission)
"Body": {
  "UserParameters": [{"Name": "User.Count", "Type": "int32", "Value": 50, "Description": "...", "Category": "Spawn"}],
  "Parameters": [{"Name": "System.Heat", "Type": "float", "Description": "..."}],   // declared in the Parameters panel
  "SystemSpawn": [],
  "SystemUpdate": [{"Name": "SystemState", "Script": "/Niagara/Modules/System/SystemState.SystemState",
                    "Inputs": {"Loop Behavior": "Once"}}],
  "Emitters": [
    {"Name": "Sparks",                                          // handle name (identity)
     "Parent": {"Emitter": "/Game/FX/NE_Base.NE_Base"},         // an emitter asset whose Is Inheritable is on
     "Properties": {"SimTarget": "GPUComputeSim"},              // Emitter Properties (values that differ from the defaults)
     "EmitterSpawn": [],
     "EmitterUpdate": [{"Name": "SpawnBurst_Instantaneous", "Script": "/Niagara/Modules/Emitter/SpawnBurst_Instantaneous.SpawnBurst_Instantaneous",
                        "Inputs": {"Spawn Count": {"Link": "User.Count"}}}],
     "ParticleSpawn": [{"Name": "InitializeParticle", "Script": "/Niagara/Modules/Spawn/Initialization/V2/InitializeParticle.InitializeParticle",
                        "Inputs": {"Lifetime": {"Dynamic": {"Script": "/Niagara/DynamicInputs/UniformRange/UniformRangedFloat.UniformRangedFloat",
                                                            "Inputs": {"Minimum": 0.4, "Maximum": 0.8}}}}}],
     "ParticleUpdate": [],
     "EventHandlers": [{"Properties": {"SourceEventName": "LocationEvent"}, "Modules": []}],
     "SimulationStages": [{"Name": "Fill", "Class": "Generic", "Modules": []}],
     "Renderers": [{"Name": "NiagaraSpriteRendererProperties_0", "Class": "SpriteRenderer",
                    "Properties": {"Alignment": "VelocityAligned"}}],
     "ScratchPads": []}]
}
```

- Module lists are in execution order (the stack's order). A module entry: `Name` (the module's function name, unique in its emitter; its identity), `Script` (module script path, or `"ScratchPad:<name>"`), `Version` (only when not the script's exposed version), `Enabled: false` (omitted = enabled), `DisplayName` (stack rename), `Note` (`{Header, Body}`), `Inputs`. A Set Parameters module has `Assign: [{Target, Type}]` instead of `Script`, its inputs keyed by target.
- `Inputs` lists the inputs that are overridden (the stack's reset arrow); the others have the module's default. Values: a literal (`2.5`, `true`, an enum name such as `"Once"` (a name ending in a parenthesized explanation may be written without it when that is unambiguous: `"Kill"` for `Kill (Emitter and Particles Die Immediately)`; extract writes the full name), `{"X":0,"Y":0,"Z":1}`, `{"R":1,"G":0.5,"B":0,"A":1}`), `{"Link": "<parameter>"}`, `{"Dynamic": {"Script", "Version"?, "Inputs"}}` (nested), `{"Expression": "<hlsl>"}`, `{"DataInterface": {"Class", "Properties"?}}`, `{"Object": "<path>"}`. `{"Unsupported": "<mode>"}` marks an input whose value mode cannot be represented (a default function, an invalid override).
- Types: `float`, `int32`, `bool`, `Vector2`, `Vector`, `Vector4`, `LinearColor`, `Quat`, `Position`, `Matrix`, `ID`, an enum or struct path, a data interface class path, `Object:<class path>`.
- Renderers keep their object names; `Class` drops the `Niagara` prefix and `Properties` suffix (`SpriteRenderer`, `RibbonRenderer`, `MeshRenderer`, ...). Simulation stages are identified by name, event handlers by position.
- An emitter asset's Body is one emitter entry without `Name`, `Enabled` and `Mode`, plus `Versions` (see "Emitter assets").
- Scripts (and each `ScratchPads` entry, with its `Name`): see "Scripts and scratch pads" below.
- Parameter collections: `"Body": {"Parameters": [{"Name": "Speed", "Type": "float", "Value": 2.5}]}` (see "Parameter collections").
- The sections "Scripts and scratch pads", "Emitter assets" and "Parameter collections" named above are the parts `scripts`, `emitter_assets` and `parameter_collections`.

## Apply (systems)

- `UserParameters` is complete when listed: a parameter that is not listed is deleted (as the editor's delete: module inputs linked to it keep its value as a local value), a new `Name` adds one, `"RenamedFrom": "User.Old"` renames one (links and bindings follow; the document should show the new name in its links too). Names start with `User.`. `Type` cannot change (`Niagara.UserParameterRetype`: delete and add). `Value` omitted keeps the value (a new parameter: the type's default). `Description` and `Category` omitted are none; `Category` is the User Parameters panel's category, nested with `|` (`"Spawn|Motion"`). A data interface's `Properties` are complete (omitted ones are its defaults); its `Class` is the type. The list's order is not authored (the extract lists parameters by name).
- `Emitters` is complete when listed: not listed = deleted, a new `Name` = added, `RenamedFrom` = renamed (applies while the system does not have the new name, so re-applying is a no-op; swapping two names takes two applies). A new emitter is added `From` an emitter asset (`"/Niagara/DefaultAssets/Templates/Emitters/Fountain"` or `{"Emitter": path, "Version": guid}`), `"Empty"` or `"Minimal"`; `From` omitted = its `Parent`, else `"Empty"`. An emitter asset that is inheritable becomes the new emitter's `Parent`, which must then be listed (omitted or `null` = no parent: the parent is removed). Only an emitter asset whose Is Inheritable (Asset Options) is on can be a `Parent`; one that is not (the engine templates such as `Fountain`) adds a copy without a parent: leave `Parent` out (listing it fails with `Niagara.NotInheritable`). A parent cannot be set or changed (`Niagara.ParentImmutable`). New emitters go to the end; the editor cannot reorder emitters (`Niagara.EmitterOrder`).
- Per emitter: `Enabled` (omitted = enabled), `Properties` (emitter Details; omitted = keep, listed = complete: properties left out go back to their defaults), `Renderers` (omitted = keep, listed = complete, in this order). A renderer: `Name` (its identity; new renderers get it), `Class`, `Enabled` (omitted = enabled), `Properties` (omitted = keep; listed = complete: properties left out go back to the class defaults). A different `Class` replaces the renderer. Renderer classes are those of the Render group's "+" menu (`SpriteRenderer`, `RibbonRenderer`, `MeshRenderer`, `LightRenderer`, `ComponentRenderer`, ...); inherited renderers cannot be deleted or moved above others.
- `Parent` can also switch to another version of the same emitter asset (`{"Emitter": path, "Version": guid}`; no `Version` = its exposed version), as the emitter header's version menu.
- `Parameters` (declared, unwritten parameters of the Parameters panel; `System.`, `Emitter.`, `Particles.` names) is complete when listed: `Name`, `Type`, `Description`.

### Module stacks

- A module list that is listed is complete and ordered (omitted = kept). A module is identified by `Name`, unique in its graph (the system's two stacks, or all stacks of one emitter); a `Name` listed in another stack of the same graph moves the module (as drag and drop). A new module is added under its `Name`, which is the script's name or the script's name plus digits, as the editor names a second copy (`AddVelocity`, then `AddVelocity001`, `AddVelocity002`; `AddVelocityUp` fails with `Niagara.InvalidModuleName`, a label goes in `DisplayName`; an existing module of older content named otherwise is kept and edited, not re-created); the stack's "+" menu usage filter applies (`Niagara.ModuleUsage`), and the add initializes inputs as the "+" menu does. Another `Script` under the same `Name` replaces the module.
- Module settings: `Enabled` (omitted = enabled), `Version` (omitted = the script's exposed version; changing it converts the inputs as the version menu does), `DisplayName` (omitted = none), `Note` (omitted = none). Inherited modules (from a parent emitter) cannot be deleted or moved (`Niagara.InheritedModule`).
- `Inputs` is complete: an input left out is reset to the module's default (the reset arrow). An input name that does not exist fails with `Niagara.InputNotFound` (it lists the inputs). A value equal to the input's default is no override: leave it out (`Niagara.InputIsDefault`, one diagnostic listing every such input of the apply). Inputs the stack nests under a mode input (InitializeParticle's `Lifetime Min` under `Lifetime Mode`) are listed by name like the others; set the mode in the same `Inputs`. A curve data interface's `Properties` keys (and other struct values, also array elements such as a mesh renderer's `Meshes`) list only the fields they set (`{"Time": 0, "Value": 1}`), as extract writes a data interface's; omitted fields have their default. A `Link` to a `User.` parameter must be in `UserParameters` when that list is given (`Niagara.UndeclaredUserParameter`: the editor's link would add it) and of the input's type (`Niagara.LinkTypeMismatch`); `{"Link": name, "Type": type}` links through a different type. `{"Dynamic"}` nested `Inputs` are complete too. `{"DataInterface": {"Class", "Properties"}}`: `Class` omitted keeps the class; `Properties` complete. `{"Unsupported"}` values cannot be written: keep them as extracted.
- Set Parameters: `{"Name", "Assign": [{"Target": "Particles.Color", "Type": "LinearColor"}], "Inputs": {"Particles.Color": {...}}}`. New ones get editor-made names (`SetVariables_<id>`): an entry whose `Name` the graph does not have takes the stack's next unclaimed Set Parameters module, in order, so a new one can use any name and the extract shows the editor's name. `Assign` is ordered; a changed target list re-targets the module.
- The apply runs in passes: a new emitter, module or handler gets its settings and inputs in the next pass, from a fresh read.

- Diff compares `Properties` and reports what an apply would change.
- Not authored yet: emitter order, copying an emitter from another system. Not read yet: GPU-only compile messages.
