# AssetDocument EQS (Environment Query) Profile Design

Profile `profile.env_query.v1`, class `/Script/AIModule.EnvQuery`, route
`env_query_profile`. Governing rules: the [design rules](asset-document-design-rules.md) (editor-equivalent apply, rollback =
functional equivalence, no typed inverses), the SoundCue design (the closest graph profile) as template.

## 1. What a human edits

The Environment Query editor (plugin `EnvironmentQueryEditor`, module `UncookedOnly`) shows one graph
(`UEnvironmentQueryGraph`, `UEnvQuery::EdGraph`):

| Editor element | Edited by | Stored as |
|---|---|---|
| Root node | layout only | `UEnvironmentQueryGraphNode_Root` |
| Option node = one generator | palette (right click / drag off Root: *Generators*), Delete, move, wire to Root | `UEnvironmentQueryGraphNode_Option`; `NodeInstance` = `UEnvQueryOption` (in the query) whose `Generator` is an instanced `UEnvQueryGenerator` (in the query) |
| Tests of an option | *Add Test...* (node context menu), Delete, drag-drop reorder (also onto another option), enable checkbox | sub nodes `UEnvironmentQueryGraphNode_Test` in the option's `SubNodes`; `NodeInstance` = `UEnvQueryTest` (in the query); `bTestEnabled`, display `TestWeightPct` / `bHasNamedWeight` |
| Details of a generator / test | Details panel of the selected node (generator / test instance) | the instance's properties: purpose, filter, scoring, contexts (`TSubclassOf<UEnvQueryContext>`), `FAIDataProvider*Value` with instanced `DataBinding` (query params), trace data, ... (all `EditDefaultsOnly`, which this Details panel shows editable) |
| Node comment bubbles, comment boxes | node comment, *Create Comment* | graph data |

Runtime content is `UEnvQuery::Options`: `UEnvironmentQueryGraph::UpdateAsset` rebuilds it from the graph: the
Root output's links sorted by option `NodePosX` (left to right = option order), per option the *enabled* test
sub nodes in sub node order (`TestOrder` = sub node index), then `RemoveOrphanedNodes` (unreferenced inner
objects go to the transient package) and `UEnvQueryManager::NotifyAssetUpdate`. The editor calls it on every
structural change, on *Save* and when the graph tab opens (`Initialize`: `SpawnMissingNodes`,
`CalculateAllWeights`, `UpdateAsset`). Options not wired to Root are kept in the graph but do not run. A query
made by the factory has no graph until the editor opens it (`SpawnTab_UpdateGraph`: new graph, Root,
`OnCreated`, `Initialize` → nodes for existing options at Root + (300·i, 100)).

## 2. Document

```jsonc
"Body": {
  "Root": {"Position": [0, 0]},
  "Options": [                                   // runtime order of the connected options
    {"Key": "Grid", "Generator": {"Class": "SimpleGrid", "Properties": {"GridSize": {"DefaultValue": 800.0}}},
     "Position": [0, 150],
     "Tests": [                                  // runtime order (TestOrder)
       {"Key": "Dist", "Class": "Distance", "Properties": {"TestPurpose": "Score", "ScoringFactor": {"DefaultValue": -1.0}}},
       {"Key": "Vis", "Class": "Trace", "Enabled": false}]},
    {"Key": "Spare", "Generator": {"Class": "ActorsOfClass"}, "Connected": false, "Position": [600, 150]}],
  "Comments": {"Note": {"Text": "fallbacks", "Position": [-40, -80], "Size": [700, 300]}}
}
```

* `Options` / `Tests` / `Comments` omitted = keep; present = complete (unlisted ones are deleted as the editor's
  Delete does). `Root` omitted = keep its position.
