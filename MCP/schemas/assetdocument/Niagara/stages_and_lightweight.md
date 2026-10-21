# Niagara reference: Stages, event handlers, lightweight

- `EventHandlers` (complete when listed) match by position: extra ones are deleted from the end, new ones added at the end. `Properties` are the handler's Details (`SourceEmitter` is an emitter name, `SourceEventName`, `ExecutionMode`, `SpawnNumber`, ...; complete when listed), `Modules` its module list.
- `SimulationStages` (complete, ordered) match by `Name`: `Class` (`Generic`, ...; another class replaces the stage), `Enabled`, `Properties` (Details, complete when listed), `Modules`. Simulation stages run on GPU emitters only.
- A lightweight emitter is added with `{"Name", "Mode": "Stateless"}` (the editor's empty lightweight emitter); `Stateless.Properties` (complete; `EmitterTemplate` rebuilds the module list), per module `Properties` (complete; modules left out go back to their defaults) and `Renderers` as for standard emitters. The mode cannot change (`Niagara.ModeChange`).
- A lightweight emitter is `{"Name", "Mode": "Stateless", "Stateless": {"Properties", "Modules": [{"Class", "Properties"}], "Renderers"}}`; its modules come from its `EmitterTemplate` property (on/off: the module's `bModuleEnabled`).
