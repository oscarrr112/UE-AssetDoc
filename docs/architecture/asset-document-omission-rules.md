# AssetDocument omission rules

The core rule (guide `AssetDocument.md`, "Omitted Versus Empty"): an omitted Body key keeps the current state;
inside a listed entry, omitted optional fields take their defaults; deviations are stated per profile. Blueprint graphs (`UbergraphPages: []`), DataTable row `Values` and AnimBlueprint `Anim:` node settings
already follow "omitted = default". This table lists the remaining "omitted = keep" fields: the rule, where it lives, and why it
is kept or what would change it.

"Silent gap" means this pattern: extract writes the field only when it is non-default, so deleting it from an
extracted document keeps the live value and Diff reports no change. Where extract always writes the field, deleting
it is a deliberate edit and "keep" is a partial-update shorthand.

| Profile / field | Rule (implementation) | Extract writes | Silent gap | Decision and reason |
| --- | --- | --- | --- | --- |
| Blueprint `Variables[].DefaultValue` | keep; `null` too (`ublueprint_members.py` default op only when stated) | always (non-null) | no (always written) | Keep. Stated in Blueprint.md. A variable's default is its value, not a setting: "keep" lets a document add flags or move a variable to a category without restating (or knowing) a large default (struct, map). The other variable settings already reset. |
| UDS / UDE member `DefaultValue` | keep; `null` resets (`user_defined_struct.py`) | always (codec-supported types) | no | Keep, same reason; `null` is the explicit reset. Stated in UserDefinedTypes.md. |
| AnimBlueprint `SM:` states / conduits / aliases / transitions settings | **default** (`anim.py` `omitted_property_resets`, `anim_blueprint_runtime._leftover_settings` cover `SM:`; the state machine adapter reports `property_defaults`) | non-default only (against a newly placed node: a new transition has `bAllowInertializationForSelfTransitions` true) | no | Parity with `Anim:` nodes. `LogicType` follows `BlendGraph`. Stated in AnimBlueprint.md. |
| BlendSpace `Axes` fields, existing `Samples` fields | keep; a new sample takes the defaults | always | no | Keep. Stated in BlendSpace.md; an axis edit (range, grid) that restates every field gains nothing. |
| Skeleton socket `Location` / `Rotation` / `Scale` / `ForceAlwaysAnimated` | keep on an existing socket; a new socket 0 / 0 / 1 / true | always | no | Keep. Stated in Skeleton.md; matches the Skeleton editor's per-field Details edits. |
| BehaviorTree comments (existing `Id`) | keep, merged per sub-field (`behavior_tree_codec.py`) | always | no | Keep. Stated in BehaviorTree.md; editor metadata (position, size, color). |
| StateTree state fields, node flags | **default** (`state_tree.py` `plan_states`, `_node_dto`, `_covers_states` / `_covers_nodes`) | non-default only | no | An existing state's / node's omitted field is a new one's. Kept: the state's kind (`Type`, `LinkedSubtree`, `LinkedAsset`), like its lists, so a name-only entry does not turn a linked state into a plain one; Diff notes it (`StateTree.OmittedKept`, Info). A task's `ConsideredForCompletion` the editor does not let one edit is not reset. Stated in StateTree.md. |
| StateTree node `Node` / `Instance` values | **default** (the read's `node_reset` / `instance_reset`; Diff / Verify through `extract_with_defaults`) | non-default only | no | Parity with BT nodes and AnimBP `Anim:` nodes. |
| StateTree node `Bindings` | omitted key = keep; listed = complete (`{}` clears) | only when non-empty | **yes** (an extract of a node without bindings omits the key; deleting a node's last binding by deleting the key keeps it) | Not changed; stated ("complete binding list; omitted = keep"). Use `{}` to clear. |
| StateTree transition `Conditions` | keep; other transition fields reset | when present | yes | Not changed; stated in StateTree.md. |
| Niagara user / system parameter `Value` | keep; a new parameter gets the type's default | whenever the parameter has a value | no (written for every parameter) | Keep. Stated in Niagara.md; a declaration-only row (type, category) should not reset a tuned value. |
| Niagara emitter / renderer `Properties` key | omitted key = keep; listed = complete | always (sparse dict) | only when the whole key is deleted | Keep (stated). The listed form is already complete, so a deleted property inside it is reset. |
| Sequencer `Settings`, `MetaData` | **listed = complete** (`level_sequence._sparse_diff`, `omitted_values`, `level_sequence_apply.settings_op`), except the timing fields `DisplayRate`, `TickResolution`, `Evaluation`, `PlaybackRange`: keep | timing fields always; flags, `Clock`, `CustomClock`, `SelectionRange`, `Author` / `Notes` only when set | no | Complete for the flags, clock, selection range and MetaData. The timing fields keep: a `Settings` listing only `PlaybackRange` must not reset the rates; Diff says so (`Sequencer.OmittedKept`, Info). With `Clock: "Custom"` an omitted `CustomClock` keeps the clock object (the editor has no "none"). Stated in Sequencer.md. |
| Generic `Properties` | sparse, keep (`generic.py`) | non-default only (`diff_only`) | yes, by design | Keep (not changed). Generic Properties cover every reflected property of any class; a complete-state rule would reset everything a document does not mention. |

## Decision: audit result

No field was changed. Every "keep" above is stated in its profile guide (so it does not contradict the guide), and
none makes an apply leave a state a person could not have produced with the same edit in the editor: "keep" is the
editor's "do not touch that row". The inconsistency already removed for AnimBP and DataTable remains for the rows marked
**yes**: a deleted field is silently kept and Diff says nothing. Candidates for a follow-up, in order of risk:
Sequencer `Settings` flags and `MetaData`, AnimBlueprint `SM:` node settings, StateTree node values and fields.

## Decision: follow-up

The three **yes** rows above (Sequencer `Settings` flags and `MetaData`, AnimBlueprint `SM:` node settings,
StateTree node values and fields) now follow the "omitted = default" rule: omitted = the editor's default for a new node or
setting, reported by Diff and applied. The rows still marked "keep" are stated in their guides; the one that keeps a
field inside an otherwise complete object (Sequencer timing fields, a StateTree state's kind) also says so in Diff
(`Sequencer.OmittedKept`, `StateTree.OmittedKept`).
StateTree node `Bindings` and transition `Conditions` keep the audit decision (omitted key = keep, `{}` / `[]` clears).