* Keys are object names (option = its `UEnvQueryOption`, test = its `UEnvQueryTest`, comment = its comment
  node). Extract writes them; a new keyed entry is renamed to its key; a keyless entry matches the first
  unclaimed live entry of the same class in order (else it is new). A keyed test listed under another option
  is moved (the editor's drag-drop onto that option).
* `Class`: engine generators / tests by short name without `EnvQueryGenerator_` / `EnvQueryTest_`, other
  classes (Blueprint generators, project tests) by class path. `list_types` kinds `env_query_generator` and
  `env_query_test` list them (the palette / Add Test classes, with new-node Details); contexts are classes
  (`list_types` kind `class`, base `/Script/AIModule.EnvQueryContext`), as the context picker offers them.
* `Properties` are the Details values, sparse over a new node of the class (the palette's `NewObject` +
  `UpdateNodeVersion`); Diff / Verify compare over those defaults (`diff_with_defaults`). Query params are
  `DataBinding` instances: `{"DefaultValue": 500.0, "DataBinding": {"Class": "/Script/AIModule.AIDataProvider_QueryParams", "Properties": {"ParamName": "Radius"}}, "DataField": "FloatValue"}`.
  Trace settings (`FEnvTraceData`: `ProjectionData`, `TraceData`) carry what `FEnvTraceDataCustomization` shows;
  `VersionNum` (no Edit flag), `SerializedChannel` (derived from `TraceChannel` by `OnTraceChannelChanged`) and
  the `bCan*` capability flags (set by the class) are left out and refused when authored
  (`EnvQuery.UnknownProperty`). They are what `FEnvTraceData::OnPostLoad` rewrites (a new node's version 0 /
  `ECC_WorldStatic` becomes 1 / its channel's collision channel), so with them a saved query would not read
  back the same after a reload. A `TraceChannel` write sets `SerializedChannel` as the customization does (only
  needed for version-1 data: version-0 data takes it from `TraceChannel` on load).
* Option order: connected options run left to right by `Position` X, so the X of connected options must
  increase along the array (`EnvQuery.OptionOrder`). A new option without `Position`, or a kept option whose
  kept X contradicts the array order, is placed after its predecessor (300 to the right, or halfway to the next
  connected option). `Connected` omitted = `true`.
* `Generator.Class` changed on a key = the option is replaced (new option node of the new class; its tests are
  moved over).
* `Enabled` omitted = `true`. `Comment` / `CommentVisible` on options and tests: node comment bubbles.

## 3. Apply (editor-equivalent)

Dedicated primitives in AssetDocumentAI (owner AI, provider `ai`, table-driven like `ai.state_tree.*`); the
shared `graph.*` primitives do not address AIGraph sub nodes and their contract is frozen:

* `ai.env_query.read.v1` (read-only): graph records (or, for a query without graph, the nodes the editor's
  first open would create), sparse properties, palette defaults on request, validation messages, query params,
  open-in-editor.
* `ai.env_query.edit.v1` (mutating, one transaction per op as the editor): ops
  `add_option` (= `GetGraphContextActions` → `FAISchemaAction_NewNode::PerformAction` with Root's output as
  `FromPin` when connected), `add_test` (= `GetGraphNodeContextActions` → `FAISchemaAction_NewSubNode`, i.e.
  `AddSubNode`), `remove` (= `FAIGraphEditor::DeleteSelectedNodes`: `Modify` + `DestroyNode`), `move_test`
  (= `SGraphNodeAI::OnDrop`: `RemoveSubNode`, `InsertSubNodeAt`, `OnSubNodeDropped`), `set_enabled` (=
  `SGraphNode_EnvironmentQuery::OnTestToggleChanged`), `set_property` (= Details edit of the generator / test
  instance: `UE::AssetDocument::EditorObjectProperties` with template semantics, i.e. `EditDefaultsOnly`
  editable as this Details panel shows them), `connect` (`TryCreateConnection` / `BreakSinglePinLink`),
  `layout` (node move, comment bubble), `add_comment` / `comment` (*Create Comment*, comment Details, resize),
  and renames of new objects to their keys (a stale orphan holding the name is moved aside first).
  A query without graph gets one first, exactly as the graph tab does.
  The palette and *Add Test* classes come from a `FGraphNodeClassHelper` built exactly as
  `FEnvironmentQueryEditorModule::CreateEnvironmentQueryEditor` builds the module's class cache (over
  `UEnvQueryNode`, observing `UEnvQueryGenerator_BlueprintBase`): the module's cache exists only after an editor
  opened, and the schema's `GetGraphContextActions` / `GetSubNodeClasses` dereference it unchecked. The node
  templates and `FAISchemaAction_NewNode` / `FAISchemaAction_NewSubNode` are then made as those functions make them.
* `ai.env_query.finalize.v1` (mutating): `Initialize` (as opening / saving in the editor: `SpawnMissingNodes`,
  `CalculateAllWeights`, `UpdateAsset`), `UpdateErrorMessages`; returns the messages.

Python plan order: checks (classes, keys, properties against the class defaults, option order) → deletes →
new options / tests (renamed to keys) → moves → connections → Details → enable flags → layout / comments →
finalize. A document the query already holds changes nothing (no finalize).

Validation after finalize: new errors fail the apply (`EnvQuery.ValidationFailed`, Core rolls back): node
error messages the editor shows (composite generator item-type mismatch, deprecated / missing classes).
Pre-existing errors → `EnvQuery.ValidationError`; warnings → `EnvQuery.ValidationWarning`: a test that does
not support the generator's item type (the runtime drops it), no connected option. Dependents: when the
query's named params change, the assets that reference the query (BehaviorTree *Run EQS Query* / EQS service
nodes, Blueprints with *Run EQS Query*) are listed as `EnvQuery.DependentParams` warnings (their per-node
`QueryConfig` lists the old params until the node is refreshed); Inspect lists `Dependents` always.

