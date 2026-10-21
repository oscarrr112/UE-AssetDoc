# MetaSound support matrix and corpus results (M0–M4)

Status: 2026-10-18, M0–M4 merged; the black-box AI trial passed; **approved by the user 2026-10-18** (M4 done). Design:
[asset-document-metasound-design.md](asset-document-metasound-design.md) (decisions in §9). Rules:
[design rules](asset-document-design-rules.md) §3 (rollback = functional equivalence), §4
(editor-equivalent code + behaviour test), §5 (support matrix). Reference editor: the MetaSound editor of UE 5.7
(`Engine/Plugins/Runtime/Metasound`: `UMetasoundEditorGraphSchema` actions, `FEditor` commands, the Members /
Interfaces / Details panels, `FMetasoundPagesDetailCustomization`), the document builder it edits through
(`FMetaSoundFrontendDocumentBuilder`) and the Content Browser (New MetaSound Source / Patch, Create Preset). The
agent-facing description is `MCP/schemas/assetdocument/MetaSound.md`.

| Class | Profile | Phase | State |
|---|---|---|---|
| `MetaSoundSource` | `profile.metasound_source.v1` (route `audio_profile`, provider `audio`) | M0–M3 | Extract / Inspect / Diff / Apply / Create |
| `MetaSoundPatch` | `profile.metasound_patch.v1` (same route and provider) | M0–M3 | Extract / Inspect / Diff / Apply / Create |
| Preset of either (`RootGraph.PresetOptions.bIsPreset`) | same profiles, `Body.Preset` | M2 | Create (Create Preset), overrides, Convert From Preset; its graph is read only (derived from the parent) |
| `UMetaSoundBuilderDocument`, `UMetaSoundSettings`, `UMetasoundEditorSettings`, the transient editor graph | — | — | not assets (out of scope, design §1) |

Classification: **Supported**, **Read only**, **Refused** (an error code; the editor cannot do it either, or doing
it would crash the editor), **Not reproduced** (view state or UI without document content), **Later** (named
follow-up).

Summary (51 surfaces): 36 Supported (one on the Default page only), 4 Read only, 4 Refused, 7 Not reproduced,
none Later. The black-box AI trial (design §6.4) passed: all five tasks applied first time, validate
agreed with apply on all 18 applies, 4 of 6 mistakes were fixed from the message alone.

## Editor surfaces

### Graph

