# StateTree support matrix

Governing rules:
[design rules](asset-document-design-rules.md) §3 (rollback = functional
equivalence), §4 (raw editor-equivalent code + behaviour test) and §5 (support matrix).

| Class | Profile | Route | Python |
|---|---|---|---|
| `/Script/StateTreeModule.StateTree` | `profile.state_tree.v1` | `state_tree_profile` | `profiles/state_tree.py` |

C++: `Source/AssetDocumentAI/Private/StateTree/` (owner `EAssetDocumentPrimitiveOwner::AI`, provider
`ai`; the AI provider delegates `ai.state_tree.*` to a table-driven provider built with
`CreateAssetDocumentAssetTablePrimitiveProvider`). Three primitives:

| Primitive | Editor path it reproduces |
|---|---|
| `ai.state_tree.read.v1` | paged records of the editor data (tree, parameters, states, nodes, bindings) |
| `ai.state_tree.edit.v1` | state tree view model (add / move / remove states), Details panel of a state / transition / node (Modify, PreEditChange, write, PostEditChangeChainProperty), node picker (the same node built in `AssetDocumentStateTreeNodes.cpp`), property bag Details, binding menu (`FStateTreeEditorPropertyBindings::AddBinding / AddFunctionBinding / RemoveBindings`) |
| `ai.state_tree.compile.v1` | Compile button (`UStateTreeEditingSubsystem::CompileStateTree`, which runs `ValidateStateTree` first) |

Create: generic factory lifecycle with the factory context = the schema class (`Body.Schema.Class`,
checked against `UStateTreeSchema` by `editor.class.resolve_canonical`). The generic factory
context setter fills the factory's single unset class setting whose meta class accepts the class
(`UStateTreeFactory::StateTreeSchemaClass`, what its class picker sets); `UStateTreeFactory`
then creates the editor data, schema and `Root` state and compiles, as in the editor.

Rollback: no inverse code. Core restores the package snapshot (existing tree) or the factory
rollback deletes the created tree. The StateTree derived-state rebuilder recompiles a restored tree
that was compiled before (a restored package does not relink the runtime data).

Compile: every apply that changed something ends with the editor compile. Errors fail the apply
(`StateTree.CompileError` at the document path of the state / node the message names; a failed
compile resets the runtime data, as in the editor) and the asset rolls back; warnings come back as
`StateTree.CompileWarning` with the success. `Compile: {Status, Messages}` on Apply / Extract /
Inspect (Blueprint shape) comes from a compile status describer registered with Core
(`AssetDocumentCompileStatus.h`): `UpToDate`, `UpToDateWithWarnings`, `Dirty`, `Error`.

Undo: one editor transaction per edit call. A successful apply clears the undo history that
references the tree. A failed apply restores the tree; when it had already recorded edits (e.g. a
compile error after the edits) the history that references the tree is cleared too, since those
steps would undo onto the restored tree; a document refused before any edit keeps the human's
history. An unsaved human edit makes ApplyFile refuse the tree (`DirtyExistingAssetTransactionUnsupported`).

Snapshots: the rebuilder classifies the compile output outside the editor data (the schema copy,
extensions, instance objects) as derived, because every compile renames the previous output to the
transient package; the editor data and its subobjects are captured and restored.

## Document

See `MCP/schemas/assetdocument/StateTree.md` for the full shape. Omitted = keep; a present list is
complete; transitions are complete values except `Conditions`.

## Support

| Area | Status | Notes |
|---|---|---|
| Schema class | Supported (create), read-only after | changing the class is a new asset (`SchemaClassImmutable`) |
| Schema settings (`Schema.Properties`) | Supported | properties the value codec cannot write are listed in Inspect and refused |
| Tree parameters | Supported | name identity, `RenamedFrom`, all Blueprint variable pin types the property bag supports |
| Evaluators, global tasks, `GlobalTasksCompletion` | Supported | refused when the schema disallows them (`ListNotAllowed` / `NodeNotAllowed`) |
| State hierarchy (subtrees, children, order) | Supported | rename / move by `RenamedFrom`; state GUIDs kept |
| State types State / Group / Linked / LinkedAsset / Subtree | Supported | type change follows the editor (tasks / links reset) |
| Selection, Enabled, Tag, Weight, TickRate, TasksCompletion, CheckPrerequisites, Description | Supported | |
| Enter event (`EnterEvent`) | Supported | tag must exist (`UnknownGameplayTag`) |
| State parameters, linked parameter values / bindings | Supported | linked states take the linked layout (`ParameterLayoutFixed`) |
| Enter conditions, tasks (instance data), considerations | Supported | struct nodes and Blueprint node classes; values through the typed value codec |
| Condition operand / indent / evaluation mode, task enabled / considered for completion | Supported | |
| Transitions: all triggers except delegate, all target kinds, priority, delay, enabled, conditions | Supported | completion triggers cannot be delayed (`InvalidDelay`) |
| Property bindings: parameters, context data, evaluators, global tasks, state parameters, events, tasks / conditions of parent states, property functions (nested) | Supported | accessibility and type checked like the binding menu |
| `OnDelegate` delegate listener / dispatcher bindings | Unsupported | the binding is a delegate dispatcher pairing the document does not model; the trigger can be read |
| Output bindings, property references from outputs | Read-only | not part of the authored bindings |
| Apply into an open StateTree editor | Supported | in place; the editor's view model refreshes its views on the apply's edits and on a rollback (no extra handling); test `StateTree.OpenEditorApply` |
| Editor colors, themes, expansion, `bExpanded` | Unsupported | editor presentation only |
| Editor data extensions / custom editor data classes' extra fields | Unsupported | schema-specific; Inspect shows the editor data class |

## Identity rules

- States: path of names (`Root/Idle`); `RenamedFrom` = old name under the same parent or old full
  path (move). The state keeps its GUID, so transitions to it follow.
- Nodes: `Name`, otherwise the k-th unnamed node of the same `Type` in the list. Matched nodes keep
  their GUID, values and bindings; a changed `Type` is a new node.
- Parameters: `Name` / `RenamedFrom` (the property bag GUID is kept, so bindings follow).
- Transitions: position.

## Tests

- Python: `Content/Python/asset_document/tests/test_state_tree.py`.
- Editor: `UEAssetDoc.StateTree.Python` (create with every feature, Extract / Diff,
  no-op re-apply, rename / move / remove / edit / rebind, invalid input, compile error rollback,
  reload, failed creates), `UEAssetDoc.StateTree.NodeConstruction`
  (raw node construction = the editor's), `UEAssetDoc.StateTree.Undo`, `UEAssetDoc.StateTree.OpenEditorApply`,
  `UEAssetDoc.GenericRuntime.Robustness.FactoryConfiguration` (class context).
