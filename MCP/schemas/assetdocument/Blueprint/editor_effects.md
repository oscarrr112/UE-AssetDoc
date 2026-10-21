# Blueprint reference: Dependents, PIE, dialogs, open editor

Dependents: as after the same edit in the Blueprint editor, a changed variable default, class default or component property also reaches every loaded object based on it that still holds the old value: child Blueprints' class defaults and inherited component overrides, placed actors in open levels and their components. An object that overrides the value keeps it. The result lists the updated objects in an Info diagnostic `Graph.DefaultsPropagated`; the child Blueprints and levels are not marked dirty (an inherited value is not saved with them, so their saved state is unchanged).

Diff: `Variables`, `Components`, `ClassDefaults` and each graph list diff as one `Replace` of the region (not per entry).

Play-In-Editor: while a play session runs, an apply (and Validate) is refused before anything is touched (`Graph.PieActive` at `/Target`): nothing changes and nothing is rolled back. Stop the play session and apply again. The same holds for the other graph profiles and for User Defined Enums / Structs.

Editor dialogs: an apply never waits for a click. A dialog the editor still opens is answered with its default (Cancel, or No for a yes/no question) and reported as the warning `GenericRuntime.EditorDialogAnswered`; the result is still checked against the document (a difference fails and rolls back). Example: removing an interface while a child Blueprint is not loaded asks to load the children; the default No leaves them unloaded, and they are fixed up when they load.

Open editor: an apply works while the Blueprint is open in the Blueprint editor. The editor shows the result as after its own edit (My Blueprint, Components tree, Details, viewport preview, graphs; tabs of removed graphs close), also after a failed apply (rolled back), and its own undo steps keep working.