| Editor surface | Body location | Class | Mechanism |
|---|---|---|---|
| Palette / context menu: place a node class | `Graphs.<p>.Nodes.<k>.Class` = class name, `Version` (major) | Supported | `AddNodeByClassName(class, major, id, page)` with the highest minor of that major, as the palette (`FMetasoundGraphSchemaAction_NewNode`); unknown class `MetaSound.UnknownNodeClass` with close names |
| Drop a MetaSound asset (Source or Patch as a node) | `Class` = asset object path | Supported | `AddGraphNode` after the drop's checks: `Referenceable` (`MetaSound.NotReferenceable`), reference loop (`AddingReferenceCausesLoop` → `MetaSound.ReferenceCycle`), register-if-outstanding as `UMetaSoundBuilderBase::AddNode` |
| Input node of a member (Members panel drag, "Get" an input; several per input) | `Class: "Input"`, `Member` | Supported | `FInputNodeTemplate::CreateNode` (template node + edge from the hidden input node) |
| Output node (place / delete an output) | `Class: "Output"`, `Member` (one per output per page) | Supported | `SetNodeLocation` on the output's vertex node; delete = its edges and location go, the vertex node stays (as the editor's delete); a second entry `MetaSound.DuplicateOutputNode` |
| Variable Get / Get Delayed / Set | `Class: "Get" / "GetDelayed" / "Set"`, `Member` | Supported | `AddGraphVariableAccessorNode` / `DeferredAccessorNode` / `MutatorNode`; a second Set `MetaSound.DuplicateSetNode` |
| Reroute | `Class: "UE.Reroute"` | Supported | `AddNodeByTemplate(FRerouteNodeTemplate)` typed from its source or consumer; untyped `MetaSound.RerouteType` |
| Add Audio Analyzer Node (wire drag from an audio output) | `Class: "UE.Audio Analyzer"`, `Inputs.Value` | Supported | `AddNodeByTemplate(FAudioAnalyzerNodeTemplate)` as `FMetasoundGraphSchemaAction_NewAudioAnalyzer`; editor-only (registration strips it). Test `FreeEdits` "add an Audio Analyzer" |
| Other template / member classes as `Class` (`UE.Input`, `Variable`, ...) | — | Refused | `MetaSound.UnsupportedNodeClass` (the member and template forms above are the way to place them) |
| Promote To Graph Input / Output / Variable, Promote All To (Common) Graph Inputs | members + member nodes + `Inputs` | Supported as a document | the document states the new member, its node and the edge |
| Wire drag / break link | `Nodes.<k>.Inputs.<pin>` (complete; one edge per input) | Supported | `IsValidEdge` (data type, constructor access → `MetaSound.IncompatibleConnection` with the converter classes the editor would insert) + loop check (`MetaSound.ConnectionCycle`), `RemoveEdgeToNodeInput` + `AddEdge` |
| Automatic converter insertion, auto-wire of a new node to the dragged pin | — | Not reproduced | UI conveniences; a document names the converter node and every edge |
| Pin default value | `Nodes.<k>.Defaults` (complete over the class defaults) | Supported | `SetNodeInputDefault` / `RemoveNodeInputDefault` (the pin's reset), literal by the pin's data type (enum entry names, object paths, arrays); bad value `MetaSound.InvalidLiteral` |
| Delete (nodes) | key absent from `Nodes` | Supported | `RemoveNode` + `RemoveUnusedDependencies`; hidden input / variable nodes are derived from members (`MetaSound.NodeNotDeletable`: remove the member) |
| Move node, hidden node | `Position`, `Hidden: true` | Supported | `SetNodeLocation` / `RemoveNodeLocation`; a new node without `Position` is laid out by graph depth |
| Node comment bubble, Hide Unconnected Pins | `Comment`, `CommentVisible`, `HideUnconnectedPins` | Supported | `SetNodeComment`, `SetNodeCommentVisible`, `SetNodeUnconnectedPinsHidden` |
| Comment box (C) and its Details | `Graphs.<p>.Comments.<k>` (`Text`, `Position`, `Size`, `Color`, `FontSize`, `ColorBubble`, `MoveMode`) | Supported | `FindOrAddGraphComment` / field writes / `RemoveGraphComment` |
| Node Details: configuration (General settings of a configurable class, e.g. Harmonix Tape Stop/Start `NumChannels`) | `Nodes.<k>.Configuration {Type?, Properties}` (complete) | Supported (M2) | `SetNodeConfiguration` + `UpdateNodeInterfaceFromConfiguration` (pins follow); errors `MetaSound.InvalidConfiguration`. Test `Configuration` |
| Update Node Class (node with a newer major) | `Nodes.<k>.Version` = the highest registered major | Supported (M2), Default page only | the editor's controller call (`INodeController::ReplaceWithVersion` + dependency clean-up, `AssetDocumentMetaSoundUpdateNode.cpp`); other targets and other pages `MetaSound.UpdateNodeNotSupported`. Test `UpdateNode` |
| Node of an unregistered class version ("class not found") | `Version` as read | Read only on Create | an extraction showing it cannot be created as is (`MetaSound.UnknownNodeClass` names the registered major); on the existing asset Update Node fixes it |
| Copy / Cut / Paste / Duplicate | — | Not reproduced | a document is the copy format |
| New From Selected (collapse) | — | Not reproduced | the 5.7 schema action is unimplemented (`PerformAction` returns null) |
| Find in MetaSound, Browse, node title / palette view | — | Not reproduced | view state |

### Members and interfaces

| Editor surface | Body location | Class | Mechanism |
|---|---|---|---|
| Members panel: add / delete input, output, variable | `Inputs`, `Outputs`, `Graphs.<p>.Variables` (complete over user members) | Supported | `AddGraphInput` / `AddGraphOutput` / `AddGraphVariable`; delete `ClearMemberMetadata` + `RemoveGraph*` |
| Member Details: name, display name, description, type, constructor, advanced, sort order | member fields, rename by `RenamedFrom` | Supported | `SetGraphInput/OutputName`, `...DisplayName`, `...Description`, `...DataType` (default and widget reset; variables: remove + add, as the editor), `SetGraphInputAccessType`, `...AdvancedDisplay`, `...SortOrderIndex`; errors `MetaSound.DuplicateMember`, `InvalidRename`, `UnknownDataType`, `InvalidMember`, `InvalidName` |
| Member default value | `Inputs.<n>.Default`, variable `Default` | Supported | `SetGraphInputDefault` / `SetGraphVariableDefault` |
| Input widget and range (literal object Details: `ClampDefault`, `Range`, `WidgetType`, orientation, unit, volume settings) | `Inputs.<n>.Widget` (complete over the settings object's defaults) | Supported (M2) | `FindOrCreateGraphInputMetadata`, then the Details write (`PreEditChange` / `PostEditChangeProperty`) on that document subobject; `MetaSound.UnknownWidgetProperty`, `InvalidWidgetValue`. Test `Widget` |
| Interface members (`UE.Source.OnPlay`, ...): default, page defaults, sort order, advanced, widget | `Inputs` / `Outputs` entries of interface members (only differing fields) | Supported | same calls; type and name fixed (`MetaSound.InvalidMember`) |
| Interfaces panel: add / remove an interface | `Interfaces` (user-modifiable only) | Supported | `ModifyInterfaces(bSetDefaultNodeLocations=false)`; fixed ones (`UE.Source`, a Source's output format) `MetaSound.InterfaceNotModifiable`, unknown `MetaSound.UnknownInterface`. Inspect lists every declared interface with its version and `AvailableInterfaces` |
| Class metadata Details (display name, description, author, keywords, categories, Referenceable, Deprecated) | `Metadata` | Supported | `SetDisplayName`, `SetDescription`, `SetAuthor`, `SetKeywords`, `SetCategoryHierarchy`, access flags |

### Asset, presets, pages

| Editor surface | Body location | Class | Mechanism |
|---|---|---|---|
| New MetaSound Source / Patch (Content Browser) | `Action: Create` | Supported | generic factory lifecycle (`UMetaSoundSourceFactory` / `UMetaSoundFactory` → `InitAsset`), then the Body; a failed Create leaves no asset, builder or registry entry (test `Rollback`) |
| Source Details (`OutputFormat`, quality, block / sample rate overrides, attenuation, concurrency, sound class, submix sends, ...) | top-level `Properties` | Supported | generic Details admission, written before the Body (`OutputFormat` swaps the output-format interface through `PostEditChangeOutputFormat`); registration / versioning / cache fields `MetaSound.NotAuthoredProperty` |
| Create Preset (Content Browser) | `Action: Create`, `Body.Preset.Parent` | Supported (M2) | the factory with `ReferencedMetaSoundObject` (restricted-mode Create Preset); parent of another class or missing `MetaSound.InvalidPresetParent`. Test `Preset` |
| Preset input override checkbox / value | preset `Inputs.<n>` (`Default`, `PageDefaults`; unlisted = inherits) | Supported (M2) | `SetGraphInputDefault` / `SetGraphInputInheritsDefault` |
| Preset graph, interfaces, outputs, member settings | — | Read only (derived) | `MetaSound.PresetGraphDerived`; another parent `MetaSound.PresetParentChanged` |
| Convert From Preset | `"Preset": null` | Supported (M2) | `UMetaSoundBuilderBase::ConvertFromPreset`, then the Body |
| Make an existing MetaSound a preset | — | Refused | `MetaSound.CannotConvertToPreset` (the editor has no such command; decision §9.4) |
| Pages Details: Add Page Graph, page delete button | `Graphs.<page>` (`Default` always kept) | Supported (M3) | `AddGraphPage(copy of the last page)` / `RemoveGraphPage`; pages are the project's `UMetaSoundSettings` pages (`MetaSound.UnknownPage`). Tests `Pages`, `PageFreeEdits` |
| Per-page input default widgets | `Inputs.<n>.PageDefaults` | Supported (M3) | `SetGraphInputDefault(..., page)` / remove per page; `PageDefaults.Default` `MetaSound.InvalidBody` |
| Focused page, audition page / platform | — | Not reproduced | editor view and editor-settings state; every op names its page, the build page is left alone |
| Page the settings no longer define | `Graphs.<32 hex digits>` | Read, kept and editable; not addable | the read names it by its id |
| A member read without an input node on a page (older content) | `Inputs.<pin>: "Input:<member>"` | Read only | Apply needs an `Input` node (`MetaSound.InvalidSource`) |

### Compile status, open editor, undo

| Editor surface | Body location | Class | Mechanism |
|---|---|---|---|
| Registration after an edit, node error / warning messages | `Compile {Status, Messages, Registered}` on Extract / Apply / Inspect | Supported | compile primitive: `RegisterGraphWithFrontend(bForceViewSync)` + `ConformObjectToDocument`, the editor's node validation derived from builder queries (`Source: "Validation"`); new errors `MetaSound.ValidationFailed` (rolled back), old ones `MetaSound.ValidationError`, warnings `MetaSound.ValidationWarning` |
| What Play would log (operator build errors, e.g. a variable Get → Set cycle) | `Compile.Messages` with `Source: "Build"` | Supported (M3) | the registered graph built into an operator; new errors `MetaSound.BuildFailed`, old ones `MetaSound.BuildError`. Test `BuildCheck` |
| Loaded MetaSounds that use this one | — | Supported | re-registered as the editor does; `MetaSound.DependentsUpdated`, `MetaSound.DependentHasUnsavedChanges` warnings |
| Apply into an open MetaSound editor | — | Supported (M2) | edits in place through the asset's builder; the editor's tick resyncs its graph; a failed apply rolls back there too. Tests `OpenEditor`, `PagesOpenEditor` |
| Undo / Redo | — | Supported | one transaction per edit call; a successful apply clears undo history that references the asset (direction reset §3.1). Tests `Undo`, `Preset`, `Pages`, `OpenEditor` |
| Opening the editor on older content (input template injection) | Extract shows the injected nodes; Inspect `InjectedNodes` | Supported | the read simulates `FInputNodeTemplate::Inject(false)` on a copy with deterministic ids; the first apply makes it on the asset. Test `EditorOpen` |
| Play / Stop / audition, analyzers' live display | — | Not reproduced | no audio device is needed by any path |
| Import / Export JSON (toolbar) | — | Not reproduced | Export writes the frontend JSON, not a document; the 5.7 Import is a stub that only logs |
| A graph or preset parent that holds Harmonix FusionPatch literals, without an audio device | — | Refused | `MetaSound.AudioDeviceRequired`: registering it in a `-NoSound` editor hits `check(SoundWaveProxy)` (HarmonixDsp) |
| An edit that would open a dialog | — | Refused | `MetaSound.InteractiveEditRefused` |

## Corpus results

The design §6.1 corpus: the Metasound plugin's sources, the TP_UEIntro_BP template's MetaSounds (copied into
`/Game` as creating a project from the template does) and Harmonix content (the plugin is enabled in the test
sandboxes only, decision §9.2). Formats: N = paged document (5.5+), L = legacy (serialized editor graph, upgraded on
load), M = between. Read = `MetaSound.Corpus`: the duplicate's extraction is stable read twice, after an unload and
reload and on a second duplicate; Diff of the extraction is empty; reading dirties, registers and leaves nothing.
Fidelity = `MetaSound.Fidelity` (§6.2): the extraction applied as `Create` on a new `/Game` asset extracts equal
(output node ids aside), validates the same, Diffs empty, re-applies as a no-op and extracts the same after a
reload. Timings in milliseconds, Windows CI PC (Development editor, `-NoSound`, `-j 3`), commit
`15a5f6a1`: extract / inspect of the read, create / no-op re-apply of the fidelity round trip. Mac (M-series, same
commit): the same 15 / 15 stable and 15 / 15 fidelity results, timings in the last two columns.

| # | Asset | Class, format | Nodes / variables / comments | Compile status as read | Read | Fidelity | PC extract / inspect | PC create / no-op | Mac extract / inspect | Mac create / no-op |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | MSS_PlayRandom_Oneshot | Source, N | 25 / 0 / 0 | UpToDate | Stable | Pass | 93 / 84 | 399 / 122 | 38 / 41 | 144 / 48 |
| 2 | MSS_PlayRandom_Loop | Source, N | 24 / 0 / 0 | UpToDate | Stable | Pass | 93 / 74 | 306 / 129 | 38 / 42 | 157 / 52 |
| 3 | MS_TuningFork | Source, N | 44 / 4 / 0 | UpToDateWithWarnings | Stable | Pass | 103 / 76 | 374 / 94 | 40 / 45 | 166 / 56 |
| 4 | MS_Opera | Source, N | 18 / 0 / 0 | UpToDateWithWarnings | Stable | Pass | 90 / 72 | 289 / 87 | 38 / 40 | 143 / 48 |
| 5 | MS_Test_FusionSampler_SynSamples | Source, M | 7 / 0 / 0 | UpToDateWithWarnings | Stable | Refused as expected (`MetaSound.AudioDeviceRequired`) | 79 / 80 | — | 36 / 39 | — |
| 6 | MSP_MidiNoteAdsr | Patch, L | 8 / 0 / 0 | Error | Stable | Refused as expected (`MetaSound.UnknownNodeClass`) | 20 / 36 | — | 13 / 12 | — |
| 7 | MS_MIDIStreamSineGenerator | Patch, N | 11 / 0 / 1 | UpToDate | Stable | Pass | 32 / 33 | 213 / 70 | 9 / 11 | 96 / 17 |
| 8 | MS_TriggerPulseSineGenerator | Patch, N | 12 / 0 / 1 | UpToDate | Stable | Pass | 32 / 37 | 193 / 59 | 9 / 12 | 94 / 16 |
| 9 | MS_Test_MidiPlayerNode_Simple | Source, L | 14 / 0 / 0 | UpToDate | Stable | Pass | 94 / 79 | 277 / 99 | 37 / 41 | 138 / 56 |
| 10 | MS_Test_MidiPlayerNode_Metronome_MidiClockTriggers | Source, L | 42 / 0 / 0 | UpToDate | Stable | Pass | 97 / 93 | 391 / 136 | 40 / 46 | 164 / 63 |
| 11 | MS_Test_MidiPlayerNode_Metronome_AuthoredTimeSigChanges | Source, L | 18 / 0 / 1 | UpToDate | Stable | Pass | 100 / 86 | 328 / 120 | 37 / 41 | 146 / 49 |
| 12 | MS_Test_StepSequencePlayer_PageChanges | Source, L | 50 / 0 / 1 | UpToDate | Stable | Pass | 101 / 89 | 412 / 131 | 42 / 48 | 179 / 66 |
| 13 | MSS_MIDISequencePlayer | Source, N | 26 / 0 / 0 | UpToDate | Stable | Pass | 102 / 77 | 362 / 187 | 40 / 44 | 156 / 51 |
| 14 | MSS_MetronomeClockGenerator | Source, N | 21 / 0 / 0 | UpToDate | Stable | Pass | 91 / 83 | 330 / 96 | 39 / 39 | 144 / 49 |
| 15 | MS_Test_MidiPlayerNode_Metronome_TempoChanges | Source, L | 24 / 0 / 0 | UpToDate | Stable | Pass | 102 / 78 | 345 / 192 | 38 / 41 | 150 / 53 |

Fixtures the engine does not ship (design §6.1 list), made with the editor's calls first and then put through §6.2:

| Fixture (test) | Made by | Covers | PC create / no-op | Mac create / no-op | Status |
|---|---|---|---|---|---|
| OneShot preset, 2 overrides (`Preset`) | Create Preset factory on MSS_PlayRandom_Oneshot + two override checkboxes | preset of a Source, overrides | 529 / 181 | 157 / 61 | UpToDate |
| MS_Opera preset (`Preset`) | the restricted-mode factory (Create Preset) | preset of a template MetaSound | 395 / 218 | 159 / 59 | UpToDate |
| patch preset (`Preset`) | factory on a builder-made patch | preset of a Patch | 228 / 36 | 94 / 16 | UpToDate |
| configured source (`Configuration`) | builder + `SetNodeConfiguration` on a Harmonix Tape Stop/Start node | node configuration, configured pins | 229 / 92 | 120 / 40 | UpToDateWithWarnings (OneShot On Finished) |
| two-page source (`Pages`) | `AddGraphPage` + page input defaults (test-project page `Low`) | pages, `PageDefaults` | 553 / 199 | 217 / 71 | UpToDate |
| 7.1 source with spatial interfaces (`Formats`) | factory, `OutputFormat` = 7.1 (Details), Interfaces panel adds `UE.Attenuation`, `UE.Spatialization`, `UE.Source.Orientation`, `UE.Listener.Orientation`; an attenuation input feeds a Sine wired to the eight 7.1 outputs | 7.1 output interface, interface members as input nodes | 406 / 169 | 135 / 53 | UpToDateWithWarnings (OneShot On Finished) |
| stereo-format patch (`Formats`) | factory patch, Interfaces panel adds `UE.InputFormat.Stereo` and `UE.OutputFormat.Stereo`, inputs wired to outputs | patch format interfaces | 264 / 67 | 79 / 14 | UpToDate |
| AI synth (`FromScratch`) | one document `Create` without ids | see below | 459 (create) | 158 (create) | UpToDate |

### Free edits (document vs the editor's calls on duplicates)

Each step is applied to one duplicate by a document and made on another by the calls the editor's actions make
(document builder, `FInputNodeTemplate::CreateNode`, `ModifyInterfaces` as the Interfaces panel, the `OutputFormat`
Details edit, page add / remove, the Details writes); graph form and validation status must be equal after every step.

| Test | Asset | Edits |
|---|---|---|
| `FreeEdits` | MSS_PlayRandom_Oneshot, MS_Opera | insert a State Variable Filter; add an Audio Analyzer on an audio output; delete a node; reconnect to another output pin; add an input with two input nodes; rename it; remove it; change an input's type; add a variable with Get, Set and Get Delayed; add and edit a comment; move nodes; `OutputFormat` Stereo ↔ Mono with the outputs re-wired; remove and re-add `UE.Source.OneShot` (PC 405–745 ms, Mac 195–232 ms per apply) |
| `Preset` | OneShot preset | override a default, override another, inherit again, override with the parent's value, Convert From Preset (PC 462–618 ms, Mac 216–236 ms) |
| `Configuration` | Tape Stop/Start source (Harmonix) | three channels with the third fed; one channel and a longer ramp (PC 314 / 367 ms, Mac 162 / 160 ms) |
| `UpdateNode` | MSP_MidiNoteAdsr | `Version` 0 → 1 on the unregistered MidiNoteTrigger node (PC 425 ms, Mac 126 ms) |
| `Widget` | new source | every input widget setting the Details panel writes |
| `PageFreeEdits` | MSS_PlayRandom_Oneshot | add page Low; move, re-default and add a node on Low; connect on Low; Low default of an input; add page High; delete a node on Low; remove the Low default; remove page Low (PC 550–760 ms, Mac 249–297 ms) |

### AI from scratch, rollback, undo

- `FromScratch`: one `Create` without ids writes a stereo one-shot synth (inputs with widget ranges, wave player,
  envelope, filter, reroute, a patch made first and used as a node, a GetDelayed / Set counter, a comment, OneShot
  On Finished wired): `UpToDate`, Diff empty, re-apply saves nothing; a preset of it overrides `Cutoff`; the guide's
  two examples apply as written; seven typical errors (unknown class, unknown pin, type mismatch, cycle, unknown
  page, preset on a regular MetaSound, open-ended reroute) and a preset with a graph fail at their document paths and
  change nothing.
- `Rollback`: a new validation error, an incompatible connection and a loop restore the asset; a failed `Create`
  leaves no object, file, registry, asset-manager entry or builder.
- `Undo`: human edit, save, AI apply, failed apply; undo / redo keep the asset whole.
- `Python`, `ListTypes`, `EditorOpen`: read path, discovery kinds, open-time injection equals the editor's.
- `SparseDefaults.MetaSound`: a partial struct in the source Details, an input `Widget` with fields at their defaults.

### Test results

Commit `15a5f6a1`. PC: the MetaSound and SparseDefaults.MetaSound groups, 20 / 20 Success; against the
`6d902305` pool baseline the only change is the new `Formats` (`- -> Success`), no regression. Mac:
the same groups plus `AtomicFile`, 31 / 31 Success. All MetaSound tests log engine warnings
(`LogMetaSound` PreSaveAsset / unregistered interface versions of the corpus content), which count as Success.

| Test (`UEAssetDoc.MetaSound.*`) | Content | PC | Mac |
|---|---|---|---|
| `Corpus` | the 15 corpus assets: read stability, Diff, no side effects | Success | Success |
| `Fidelity` | the 15 corpus assets through §6.2 (13 pass, 2 refused as expected) | Success | Success |
| `Formats` | 7.1 source with the four spatial interfaces; stereo-format patch | Success | Success |
| `FreeEdits` | 15 edits on MSS_PlayRandom_Oneshot and MS_Opera, incl. the Audio Analyzer | Success | Success |
| `Preset` | three preset fixtures; five override / inherit / Convert From Preset edits | Success | Success |
| `Configuration` | Harmonix Tape Stop/Start node configuration, two edits | Success | Success |
| `UpdateNode` | MSP_MidiNoteAdsr MidiNoteTrigger v0 → v1 | Success | Success |
| `Widget` | new source, every input widget setting | Success | Success |
| `Pages` | two-page fixture, the guide's two-page example, page add / remove | Success | Success |
| `PageFreeEdits` | eight page edits on MSS_PlayRandom_Oneshot | Success | Success |
| `PagesOpenEditor` | a page apply while the MetaSound editor is open | Success | Success |
| `OpenEditor` | an apply while the MetaSound editor is open, failed apply rolled back there | Success | Success |
| `BuildCheck` | variable Get → Set cycle: `MetaSound.BuildFailed` at the node; read shows the build error | Success | Success |
| `FromScratch` | AI synth, its preset, the guide's examples, seven typical errors | Success | Success |
| `Rollback` | validation error, incompatible connection, loop; failed Create leaves nothing | Success | Success |
| `Undo` | human edit / save / AI apply / failed apply; undo and redo | Success | Success |
| `EditorOpen` | input-template injection on older content equals the editor's | Success | Success |
| `Python` | Python read path | Success | Success |
| `ListTypes` | `metasound_node` / `metasound_datatype` / `metasound_interface` discovery | Success | Success |
| `SparseDefaults.MetaSound` (`UEAssetDoc.`) | partial Details struct, `Widget` with fields at defaults | Success | Success |

## Known limits

- Opening a MetaSound editor in a `-NoSound` editor can hit an engine assert (Harmonix `KeyzoneSettings.cpp:48`,
  while the node palette registers Harmonix Fusion test assets). Run asset editors with an audio device.
- Tests run in `-NoSound` editors. MS_Test_FusionSampler_SynSamples (and any graph whose literals hold a Harmonix
  FusionPatch) reads but is refused for Create / Update with `MetaSound.AudioDeviceRequired`; with an audio device
  the same paths register it. The open-editor tests unload such MetaSounds first because the engine itself
  registers loaded MetaSounds when the first MetaSound editor of a session opens.
- MSP_MidiNoteAdsr's MidiNoteTrigger node is version 0, which 5.7 does not register: its extraction cannot be
  created as is (`MetaSound.UnknownNodeClass` names version 1); on the asset, `Version: 1` is the editor's Update Node.
- Update Node acts on the Default page only (the editor's controller path edits the default page); on other pages it
  is refused with `MetaSound.UpdateNodeNotSupported` (delete the node and add the new version).
- Minor node versions are derived (auto-updated on every registration); only the major is authored.
- Pages are "Experimental" in 5.7 and come from the project's MetaSound settings; a document cannot add a page the
  settings do not define.
- The editor's node validation is derived from builder queries (the private `FGraphBuilder` cannot be linked);
  `EditorOpen` and the fidelity status comparison check it against the editor's own open path.
- The analyzer node of the editor (`UE.Audio Analyzer`) has no persisted settings; its display is editor-only.

## Follow-ups (not in this matrix's scope)

- Fidelity with an audio device (FusionPatch content) needs an editor run without `-NoSound`.