An asset open in the Environment Query editor is edited in place and its graph refreshed as after the editor's own
Undo, as for SoundCue (test `EnvQuery.OpenEditorApply`). Opening the editor dirties the package without a change of
the human's (`SpawnTab_UpdateGraph` -> `Initialize` -> `CalculateAllWeights` -> `SetDisplayedWeight`'s `Modify`, the
editor-only test weights the graph displays); the apply's finalize recomputes them, so the apply accepts a package
dirtied only by the open (`AcceptsOpenTimeChanges`, Core's open-time tracking in `AssetDocumentOpenEditor.cpp`). Any
later modification of the package (a human edit) falls back to `DirtyExistingAssetTransactionUnsupported`.

## 4. Rollback, Undo

Every edit records into the apply transaction (the editor's own `Modify` calls). Rollback is Core's package
snapshot (functional equivalence); the EnvQuery derived-state rebuilder notifies the EQS manager
(`NotifyAssetUpdate`) and the graph panel. `RemoveOrphanedNodes` moves objects with `REN_NonTransactional`;
the snapshot's identity restore brings them back. The snapshot's `PostEditUndo` runs
`UAIGraphNode::ResetNodeOwner`, which renames every restored node instance to a fresh unique name (the editor's
own Undo does the same); since keys are those names, the rebuilder captures the names of the query's subobjects
with the snapshot (`CaptureDerivedState`) and renames them back after the restore, so a refused apply leaves
the document, keys included, as it was. (A human Undo in the editor still renames them: the next Extract shows
the new names.) Undo of a successful apply uses the editor's transaction
(as SoundCue: release of the undo history at the end of ApplyFile per design rules §3.1).

## 5. Fidelity plan

* Corpus: engine + enabled plugin content holds **no** EQS asset (UE 5.7: 0 `EnvQuery` packages; feature packs
  are compressed). Fidelity therefore runs on editor-built fixtures: every palette generator and test class,
  non-default settings, query-param bindings, a disabled test, an unconnected option, a composite generator,
  comment boxes. Round trip: extract → create a new asset from the document → native comparison
  (`Options` / instance property text, graph layout, enable flags) and extract equality.
* Runtime equality: both queries run in a test world (`RunInstantQuery`, `AllMatching`) over a few actors;
  item locations and scores must match.
* Free edits on both sides (document vs the editor operations): add / remove / reorder tests, move a test to
  another option, change filter / score, switch generator, bind a query param.
* Rollback (new error, bad property) and Undo; a BehaviorTree with a *Run EQS Query* task on the new query.
  BehaviorTree-profile limit: a new *Run EQS Query* task (or EQS service) is
  refused with `MissingBlackboardKey` on `EQSRequest/EQSQueryBlackboardKey`, a selector the editor leaves None
  and shows disabled while the query template is used (`bUseBBKeyForQueryTemplate` is an inline toggle without
  its own row, so a document cannot switch modes either); giving a key fails the write of the disabled field,
  and the selector completion needs an editable selector that allows None. The BT profile also refuses its own
  extract of a tree with an editor-made task. The test therefore builds the tree through the BT profile, adds the
  task from the BT editor's palette, runs it, and records both refusals as tripwires.

## 6. Decisions

* Asset-specific primitives (not `graph.*`): sub nodes and template-semantics Details are outside the frozen
  graph contract.
* Keys = instance object names (runtime-visible, survive save / duplicate); generator objects keep editor names.
* `EditorObjectProperties::FReferencePolicy::bTemplateSemantics` (shared AssetDocumentEditor change): admits
  and writes `EditDefaultsOnly` properties, as a Details panel showing `DisableEditOnInstance` nodes does.
* No top-level `Properties` (UEnvQuery has no Details properties).
