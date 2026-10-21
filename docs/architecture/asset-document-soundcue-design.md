# AssetDocument SoundCue profile: design

Status: design only, 2026-10-06. No product code.
Governing rules: [design rules](asset-document-design-rules.md) §3 (rollback = functional
equivalence), §3.1 (Undo history), §4 (editor-equivalent code; raw paths only where the editor does the same, in their
own file with a behaviour test; no private headers, no unexported symbols) and §5 (support matrix); the shared graph
contract [graph primitives v1](asset-document-graph-primitives.md). The reference is the **Sound Cue editor** of UE
5.7 (`Engine/Source/Editor/AudioEditor`: `USoundCueGraphSchema`, `USoundCueGraphNode`, `FSoundCueEditor`) and the
Content Browser's "Create Cue". Legacy generators are not a reference. Acceptance is editor fidelity on real content,
free edits, and an AI writing cues from scratch.

Paths: `RT` = `Engine/Source/Runtime/Engine`, `AED` = `Engine/Source/Editor/AudioEditor`.

## 1. Classes in scope

| Class | Scope | Reason |
|---|---|---|
| `/Script/Engine.SoundCue` | **Exact profile** `profile.sound_cue.v1` | The sound node graph (`SoundCueGraph`, `AllNodes`, `FirstNode`, per-node `ChildNodes`) is not Details-editable; the generic route sees only the cue's Details (coverage audit row 6). |
| All concrete `USoundNode` classes (20 in Engine: Wave Player, Dialogue Player, Random, Mixer, Concatenator, Modulator, Continuous Modulator, Attenuation, Looping, Delay, Switch, Branch, Enveloper, Oscillator, Doppler, Sound Class, Wave Param, Mature, Group Control, Crossfade by Distance, Crossfade by Param, Quality Level; plus plugin nodes such as SoundMod's `USoundNodeModPlayer` and project nodes) | Same profile, node `Class` | They are the nodes of the graph; their Details are reflected properties. Project node classes work without code (the palette lists every non-abstract `USoundNode` child). |
| Comment boxes (`UEdGraphNode_Comment` in the cue graph) | Same profile, `Comments` | The schema's comment action; graph-only (ignored by compile). |
| `USoundCueTemplate` subclasses (SoundCueTemplates plugin: `SoundCueContainer`, `SoundCueDistanceCrossfade`) | Generic profile (their own Details); graph refused | Their graph is rebuilt from template settings (`OnRebuildGraph`); it is derived. The exact profile matches the class exactly, so these keep the generic route. |
| `USoundCueGraph`, `USoundCueGraphNode*`, `USoundCueGraphSchema` | Internal | Editor-only mirror objects the profile edits through graph primitives; never authored as properties. |
| Other audio assets (SoundWave, SoundClass, SoundMix, Submix, Attenuation, Concurrency, MetaSound) | Out (generic / MetaSound profile) | Not node graphs of this kind. |

## 2. Editor data model: authored vs derived

The cue graph exists twice: the editor graph (`USoundCue::SoundCueGraph`, editor-only, serialized) and the runtime
tree (`FirstNode` + every node's `ChildNodes`). The **editor graph is the authoring source**: every structural edit in
the Sound Cue editor goes through the schema (`TryCreateConnection`, `BreakPinLinks`, `BreakNodeLinks`), the graph
node (`AddInputPin`, `RemoveInputPin`) or `DeleteSelectedNodes`, and each of them ends with
`USoundCue::CompileSoundNodesFromGraphNodes()` (the file-local `FSoundCueAudioEditor` in `AED/Private/SoundCueGraph.cpp`),
which rebuilds `ChildNodes` (input pin *i* → child *i*, `LinkedTo[0]` or null) and `FirstNode` (the Root pin) from the
links. Details edits never compile. Load (`PostLoad`) never compiles either; it only removes graph nodes whose sound
node class is gone (`RemoveNullNodes`).

### Authored (Extract emits, Apply writes)

- The graph nodes: one `USoundCueGraphNode` per `USoundNode` (`SoundNode` instanced), position (`NodePosX/Y`),
  comment bubble (`NodeComment`, `bCommentBubbleVisible`), and the number of input pins (= child slots; Add / Delete
  Input).
- The links: each input pin's single link, the Root pin's link.
- Each `USoundNode`'s Details values (`Weights`, `InputVolume`, `CrossFadeInput`, `GroupSizes`, `SoundWaveAssetPtr`,
  `bLooping`, modulation ranges, curves, ...).
- Comment boxes (`UEdGraphNode_Comment`: text, position, size, colour, font size, bubble flags, move mode).
- Cue Details (`VolumeMultiplier`, `PitchMultiplier`, `bOverrideAttenuation`, `AttenuationOverrides`,
  `SubtitlePriority`, `bPrimeOnLoad`, `bExcludeFromRandomNodeBranchCulling`, and the `USoundBase` Details: sound
  class, attenuation, concurrency, submix / bus sends, source effect chain, priority, virtualization, ...).

### Derived (never emitted)

| State | Recomputed by |
|---|---|
| `FirstNode`, `ChildNodes` of every node | `CompileSoundNodesFromGraphNodes` (finalize) |
| Sizes of the per-child arrays (Random `Weights`, Mixer / Concatenator `InputVolume`, Crossfade `CrossFadeInput`, Group Control `GroupSizes`) | `SetChildNodes` / `InsertChildNode` / `RemoveChildNode` overrides (values stay authored) |
| `AllNodes` (every sound node of the cue) | `ConstructSoundNode` (add) and node delete (remove) |
| Pin names (`Input`, `Input2`, ...; Switch "Parameter Unset", "0", "1"...; Branch "True"/"False"/"Parameter Unset"; Quality Level names) | `CreateInputPin` / `RenameNodePins` from `GetInputPinName` |
| `Duration`, `MaxDistance`, `TotalSamples`, `bHasDelayNode`, `bHasConcatenatorNode`, `bHasPlayWhenSilent`, attenuation-node flag | `CacheAggregateValues` (cue `PostEditChange`, `Serialize` on save) |
| Random `bSoundCueExcludedFromBranchCulling`, `HasBeenUsed`, `NumRandomUsed` | cue `PostEditChangeProperty` (`RecursivelySetExcludeBranchCulling`), transient |
| Wave player hard `SoundWave` pointer | `LoadAsset` (wave player `PostEditChangeProperty`, cue `PostLoad`) |
| `CookedQualityIndex`, Quality Level `CookedQualityLevelIndex` | cook only |
| Graph `NodeGuid`s | `CreateNewGuid` on node creation and on **duplication** (`USoundCueGraphNode::PostDuplicate`) |

Consequence for identity: graph node GUIDs change when the asset is duplicated, so they cannot be the durable key.
The sound node's **object name** (`SoundNodeRandom_2`) survives save, reload and duplication (§3.3).

## 3. Document shape

### 3.1 Example (small, complete: randomized, pitch-varied footstep with an attenuation override)

```jsonc
{
  "SchemaVersion": 1, "Target": "/Game/Audio/SC_Footstep", "Class": "/Script/Engine.SoundCue",
  "Action": "CreateOrUpdate",
  "Properties": {"VolumeMultiplier": 0.8, "SoundClassObject": "/Game/Audio/SCL_SFX.SCL_SFX",
                 "AttenuationSettings": "/Game/Audio/ATT_Small.ATT_Small"},
  "Body": {
    "Root": {"Input": "Mod", "Position": [200, 0]},
    "Nodes": {
      "Mod":  {"Class": "Modulator", "Position": [-50, 0],
               "Properties": {"PitchMin": 0.9, "PitchMax": 1.1}, "Inputs": ["Rand"]},
      "Rand": {"Class": "Random", "Position": [-300, 0],
               "Properties": {"Weights": [1.0, 1.0, 0.5], "bRandomizeWithoutReplacement": true},
               "Inputs": ["Step1", "Step2", "Scuff"]},
      "Step1": {"Class": "WavePlayer", "Position": [-600, -150],
                "Properties": {"SoundWaveAssetPtr": "/Game/Audio/SW_Step1.SW_Step1"}},
      "Step2": {"Class": "WavePlayer", "Position": [-600, 0],
                "Properties": {"SoundWaveAssetPtr": "/Game/Audio/SW_Step2.SW_Step2"}},
      "Scuff": {"Class": "WavePlayer", "Position": [-600, 150],
                "Properties": {"SoundWaveAssetPtr": "/Game/Audio/SW_Scuff.SW_Scuff"},
                "Comment": "rarer", "CommentVisible": true},
      "Spare": {"Class": "Delay", "Position": [-300, 300], "Properties": {"DelayMax": 0.2}, "Inputs": [null]}
    },
    "Comments": {
      "Variations": {"Text": "Variations", "Position": [-650, -220], "Size": [400, 520]}
    }
  }
}
```

`Spare` is an orphan (not reachable from `Root`): legal, kept, reported as an info message, exactly as the editor
shows a dangling node.

### 3.2 Regions

All regions: **omitted = keep, present = complete state**.

| Region | Shape | Notes |
|---|---|---|
| `Root` | `{Input, Position?}` | `Input`: node key or `null` (the Root pin's link, i.e. `FirstNode`). `Position` of the output node; omitted keeps it. |
| `Nodes` | `{"<key>": Node}` | Complete: a key not listed is deleted (the editor's delete: links broken, removed from `AllNodes`, compiled). |
| `Comments` | `{"<key>": {Text, Position, Size, Color?, FontSize?, BubbleVisible?, ColorBubble?, MoveMode?}}` | Complete. Keys are comment object names, like nodes. |

Node:

| Field | Meaning | Omitted |
|---|---|---|
| `Class` | `USoundNode` class: short name without the `SoundNode` prefix for engine nodes (`WavePlayer`, `Random`, `Mixer`, `Modulator`, `ModulatorContinuous`, `Attenuation`, `Looping`, `Delay`, `Switch`, `Branch`, `Concatenator`, `Enveloper`, `Oscillator`, `Doppler`, `SoundClass`, `WaveParam`, `Mature`, `GroupControl`, `DistanceCrossFade`, `ParamCrossFade`, `QualityLevel`, `DialoguePlayer`), otherwise the class path. A different `Class` on an existing key replaces the node (delete + create). | required |
| `Inputs` | ordered array, one entry per input pin (child slot): a node key or `null` (unconnected). Its length is the number of input pins. | the class's starting connectors with nothing connected (`CreateStartingConnectors`: 1 for most, 4 for Switch, 3 for Branch, 2 for Mature / Group Control / crossfades, project quality-level count for Quality Level, 0 for players) |
| `Properties` | the node's Details values, complete over editable properties (an omitted property has the value a new node of that class has; extract lists only differences). Per-child arrays must have one entry per `Inputs` slot (`GroupSizes`: slots − 1); omitted = the editor's default per slot (1.0, or `FDistanceDatum` defaults). | defaults |
| `Position` | `[x, y]` integers | keep (new node: column layout left of its first parent, as the palette places it at the cursor) |
| `Comment`, `CommentVisible` | node comment bubble | none / false |

Child-count rules are the node's own (`GetMinChildNodes` / `GetMaxChildNodes`, ≤ 32; Branch exactly 3; Quality Level
exactly the project's quality-level count; players 0). A violation fails `SoundCue.ChildCount` with the allowed
range. A node may be the input of several parents (the graph is a DAG; the editor and the SoundCueTemplates plugin
share wave players this way). Cycles fail `SoundCue.ConnectionCycle` (the schema's `ConnectionCausesLoop`).

`Properties` (top-level, sparse, generic admission) are the cue's Details. Body-owned and refused there
(`SoundCue.NotAuthoredProperty`): `FirstNode`, `AllNodes`, `SoundCueGraph`; derived and refused: `Duration`,
`MaxDistance`, `TotalSamples`, `bHasDelayNode`, `bHasConcatenatorNode`, `bHasPlayWhenSilent`, `CookedQualityIndex`.

### 3.3 Identity, keys and free edits

- **Keys are sound node object names** (letters, digits, `_`, `-`), as Material's expression keys. Extract writes the
  editor's names (`SoundNodeWavePlayer_3`); a new key creates a node and renames its sound node object to the key
  (the Material profile's established practice; raw rename in its own file, behaviour test: a renamed node saves,
  reloads, duplicates and plays). Object names survive save, reload and Content Browser duplication; graph
  `NodeGuid`s do not (regenerated on duplicate), so they are neither keys nor emitted.
- **Comments** follow the same rule with the comment node's object name.
- **Pins are positions**, not names: `Inputs[i]` is input pin *i* (`USoundCueGraphNode_Base::GetInputPin(i)`), which
  is what compile maps to `ChildNodes[i]`. Pin names are derived (and change on Switch insert / remove), so the
  document never uses them.
- Re-applying an extract is a no-op; an AI document's keys become the object names, so it keeps matching what its
  first apply created.

| Free edit | Document change | Operation (§4) |
|---|---|---|
| Insert a node into a link | new key; parent's `Inputs[i]` → new key; new node's `Inputs` = old child | spawn, connect ×2 |
| Delete a node | remove the key (and references to it) | node remove (links break, compile) |
| Reconnect / disconnect | change / null an `Inputs` entry, `Root.Input` | connect (replaces the pin's link) / break |
| Add / remove a child slot (Random gets a 4th variation) | `Inputs` longer / shorter (+ per-child property entries) | Add Input / Delete Input on the graph node |
| Reorder variations | permute `Inputs` (and the matching `Weights`) | break + connect per changed pin, then property write |
| Replace a node's class (Mixer → Random) | change `Class` | delete + create, relink |
| Change a wave | `Properties.SoundWaveAssetPtr` | Details edit (→ `LoadAsset`) |
| Move / annotate | `Position`, `Comment`, `Comments` | layout, comment bubble, comment node |

### 3.4 Discovery

- `graph.actions.enumerate.v1` (through `list_types` with `kind: "sound_node"`): every node class the palette offers,
  with display name, category, min / max children and the default `Properties` of a new node.
- `inspect_asset_document_target` on a cue: the extract plus `NodeFacts` (per node: class, pin names in slot order,
  min / max children, whether it is reachable from Root), `Waves` (all referenced wave assets with duration and
  looping), `Status` (§5), and the derived `Duration` / `MaxDistance` the editor shows.

## 4. Editor-equivalent operations: the shared graph primitives

### 4.1 Mapping onto `graph.*`

SoundCue is the first non-Blueprint graph asset whose whole Body maps onto the shared graph contract. Python owns the
document (keys, `Inputs` arrays, properties) and plans `graph.*` calls; C++ adds one schema adapter and one host
adapter, registered by `AssetDocumentAudio`. No `audio.soundcue.*` edit primitive family is needed.

| Editor action | Editor path | Graph primitive | Adapter work |
|---|---|---|---|
| Place a node (palette / context menu) | `FSoundCueGraphSchemaAction_NewNode::PerformAction(Graph, nullptr, Location, bSelectNewNode=false)`: transaction "New Sound Node", `ConstructSoundNode` (adds to `AllNodes`, `SetupSoundNode`), `CreateStartingConnectors`, position, `AutowireNewNode`, cue `PostEditChange` | `graph.node.spawn.v1` with `action_key` `SoundCue:Node:/Script/Engine.SoundNodeRandom` | schema adapter `EnumerateActions` / `ResolveAndSpawn` build the action for `SoundNodeClass` and call `PerformAction`. `ConnectToSelectedNodes` sees no selection (no open editor: §4.3) |
| Comment box | `FSoundCueGraphSchemaAction_NewComment::PerformAction` | spawn `SoundCue:Comment` | same adapter |
| Key the node | (none: editor names are automatic) | `graph.node.rename.v1` | adapter renames the sound node (or comment) object to the key; isolated raw file, Material precedent |
| Connect a pin | `USoundCueGraphSchema::TryCreateConnection` (loop / direction checks in `CanCreateConnection`; an occupied input is replaced, `BREAK_OTHERS`), then compile | `graph.pin.connect.v1` `{from: {node, "Output", output}, to: {node, <input pin i>, input}}` | none (generic schema path); `Graph.ConnectDisallowed` carries the schema's reason (loop, same node) → `SoundCue.ConnectionCycle` |
| Break a link | `USoundCueGraphSchema::BreakPinLinks` (transaction "Break Pin Links", compile) | `graph.pin.break.v1` | none |
| Add Input / Delete Input (node context menu) | `USoundCueGraphNode::AddInputPin` (`CanAddInputPin`) / `RemoveInputPin(pin)` (only while above `GetMinChildNodes`); both transact and compile | `graph.node.dynamic_pin.add.v1` / `.remove.v1` | schema adapter `CanAddDynamicPin` / `AddDynamicPin` / `RemoveDynamicPin` call those two methods |
| Delete a node | `FSoundCueEditor::DeleteSelectedNodes` (private): per node `CanUserDeleteNode` (not Root), `BreakAllNodeLinks`, `FBlueprintEditorUtils::RemoveNode`, remove its sound node from `AllNodes`, then compile | `graph.node.remove.v1` | schema adapter `RemoveNode` replicates those steps (the method is private to the editor module): isolated raw file + behaviour test comparing with the editor's delete on an open editor in automation |
| Node Details | Details panel on the graph node's `SoundNode` (`PostEditChangeChainProperty`: wave player → `LoadAsset`) | `graph.node.property.set.v1`, container `sound_node` | adapter `ResolvePropertyContainer(node, "sound_node")` returns `GraphNode->SoundNode` |
| Move / comment bubble | node drag / comment edit | `graph.node.layout.set.v1` | none |
| Comment box text / size / colour | comment Details | `graph.node.property.set.v1` container `node` | none |
| Root link | connect into the Root node's `Root` pin | `graph.pin.connect.v1` / `break` | `DescribeNode`: Root has `implicit_role: "root"` |
| (implicit after each structural edit) | `CompileSoundNodesFromGraphNodes`, cue `PostEditChange` (`CacheAggregateValues`) | `graph.finalize.v1` once at the end | host adapter `Finalize` (§5) |

Schema adapter (`USoundCueGraphSchema`, tag `SoundCue`), C++ in `AssetDocumentAudio/Private/SoundCue/`:

- `EnumerateActions`: non-abstract `USoundNode` classes, filtered like `UpdateSoundNodeList` (class viewer filters,
  hidden / deprecated classes out), keys `SoundCue:Node:<class path>`, plus `SoundCue:Comment`. The palette's
  "from selected waves" variant is not offered (documents name waves in properties).
- `DescribeNode`: action key from `SoundNode` class; Root → `implicit_role: "root"`; extras `name` (sound node object
  name), `child_limits: {min, max}`, `input_pins: [names in slot order]`, `reachable: bool`.
- `ResolvePropertyContainer("sound_node")`, `RemoveNode`, dynamic pins, `graph.node.rename.v1` behaviour as above.

Host adapter (`USoundCue`):

- `EnumerateGraphs`: the one `SoundCueGraph`, role `other`, path `SoundCue`, no create / rename / remove.
- `Finalize`: §5.1.
- `NotifyGraphChanged`: `Graph->NotifyGraphChanged()` (what the editor's `PostUndo` / `NotifyPostChange` do).
- Registered also as the Core derived-state rebuilder `AssetDocumentAudio.SoundCue` (§5.3).

Python: `profiles/sound_cue.py` (regions, keys, `Inputs` ↔ pin index, per-child array defaults, plan) on
`graphs/` (`model.py`, `reconcile.py`, `executor.py`), with `graphs/schemas/sound_cue.py` (`SchemaAdapter`: tag
`SoundCue`, identity by `extras.name`, Root implicit id, a class change → replace). Reused: `PropertiesRegion` for
`Properties`, `generic_factory_lifecycle` for create.

### 4.2 Plan order

1. `Properties` (cue Details).
2. Deletes (nodes, comments not listed; nodes whose `Class` changed).
3. Spawns (new keys, comments) + rename to key.
4. Pin counts: `dynamic_pin.add` / `.remove` until each node has `len(Inputs)` input pins (removals from the end;
   a removal in the middle is "remove pin *i*": `RemoveInputPin` on that pin, which shifts later slots, as the
   editor's Delete Input does; the planner chooses the cheaper of shift vs relink).
5. Links: for every slot whose source differs: `connect` (replaces) or `break`; `Root`.
6. Node `Properties` (after pin counts, so per-child arrays have their final length), comment bubbles, positions.
7. `graph.finalize.v1`.

### 4.3 Create, open editors, environment

- **Create.** Generic factory lifecycle with `USoundCueFactoryNew` (no dialog; `InitialSoundWaves` empty → an empty
  cue whose `PostInitProperties` made the graph and Root). The Content Browser's "Create Cue" from selected waves
  (one wave → a player wired to Root; several → a Random over players) is just a document the AI writes; Inspect on
  a SoundWave could later suggest it, not needed now.
- **Open editor.** A cue open in the Sound Cue editor is edited in place. The editor works on the real asset; after
  the apply (or its rollback) the editor is refreshed as after its own Undo: selection cleared (the schema's
  `ConnectToSelectedNodes` reacts to it) and `NotifyGraphChanged` (test `SoundCue.OpenEditorApply`).
- **No audio device needed.** `USoundCue::PostEditChangeProperty` only restarts playing `UAudioComponent`s;
  `-NoSound` editors are fine. Preview playback (`PlayCue`, `PlayNode`) is not reproduced.
- **Undo.** Every schema / node call records its own transaction; §3.1 applies (success clears history referencing
  the cue; failure after such calls clears it too).

## 5. Compile, validation and rollback

### 5.1 "Compile"

There is no compile button; the compile is `CompileSoundNodesFromGraphNodes`, which every structural edit already
runs. `Finalize` makes the end state certain and validates what runtime code assumes but nothing checks:

1. `CompileSoundNodesFromGraphNodes`, then `SoundCue->PostEditChange()` (`CacheAggregateValues`,
   `RecursivelySetExcludeBranchCulling`) and `Graph->NotifyGraphChanged()`.
2. Invariants (errors): every graph node has a sound node in `AllNodes` whose outer is the cue; input pin count ==
   `ChildNodes.Num()` within the node's min / max (the engine's `SetChildNodes` silently ignores out-of-range arrays);
   per-child array sizes match (Mixer / Concatenator / crossfade arrays are indexed unchecked at runtime: a mismatch
   crashes playback); no cycle (`GetAllNodes` from each node); Delay has its child slot (its `GetDuration` reads
   `ChildNodes[0]`); Quality Level matches the project's quality levels.
3. Warnings / infos: unconnected child slots on nodes that need input (Modulator, Attenuation, Looping, Delay, ...),
   wave players without a wave, nodes not reachable from Root, `FirstNode` null ("cue is silent"), Random weights all
   zero.

Result: `{status, messages[{severity, message, node_guid, pin?}]}` (the graph contract's finalize shape); Python maps
node GUIDs to keys.

### 5.2 Failure policy and status shape

As Material: errors the cue did not have before fail the apply (`Graph.FinalizeFailed` mapped to
`SoundCue.ValidationFailed` at `/Body/Nodes/<key>`) and roll back; pre-existing errors return as
`SoundCue.ValidationError` warnings, warnings as `SoundCue.ValidationWarning`. Extract / Apply / Inspect carry
`Compile: {Status, Messages[{Severity, Message, Node, Path}]}` (`UpToDate`, `UpToDateWithWarnings`, `Error`) through
a compile-status describer.

### 5.3 Rollback

- Existing cue: Core package snapshot restores the cue, graph, graph nodes and sound nodes (objects created by the
  apply are removed; the snapshot breaks their pin links first, a rule the graph module already has). The rebuilder
  `AssetDocumentAudio.SoundCue` then runs the editor-undo equivalent: `SoundCue->PostEditUndo` (deepest first by
  Core), `CompileSoundNodesFromGraphNodes` is **not** run (the restored runtime tree is already consistent and
  compiling would also be correct), `CacheAggregateValues`, `NotifyGraphChanged`.
- Created cue: factory rollback deletes the package.
- Dependents: none structurally (a cue references waves and settings; nothing is compiled from a cue). Playing audio
  components are restarted by `PostEditChangeProperty`, as in the editor.

## 6. Fidelity acceptance plan

### 6.1 Content

The install ships 39 cues; only four node types occur in them (Wave Player, Random, Mixer, Modulator):

| # | Asset | Mounted by default | Content |
|---|---|---|---|
| 1 | `/Engine/EditorSounds/Notifications/CompileSuccess_Cue` | yes | single wave player |
| 2 | `/Engine/VREditor/Sounds/VR_object_grabbed_loop_Cue` | yes | looping wave player |
| 3 | `/Engine/VREditor/Sounds/UI/Click_on_Button_Cue` | yes | single wave player |
| 4 | `/NetworkPredictionExtras/Audio/SciFiLaserShot_Cue` | NetworkPredictionExtras (beta, off) | Modulator → Random (3 weights) → 3 players |
| 5 | `/NetworkPredictionExtras/Audio/SFX_Ship_HitGround_01_Cue` | same | Random + Modulator with ranges |
| 6 | `/NetworkPredictionExtras/Audio/SFX_PhysicsAttract_Cue` | same | Mixer (4 inputs, `InputVolume`), looping players, cue attenuation override |
| 7 | `/VirtualScouting/Audio/SC_Click_03`, `/VirtualScouting/Audio/SC_buttonPush` | VirtualScouting (off) | Mixer + Modulator |
| 8 | `Templates/TemplateResources/Standard/XRFramework/Content/Audio/Fire_Cue` (copied to `/Game/XRFramework/Audio`) | template copy | looping player, `NaturalSound` attenuation override |

Built by the test with the editor's own calls (schema actions, `AddInputPin`, `TryCreateConnection`, Details writes),
saved, reloaded and used as input, so every node type is covered:

- `SC_AllNodes`: one of every engine node class in a DAG (a wave player shared by two parents), Switch with 5 slots,
  Branch, Group Control with 3 groups, Concatenator, Crossfade by Distance / Param with non-default data, Enveloper
  with curves, Continuous Modulator, Attenuation override, Sound Class override, Wave Param, Mature, Looping
  (count 3), Delay, Oscillator, Doppler, comments, comment bubbles, an orphan node;
- `SC_Quality`: Quality Level with the test project's quality levels (two levels configured in the sandbox);
- `SC_FromFactory`: `USoundCueFactoryNew` with three `InitialSoundWaves` (the Content Browser's Create Cue).

### 6.2 What is compared

For each cue: duplicate into `/Game`, save → Extract → apply as a new asset (`Create`) → compare:

- **document**: the two extracts are equal;
- **graph**: same nodes by key with class, properties (reflected compare), input pin count, link per slot, Root link,
  positions, comments;
- **runtime tree**: `FirstNode` and each node's `ChildNodes` equal by key, `AllNodes` the same set, per-child arrays
  equal;
- **derived**: `Duration`, `MaxDistance`, `bHasDelayNode`, `bHasConcatenatorNode`, `bHasPlayWhenSilent` equal after
  `CacheAggregateValues`;
- **status**: equal.

Then re-apply → no-op; reload → extract equal.

### 6.3 Free edits (document vs the editor's calls on duplicates of #4, #6 and `SC_AllNodes`)

Insert an Attenuation node between Modulator and Random; delete a wave player; add a 4th variation to Random with a
weight; delete the second Random input (slots shift as the editor's Delete Input does); reorder Mixer inputs with
their volumes; replace Mixer by Concatenator; change a wave; retarget Root; add and resize a comment; grow a Switch
from 4 to 6 slots (pin names update); move nodes. The editor-side edits use the same schema / graph node methods the
UI commands call; graphs, runtime trees and statuses must match.

Undo test: human edit in the Sound Cue editor, save, AI apply, Ctrl+Z behaviour per §3.1.

### 6.4 AI from scratch

- **Python test:** one `Create` builds a weapon cue: Modulator → Random (4 weighted variations) → players, plus a
  Concatenator tail, a Switch on a `Surface` parameter with three branches, an attenuation override, comments; status
  `UpToDate`; the cue is playable (`IsPlayable`, `Duration` > 0). Then error cases at document paths with rollback:
  unknown class, cycle, 33 inputs, Branch with 2 inputs, `Weights` length mismatch, unknown key in `Inputs`, a Delay
  with no slot, a wave path that is not a SoundWave.
- **Black-box trial** (MCP tools + schema section only): "make a footstep cue with 4 random variations and slight
  pitch variation", "add distance-based crossfade between a near and a far loop", "add a looping ambience with a
  random one-shot layer mixed in".

## 7. Risks, unknowns and phases

### 7.1 Risks

| Risk | Effect | Answer |
|---|---|---|
| Node delete lives in the private `FSoundCueEditor` | same result needed | small isolated raw file (`BreakAllNodeLinks`, `FBlueprintEditorUtils::RemoveNode`, `AllNodes.Remove`, compile) + behaviour test against the editor's own delete |
| Rename of sound node objects to keys is not something the editor does | unusual object names | Material already does it; names are only identity; behaviour test covers save / reload / duplicate / play |
| `InsertChildNode` overrides (Mixer, Concatenator) grow their arrays even when the base refuses past 32 | array / child mismatch | we never call `InsertChildNode` directly: pins change through `AddInputPin` (guarded by `CanAddInputPin`) and compile; the 32 limit is checked in Python first |
| Quality Level child count depends on project settings | documents not portable between projects | validated against the current settings (`SoundCue.ChildCount` names the levels); Inspect lists them |
| `PerformAction` reacts to the editor's selection | spurious links | open editors refused until S2; spawn never passes a `FromPin` |
| Thin real corpus (4 node types; 3 cues need non-default plugins) | coverage | editor-built fixtures cover every node class; plugins optional (§8) |
| Graph-primitive gaps (positional pins, a property container other than `template`) | contract change | `input_pins` extras and the `sound_node` container are additive (adapter-specific values the contract already allows); `graph.node.rename.v1` meaning for SoundCue documented in the contract's adapter section (Root approval needed per the contract header) |

### 7.2 Phases (each independently mergeable)

| Phase | Content | Acceptance |
|---|---|---|
| **S0 adapters + read** | schema + host adapters in `AssetDocumentAudio`, `graph.*` read paths for cues, Python Extract / Inspect / Diff (Apply refused `SoundCue.ReadOnly`), node-class discovery, compile-status describer | extract stable on #1–#8 and the fixtures; `graph.query.v1` snapshots of the fixtures match the editor's pins |
| **S1 apply** | all regions, spawn / connect / break / dynamic pins / remove / rename / properties / layout / comments, finalize + validation, rebuilder, open-editor refusal, create through the factory | §6.2 on all assets; §6.3 edits; §6.4 Python test; rollback after a failed validation and a failed connect; Undo test |
| **S2 AI surface** | `MCP/schemas/assetdocument/SoundCue.md`, apply into an open Sound Cue editor (refresh with `NotifyGraphChanged`) if decided, black-box trial | §6.4 trial; matrix approved |

## 8. Decisions

Decided by Root / the user on 2026-10-06 (recorded with S0 / S1):

1. **Node identity = the sound node's object name**, renamed to the document key on create (as Material names its
   expressions); graph `NodeGuid`s are neither keys nor emitted. Comment boxes: the comment node's object name.
2. **Inputs are positional**: `Inputs` has one entry per input pin (child slot), never pin names.
3. **Corpus plugins**: `NetworkPredictionExtras` is enabled in the test sandboxes; `VirtualScouting` is optional
   (its cues run when mounted). Node coverage comes from the editor-built fixtures.
4. **Graph contract additions approved**: the `sound_node` property container, the `input_pins` / `child_limits` /
   `reachable` extras, and the SoundCue meaning of `graph.node.rename.v1` (rename the identity object; the shared
   handler asks the schema adapter first through `IAssetDocumentGraphSchemaAdapter::RenameNode`). Input pin *i* also
   answers to the pin alias `"#<i>"` (direction input).
5. **Open-editor apply** is in place plus the editor's Undo refresh (`SoundCue.OpenEditorApply`).
