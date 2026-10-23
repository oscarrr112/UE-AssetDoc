# SoundCue support matrix and corpus results

Design: [asset-document-soundcue-design.md](asset-document-soundcue-design.md)
(§8 Decisions). Rules: [design rules](asset-document-design-rules.md) §3 (rollback = functional
equivalence), §4 (editor-equivalent code + behaviour test), §5 (support matrix). Reference editor: the Sound Cue editor
of UE 5.7 (`USoundCueGraphSchema`, `USoundCueGraphNode`, `FSoundCueEditor`) and the Content Browser's New Sound Cue.
The agent-facing description is `MCP/schemas/assetdocument/SoundCue.md`; the graph contract additions are
[graph primitives](asset-document-graph-primitives.md) §10.

| Class | Profile | State |
|---|---|---|
| `SoundCue` | `profile.sound_cue.v1` (route `audio_profile`) | Extract / Inspect / Apply / Diff / Create |
| `SoundCue` made from a Sound Cue Template | same | read only (`SoundCue.UnsupportedAsset`: its graph is derived from the template) |

Classification: **Supported**, **Read only**, **Refused** (an error code; the editor cannot do it either, or the
runtime would crash on it).

## Editor surfaces

| Editor surface | Body location | Class | Mechanism |
|---|---|---|---|
| Cue Details (volume, pitch, attenuation, sound class, concurrency, ...) | top-level `Properties` | Supported | shared Properties region; derived / internal fields refused (`SoundCue.NotAuthoredProperty`) |
| New Sound Cue (Content Browser) | `Action: Create` | Supported | `USoundCueFactoryNew` through the generic factory lifecycle (empty cue + Output node) |
| Create Cue from selected waves | — | Supported as a document | the AI writes the players / Random; the factory's `InitialSoundWaves` result round trips (`SC_FromFactory`) |
| Palette / context menu: place a node | `Nodes.<k>.Class` | Supported | `graph.node.spawn.v1` → the palette action `FSoundCueGraphSchemaAction_NewNode::PerformAction` (built through its reflected struct), then renamed to the key |
| Comment box (C) | `Comments` | Supported | `GetCreateCommentAction`; text / position through layout, `Size` as the corner drag (`ResizeNode`), other fields as comment Details |
| Wire drag, Break Link | `Inputs`, `Root.Input` | Supported | schema `TryCreateConnection` / `BreakPinLinks`; a loop → `SoundCue.ConnectionCycle` |
| Add Input / Delete Input | length of `Inputs` | Supported | `USoundCueGraphNode::AddInputPin` / `RemoveInputPin` on the slot whose removal keeps the most links (a slot deleted in the middle shifts the later ones, pin names as the editor leaves them) |
| Delete (node) | node absent from `Nodes` | Supported | `FSoundCueEditor::DeleteSelectedNodes` is private: the same result from `AssetDocumentSoundCueRawEdits.cpp`; behaviour test `SoundCue.EditorDelete` presses Delete in an open editor and compares |
| Node Details | `Nodes.<k>.Properties` | Supported | `graph.node.property.set.v1`, container `sound_node` (`PostEditChangeChainProperty` as the Details panel; wave player → `LoadAsset`) |
| Per-child arrays (Random `Weights`, Mixer / Concatenator `InputVolume`, crossfade `CrossFadeInput`, Group Control `GroupSizes`) | node `Properties` | Supported | follow Add / Delete Input like the editor, then set; wrong length → `SoundCue.ChildArrayLength` |
| Node move, comment bubble | `Position`, `Comment`, `CommentVisible` | Supported | `graph.node.layout.set.v1` |
| Compile / validation | — | Supported | `graph.finalize.v1`: `CompileSoundNodesFromGraphNodes`, `CacheAggregateValues`; new errors fail at the node path and roll back (`SoundCue.ValidationFailed`); errors the cue already had and warnings are reported (`SoundCue.ValidationError` / `.ValidationWarning`) |
| Child limits (32, Branch 3, Quality Level = project levels, players 0) | `Inputs` | Refused when exceeded | `SoundCue.ChildCount` before any edit (`CanAddInputPin` agrees) |
| A Delay without its input slot | `Inputs: []` | Refused | `SoundCue.ValidationFailed` before any edit: `USoundNodeDelay::GetDuration` reads input 0 unchecked, the next compile would crash the editor |
| Apply into an open Sound Cue editor | — | Supported | in place, then the editor's Undo refresh (selection cleared, `NotifyGraphChanged`); test `SoundCue.OpenEditorApply` |
| Preview playback (Play Cue / Play Node) | — | not reproduced | no audio device is needed by any path |

## Corpus results

Timings: Development editor, several test suites running at the same time, in milliseconds. `extract` = first Extract of the duplicate, `create` = the extracted document applied as a new
`/Game` asset (`Action: Create`), `extract new` = Extract of the created cue, `reapply` = the same document applied
again (no-op: nothing written, nothing saved), `reload` = Extract after a package reload. Each round trip compares
(§6.2 of the design) the documents, the graph (nodes by key with class, properties, input pins, link per slot, Root
link, positions, comments), the runtime tree (`FirstNode`, `ChildNodes`, `AllNodes`, per-child arrays), the derived
values (`Duration`, `MaxDistance`, `bHasDelayNode`, `bHasConcatenatorNode`, `bHasPlayWhenSilent`) and the status.

