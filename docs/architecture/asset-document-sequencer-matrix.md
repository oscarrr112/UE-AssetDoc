# Sequencer support matrix and corpus results (S3 remaining tracks, S4 Template Sequences, S5 Director, events, groups)

Status: 2026-10-09. Design:
[asset-document-sequencer-design.md](asset-document-sequencer-design.md) (decisions in §8, S1 notes in §8.1, S2 notes in
§8.2, S3 notes in §8.3, S4 notes in §8.4, S5 notes in §8.5). Rules: [design rules](asset-document-design-rules.md) §4 (editor-equivalent code + behaviour
test), §5 (support matrix). Reference editor: Sequencer (`Engine/Source/Editor/Sequencer`, track editors in
`MovieSceneTools`). The agent-facing description is `MCP/schemas/assetdocument/Sequencer.md`.

| Class | Profile | Phase | State |
|---|---|---|---|
| `LevelSequence` | `profile.level_sequence.v1` (`seq_profile`, provider `seq`, owner `Cinematics` = 12) | S5 | Extract / Inspect / Diff / Create / Update: settings, bindings, tracks (every track kind of §7.2 S1–S3, plugin tracks by reflection), sections, channels, parameter sections, conditions, time warps, folders, markers, metadata, sequence Details, the Director Blueprint, event keys, director bindings, section / node groups, binding references into sub / shot sequences |
| `TemplateSequence` | `profile.template_sequence.v1` (`seq_profile`, provider `seq`) | S4 | Extract / Inspect / Diff / Create / Update: `Settings`, `Root` (root spawnable: bound actor class, template, spawnable Details, tracks, component bindings), `Tracks`, `Folders`, `Markers`, `Groups` (S5); change bound class. Create needs `Root.Class` (the factory's class picker) |
| `CameraAnimationSequence` | `profile.camera_animation_sequence.v1` (`seq_profile`, provider `seq`) | S4 | as `TemplateSequence`; created with a Cine Camera Actor root (its factory); camera actor classes only (`Sequencer.CameraClassRequired`) |

Classification: **Authored** (extracted exactly and written through the editor's mechanism), **Read** (extracted,
authoring in the phase named), **Kept** (listed as found, preserved untouched on update), **Later** (phase named).

## Editor surfaces

| Editor surface | Body location | Class | Mechanism (write; `seq.edit.v1` op) |
|---|---|---|---|
| Sequence settings: display rate, tick resolution (with key / section migration), evaluation type, clock, playback / selection / view range, locks, marker flags, read-only | `Settings` | Authored | `UMovieScene` setters as the Sequencer toolbar; tick resolution via `UE::MovieScene::TimeHelpers::MigrateFrameTimes` + `SetTickResolutionDirectly` as the toolbar's resolution menu (`settings`) |
| Binding tree: possessables, component children, names, rename (`RenamedFrom`, GUID kept), order, delete | `Bindings[]`, `Children` | Authored | `UMovieScene::AddPossessable` / `RemovePossessable` (the tree's Delete: children, tracks, references, folder entries), F2 rename (`FMovieScenePossessable::SetName` + `SetObjectDisplayName`), `SortingOrder` (`binding.add / remove / rename / order`) |
| Possessable locators (one or several references) | `Bind` | Authored | `FUniversalObjectLocator` from string, `GetBindingReferences().AddBinding` (`binding.set`) |
| Spawnables (`MovieSceneSpawnableActorBinding`) with their actor template | `Spawnable{Class, Template, ...}` | Authored | `UMovieSceneSpawnableActorBinding` custom binding (as Convert to Spawnable makes it) + its template actor; template Details reset to the archetype then written, `PostEditChange` per template object (`binding.set`) |
| Replaceable / plugin custom bindings | `CustomBinding{Class, Properties}` | Authored | custom binding object by reflection (`binding.set`) |
| Conversion possessable ↔ spawnable | binding kind change | Authored | the binding's GUID, tracks and children kept (`binding.set`) |
| Binding tags | `Tags` | Authored | `FMovieScenePossessable::Tags` (`binding.set`) |
| Mixed locator + custom binding on one binding | raw `References` | Kept (`Sequencer.UnsupportedBindingShape`) | — |
| Tracks: property tracks of every type, transform, spawn, visibility, attach, path, actor reference, skeletal animation, material, camera shake, event, audio, camera cut, shot, subsequence, fade, slomo, level visibility, data layer, CVar, MPC, time warp | `Tracks[]` (global / per binding) | Authored (common kinds of §7.2; others by class path) | `AddTrack` / `RemoveTrack`, property binding (`SetPropertyNameAndPath`), track Details, rows, mute, order (`track.add / remove / set / order`) |
| Sections: range, row, priority, active / locked, blend, easing, pre / post roll, when finished, section Details (camera cut binding, sub-sequence + parameters) | `Sections[]` | Authored | `CreateNewSection` / `AddSection` / `RemoveSection`, `UMovieSceneSection` setters, section Details; cycle check before sub-section writes (`section.add / set / remove`) |
| Channels: float / double curves (every interpolation, user / break tangents, weights, stated tangents, infinity), bool, byte, enum, integer, string, text, object path, camera shake trigger | `Channels.<name>` | Authored | channel data `Set` in time order, `AutoSetTangents` (stated tangents kept), `SetDefault`, extrapolation; > 64 keys in columns, chunks via `channel.append` that name the earlier keys whose stated tangents are kept (`stated` runs) |
| Event keys: endpoint picker, "Create Quick Binding" / "Create Event Endpoint", bound object pin, payload; trigger and repeater sections | `Channels.Value.Keys[].Event`, repeater `Event` `{Endpoint, BoundObjectPin?, Payload?}`; section `Class` `Trigger` / `Repeater` | Authored (S5) | `FMovieSceneEventUtils::SetEndpoint` to the Director node found by name, payload variables by pin (`section.add / set`, channel writes); a missing endpoint is `Sequencer.UnknownEventEndpoint` |
| Parameter sections: material, MPC, component / primitive material, custom primitive data, particle parameters, Niagara parameters, DMX fixture patches (any stored array of channel-holding structs) | section `Parameters[]` `{Type?, Name?, Properties?, Channels?, Parameters?}` | Authored | the "+ Parameter" menus: parameter arrays rebuilt from the list, then the channel proxy (`PostEditImport`, as a paste); custom primitive data names checked against the proxy's index rule; lists over one op continue with `parameters.append`; channel chunks address an entry by index path |
| Time warp (track and sub / shot `TimeWarp` variants) | section `Properties.TimeWarp`: number (play rate), `{FixedFrame}`, `{FrameRate}`, `{Loop}`, `{Clamp}`, `{LoopFloat}`, `{ClampFloat}`, `{Custom: {Class, Properties}}` | Authored | `FMovieSceneTimeWarpVariant` setters; a custom getter is a new instanced object (class edited in place when unchanged), then the proxy rebuilt |
| Conditions (track, section, track row; group / platform / scalability / plugin classes) | `Properties.ConditionContainer.Condition`, track `RowConditions {"<row>": condition}` | Authored | the Details condition picker: instanced `{Class, Properties}` objects (nested groups), row metadata by reflection |
| Plugin channel types without reflected key arrays | `{"Unsupported": "<type>"}` | Kept (`Sequencer.UnsupportedChannel`) | — |
| Channel overrides (procedural channels) | section `"Unsupported": "ChannelOverrides"` | Kept (`Sequencer.UnsupportedChannelOverride`) | registry untouched when the section is rewritten (`Apply.Preserved`) |
| Section groups (section context menu Group / Ungroup) | section `Group` (number shared by a group's sections) | Authored (S5) | ungroup all, then `UMovieScene::GroupSections` per group (`section_groups.set`); compared as a partition |
| Node groups (Group Manager: new group, add / remove selected, rename, delete) | `Groups[{Name, Members}]`, members `{Folder}` / `{Binding}` / `{Track, Node?}` / `{Raw}` | Authored (S5) | `UMovieSceneNodeGroupCollection` / `UMovieSceneNodeGroup` edited in place by position (`groups.set`); a `Raw` path stays unresolved (a new track that would match it is named otherwise) |
| Binding reference into a sub / shot sequence (camera cut into a shot, the binding picker's sub-sequence entries) | `{"Binding": <path in that sequence>, "Sequence": <section path> \| [paths]}` | Authored (S5) | the picker's sequence ID stack (`FSequenceBindingTree`), depth ≤ 4; tracks holding such references written last; IDs no section reaches stay raw (`Sequencer.UnsupportedRemoteBinding`, kept) |
| Control Rig sections | generic channels | Read (`Sequencer.ControlRigTrack`) | Control Rig profile owns authoring |
| Folders (name, colour, nesting, contents) | `Folders[]`, binding / track `Folder` | Authored | `UMovieSceneFolder` tree rebuilt as a whole (`folders.set`) |
| Marked frames (label, colour, comment, determinism fence) | `Markers[]` | Authored | `UMovieScene::MarkedFrames` written as listed (`AddMarkedFrame` would invent labels) (`markers.set`) |
| Sequence metadata (author, notes) | `MetaData` | Authored | `ULevelSequenceMetaData` (`metadata.set`) |
| Director Blueprint ("Open Director Blueprint", then the Blueprint editor: variables, functions, custom events, macros, dispatchers, interfaces, class defaults) | `Director` (Blueprint profile keys) | Authored (S5) | hosted Blueprint: `blueprint.member.*` and the K2 graph executor on the sequence's handle; `director.create` (`GetOrCreateDirectorBlueprint`) / `director.remove` (refused while called: `Sequencer.DirectorInUse`); compiled after the apply (errors roll back) |
| Director bindings ("Replaceable from Director Blueprint" and other dynamic bindings) | `CustomBinding.Properties.DynamicBinding` `{Endpoint, ResolveParamsPin?, Payload?}` | Authored (S5) | `FMovieSceneDynamicBindingUtils::SetEndpoint` (`binding.set`) |
| Other sequence Details | `Properties` | Authored | generic Details write |
| Sequence Validator rules + profile checks | Inspect `Status` | Read | `seq.status.v1` before / after: new errors fail the apply (`Sequencer.ValidationFailed`) |
| Dependents (parent sequences, Blueprints, level sequence actors) of deleted / renamed bindings | apply warnings | Warn | `Sequencer.DependentsAffected` (asset registry referencers; a parent sequence and a Blueprint "Get Sequence Binding" node tested) |
| Sequence or a parent open in the Sequencer | — | Supported (S6): in place, the open Sequencers refresh | test `Sequencer.OpenEditorApply` |
| Undo | — | Core rule | the edit is one transaction; Core clears undo history that references the package |

## Template Sequences (S4)

| Editor surface | Document | State | Mechanism |
|---|---|---|---|
| New Template Sequence dialog (class picker) / New Camera Animation Sequence | Create with `Root.Class` | Authored | the asset's factory (`TemplateSequenceFactoryNew` with `BoundActorClass`; `CameraAnimationSequenceFactoryNew`) through the generic factory lifecycle, then the plan |
| Root spawnable (`UTemplateSequence` spawnable 0, legacy `FMovieSceneSpawnable`) | `Root{Name, Class, Template, Ownership, ContinuouslyRespawn, NetAddressableName, LevelName, SpawnTransform, Tags, Folder, Tracks, Children}` | Authored | `binding.set` entry `Root`: template Details as for level spawnables, spawnable fields by reflection (sparse against a fresh spawnable; `NetAddressableName` default as `UMovieScene::AddSpawnable` sets it) |
| Details > Bound Actor Class | `Root.Class` | Authored | `template.root {class}` (`AssetDocumentSequencerTemplateRoot.cpp`): `FTemplateSequenceEditorUtil::ChangeActorBinding`'s data steps (new spawnable from the spawn register's spawners, `BoundActorClass`, `MoveBindingContents`, `RemoveSpawnable`, edit-change notifications); the preview actor, viewport placement and camera lock are not reproduced |
| Component bindings ("+ Track > component") | `Children[{Name, Class, Component, Tags, Tracks}]` | Authored | possessable parented to the root, `AddChildPossessable`, `BoundActorComponents` (component name; omitted = `Name`, `""` = none) |
| Global tracks, folders, markers, settings | as Level Sequences | Authored | the Level Sequence plan (`level_sequence_apply.Plan` over `/Body/Root`) |
| Status | `Sequencer.RootClassInvalid` (error at `/Body/Root/Class`), no root / root-template class mismatch, component not found on the root (`UnassignedBindingsAndAssets`) | Authored | `seq.status.v1` |
| Thumbnail | — | not authored | asset thumbnail |


PC (Windows, Development editor, 2026-10-08, `Sequencer.Apply.CorpusRoundTrip`). For each asset: extract, Create a new
sequence from the document, extract equal (`MetaData.Author` left out, §6.2 (4)), every curve evaluates equal at
keys, midpoints and ±1 tick (§6.2 (3)), status equal, the unchanged document re-applies as a no-op, equal after unload +
reload. 20 of 20 in 10.0 s (S2: 18 of 18, #12 skipped; S5 run 2026-10-09: 20 of 20 in 10.7 s, node groups included). Reading alone (`Sequencer.Corpus`): 19 of 19 in 4.8 s.

| Asset | Extract ms | Create ms | No-op re-apply ms | Doc chars | Curves equal |
|---|---|---|---|---|---|
| fixture (Read test content) | 35 | 243 | 68 | 3184 | 18 |
| #1 `NewLevelSequence` | 32 | 240 | 66 | 3645 | 12 |
| #2 `SEQ_Orbit_Studio_Dark` | 32 | 185 | 60 | 3099 | 25 |
| #3 `SEQ_Truck_Studio_White` | 22 | 193 | 64 | 2013 | 13 |
| #4 `Camera1_Seq` | 24 | 180 | 67 | 3240 | 23 |
| #5 `Seq_CineCameraActor01` / `02` / `03` | 31 / 30 / 30 | 173 / 175 / 174 | 68 / 67 / 69 | 3311 / 3184 / 2836 | 14 / 13 / 14 |
| #6 `Seq_Master_Cameras` | 25 | 178 | 62 | 954 | 18 |
| #7 `Seq_MasterSunSky` | 29 | 145 | 61 | 834 | 18 |
| #8 `Seq_SunSkyEquinox` / `SummerSolstice` / `WinterSolstice` | 29 / 18 / 28 | 162 / 198 / 157 | 61 / 64 / 62 | 875–876 | 1 each |
| #9 `TimeofDaySeq` | 23 | 164 | 57 | 737 | 1 |
| #10 `LS_Shot_01_626` / `02_773` / `03_316` | 30 / 19 / 27 | 201 / 141 / 190 | 54 / 61 / 46 | 863 / 828 / 828 | 1 each |
| #11 `LS_StillBlank` (legacy spawnable) | 28 | 210 | 56 | 2069 | 13 |
| #12 `LS_FixtureDemo` (DMX library track, 34 fixture patches, node group) | 158 (S5: 170) | 994 (S5: 1156) | 438 (S5: 467) | 131476 (S5: 131563, with `Groups`) | 354 |
| #12 `LS_WaterFountain` (DMX) | 33 | 302 | 104 | 26404 | 40 |

#12: the node group of `LS_FixtureDemo` is reproduced by Create since S5; its one member is a stale outliner path
(`{"Raw": "MovieSceneDMXLibraryTrack_0"}`, naming no track of the source) and stays stale in the copy (the copy's DMX
track would get that object name, so it is named `MovieSceneDMXLibraryTrack_1`). Both DMX sections end inclusively one frame before a
whole frame, so their document `End` is one tick past it and the `WholeSectionRanges` warning names it on the source
and on the copy alike (the validator checks the document's bounds since S3).

Fixes the round trip drove: the `Fade` track is a property track without a property; `-0.0` reads as `0`; a new
sequence's factory author is not compared; stored tangents the write rule would not reproduce are read as stated
tangents (`Seq_CineCameraActor02`: an Auto key saved with a tangent twice the computed one), and a curve channel is not
given `PostEditChange` after the write (it would recompute them).

## Behaviour tests (`UEAssetDoc.Sequencer.*`)

| Test | Covers |
|---|---|
| `Read` | built fixture (spawnable cine camera with a component binding, level possessable, folder, every interpolation mode, sub-frame key, 100-key dense channel, camera cut, fade, subsequence, marker, metadata), read after save + reload: extract stable, Diff empty, no side effects, region shapes, Inspect facts, status `UpToDate` |
| `ReadOnly` | unchanged document applies without saving; a Director with a Blueprint key a Director does not have (`Components`) is refused with `Sequencer.InvalidBody` at `/Body/Director/Components` and nothing changes |
| `Status` | broken camera cut binding, unbound possessable, unresolved property track, sub-sequence cycle: messages at document paths |
| `ListTypes` | `sequencer_track` / `sequencer_section` / `sequencer_binding` |
| `Corpus` | design §6.1 corpus read (template content copied in at its `/Game` paths, plugin content from its mount) |
| `Inspect` | locator resolution with and without the map open; a spawnable's component locator |
| `Apply.CorpusRoundTrip` | the round trip above |
| `Apply.FreeEdits` | §6.3 free edits, each either as a twin (the same edit through the engine API on copy B, the document of B applied to copy A, equal extract, no-op re-apply, equal after reload, evaluation compared) or as a document edit (Diff empty, no-op re-apply): rename a binding (GUID kept, camera cut follows), add a possessable, second locator, delete a possessable, spawnable template Details (`CurrentAperture`), Float track on `FocusSettings.ManualFocusDistance`, split a section (row 1, additive), ease in, mute, reorder tracks, three sections then delete the middle one, key edits (1321 samples equal per channel), display rate 30 → 24, tick resolution 24000 → 60000, playback range move + lock, sub-section retime (offset, time scale, bias), add a sub section, nested folders with a moved binding / track and colour, markers add / move / delete with a determinism fence |
| `Apply.Rollback` | after a binding add and a marker change in the same document: unknown property (`ValidationFailed`), camera cut to a missing binding (`UnknownBinding`), sub-sequence cycle (`CircularSubsequence`), malformed locator (`InvalidLocator`), unknown track type (`UnknownTrackType`), folder not listed (`InvalidFolder`): error at the path, package clean |
| `Apply.Dependents` | a parent sequence: rename gives no warning, delete gives `Sequencer.DependentsAffected` naming the parent and the binding |
| `OpenEditorApply` | S6: with the sequence open in Sequencer and not saved, an apply (marker label, Fade track deleted) lands in place and the same Sequencer's outliner drops the track; a failed apply (unknown track type) rolls back and Sequencer shows the rolled-back sequence; a human edit is saved, undone and redone; with the parent open, an apply to the sequence lands and the parent's Sequencer, focused on the sub section, shows it |
| `Apply.Undo` | human edit, then AI edit: undo history for the package cleared, the human edit kept; a failed apply rolls back and leaves no history |
| `Apply.TrackKinds` | S3 kinds built as the editor's menus do (CVar with platform / group conditions, level visibility with a row condition, data layer, slomo with 1000 keys and stated / User tangents, time warp play-rate curve, MPC with a 400-key stated-tangent parameter, custom primitive data, component material, particle parameter, primitive material, particle, camera shake, shake source trigger): the twin applies, equal extract and evaluation, no-op re-apply, equal after reload; document edits of parameters (rename, add), conditions (group → platform, row condition removed), a fixed-frame time warp; custom primitive data `"35x"` refused at its path |
| `Apply.Preserved` | section group, node group (listed as extracted), a camera cut bound to a sequence ID no sub section has (kept raw) and a Perlin-noise channel override on a section the update rewrites are kept by an unrelated update |
| `Apply.DependentsBlueprint` | deleting the binding a Blueprint's "Get Sequence Binding" node names warns `Sequencer.DependentsAffected` naming the Blueprint |
| `Template.RoundTrip` | a Template Sequence (Cine Camera Actor root) and a Camera Animation Sequence made by their factories with a component binding, transform / focal-length keys, a template Details edit, folder and marker: Create from the extract, equal extract and status, `BoundActorComponents` and child possessables equal, no-op re-apply, equal after reload |
| `Template.ChangeClass` | the Bound Actor Class picker as a twin (Cine Camera Actor → Camera Actor: one spawnable, new GUID, tracks and component binding moved, equal extract, no-op re-apply, equal after reload); a document edit of `Root.Class` alone (Diff empty, name / tracks / children kept); a Camera Animation Sequence refuses a Static Mesh Actor (`Sequencer.CameraClassRequired` at `/Body/Root/Class`, unchanged) |
| `Template.CameraShake` | a Camera Animation Sequence created from a document (Camera Actor root, `Location.X` and component `FieldOfView` keys) played by a `USequenceCameraShakePattern`: the scrubbed view's location and FOV follow the keys |
| `Apply.CineCameraSpawnable` | Cine Camera spawnables from documents (the profile section's example, without `Template`, possessable → spawnable): apply passes the post-save check, Diff empty, template after reload as listed |
| `BindingConversion` | spawnable → possessable (GUID, tracks, children kept) → spawnable with its template; a possessable made spawnable |
| `Director.Twin` | the editor's director work on a copy (Open Director Blueprint, a Quick Binding custom event on Door's event key, a function endpoint `OnBeat(Count)` with payload `3` on a global event key, "Replaceable from Director Blueprint" on Door with a new resolver endpoint, compile) applied to a copy without a Director: equal extract (graph / node GUIDs aside), no-op re-apply, equal after reload; the keys and binding call the document side's own compiled Director |
| `Director.FreeEdits` | a Director (variable, `OpenDoor(Door, Amount)` printing) with a Door event key calling it (bound pin, payload) in one document; the function renamed in the same document (same node, key follows); deleting the used endpoint refused (`UnknownEventEndpoint`, unchanged); a compile error rolls back (marker change included, Director compiles again, no undo history); `"Director": null` refused while called (`DirectorInUse`), then removed with the keys |
| `Director.Rollback` | a Director created by a failing apply is gone afterwards; a failed edit of an existing Director (variable added, function renamed, then a camera cut to a missing binding) leaves it and its compiled class as before |
| `Director.Undo` | human Blueprint-editor edit + save, then an AI Director edit, undo / redo; a rolled back compile error, undo / redo |
| `Groups` | section group + node group (camera in its folder, Door, fade track) as an editor twin; document edits: group three sections across bindings, children and global tracks with a node group, regroup / rename / second node group, ungroup all; a one-section group refused at its `Group` |
| `RemoteBinding` | camera cut to a shot's camera as an editor twin (the binding picker's ID); a shot section and a camera cut into it created in one document |
| `FromScratch` | §6.4 scripted: `LS_Outro`, `LS_Intro` (24 fps, two spawned cine cameras, dolly + focus pull, camera cuts, fade, markers, a possessed sun, a last-frame event calling Director `IntroDone` that prints) and `LS_IntroMaster` (two shots, a camera cut into the intro's dolly camera, listed after the shot track), then "ease out the dolly", "camera shake on the reverse shot", "outro two seconds later": 6 applies, 0 failed, Diff empty after each, 1.7 s in all |

## Not in S5

Channel overrides (kept untouched), Control Rig sections (Control Rig profile), plugin channel types without reflected
key arrays (kept), binding references no sub / shot section of the sequence reaches (kept raw), Level Instance
contents.