| Asset | Content | nodes | extract | create | extract new | reapply | reload | Result |
|---|---|---|---|---|---|---|---|---|
| CompileSuccess_Cue | single player | 1 | 47 | 230 | 24 | 89 | 52 | round trips |
| VR_object_grabbed_loop_Cue | looping player | 1 | 50 | 225 | 26 | 84 | 55 | round trips |
| Click_on_Button_Cue | single player | 1 | 53 | 228 | 26 | 88 | 45 | round trips |
| SciFiLaserShot_Cue (NetworkPredictionExtras) | Modulator → Random → 3 players | 5 | 37 | 227 | 28 | 90 | 50 | round trips |
| SFX_Ship_HitGround_01_Cue (NetworkPredictionExtras) | Random + Modulator ranges | 5 | 40 | 205 | 27 | 73 | 55 | round trips |
| SFX_PhysicsAttract_Cue (NetworkPredictionExtras) | Mixer, cue attenuation override | 9 | 46 | 247 | 29 | 90 | 59 | round trips |
| Fire_Cue (XRFramework template copy) | looping player, `NaturalSound` override | 1 | 53 | 210 | 27 | 87 | 61 | round trips |
| SC_AllNodes (editor-built) | every engine node class, shared player, comments, orphan | 28 + 2 comments | 70 | 448 | 43 | 88 | 65 | round trips |
| SC_Quality (editor-built) | Quality Level, two project levels | 3 | 49 | 205 | 26 | 84 | 57 | round trips |
| SC_FromFactory | New Sound Cue factory with three waves | 4 | 47 | 212 | 27 | 62 | 53 | round trips |
| SC_Click_03, SC_buttonPush (VirtualScouting) | Mixer + Modulator | — | — | — | — | — | — | not mounted in the sandboxes (optional, decision 3) |

### Free edits (document vs the editor's own calls on duplicates)

`SoundCue.FreeEdits` applies a document to one duplicate and makes the same edit with the editor's calls (palette
action, `TryCreateConnection`, `BreakPinLinks`, `AddInputPin` / `RemoveInputPin`, Details edit, the Delete replica) on
another; graph, runtime tree, derived values and status must be equal, and the document must Diff empty afterwards. Every
step is equal; apply times 234–335 ms (LaserShot 234–272, PhysicsAttract 248–252, AllNodes 325–335). The weapon cue
of `SoundCue.FromScratch` is created in 279 ms.

| Duplicate of | Edits |
|---|---|
| SciFiLaserShot_Cue | insert an Attenuation between Modulator and Random; add a 4th weighted variation; delete Random input 2; delete a player; change a wave, retarget Root, move nodes, comment bubble, add a comment box |
| SFX_PhysicsAttract_Cue | reorder the Mixer inputs with their volumes; replace the Mixer by a Concatenator on the same key |
| SC_AllNodes | grow a Switch from 5 to 7 slots; delete a Switch slot; change a node's class on the same key (inputs kept) |

### AI from scratch, rollback, Undo

- `SoundCue.FromScratch`: one `Create` writes a weapon cue (Modulator → Random with four weighted variations,
  Concatenator tail, Switch on `Surface`, attenuation override, comments); status `UpToDate`, playable, the document
  Diffs empty and a re-apply saves nothing. Eight refused documents (unknown class, cycle, 33 inputs, Branch with 2,
  `Weights` length, unknown key, a Delay without its slot, a cue where a wave belongs) fail at their document path and
  leave no asset.
- `SoundCue.Rollback`: a failed validation and a failed connect leave graph and runtime tree as before.
- `SoundCue.Undo`: human edit, save, AI apply, Ctrl+Z / redo behaviour per [design rules](asset-document-design-rules.md) §3.1.
- `SoundCue.OpenEditorApply`: with the editor open, apply, a rolled-back failed apply, a human edit
  with Undo / Redo and a second apply; the open graph panel shows each state.
- `SoundCue.RenamedNodes`: keys survive save, reload and duplication.

## Engine notes

- **Private headers.** `SoundCueGraphSchema.h` includes the private `SSoundCuePalette.h`; the schema is found by class
  path, the palette action is built through its reflected struct (`/Script/AudioEditor.SoundCueGraphSchemaAction_NewNode`)
  and the comment action comes from `GetCreateCommentAction`.
- **Delete Input does not rename pins.** `RemoveInputPin` keeps the remaining pins' names (`Input`, `Input3`, ...); the
  document's `Inputs` is positional, so names never matter, and Apply removes the same slot the editor would.
- **Group Control `GroupSizes`** has one entry fewer than the inputs (the last group takes the rest), padded with 1
  when an input is added.
- **Quality levels** come from `UAudioSettings::QualityLevels` (read by reflection); a Quality Level node has one
  input per level and documents are checked against the current project's levels.
- **Float32 values.** Node and comment Details are float32; Extract writes the shortest decimal that stores as the
  same float32 (0.6, not 0.6000000238418579), and a document's float32-exact numbers canonicalize the same way, so an
  AI's 0.92 Diffs empty.
- **Older content.** The NetworkPredictionExtras cues were saved with an empty engine version (a load warning only).
