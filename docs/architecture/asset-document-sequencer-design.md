# AssetDocument Sequencer profiles (Level Sequence, Template Sequence, Camera Animation Sequence): design

Status: implemented, including apply on a sequence open in Sequencer (test `Sequencer.OpenEditorApply`). Decisions in §8; support matrix: [asset-document-sequencer-matrix.md](asset-document-sequencer-matrix.md).

Governing rules: [design rules](asset-document-design-rules.md) §3 (rollback = functional
equivalence), §3.1 (Undo history), §3.2 (dependent assets), §4 (editor-equivalent code; raw paths allowed when the
editor does it that way, in their own file with a behaviour test; no private headers, no unexported symbols) and §5
(support matrix). Reference editor: **Sequencer** as the Level Sequence editor and the Template Sequence editor host
it (`Engine/Source/Editor/Sequencer`, track editors in `Engine/Source/Editor/MovieSceneTools`), the Content Browser
actions, and the editor scripting layer that ships with it (`Engine/Plugins/MovieScene/SequencerScripting`,
`ULevelSequenceEditorSubsystem` in `Engine/Plugins/MovieScene/LevelSequenceEditor`). Acceptance is fidelity on real
content, free edits, and an AI writing a sequence from scratch.

Engine source read (read only): the UE 5.7 install, `<UE_ROOT>`. Paths are relative to `Engine/` unless stated.
Runtime modules: `Source/Runtime/MovieScene` (data model, channels, compiler), `Source/Runtime/MovieSceneTracks`
(built-in tracks / sections / custom bindings), `Source/Runtime/LevelSequence`, `Source/Runtime/UniversalObjectLocator`.
Plugins: `Plugins/MovieScene/TemplateSequence` (Template + Camera Animation Sequence; off by default but pulled in by
the default-on `GameplayCameras` / `EngineCameras` plugins), `Plugins/MovieScene/SequencerScripting` (pulled in by the
default-on `LevelSequenceEditor`), `Plugins/MovieScene/SequenceValidator` (default on, experimental).

Scope boundary: **World / Level editing is not an asset document.** A sequence binds to level
actors through *binding references*; the document stores those references as data and never edits, spawns, moves or
saves anything in a level. Level-side work (placing a `LevelSequenceActor`, its binding overrides, picking actors) is
the job of the separate structured level/actor tool set ([level-editing-tools-design.md](level-editing-tools-design.md)); §3.5 says what crosses that line.

## 1. Classes in scope

| Class | Decision | Profile | Reason |
|---|---|---|---|
| `/Script/LevelSequence.LevelSequence` | **exact profile** | `profile.level_sequence.v1` | The cinematic asset. All content (bindings, tracks, sections, channels, folders, markers, the Director Blueprint) lives in the `UMovieScene` subobject graph; the generic route emits nothing. |
| `/Script/TemplateSequence.TemplateSequence` | **exact profile**, same movie-scene body | `profile.template_sequence.v1` | A reusable sequence animating one root actor of a class (`BoundActorClass`), played by `UTemplateSequencePlayer` or as a Template track in a Level Sequence. Same component as the Level Sequence body plus a root-binding rule. |
| `/Script/TemplateSequence.CameraAnimationSequence` | **exact profile** (Template Sequence body, class fixed to a cine camera) | `profile.template_sequence.v1` (variant) | Camera animations played as camera shakes (`UCameraAnimationCameraModifier`, Sequence Camera Shake pattern). It is a `UTemplateSequence` whose factory always binds `ACineCameraActor`. |
| Director Blueprint (`ULevelSequence::DirectorBlueprint`, parent `ULevelSequenceDirector`, an inner `UBlueprint` of the sequence package) | **in scope as a Body region** | Blueprint graph component hosted in the sequence | Event tracks call its functions and custom events; dynamic / director bindings call its functions. Authoring event keys without it is impossible. Reuses the Blueprint profile's member and graph components; it is not a separate asset. |
| `/Script/LevelSequence.LegacyLevelSequenceDirectorBlueprint` | out | — | Pre-4.26 director class; `PostLoad` already upgrades. It goes through the Blueprint lifecycle. |
| `/Script/ActorSequence.ActorSequence` | out | — | Not an asset: it is owned by a `UActorSequenceComponent` inside a Blueprint or a level actor. The movie-scene component of this design could be hosted by the Blueprint profile (components region) the way the Director Blueprint is hosted here. |
| `/Script/UMG.WidgetAnimation` | stays in the WidgetBlueprint profile | — | Already authored by `Body.Animations` (one-section property tracks). §7.1 describes moving its tracks onto this design's shared movie-scene component so UMG gains sections, events and the same key forms; a separate decision. |
| `/Script/CineAssemblyTools.CineAssembly` (experimental plugin, subclass of LevelSequence) | out (generic route) | — | Experimental "Cinematic Assembly" product layer (schemas, naming tokens) on top of a Level Sequence. Its movie scene would work with the Level Sequence profile; registering the subclass is a one-line change once the plugin leaves experimental. |
| `/Script/DaySequence.DaySequence` (experimental plugin) | out | — | Day/night cycle sequences bound through `DaySequenceBindingReferences` to a `DaySequenceActor`. Same movie-scene component would apply; plugin is experimental and off by default. |
| `NiagaraSequence`, `ContextualAnimMovieSceneSequence`, MetaHuman sequences, `AnimSequencerDataModel`, Avalanche `AvaSequence` | out | — | Transient or tool-internal sequences (no standalone assets), or owned by other asset types. |
| Movie Render Queue / Movie Render Graph configs (`MoviePipelinePrimaryConfig`, `MovieGraphConfig`) | out here | — | Different assets (render settings / a graph); not Sequencer content. They reference sequences, not the other way round. |
| `LevelSequenceActor`, `ALevelSequenceActor` binding overrides, `LevelSequencePlayer` settings | out (level tool set) | — | Level content. |

Editor state that is **not** content (kept as is, never written, Body-owned): `UMovieScene::EditorData` (view / work
range, expansion states, pinned nodes), `NavigationToolState`, transient solo / mute nodes, the Sequencer's selection
and curve-editor state, thumbnails (`ThumbnailReferenceOffset`), and per-user `USequencerSettings`.

## 2. Editor data model: authored vs derived

### 2.1 Authored (Extract emits it, Apply writes it)

`ULevelSequence` (`Source/Runtime/LevelSequence/Public/LevelSequence.h`)

- `MovieScene` (one `UMovieScene` subobject, created by `ULevelSequence::Initialize`).
- `BindingReferences` (`FUpgradedLevelSequenceBindingReferences` → `FMovieSceneBindingReferences`,
  `Source/Runtime/MovieScene/Public/MovieSceneBindingReferences.h`): per binding GUID an ordered list of
  `FMovieSceneBindingReference {ID, Locator (FUniversalObjectLocator), ResolveFlags, CustomBinding (instanced
  UMovieSceneCustomBinding)}`. This is where a binding says **which level object** it animates (a locator such as
  `uobj://actor?payload0=/Game/Maps/Lobby.Lobby:PersistentLevel.SM_Door_3`) or **how it makes its object** (a custom
  binding: `UMovieSceneSpawnableActorBinding` with an `ActorTemplate`, `UMovieSceneReplaceableActorBinding`,
  `UMovieSceneSpawnableDirectorBlueprintBinding`, `UMovieSceneReplaceableDirectorBlueprintBinding`, plugin types).
- `DirectorBlueprint` (inner `UBlueprint`, parent `ULevelSequenceDirector`).
- `MetaDataObjects` (`UMovieSceneMetaData`: `Author`, `Created`, `Notes`), `AssetUserData` (EditAnywhere, instanced).

`UMovieScene` (`Source/Runtime/MovieScene/Public/MovieScene.h:1279–1412`)

- Time: `TickResolution`, `DisplayRate`, `EvaluationType` (`WithSubFrames` / `FrameLocked`), `ClockSource` and
  `CustomClock`, `PlaybackRange`, `SelectionRange`, `bPlaybackRangeLocked`, `bReadOnly`.
- `Possessables` (`FMovieScenePossessable`: `Guid`, `Name`, `PossessedObjectClass`, `ParentGuid`, `Tags`,
  `SpawnableObjectBindingID`) and, in Template Sequences only, `Spawnables` (`FMovieSceneSpawnable` with an
  `ObjectTemplate`). In 5.7 Level Sequences **have no `FMovieSceneSpawnable`s**: `ULevelSequence::PostLoad` →
  `ConvertOldSpawnables` (`LevelSequence.cpp:499`) turns every old spawnable into a possessable plus a
  `UMovieSceneSpawnableActorBinding` custom binding in `BindingReferences` (same GUID).
- `ObjectBindings` (`FMovieSceneBinding {ObjectGuid, Tracks, SortingOrder}`), `Tracks` (global tracks),
  `CameraCutTrack`, `BindingGroups`.
- `MarkedFrames` (`FMovieSceneMarkedFrame`: `FrameNumber`, `Label`, `Comment`, `CustomColor` / `bUseCustomColor`,
  `bIsDeterminismFence`, `bIsInclusiveTime`), `bMarkedFramesLocked`, `bGloballyShowMarkedFrames`.
- `RootFolders` (`UMovieSceneFolder`: `FolderName`, `ChildFolders`, `ChildTracks`, `ChildObjectBindingStrings`
  (GUID strings), `FolderColor`, `SortingOrder`).
- `SectionGroups`, `NodeGroupCollection` (Sequencer "groups" filter), `ObjectsToDisplayNames`, `ObjectsToLabels`
  (track labels / tags).

Tracks (`UMovieSceneTrack`, `MovieSceneTrack.h`) — instanced subobjects of the movie scene

- The class (one of ~45 built-in classes in `Source/Runtime/MovieSceneTracks/Public/Tracks`, plus plugin tracks:
  Niagara, Control Rig, DMX, Media, Geometry Cache, Groom, Chaos cache, Text, ...).
- Property tracks (`UMovieScenePropertyTrack::PropertyBinding`: `PropertyName`, `PropertyPath`).
- Details: `EvalOptions` (`bEvalNearestSection`, `bEvaluateInPreroll/Postroll`), `DisplayOptions`, `TrackTint`,
  `ConditionContainer`, `bIsEvalDisabled` (mute), `RowsDisabled`, `TrackRowMetadata` (per-row conditions),
  `SortingOrder`, the display name of nameable tracks (`UMovieSceneNameableTrack`, editor-only).
- `Sections` (`UMovieSceneSection`, `MovieSceneSection.h`): `SectionRange`, `RowIndex`, `OverlapPriority`,
  `bIsActive`, `bIsLocked`, `BlendType`, `Easing` (`FMovieSceneEasingSettings`: auto / manual ease-in/out durations,
  easing function objects), `PreRollFrames`, `PostRollFrames`, `EvalOptions.CompletionMode` ("When Finished"),
  `TimecodeSource`, `ConditionContainer`, `ColorTint`, plus the section class's own UPROPERTYs (camera cut
  `CameraBindingID`, sub/shot `SubSequence` + `Parameters`, skeletal animation `Params`, audio `Sound`, attach
  `ConstraintBindingID` + socket, transform `TransformMask` + `bUseQuaternionInterpolation`, ...).
- **Channels** inside sections (UPROPERTY structs exposed through `UMovieSceneSection::GetChannelProxy()` with
  `FMovieSceneChannelMetaData` names such as `Location.X`, `Rotation.Z`, `Weight`):
  `FMovieSceneFloatChannel` / `FMovieSceneDoubleChannel` (`Times`, `Values` = `{Value, InterpMode, TangentMode,
  Tangent{ArriveTangent, LeaveTangent, ArriveTangentWeight, LeaveTangentWeight, TangentWeightMode}}`,
  `PreInfinityExtrap`, `PostInfinityExtrap`, `DefaultValue`, `bHasDefaultValue`), bool, byte (with `Enum`), integer,
  string, text, object path, event (`FMovieSceneEvent`: endpoint, `BoundObjectPinName`, `PayloadVariables`),
  actor-reference (binding IDs), time-warp channels, and plugin channel types. Parameter sections (material,
  MPC, Niagara parameters, Control Rig) hold **dynamic channel sets** (`ScalarParameterNamesAndCurves`, ...).
- `UMovieSceneSectionChannelOverrideRegistry` (procedural channel overrides, e.g. noise): rare.

`UTemplateSequence` (`Plugins/MovieScene/TemplateSequence/Source/TemplateSequence/Public/TemplateSequence.h`):
`MovieScene`, `BoundActorClass` (`TSoftClassPtr<AActor>`), `BoundActorComponents` (`TMap<FGuid, FName>`: component
bindings under the root). The root binding is the first spawnable (`GetRootObjectBindingID`).
`UCameraAnimationSequence` adds nothing but the display name and factory.

### 2.2 Derived (never emitted; recomputed)

| State | Recomputed by |
|---|---|
| `UMovieSceneSequence::CompiledData` (instanced `UMovieSceneCompiledData`), evaluation templates, hierarchy, entity ledgers | `UMovieSceneCompiledDataManager::GetPrecompiledData()->Compile(Sequence)` (editor: lazily on play / after `MarkAsChanged`; an editor save resets it, only cooking writes it) |
| Signatures (`UMovieSceneSignedObject::Signature`), `EvaluationField`, `EvaluationFieldGuid` / `Version` on tracks | `MarkAsChanged()` / track evaluation-field rebuild |
| Key handles (`FMovieSceneKeyHandleMap`, `Transient`) | per load / per edit |
| Tangents of `Auto` / `SmartAuto` cubic keys | `FMovieSceneFloatChannel::AutoSetTangents` (called by the Sequencer after key edits and by `AddKeyToChannel`) |
| Event `Ptrs.Function`, `Ptrs.BoundObjectProperty`, `CompiledFunctionName` | Director Blueprint compile (`UMovieSceneEventBlueprintExtension` generates the entry points, `FMovieSceneEventUtils::GenerateEntryPoint`) |
| `ULevelSequence::DirectorClass` | Director Blueprint compile (`OnDirectorRecompiled`) |
| `PossessedObjectClass` of a spawnable possessable | the custom binding's `GetBoundObjectClass()` |
| Camera cut `InitialCameraCutTransform` / `bHasInitialCameraCutTransform` | Sequencer caches it from the evaluated camera (blend-in from gameplay); written by the editor when cuts are evaluated |
| Old spawnables / deprecated object references / dynamic bindings | `ULevelSequence::PostLoad` (`ConvertOldSpawnables`, `ConvertDynamicBindingPossessable`, `ObjectReferences_DEPRECATED` → locators) |
| `UMovieSceneMetaData::Created` (and `Author` on create / duplicate) | `ULevelSequence::Initialize` / `PostDuplicate` (duplicate clears `Notes` too, `LevelSequence.cpp:346`) |
| `UMovieScene::GeneratedConditions` | the compiler (keep-alive of generated group conditions) |
| `EditorData`, `NavigationToolState`, solo / mute nodes | Sequencer UI state (§1) |

Extract rule: emit 2.1 in the document shapes of §3, with float / frame values as the editor shows them, omit 2.2
and editor state. Everything in 2.2 is Body-owned and refused through `Properties`
(`Sequencer.NotAuthoredProperty`).

## 3. Document shape

### 3.1 Example (small, complete)

A 5-second shot at 30 fps: a spawned cine camera dollies in on a door that is a level actor; the door opens at
frame 60 through a Director event; a fade in; a marker; the camera and door are in folders.

```jsonc
{
  "SchemaVersion": 1, "Target": "/Game/Cinematics/LS_DoorReveal", "Class": "/Script/LevelSequence.LevelSequence",
  "Action": "CreateOrUpdate",
  "Properties": {},
  "Body": {
    "Settings": {
      "DisplayRate": {"Numerator": 30, "Denominator": 1},
      "TickResolution": {"Numerator": 24000, "Denominator": 1},
      "Evaluation": "WithSubFrames",
      "PlaybackRange": {"Start": 0, "End": 150}
    },
    "Folders": [{"Path": "Cameras"}, {"Path": "Set", "Color": {"R": 80, "G": 160, "B": 80, "A": 255}}],
    "Bindings": [
      {"Name": "ShotCam", "Folder": "Cameras",
       "Spawnable": {"Class": "/Script/CinematicCamera.CineCameraActor",
                     "Template": {"Components": {"CameraComponent": {"CurrentAperture": 2.8}}}},
       "Tracks": [
         {"Type": "Transform",
          "Sections": [{"Channels": {
            "Location.X": {"Keys": [{"Frame": 0, "Value": -600}, {"Frame": 150, "Value": -250}]},
            "Location.Z": {"Default": 160},
            "Rotation.Z": {"Keys": [{"Frame": 0, "Value": 10, "Interp": "Linear"}, {"Frame": 150, "Value": 0}]}}}]}],
       "Children": [
         {"Name": "CameraComponent", "Bind": ["uobj://subobj?payload0=CameraComponent"],
          "Class": "/Script/CinematicCamera.CineCameraComponent",
          "Tracks": [
            {"Type": "Float", "Property": "CurrentFocalLength",
             "Sections": [{"Channels": {"Value": {"Keys": [{"Frame": 0, "Value": 24}, {"Frame": 150, "Value": 50}]}}}]}]}]},
      {"Name": "SM_Door", "Folder": "Set", "Class": "/Script/Engine.StaticMeshActor",
       "Bind": ["uobj://actor?payload0=/Game/Maps/Lobby.Lobby:PersistentLevel.SM_Door_3"],
       "Tracks": [
         {"Type": "Transform",
          "Sections": [{"Start": 60, "End": 90, "Blend": "Additive", "Channels": {
            "Rotation.Z": {"Keys": [{"Frame": 60, "Value": 0}, {"Frame": 90, "Value": -95, "Interp": "Auto"}]}}}]},
         {"Type": "Event", "Name": "DoorEvents",
          "Sections": [{"Class": "Trigger", "Channels": {
            "Events": {"Keys": [{"Frame": 60, "Event": {"Endpoint": "PlayDoorCreak", "BoundObjectPin": "Door"}}]}}}]}]}
    ],
    "Tracks": [
      {"Type": "CameraCut",
       "Sections": [{"Start": 0, "End": 150, "Properties": {"CameraBindingID": {"Binding": "ShotCam"}}}]},
      {"Type": "Fade",
       "Sections": [{"Start": 0, "End": 150, "Channels": {
         "Fade": {"Keys": [{"Frame": 0, "Value": 1}, {"Frame": 15, "Value": 0}]}}}]}
    ],
    "Markers": [{"Frame": 60, "Label": "Door opens"}],
    "Director": {
      "FunctionGraphs": [
        {"Name": "PlayDoorCreak",
         "Signature": {"Inputs": [{"Name": "Door", "Type": "object:/Script/Engine.Actor"}]},
         "Nodes": [
            {"Id": "Entry", "Action": "K2:FunctionEntry"},
            {"Id": "Play", "Action": "K2:CallFunction:/Script/Engine.GameplayStatics:PlaySoundAtLocation",
             "Pins": {"Sound": "/Game/Audio/S_DoorCreak.S_DoorCreak"}},
            {"Id": "Loc", "Action": "K2:CallFunction:/Script/Engine.Actor:K2_GetActorLocation"}],
         "Links": [{"From": "Entry:then", "To": "Play:execute"}, {"From": "Entry:Door", "To": "Loc:self"},
                   {"From": "Loc:ReturnValue", "To": "Play:Location"}]}]
    },
    "MetaData": {"Notes": "Door reveal for the lobby intro."}
  }
}
```

(Director graph, pin and action names follow the Blueprint profile's graph shape; the exact action keys are those
`graph.actions.enumerate.v1` lists for the director Blueprint, read from Inspect, §3.8.)

### 3.2 Body regions (Level Sequence)

| Key | Content | Present = |
|---|---|---|
| `Settings` | `{DisplayRate, TickResolution, Evaluation, Clock?, PlaybackRange, SelectionRange?, PlaybackRangeLocked?, ReadOnly?, MarkersLocked?, ShowMarkers?}` | sparse object (only listed fields are written; Extract lists all of `DisplayRate`, `TickResolution`, `Evaluation`, `PlaybackRange` and the non-default others) |
| `Bindings` | binding tree (§3.4); root bindings, components as `Children` | complete, ordered (unlisted bindings and their tracks are deleted) |
| `Tracks` | global tracks (camera cut, shots, subsequences, fade, audio, level visibility, data layers, slomo, CVars, MPC, events, ...) (§3.5) | complete, ordered |
| `Folders` | `[{Path, Color?}]` every folder, `Path` = `Parent/Child`; membership is the `Folder` field of root bindings and global tracks | complete, ordered (order = sorting order among siblings) |
| `Markers` | `[{Frame, Label?, Comment?, Color?, DeterminismFence?, Inclusive?}]` | complete; sorted by frame (the editor keeps them sorted) |
| `Director` | the Director Blueprint's members and graphs, in the Blueprint profile's keys (`ImplementedInterfaces`, `Variables`, `ClassDefaults`, `EventDispatchers`, `UbergraphPages`, `FunctionGraphs`, `MacroGraphs`; §3.6) | the Blueprint profile's region rules; `null` = no Director Blueprint (only allowed when no event / director binding uses it) |
| `MetaData` | `{Author?, Notes?}` | sparse |
| `Groups` | Sequencer node groups `[{Name, Members: [<node path>]}]` | complete |

Template / Camera Animation Sequence `Body`: `Settings`, `Root` (§3.4.3), `Tracks`, `Folders`, `Markers` (no
`Director`, no level bindings: a Template Sequence binds only its root spawnable (legacy `FMovieSceneSpawnable`,
`GetRootObjectBindingID` = spawnable 0) and components of it, located by name through `BoundActorComponents`;
`UTemplateSequence::IsTrackSupportedImpl` adds Skeletal Animation and Spawn to the base rules,
`TemplateSequence.cpp:292`).

Top-level `Properties`: generic and sparse, for `AssetUserData` (the only Details-visible property) and future
reflected sequence flags. Everything in §2 is Body-owned.

### 3.3 Time, values and keys

- **Frames.** Every time in the Body (key `Frame`, section `Start` / `End`, playback / selection range, marker
  `Frame`, section frame-number properties such as `PreRollFrames`, easing durations) is in **display frames** of
  `Settings.DisplayRate`, the numbers Sequencer shows. Fractions are allowed (sub-frame keys). Apply converts with
  `FFrameRate::TransformTime(DisplayRate → TickResolution)` and rounds to the nearest tick; Extract writes the
  shortest decimal that converts back to the same tick, so extract → apply is exact and a no-op. The Body is read
  after `Settings`, so a document that changes `DisplayRate` states every frame in the new rate (the editor's display
  rate change does not move keys; the Diff shows the frame numbers changing). Changing `TickResolution` is the
  editor's "Advanced → Tick Resolution" which migrates all times (`UE::MovieScene::TimeHelpers::MigrateFrameTimes`,
  `MovieSceneTimeHelpers.h:33`); since document times are display frames nothing else in the document changes.
- **Ranges.** `Start` is inclusive, `End` exclusive, as `TRange<FFrameNumber>` and the Sequencer section bar; an
  omitted bound is open (infinite). A section with neither is infinite (what `bSupportsInfiniteRange` tracks create).
- **Frame rates.** `{"Numerator": 30, "Denominator": 1}` (same form as the WidgetBlueprint `Animations` region).
- **Channel** (keyed by its channel-proxy metadata name; a section with one channel calls it by its metadata name,
  e.g. `Value`, `Fade`, `Weight`, `Events`):

  ```jsonc
  "Location.X": {"Keys": [ ...keys... ], "Default": 160, "PreInfinity": "Constant", "PostInfinity": "Cycle"}
  ```

  `Keys` omitted = no keys; `Default` omitted = no default value (`bHasDefaultValue = false`); `PreInfinity` /
  `PostInfinity` omitted = `Constant` (curve channels only). A channel not listed in a listed section is cleared
  (no keys, no default). Channel names are those the Sequencer's channel tree shows (`FMovieSceneChannelMetaData::Name`);
  Inspect lists them per section class.
- **Curve keys** (float / double):
  `{"Frame", "Value", "Interp"?, "Arrive"?, "Leave"?, "ArriveWeight"?, "LeaveWeight"?, "WeightMode"?}`. `Interp` is
  the Sequencer key menu: `SmartAuto` (default: the Sequencer's default key interpolation), `Auto`, `User`, `Break`,
  `Linear`, `Constant` (`EMovieSceneKeyInterpolation`, mapped to `ERichCurveInterpMode` + `ERichCurveTangentMode`
  as `AddKeyToChannel` does). `Arrive` / `Leave` are the stored tangents (`FMovieSceneTangentData`: value change per tick, the unit
  `AutoSetTangents` writes; the profile doc gives a worked example) and are written, with the weights, for
  `User` / `Break` keys; tangents of other keys are derived (`AutoSetTangents`) and neither extracted nor compared,
  exactly like the Blueprint Timelines region — except stored tangents the derivation would not reproduce (stated
  tangents, §8.2).
- **Discrete keys** (bool, integer, byte / enum, string, text, object path): `{"Frame", "Value"}`; enum values by
  name, objects by path.
- **Event keys**: `{"Frame", "Event": {"Endpoint": "<director function or custom event name>", "BoundObjectPin"?,
  "Payload"?: {"<pin>": "<value text>"}}}`; repeater sections hold one `Event` instead of a channel (§3.5).
- **Unknown channel types** (plugin channels without a dedicated codec): read and written through reflection
  (`Times` + `Values` of the channel struct with the typed value codec), keys `{"Frame", "Value": <typed JSON>}`;
  channels whose struct has no reflected key arrays are `Unsupported` (§3.9).
- **Binding references inside values.** Any `FMovieSceneObjectBindingID` value (camera cut, attach, path, actor
  reference keys, constraint targets, event bound objects) is written as `{"Binding": "<binding path>"}` (§3.4.2),
  resolved by the profile to the binding GUID in the local sequence space. A reference into a subsequence's binding
  is `{"Binding": "<path inside that sequence>", "Sequence": "<document path of the sub / shot section>"}`; a
  reference into a sequence ID no sub / shot section reaches stays raw and is preserved (§3.9).

### 3.4 Bindings

#### 3.4.1 Binding entry

```jsonc
{"Name": "SM_Door",                      // identity among siblings (§3.4.2); the Sequencer's binding label
 "RenamedFrom": "Door_Old",              // rename (keeps GUID, references follow)
 "Id": "8F0C...",                        // only when two siblings share a Name (Extract emits it then); accepted always
 "Class": "/Script/Engine.StaticMeshActor", // PossessedObjectClass (possessables; derived for spawnables)
 "Bind": ["uobj://actor?payload0=..."],            // possessable: the binding references (locators), in binding-index order
 "Spawnable": {...},                      // or: spawnable (§3.4.3)
 "CustomBinding": {"Class": "...", "Properties": {...}}, // or: any other custom binding type
 "Tags": ["Hero"],                        // FMovieScenePossessable::Tags
 "Folder": "Set",                         // root bindings only
 "Tracks": [...],                         // complete, ordered
 "Children": [...]}                       // component / sub-object bindings (ParentGuid = this binding), complete
```

Exactly one of `Bind`, `Spawnable`, `CustomBinding`. `Bind: []` is a possessable with no reference (an "unbound"
binding, which the editor shows in red); that is legal content.

#### 3.4.2 Identity and references

- A binding is identified by its `Name` among its siblings (root bindings, or the `Children` of one binding). Two
  siblings with the same name are legal in Sequencer; Extract then adds `Id` (the GUID string) to those entries and
  the document must keep it. Matching: `Id` first, else `Name`, else `RenamedFrom`.
- The GUID is **never** regenerated for a matched binding. It is what external content stores: Blueprint pins of
  type `FMovieSceneObjectBindingID` (`K2Node_GetSequenceBinding`), `ALevelSequenceActor` binding overrides in levels,
  event bound-object pins, camera cuts in parent sequences. A new binding gets a new GUID (as the editor).
  Rename keeps it (`FMovieScenePossessable::SetName` + `UMovieScene::SetObjectDisplayName` as the tree rename).
- References inside the document use the **binding path**: names from the root separated by `/`
  (`"ShotCam/CameraComponent"`), with `#<Id>` instead of a name segment where `Id` is needed.
- Stability: names and GUIDs are serialized data; save / reload / Content Browser duplicate keep them
  (`ULevelSequence::PostDuplicate` only re-links the director class and resets the metadata).
- Reordering `Bindings` / `Children` changes `SortingOrder` (the tree's drag reorder); it is not a delete + add.
- Moving a binding to another parent (re-parenting a component binding) is not something Sequencer offers; a
  binding whose parent changes is delete + add (with a warning `Sequencer.BindingRecreated` listing what referenced
  the old GUID inside the asset).

#### 3.4.3 Possessables, spawnables, custom bindings and Template roots

- **Possessable** (`Bind`): one locator string per binding reference, in the canonical
  `FUniversalObjectLocator::ToString` form (`uobj://<type>/<type>?payload0=...&payload1=...`, `UniversalObjectLocator.cpp:341`;
  fragment types `actor`, `subobj`, `uobj`, `asset`, `animinst`, plus plugin types). Apply parses with
  `FUniversalObjectLocator::TryParseString` and stores with `FMovieSceneBindingReferences::AddBinding` — **no world
  needs to be loaded**, which is exactly what makes a level-bound sequence authorable as an asset document. Inspect
  (§3.8) resolves each locator against the editor world that is currently open, read only, and reports
  `{Resolved: true, Actor: "<label>", Class}` or `{Resolved: false}`. For a child binding the locator is relative to
  the parent's object (`uobj://subobj?payload0=CameraComponent`).
- **Spawnable** (Level Sequence: `UMovieSceneSpawnableActorBinding` in `BindingReferences`; Template Sequence: the
  legacy `FMovieSceneSpawnable`):

  ```jsonc
  "Spawnable": {"Class": "/Script/CinematicCamera.CineCameraActor",   // template class (BP classes allowed)
                "Template": {"Properties": {...}, "Components": {"<component name>": {...}}}, // sparse vs class defaults
                "Ownership": "InnerSequence", "ContinuouslyRespawn": false,
                "NetAddressableName": false, "LevelName": ""}
  ```

  `Template` holds the spawnable's saved default state: reflected actor properties and per-component properties
  that differ from a fresh actor of `Class` (the "Save Default State" result). Spawnables also carry a **Spawn**
  track (bool "Spawned" channel) that the editor creates with them; it is an ordinary track in `Tracks`
  (`{"Type": "Spawn", ...}`) so it round-trips; a new spawnable without a listed Spawn track gets the editor's
  default (spawned for the whole sequence).
- **Other custom bindings** (`CustomBinding`): `{Class, Properties}` for `UMovieSceneReplaceableActorBinding`,
  director-Blueprint bindings (`DynamicBinding.Function` names a director function: `{"Function": "GetHero"}`),
  and plugin binding types (`UMovieSceneCustomBinding` subclasses found by `MovieSceneHelpers::
  GetPrioritySortedCustomBindingTypes`). Converting a binding between kinds (possessable ↔ spawnable ↔ replaceable)
  is the editor's "Convert to ..." (§4.3) and keeps the GUID and tracks.
- **Template / Camera Animation root** (`Root`): `{"Class": "/Script/CinematicCamera.CineCameraActor", "Template"?,
  "Tracks", "Children"}`; `Class` is `BoundActorClass`. Changing it is the Template Sequence editor's "change bound
  class" (§4.3). A Camera Animation Sequence's root class must be a camera actor class (`Sequencer.CameraClassRequired`).

### 3.5 Tracks and sections

```jsonc
{"Type": "Float",                  // short type (table below) or the track class path for any other track
 "Property": "CurrentFocalLength", // property tracks only: PropertyBinding.PropertyPath
 "Name": "DoorEvents",             // nameable tracks: display name; also disambiguates (§3.5.2)
 "Id": "MovieSceneEventTrack_1",   // only when Type/Property/Name do not identify the track
 "Folder": "Set",                  // global tracks only
 "Muted": false,                   // bIsEvalDisabled
 "Properties": {...},              // track Details (EvalOptions, DisplayOptions, TrackTint, conditions), sparse
 "Sections": [ ...sections... ]}   // complete, ordered
```

Section:

```jsonc
{"Class": "Trigger",       // only for tracks with several section classes (event Trigger/Repeater, sub tracks)
 "Start": 60, "End": 90,   // omitted = open bound
 "Row": 0,                 // RowIndex; omitted = 0
 "Active": true, "Locked": false,
 "Blend": "Absolute",      // Absolute | Additive | Relative | AdditiveFromBase | Override; omitted = the track's default
 "Easing": {"In": 10, "Out": 0, "InFunction"?: {...}, "OutFunction"?: {...}},  // manual durations; omitted = auto
 "PreRoll": 0, "PostRoll": 0, "WhenFinished": "RestoreState",
 "Properties": {...},      // the section class's own UPROPERTYs (sparse, typed value codec; channels excluded)
 "Channels": {...},        // §3.3
 "Parameters": [...],      // parameter sections only: dynamic channel sets (§3.5.3)
 "Event": {...}}           // repeater sections only
```

#### 3.5.1 Track type tokens

| Token | Class (`/Script/MovieSceneTracks.` unless noted) | Scope |
|---|---|---|
| `Transform` | `MovieScene3DTransformTrack` (property `Transform`) | binding |
| `Float`, `Double`, `Bool`, `Byte`, `Enum`, `Integer`, `String`, `Text`, `Vector`, `Color`, `Rotator`, `EulerTransform`, `ObjectProperty`, `Visibility` | the property track of that type | binding (needs `Property`) |
| `Spawn` | `MovieSceneSpawnTrack` (`/Script/MovieScene.`) | spawnable binding |
| `Attach`, `Path`, `ActorReference` | `MovieScene3DAttachTrack`, `MovieScene3DPathTrack`, `MovieSceneActorReferenceTrack` | binding |
| `SkeletalAnimation` | `MovieSceneSkeletalAnimationTrack` | binding |
| `Material`, `PrimitiveMaterial`, `CustomPrimitiveData` | `MovieSceneComponentMaterialTrack`, `MovieScenePrimitiveMaterialTrack`, `MovieSceneCustomPrimitiveDataTrack` | binding |
| `CameraShake`, `CameraShakeSource`, `CameraShakeSourceTrigger`, `Particle`, `ParticleParameter` | the matching classes | binding |
| `Event` | `MovieSceneEventTrack` | binding or global |
| `Audio` | `MovieSceneAudioTrack` | binding or global |
| `CameraCut` | `MovieSceneCameraCutTrack` (stored in `UMovieScene::CameraCutTrack`; at most one) | global |
| `Shot`, `Subsequence` | `MovieSceneCinematicShotTrack`, `MovieSceneSubTrack` (`/Script/MovieScene.`) | global |
| `Fade`, `Slomo`, `LevelVisibility`, `DataLayer`, `CVar`, `MaterialParameterCollection`, `TimeWarp` | the matching classes | global |
| `<class path>` | any other `UMovieSceneTrack` subclass (Niagara, Control Rig, DMX, Media, Geometry Cache, Text, ...) | as the class allows |

`list_types` with `kind: "class", base: "/Script/MovieScene.MovieSceneTrack"` lists the track classes the sequence
supports (`UMovieSceneSequence::IsTrackSupported`); unsupported ones are refused
(`Sequencer.TrackNotSupported`), as the "+ Track" menu would not offer them.

#### 3.5.2 Identity of tracks and sections

- Track, within its scope (one binding, or the global list): `Type` + `Property` for property tracks (the
  Sequencer itself finds property tracks by `(class, binding, property path)`); `Type` + `Name` for nameable
  tracks; otherwise `Type`, and if a scope has several tracks of one type, the k-th one of that type in list
  order — the same rule as StateTree nodes. When that is ambiguous after an edit (two Event tracks without names),
  Extract emits `Id` (the track's object name, stable across save / reload / duplicate because instanced subobject
  names are serialized) and the document must keep it. A matched track keeps its object (and its signature
  history), so a track keeps its folder, label and conditions.
- Sections: by position in `Sections`. Extract order is the track's `Sections` array order (which Sequencer keeps
  sorted by row, then start). An edit to section `i` edits that section object in place; inserting a section in
  the middle edits the following ones in place and adds the last one (functional equivalence: sections are not
  referenced from outside their track except by `SectionGroups`, which the apply rewrites).
- Keys: the channel is compared as a whole; a differing channel is rewritten with the editor's channel data
  interface (§4). Key handles are transient and never authored.

#### 3.5.3 Parameter sections (dynamic channels)

Material, MPC, primitive material, custom primitive data, Niagara parameter and particle parameter sections own
channel sets created per parameter (`UMovieSceneParameterSection::AddScalarParameterKey` and siblings,
`ScalarParameterNamesAndCurves` ...). They use

```jsonc
"Parameters": [{"Type": "Scalar", "Name": "Glow", "Channels": {"ParameterCurve": {"Keys": [...]}}},
               {"Type": "Color", "Name": "Tint", "Channels": {"RedCurve": {...}, "GreenCurve": {...}, ...}}]
```

Implementation (§8.3): the sets are found by reflection (every stored array of a struct holding channels), so plugin
sections of the same shape need no code. `Type` is the array name without its `ParameterNamesAndCurves` /
`ParameterInfosAndCurves` suffix (`Bool`, `Scalar`, `Vector2D`, `Vector`, `Color`, `Transform`, ...; omitted when the
class has one set); `Name` is the parameter name (`ParameterInfo.Name` for component material parameters; custom
primitive data names the first float index, `"0"` ... `"35"`); `Properties` holds the element's other fields
(sparse); `Channels` its channel members by field name; `Parameters` the element's own sets (DMX: fixture patch →
function channels). Entries are listed set by set in the class's order. Section catalog rows describe the sets.
Control Rig parameter sections are the same idea but belong to the Control Rig design ([asset-document-controlrig-design.md](asset-document-controlrig-design.md)).

#### 3.5.4 Sub and shot sections

`Properties.SubSequence` (sequence path) plus `Properties.Parameters` (`StartFrameOffset`, `TimeScale`,
`HierarchicalBias`, `bCanLoop`, `EndFrameOffset`, `FirstLoopStartFrameOffset`, flags) and, for shots,
`Properties.ShotDisplayName`. Adding a sequence that (recursively) contains the edited sequence is refused
(`Sequencer.CircularSubsequence`, the check of `UMovieSceneSubTrack::ContainsSequence`, exported). Sub-sequence
offsets are in the **inner** sequence's frames (they are inner-sequence times in the data); this is the one place
where frames are not the outer display rate, stated in the profile doc and checked by a fidelity test.

### 3.6 The Director Blueprint (`Director`)

The director is a normal Blueprint (parent `ULevelSequenceDirector`) stored inside the sequence package. The region
uses the Blueprint profile's own keys for members and graphs (`ImplementedInterfaces`, `Variables`, `ClassDefaults`,
`EventDispatchers`, `UbergraphPages`, `FunctionGraphs`, `MacroGraphs`), through a **hosted-Blueprint adapter**: the existing
`ublueprint_*` Python components and `blueprint.*` / `graph.*` primitives get a target object path
(`/Game/Cinematics/LS_DoorReveal.LS_DoorReveal:LS_DoorReveal_DirectorBP`) instead of a package. Rules:

- Present `Director` with no existing director → `FMovieSceneSequenceEditor::Find(Seq)->GetOrCreateDirectorBlueprint
  (Seq)` (exported, `Sequencer/Public/MovieSceneSequenceEditor.h:54`), which is what Sequencer's "Open Director
  Blueprint" button calls (`FKismetEditorUtilities::CreateBlueprint` with `ULevelSequenceDirector`, inner object,
  `SetDirectorBlueprint`).
- `Director: null` removes it; refused while event keys or director bindings reference it
  (`Sequencer.DirectorInUse`).
- Event keys reference director **endpoints** by name: a function graph or a custom event in an event graph
  (the Sequencer's "Create Quick Binding" makes a custom event; "Create Event Endpoint" makes a function). The
  profile binds the key with `FMovieSceneEventUtils::SetEndpoint` (exported, `MovieSceneEventUtils.h:115`) to the
  node found by name; a missing endpoint is `Sequencer.UnknownEventEndpoint` at the key's path. `BoundObjectPin`
  is the endpoint pin that receives the bound object; `Payload` sets the other pins' values.
- The director compiles with the sequence (§5): compile errors fail the apply (`Graph.FinalizeFailed`) as for any
  Blueprint, and the compile regenerates the event entry points (`Ptrs.Function`) and `DirectorClass`.

### 3.7 Omitted versus present

Per the core guide: an omitted region keeps everything; a present region is complete. Deviations:

- `Settings`, `MetaData`, and every `Properties` object (track, section, template) are sparse.
- In a listed binding, omitted `Tracks` / `Children` mean **none** (complete), omitted `Tags` none, omitted `Folder`
  root level.
- In a listed section, omitted header fields take the **section class defaults** (`Row` 0, active, unlocked, no
  pre/post roll, the track's default blend type, auto easing, "When Finished" = the class default); omitted
  channels are cleared.
- A new section with no `Start`/`End` is infinite only if the track supports infinite sections; otherwise
  `Sequencer.SectionRangeRequired`.
- `Bind` order is the binding-index order (binding index 0 is what a single-object binding evaluates).

### 3.8 Inspect and discovery (read only)

`inspect_asset_document_target` on a sequence returns the document plus:

- `Bindings`: per binding its GUID, kind, resolved object in the open editor world (`Resolved`, `Actor` label,
  `Class`) — read only, never loads a map — and the animatable properties of its class (what "+ Track" →
  property list offers: `FPropertyTrackEditor` filtering, type token per property).
- `Tracks`: per track class, its section classes, their channel names / types and writable `Properties`.
- `Director`: the endpoints (functions and custom events) and their pins, so an AI can write event keys.
- `Compile` (§5) and the `Validation` findings.
- `References`: assets it references (subsequences, animations, sounds, materials) with existence checks.

`inspect_asset_document_target` on a level actor path is the level tool set's business; this profile only
reports how a locator resolves. `list_types` kinds: `sequencer.track` (supported track classes for a sequence
class), `sequencer.section`, `sequencer.binding` (custom binding types).

### 3.9 Not authorable yet (stable diagnostics, preserved on update)

| Content | Diagnostic | Behaviour |
|---|---|---|
| Channel override registry (procedural channels) | `Sequencer.UnsupportedChannelOverride` | extracted as `{"Unsupported": ...}` on the section; must be listed unchanged |
| Cross-sequence binding references the document cannot name (a sequence ID no sub / shot section of this sequence has) | `Sequencer.UnsupportedRemoteBinding` | raw `{"Guid", "SequenceID", "ParentIndex"}` (`FMovieSceneObjectBindingID`) extracted and preserved; references into a sub / shot section are authored (`{"Binding", "Sequence"}`) |
| Mixed binding references (locator + custom binding on one binding) | `Sequencer.UnsupportedBindingShape` | preserved |
| Channel types with no reflected key arrays | `Sequencer.UnsupportedChannel` | preserved |
| Control Rig parameter sections | `Sequencer.ControlRigTrack` (owned by the Control Rig design) | read via the generic channel proxy; authoring belongs to the Control Rig design |


## 4. Editor-equivalent operations

### 4.1 What the editor does, and the three layers we can call

A human edits a sequence through three layers, and they differ in what they do around the data change:

1. **Sequencer UI** (`FSequencer`, track editors such as `FKeyframeTrackEditor`, `F3DTransformTrackEditor`,
   `FCameraCutTrackEditor`, `FSubTrackEditorBase`, `FEventTrackEditor`). Every action opens its own
   `FScopedTransaction` (text per call site), calls `Modify()` on the sequence / movie scene / track / section, edits
   the data model, then `ISequencer::NotifyMovieSceneDataChanged(EMovieSceneDataChangeType)`
   (`ISequencer.h:190`, `Sequencer.cpp:2746`). The notification only refreshes the open UI (tree, curve editor,
   playback state); persistence is `Modify()` + `MarkAsChanged()` (signature) on the data objects. Many actions need
   an `ISequencer` with a playback world: Convert to Spawnable / Possessable / custom binding
   (`FSequencerUtilities::ConvertTo*`, `SequencerUtilities.h:278–286`), Create Camera, Add Actors, Rebind Component,
   Copy / Paste Bindings, Fix Actor References, Bake.
2. **Headless editor utilities** that the UI itself calls and that need no `ISequencer`:
   `FSequencerUtilities::CreateOrReplaceBinding(nullptr, Sequence, Object, Params)` (`SequencerUtilities.h:325`, used by
   Take Recorder and Movie Render Graph; own transaction), `FSequencerUtilities::CopyTracks/PasteTracks/CopySections/
   PasteSections/CopyFolders/PasteFolders` (string in, string out), `FMovieSceneSequenceEditor::
   GetOrCreateDirectorBlueprint`, `FMovieSceneEventUtils::*` (`MovieSceneEventUtils.h`, exported),
   `MovieSceneToolHelpers::CreateCameraCutSectionForCamera`, `UMovieSceneCameraCutTrack::AddNewCameraCut`,
   `UMovieSceneSubTrack::AddSequenceOnRow`, `ULevelSequence::CreateSpawnable(UClass/UObject)` →
   `MovieSceneHelpers::TryCreateCustomSpawnableBinding` (adds the possessable, the custom binding and a Spawn track).
3. **SequencerScripting** (`UMovieSceneSequenceExtensions`, `UMovieSceneBindingExtensions`,
   `UMovieSceneTrackExtensions`, `UMovieSceneSectionExtensions`, `UMovieSceneScripting*Channel`,
   `USequencerToolsFunctionLibrary::CreateEvent` / `CreateQuickBinding`). It is what Python and Editor Utility
   Blueprints use. It calls the same data-model methods with `Modify()` only: **no transaction, no
   `MarkAsChanged`, no notification**, and channel setters do not re-run `AutoSetTangents`.

The profile uses the data-model calls of layers 2 and 3 (all exported: `UMovieScene`, `UMovieSceneTrack`,
`UMovieSceneSection`, channel `AddKeyToChannel` / `TMovieSceneChannelData`, `FMovieSceneBindingReferences`) and adds
what layer 1 adds around them: one `FScopedTransaction` per op, `Modify()` on every touched object, `MarkAsChanged()`
on the movie scene (so compiled data and parent sequences see the change), `AutoSetTangents` after key writes. That
is the UI's effect without the UI. `NotifyMovieSceneDataChanged` is not reproduced because no Sequencer is open
(open sequences refresh after the apply, §5.4). The few UI-only operations become isolated raw files (B1 rule, §4.4).

### 4.2 Mapping

| Edit (document) | What a human does | Our call | New C++? |
|---|---|---|---|
| New Level Sequence | Content Browser → Cinematics → Level Sequence | generic factory lifecycle (`ULevelSequenceFactoryNew`, no dialog: `Initialize` + project default playback range) then the Body | no |
| New Template / Camera Animation Sequence | Content Browser → Template Sequence (class picker dialog) / Camera Animation Sequence | factory with `BoundActorClass` set from `Body.Root.Class` (the dialog's value; a profile-specific factory context like StateTree's schema), `UCameraAnimationSequenceFactoryNew` as is | primitive (factory setup) |
| `Settings` display rate, evaluation, clock, playback / selection range, locks | toolbar / Sequencer settings menu | `SetDisplayRate`, `SetEvaluationType`, `SetClockSource` / `SetCustomClock`, `SetPlaybackRange`, `SetSelectionRange`, `SetPlaybackRangeLocked` | op |
| `Settings.TickResolution` | Advanced → Tick Resolution | `UE::MovieScene::TimeHelpers::MigrateFrameTimes(Old, New, MovieScene)` then `SetTickResolutionDirectly` (what the editor's change-tick-resolution path does) | op |
| Add possessable with locator(s) | drag a level actor into Sequencer / "+ Track → Actor To Sequencer" | `UMovieScene::AddPossessable(Name, Class)`, `GetBindingReferences()->AddBinding(Guid, ParsedLocator)`, `ParentGuid` for children (what `ULevelSequence::FindOrAddBinding` + `BindPossessableObject` do with a live object) | op (+ locator parse) |
| Replace / add / remove a locator | binding menu → Assign Actor / Add / Remove selected actors | `AddOrReplaceBinding(Guid, Locator, Index)`, `RemoveBinding` + re-add in order | op |
| Add spawnable | "+ Track → Actor To Sequencer" with Convert, or drag a class / asset | `ULevelSequence::CreateSpawnable(UClass*)` (scripting `AddSpawnableFromClass`) → custom binding + Spawn track; then template properties | op |
| Spawnable template properties | select spawned actor, edit Details, "Save Default State" | Modify + typed value codec write on `UMovieSceneSpawnableActorBinding::ActorTemplate` (and its components) with `PreEditChange` / `PostEditChangeProperty`, as Details editing the template object does | op |
| Convert possessable ↔ spawnable ↔ custom binding | binding context menu → Convert To ... | **raw file** `AssetDocumentSequencerBindingConversion.cpp`: keep the GUID, `AddOrReplaceBinding` with the new custom binding (`CreateNewCustomBinding` from the class CDO or the template), add / remove the Spawn track, update `PossessedObjectClass` — the data steps of `FSequencerUtilities::ConvertTo*` without the world (the editor copies the live actor; the document supplies class + template). Behaviour test against the scripting / utility path on a duplicated corpus asset | raw file |
| Rename binding | F2 in the tree | `FMovieScenePossessable::SetName` + `UMovieScene::SetObjectDisplayName` | op |
| Tags | binding menu → Tags | `UMovieScene::TagBinding` / `UntagBinding` / `FMovieScenePossessable::Tags` | op |
| Delete binding | Delete | `UMovieScene::RemovePossessable` (children and tracks), `BindingReferences.RemoveBinding`, folder cleanup | op |
| Reorder bindings / tracks / folders | drag in tree | `SortingOrder` writes (editor-only data), as the tree's drop handler | op |
| Add track | "+ Track" / binding "+ Track" | `UMovieScene::AddTrack(Class, Guid)` (refused by `IsTrackClassAllowed` / `IsTrackSupported`), `PropertyBinding` via `SetPropertyNameAndPath` for property tracks, display name for nameable tracks | op |
| Track Details (eval options, mute, tint, conditions) | track context menu / Details | Modify + typed write + `PostEditChangeProperty`; mute via `SetEvalDisabled` | op |
| Add / delete section, range, row | "+ Section", drag / resize, Delete | `CreateNewSection` + `AddSection`, `SetRange` / `SetStartFrame` / `SetEndFrame` (honour `TryModify` locks), `SetRowIndex`, `RemoveSection`; then `UMovieSceneTrack::FixRowIndices` if the track does (what drag-drop does) | op |
| Section Details (blend, easing, pre/post roll, when finished, section class properties) | section Details panel | Modify + typed write + `PostEditChangeProperty` on the section; easing functions as instanced objects | op |
| Keys / channel defaults / extrapolation | key with Enter / S, curve editor, key Details | channel data interface: `GetData().Reset()`, `AddKey` in time order, value / tangent writes, `SetDefault`, `PreInfinityExtrap` / `PostInfinityExtrap`, then `AutoSetTangents()` (as `AddKeyToChannel` and the key editors do) | op |
| Parameter sections | "+ Parameter" on material / MPC / Niagara tracks | `AddScalarParameterKey` / `AddColorParameterKey` / ... (create the curve), `RemoveScalarParameter` ..., then channel writes | op |
| Camera cut | Camera cut track "+" / camera lock button | `UMovieScene::AddCameraCutTrack`, `UMovieSceneCameraCutTrack::AddNewCameraCut(BindingID, Start)` or section writes with `SetCameraBindingID` | op |
| Subsequence / shot | "+ Shot", "+ Subsequence", drag asset | own DFS cycle check, then `UMovieSceneSubTrack::AddSequenceOnRow` / `UMovieSceneSubSection::SetSequence` | op |
| Folders | "Add Folder", drag into folder, folder color | `UMovieScene::AddRootFolder` / `UMovieSceneFolder::AddChildFolder`, `AddChildTrack`, `AddChildObjectBinding`, `SetFolderColor` | op |
| Markers | M / marker Details | `AddMarkedFrame`, `SetMarkedFrame`, `DeleteMarkedFrame`, `SortMarkedFrames`, `SetMarkedFramesLocked` | op |
| MetaData | Details of the sequence | `FindOrAddMetaData<UMovieSceneMetaData>()` Modify + write | op |
| Director Blueprint create | toolbar "Open Director Blueprint" | `FMovieSceneSequenceEditor::Find(Seq)->GetOrCreateDirectorBlueprint(Seq)` | op |
| Director members and graphs | Director Blueprint editor | the Blueprint profile's existing `blueprint.*` / `graph.*` primitives on the hosted Blueprint (§3.6) | adapter |
| Event endpoints and keys | event key "Create Event Endpoint" / "Create Quick Binding", key Details | `FMovieSceneEventUtils::SetEndpoint(Event, Section, Node, BoundObjectPin)` to the named function entry / custom event; `BindEventSectionToBlueprint` (event section ↔ director) when the section is new; `PayloadVariables` writes. Creating a missing endpoint is a Director graph edit in the same document (the AI writes the function), not an implicit side effect | op |
| Director bindings | binding → "Dynamic binding" picker | `CustomBinding` class `UMovieScene*DirectorBlueprintBinding` with `DynamicBinding` endpoint set like events | op |
| Folders of nodes, groups | "Group" / node groups panel | `GroupSections` / `UngroupSection`, `UMovieSceneNodeGroupCollection` adds | op |

### 4.3 Planning order (Python)

`Settings` (rates and tick-resolution migration first: all later frames are converted with the final rates) →
Director members (functions / custom events must exist before event keys bind to them) → binding renames →
binding deletes → binding creates (parents before children, so child locators and component templates resolve) →
binding kind conversions → locators / tags / spawnable templates → track deletes → track creates → per track:
Details, sections (deletes from the end, in-place edits, appends), section headers before channels, channels,
parameter sets → camera cut and sub / shot sections (they reference bindings and other sequences) → folders and
order → markers → MetaData → Director graphs (the rest) → compile (§5). Each op carries its document path; C++ op
errors come back as `/ops/<n>` and are mapped back (as Material / IK Rig).

### 4.4 Primitives and module

New module **`AssetDocumentSequencer`** (Editor, PostDefault), owner `EAssetDocumentPrimitiveOwner::Cinematics`
(value 12; a new owner is appended at the end of the enum), provider id `seq`. Dependencies: `MovieScene`,
`MovieSceneTracks`, `LevelSequence`, `UniversalObjectLocator`, `Sequencer` (`FMovieSceneSequenceEditor`,
`FSequencerUtilities`), `MovieSceneTools` (`FMovieSceneEventUtils`, `MovieSceneToolHelpers`), `TemplateSequence`,
`LevelSequenceEditor` (module load only: it registers the Level Sequence's `FMovieSceneSequenceEditor`),
`AssetDocumentEditor` (typed value codec, non-interactive scope), `AssetDocumentBlueprint` / `AssetDocumentGraph`
(hosted director). `UEAssetDoc.uplugin` adds the `TemplateSequence`, `LevelSequenceEditor` and `SequencerScripting`
plugins (all already enabled in a default editor through `GameplayCameras` / `EngineCameras` / `LevelSequenceEditor`).

- `seq.read.v1`: paged read of one sequence in document-ready records: settings, bindings (GUID, kind, name, class,
  parent, locator strings, custom binding class + reflected properties, template diff vs class defaults), tracks
  (class, property binding, display name, object name, Details diff vs class defaults), sections (header, reflected
  properties diff vs a new section of the class, channels via the channel proxy: name, type, keys, default,
  extrapolation), parameter sets, folders, markers, metadata, director endpoints. Frames converted to display frames
  in C++ (exact rational conversion, §3.3).
- `seq.edit.v1`: op list over one sequence with the ops of §4.2 (`settings`, `binding.add/remove/rename/convert/
  locators/tags/template/order`, `track.add/remove/set/order`, `section.add/remove/set`, `channel.set`,
  `parameter.add/remove`, `folder.*`, `marker.*`, `metadata`, `director.create/remove`, `event.bind`).
- `seq.status.v1`: §5 checks and the compile.
- `seq.inspect.v1`: §3.8 (locator resolution in the open editor world, animatable properties per class, channel
  catalogue per section class, director endpoints).
- `seq.catalog.v1`: `list_types` kinds.
- Raw files (each with one behaviour test): `AssetDocumentSequencerBindingConversion.cpp` (Convert To ...),
  `AssetDocumentSequencerTemplateRoot.cpp` (Template Sequence "change bound class": replicate
  `TemplateSequenceEditorUtil.cpp:20–100` — new root spawnable from the class via the level sequence module's object
  spawners, `MoveBindingContents`, `RemoveSpawnable`, `PreEditChange` / `PostEditChangeProperty(BoundActorClass)` —
  without `ISequencer::MakeNewSpawnable`).
- Generic property admission covers only `AssetUserData`. Everything else is Body.

Python: `profiles/level_sequence.py` (header, regions, apply plan), `profiles/template_sequence.py`,
`profiles/sequencer_bindings.py` (identity, paths, locators, templates), `profiles/sequencer_tracks.py` (type tokens,
track / section identity, section headers), `profiles/sequencer_channels.py` (frame conversion rules, key forms,
diff that ignores derived auto tangents — shareable with the UMG profile), `profiles/sequencer_director.py`
(hosted Blueprint adapter onto `ublueprint_*`).

## 5. Compile, validation, rollback

### 5.1 What "compile" is for a sequence

A sequence has no compile button. Three things play that role in the editor:

1. **Movie scene compilation** to evaluation data (`UMovieSceneCompiledDataManager::GetPrecompiledData()->Compile
   (Sequence)`, `MovieSceneCompiledDataManager.h:250`): happens lazily when the sequence plays or is scrubbed, after
   every signature change. It builds the hierarchy of subsequences, evaluation fields and entity fields; it reports
   nothing, and **it has no cycle guard** (`CompileHierarchy` / `GenerateSubSequenceData` recurse into every sub
   section; the only cycle protection is in the sub-track editor's add path, which itself does not catch indirect
   cycles). A sub-sequence cycle written by a document would hang or overflow the stack in the compiler, so the
   profile checks the full sub-sequence graph (DFS over `UMovieSceneSubSection::GetSequence()` of this sequence and
   every sequence it reaches, loaded from disk read-only) **before** writing any sub / shot section, and refuses with
   `Sequencer.CircularSubsequence` at the section path.
2. **Director Blueprint compile** (`FKismetEditorUtilities::CompileBlueprint`, what `FSequencer::
   RecompileDirtyDirectors`, `Sequencer.cpp:12417`, does before playback): generates event entry points
   (`UMovieSceneEventBlueprintExtension`), resolves `FMovieSceneEvent::Ptrs`, sets `DirectorClass`.
3. **Sequence validation**: the editor shows problems in place (red unbound bindings, missing assets, warning icons)
   and, in 5.7, the experimental **Sequence Validator** panel (`Plugins/MovieScene/SequenceValidator`, four rules:
   unassigned bindings / assets, duplicate keys, sections not on whole frames, section bounds within two frames of
   camera cuts / playback bounds). Its rule runner (`FSequenceValidationRule::Run`) is not exported, so the profile
   runs its own checks for those four rules (small, read-only) and reports them with the rule names.

`seq.status.v1` runs, in this order: the cycle DFS, the profile checks (below), the director compile when the
director changed or is not up to date, then the movie scene compile (cheap; it refreshes precompiled data the way the
next playback would and proves the data compiles). Checks:

- errors: unknown track / section / binding class, a property track whose `Property` does not resolve on the bound
  class (`Sequencer.UnknownProperty`, using the same property path resolution as `FTrackInstancePropertyBindings`),
  a camera cut / attach / actor-reference value naming a missing binding, an event key whose endpoint is missing,
  a sub-sequence cycle, a parameter that the material / MPC does not have (`Sequencer.UnknownParameter`), a Template
  root class that is not an actor (`Sequencer.RootClassInvalid`);
- warnings: Sequence Validator rules; a possessable with no locator; a locator that does not resolve **in the
  currently open world** (info only: the level may simply not be loaded); a section on a muted / disabled row;
  director compile warnings.

### 5.2 Failure policy and status shape

As Material and IK Rig: the before-status is read first; **new errors** fail the apply with
`Sequencer.ValidationFailed` (or the specific code) at the document path and roll back; errors the asset already had
come back as `Sequencer.ValidationError` warnings; warnings come back as `Sequencer.ValidationWarning`. Director
compile errors fail with `Graph.FinalizeFailed` exactly as the Blueprint profile does. Extract / Apply / Inspect carry

```json
{"Status": "UpToDateWithWarnings",
 "Messages": [{"Severity": "Warning", "Rule": "UnassignedBindingsAndAssets",
               "Message": "Section SkeletalAnimation [0..120) has no animation.",
               "Path": "/Body/Bindings/0/Tracks/1/Sections/0"},
              {"Severity": "Info", "Message": "Locator does not resolve in the open world (/Game/Maps/Lobby is not loaded).",
               "Path": "/Body/Bindings/1/Bind/0"}],
 "Director": {"Status": "UpToDate", "Messages": []}}
```

`Status`: `UpToDate`, `UpToDateWithWarnings`, `Error`, `Dirty` (director not compiled since the last change).

### 5.3 Rollback

- Existing asset: the Core package snapshot covers everything in the sequence package — movie scene, tracks,
  sections, custom bindings and spawnable templates, folders, and the inner Director Blueprint. A derived-state
  rebuilder (`AssetDocumentSequencer.Sequence`) then does what the editor's undo does: `PostEditUndo` on the movie
  scene (re-caches channel proxies, `MarkAsChanged`), `UMovieSceneCompiledDataManager::GetPrecompiledData()->Reset
  (Sequence)`, and, when the director was touched, the Blueprint rebuilder (recompile, `DirectorClass`).
- Created asset: factory rollback deletes the package (the director and templates are inner objects).
- Dependents ([design rules](asset-document-design-rules.md) §3.2): a sequence is referenced by parent sequences (sub / shot sections, camera cuts
  into shots), Blueprints (`K2Node_GetSequenceBinding` pins storing binding GUIDs), Template tracks and level actors
  (binding overrides). None of them is edited. Renaming keeps GUIDs, so nothing breaks; **deleting** a binding or
  re-creating it (`Sequencer.BindingRecreated`) changes what those references find, so the apply lists the
  referencers from the asset registry and reports `Sequencer.DependentsAffected` (warning), like the IK Rig decision.
  The compiled data of loaded parent sequences is invalidated through the signature, as in the editor.
- Undo: each op records its own transaction, as the UI does; a successful apply clears the Undo history that
  references the package (§3.1); a failed apply after ops clears it too (as StateTree / Niagara).

### 5.4 Open editors and environment

- A sequence open in a Sequencer (Level Sequence editor toolkit, Template Sequence editor, or as the focused / any
  sub-sequence of an open Sequencer): the apply lands in place and every open Sequencer whose root reaches the
  sequence is refreshed as after its own Undo (`NotifyMovieSceneDataChanged(Unknown)`, then `ForceEvaluate`), after a
  success and after a rollback.
- Spawnable templates and director compiles run inside `FAssetDocumentEditorNonInteractiveScope`. Nothing in the
  paths above opens dialogs; the Blueprint compile path is already used by the Blueprint profile.
- No level is loaded, saved or dirtied by any primitive. Inspect's locator resolution only looks at the world already
  open in the editor (`GEditor->GetEditorWorldContext().World()`), never loads a map, and the apply never consults
  it.

## 6. Fidelity acceptance plan

### 6.1 Corpus (real content in the UE 5.7 install)

An inventory of all `.uasset` / `.umap` files under `<UE_ROOT>` finds **21 real
sequence assets** (19 Level Sequences, 2 Day Sequences) and **no** Template Sequence, Camera Animation Sequence,
Actor Sequence or Director Blueprint asset. Most live in template projects (`Templates/*/Content`, i.e. `/Game` in
those projects) and are copied into the sandbox under `/Game/AssetDocumentTests/Sequencer/<Template>/...` together
with the maps they bind to (for locator resolution tests). Plugin content is read from its mount. Nothing in the
install is modified.

| # | Asset (path under the UE 5.7 root) | Covers |
|---|---|---|
| 1 | `Templates/TP_UEIntro_BP/Content/DemoTemplate/Sequencer/NewLevelSequence` | 5.7-saved **spawnable** (`MovieSceneSpawnableActorBinding`, CineCameraActor template) + possessable, spawn track, transform, camera cut, two float tracks on the camera component, `MovieSceneDoubleChannel`, metadata |
| 2 | `Templates/TP_PhotoStudioBP/Content/Studios/Sequences/SEQ_Orbit_Studio_Dark` | two transform tracks, four float tracks (focal length, aperture, focus distance), camera cut, bool property (`bVisible`) |
| 3 | `Templates/TP_PhotoStudioBP/Content/Studios/Sequences/SEQ_Truck_Studio_White` | smaller variant of #2 |
| 4 | `Templates/TP_AEC_ProdConfigBP/Content/ProductAssets/Sequences/Camera1_Seq` | 4.27-era asset upgraded on load (legacy binding references → locators), **bool track**, camera look-at tracking struct, section eval options |
| 5 | `Templates/TP_AEC_ArchvisBP/Content/ArchvisProject/Sequences/Cameras/Seq_CineCameraActor01` (and `03`, `02`) | **fade track**, transform, camera float tracks, camera cut, dynamic binding upgrade |
| 6 | `.../ArchvisProject/Sequences/Cameras/Seq_Master_Cameras` | **cinematic shot track** referencing #5 (shots) |
| 7 | `.../ArchvisProject/Sequences/SunSky/Seq_MasterSunSky` | **subsequence track** (three sub sequences) |
| 8 | `.../ArchvisProject/Sequences/SunSky/Seq_SunSkyEquinox` (+ `SummerSolstice`, `WinterSolstice`) | possessable bound to the level actor `PersistentLevel.SunSky_2` of a Blueprint class (`/SunPosition/SunSky.SunSky_C`), float + **integer** tracks |
| 9 | `Templates/TP_AEC_ProdConfigBP/Content/ProductAssets/Sequences/TimeofDaySeq` | dense float curve (77 KB for one track): key-count / extract-size budget |
| 10 | `.../ArchvisProject/Sequences/Stills/LS_Shot_01_626` | camera possessable, focal-length track, camera cut |
| 11 | `Engine/Plugins/MovieScene/MovieRenderPipeline/Content/Editor/Stills/Sequencer/LS_StillBlank` | **legacy `FMovieSceneSpawnable`** (pre-5.4) converted on load by `ConvertOldSpawnables`: extract must show the converted state |
| 12 | `Templates/TP_DMXBP/Content/DMXTemplate/Sequences/LS_FixtureDemo` (+ `LS_WaterFountain`) | **plugin track** `MovieSceneDMXLibraryTrack` with plugin channel types (`DMXFixturePatchChannel`, `DMXFixtureFunctionChannel`), node groups: the generic reflected-channel path and `Unsupported` reporting |
| 13 | `Templates/TP_AEC_ArchvisBP/Content/ArchvisProject/Levels/Exterior.umap`, `TP_PhotoStudioBP/.../Studio_Dark.umap`, `TP_UEIntro_BP` `Lvl_IntroRoom` (World Partition, external actors) | maps the sequences bind to: Inspect locator resolution (loaded / not loaded / external actor) |

Not in the corpus: Day Sequences (`/DaySequence/DS_24hr`, `/CelestialVault/DS_CelestialVault`: out of scope, but
they are the only real **material parameter** tracks and are used as a read-only extract smoke test if the plugin is
enabled), the 12 Widget Blueprints with UMG animations (UMG profile).

Plugins: the corpus needs **MovieRenderPipeline** (#11), **DMXEngine** + **DMXProtocol** (+ their template
dependencies, #12) and, so that #8 resolves its bound class, **SunPosition**, all enabled in the test sandbox
.uproject only, like Harmonix / MoverExamples. Tests that need them skip with a note when they are off. The
UEAssetDoc plugin depends only on the default ones (§4.4).

**Built fixtures** cover what no shipped asset has. They are written once with the engine's own scripting
(`Engine/Plugins/MovieScene/SequencerScripting/Content/Python/sequencer_examples.py`, `sequencer_key_examples.py`,
`sequencer_anim_examples.py` patterns) in an editor-side test fixture and saved under
`/Game/AssetDocumentTests/Sequencer/Fixtures/`: skeletal animation (MoverExamples' mannequin and animations, already
enabled for IK Rig), audio (engine sounds), event trigger / repeater tracks with a Director Blueprint (function
endpoint, quick-binding custom event, payloads, bound object pin), director binding, MPC and component material
parameter tracks, visibility, attach / path, actor reference, CVar, level visibility, data layer, slomo, camera shake,
string / enum / byte / text / object-path channels, weighted tangents and every interpolation mode, easing with each
function, conditions on tracks / sections, section groups, node groups, a Template Sequence and a Camera Animation
Sequence (with component tracks), and a three-level shot hierarchy with a camera cut into a shot's camera (remote
binding reference).

### 6.2 What is compared

For each corpus asset and fixture, two round trips: (1) extract → apply as a new sandbox asset (`Create`) → extract;
(2) Content Browser duplicate → extract. Then:

1. **Documents** equal (floats normalised, frames exact, identities of §3.4.2 / §3.5.2). The re-apply of each extract
   is a no-op (diff empty, no transaction).
2. **Data facts** read from the engine objects, not the document: per binding (kind, class, locator strings,
   custom binding class and non-default template properties, parent), per track (class, property path, eval options,
   mute), per section (range in ticks, row, blend, easing, pre/post roll, completion mode, reflected properties),
   per channel (times in ticks, values, interp / tangent modes, **stored tangents of User / Break keys**, defaults,
   extrapolation), folders, markers.
3. **Evaluation** where cheap: every float / double channel evaluated (`FMovieSceneFloatChannel::Evaluate`, exported)
   at each key and at the midpoint between keys; equal within float tolerance. This catches auto-tangent and
   interpolation drift that the document comparison ignores on purpose. Plus the compiled hierarchy (sub-sequence
   count, per sub-sequence play range and time transform) from `Compile`.
4. **Not compared**: signatures, compiled data blobs, key handles, editor view state, metadata `Created` / `Author`
   (duplicate resets them), thumbnails.

Pass bar: 100 % of the corpus, or a listed `Unsupported` code per asset. A smoke run extracts all 21
sequences (with the optional plugins on) headless, with no crash and no unknown diagnostic, and records the time.

### 6.3 Free-edit scenarios (on duplicates)

- **Bindings** (on #1): rename the camera (camera cut and folder follow, GUID unchanged); add a possessable by
  locator to an actor of the copied map; give a binding a second locator; delete the possessable; convert the camera
  spawnable to a possessable and back (GUID and tracks kept); edit the spawnable template (`CurrentAperture`) and
  check a spawned preview actor gets it (behaviour test spawns through `UMovieSceneSpawnableActorBinding::SpawnObject`
  in an editor world).
- **Tracks and sections** (on #2): add a `Float` track on `CurrentFocalLength`; split a transform section into two
  sections on rows 0 / 1 with additive blend; ease-in 10 frames; mute a track; reorder tracks; delete a section in
  the middle of three.
- **Keys** (on #5): change one key to `Constant`, one to `Break` with explicit tangents, add sub-frame keys, set
  post-infinity `Cycle`, set a default on an empty channel; check evaluation against the editor's own key edit done
  through the scripting API on a second duplicate.
- **Time** (on #6): change display rate 30 → 24 (frames in the new rate), change tick resolution 24000 → 60000
  (migration; evaluation equal), move the playback range, lock it.
- **Hierarchy** (on #6 / #7): add a shot, retime a shot (`StartFrameOffset`, `TimeScale`), reorder shots; add a
  subsequence that would create an indirect cycle (A → B → A): refused before any change.
- **Folders / markers**: nest folders, move bindings and tracks between folders, colour a folder; add, move and
  delete markers, set a determinism fence.
- **Director / events** (fixture): add a function endpoint and an event key that calls it with a bound object pin
  and a payload; rename the function in the same document (key follows); delete the endpoint while a key uses it
  (refused); a director compile error rolls back the whole apply.
- **Template** (fixture): change a Template Sequence's bound class (tracks follow the new root); Camera Animation
  Sequence focal length and transform keys.
- **Failure**: unknown property, a camera cut to a missing binding, a sub-sequence cycle, a malformed locator
  (`Sequencer.InvalidLocator`) — each fails before or rolls back after the change; Extract afterwards equals the
  document before.
- **Dependents**: delete a binding that a Blueprint `Get Sequence Binding` node references → `DependentsAffected`.
- **Open editor**: apply while the sequence is open in Sequencer lands in place and the open Sequencers refresh (`OpenEditorApply`).

### 6.4 AI from scratch

A fresh AI session with only the MCP tools, `MCP/schemas/assetdocument/Sequencer.md` and the level tool set
for finding actors writes, in the copied Archvis map:

- a 12-second intro `LS_Intro` at 24 fps: a spawned cine camera on a slow dolly with a focus pull, a second camera
  for a reverse shot, camera cuts between them, fade in and out, a marker per beat;
- a possessable binding to the map's sun actor that animates time of day;
- a shot track master `LS_IntroMaster` with `LS_Intro` and a new `LS_Outro` shot;
- an event at the last frame that calls a Director function printing "Intro done";

then three follow-ups: "make the dolly ease out", "add a camera shake on the reverse shot", "move the whole outro
two seconds later". It applies, fixes its own diagnostics, and the result plays in the editor (human check in
Sequencer, plus the evaluation facts). Recorded (calls, failed calls, diagnostics, wall time).

## 7. Risks and unknowns

- **Bindings live outside the asset's world.** A possessable is only data (a locator) until a world resolves it;
  the editor's own add path needs a live actor. Mitigation: author locators as strings (§3.4.3) through exported
  `FUniversalObjectLocator::TryParseString` + `FMovieSceneBindingReferences::AddBinding`; Inspect resolves in the
  open world read-only; the level tool set gives an AI the actor paths. Unknown: World Partition external
  actors and level instances produce locator strings with container IDs / streaming parameters — the corpus #13
  (`Lvl_IntroRoom`) covers it; if a form cannot be parsed back exactly, those bindings are extracted with the raw
  string and kept (`Sequencer.UnsupportedLocator`).
- **UI-only operations.** Convert To..., Create Camera, Fix Actor References, Copy/Paste Bindings, Template "change
  bound class" need an `ISequencer`. Mitigation: the data steps in two isolated raw files with behaviour tests (§4.4);
  Create Camera = a spawnable of `ACineCameraActor` plus a camera cut (document content, no special op); Fix Actor
  References (rebinding by actor label) is a level-tool feature, not a document key.
- **No transactions / notifications in the scripting layer; no cycle guard in the compiler.** Mitigation: the primitive
  owns transactions, `Modify`, `MarkAsChanged`, `AutoSetTangents`; the cycle DFS runs before any sub-section write.
- **Time exactness.** Display frames vs ticks, non-integer ratios (29.97), sub-frame keys, frame-number properties
  whose rate is not the outer display rate (sub-section offsets). Mitigation: rational conversion in C++, shortest
  round-tripping decimals, an explicit per-property rate table, and the evaluation comparison in 6.2. Unknown:
  `FMovieSceneTimeWarpVariant` (play rate / time scale) is custom-serialised; the typed value codec needs a small
  adapter (`{"PlayRate": 1.5}` for the fixed form, reflected payload otherwise).
- **Generic reflection over ~45 built-in + plugin track classes.** Section properties include instanced objects
  (easing functions, conditions, constraint containers), interface pointers and custom structs. Mitigation: typed value
  codec + per-type adapters for `FMovieSceneObjectBindingID`, `FMovieSceneTimeWarpVariant`, `FFrameNumber`,
  easing / condition instanced objects; anything else is reported `Unsupported` and preserved, never dropped.
- **Dense data.** Baked or recorded sequences (Take Recorder, mocap, #9) have thousands of keys per channel;
  extract size and diff time grow. Mitigation: paged read (`seq.read.v1`), C++ channel comparison (hash per channel,
  details only for differing channels), and the dense key form of decision §8.1.
- **Director Blueprint hosting.** The Blueprint profile assumes the Blueprint is the asset. Unknown: package /
  target assumptions in `ublueprint_runtime` (target paths, compile status, rebuilder, sidecar) for an inner
  Blueprint. Mitigation: the Blueprint member / graph primitives run on `LS_X.LS_X:LS_X_DirectorBP` (tested); the target
  resolution is adapted in one place.
- **Load-time conversions.** `ConvertOldSpawnables`, dynamic-binding conversion, `ObjectReferences_DEPRECATED` and
  `FLevelSequenceBindingReferences` upgrades run in `PostLoad` and dirty nothing; the extract shows the converted
  state and applying it writes the new form. That is the editor's behaviour (it saves the new form on next save);
  fidelity #4 and #11 check it.
- **Version-specific APIs.** Custom bindings (5.4+), `FUniversalObjectLocator` (5.4+), time-warp variants, decorations
  (5.6+), track row metadata / conditions (5.5+) are recent and still moving. Mitigation: one module; `ENGINE_MINOR_VERSION`
  gates; the corpus test catches drift.
- **Editor-only data.** Folders, markers' comments / colours, metadata, display names, sorting order and the director
  are `WITH_EDITORONLY_DATA`; they are authored content for an editor tool and exist in every editor build.
- **Performance of the compile check.** Compiling a large shot hierarchy loads every subsequence. Mitigation: compile
  only when sub / shot sections or bindings changed, otherwise reuse; record time on #6 / #7.

### 7.1 Relations to other work

- **UMG Widget animations** use the same `UMovieScene` data. `widget_blueprint_animation.py` could use
  `sequencer_channels.py` (key forms, sections) and lift its "one section, plain keys" limit. Its key default
  (`Interp` Linear, `Tangent.Mode`) differs from this design's Sequencer key menu tokens; decision §8.2.
- **Control Rig ([asset-document-controlrig-design.md](asset-document-controlrig-design.md))**: Control Rig tracks in sequences (`MovieSceneControlRigParameterTrack`) store per-control
  channels in a parameter-section-like layout; this design reads them through the generic channel proxy and leaves
  authoring to the Control Rig design, which should reuse §3.3 / §3.5.3 (decision §8.6).
- **Level / actor tool set ([level-editing-tools-design.md](level-editing-tools-design.md))**: provides actor queries that return locator strings and the place / bind
  operations on `ALevelSequenceActor`; this profile never edits levels.
- **Generic Blueprint profile**: hosts nothing new; the director adapter is the first "Blueprint inside another
  asset", which Actor Sequences (Blueprint components) could reuse.

## 8. Decisions

1. **Dense channels: two key forms.** A channel is extracted as key objects up to 64 keys, and in the columnar form
   `{"Frames": [...], "Values": [...], "Interp": <token or list>, "Arrive"?, "Leave"?, ...}` when it has more than 64
   keys. Apply accepts both; both canonicalise to the same data and nothing is elided.
   Note: `Interp` is one token when every key has it, a list otherwise, and omitted when every key is `SmartAuto`.
   Tangent / weight columns hold `null` for keys without explicit tangents. The read emits the columns in chunks of
   400 keys, and the profile joins them.
2. **Key interpolation tokens.** The Sequencer key menu tokens are used, with `SmartAuto` as the omitted default:
   `SmartAuto`, `Auto`, `User`, `Break`, `Linear`, `Constant`. Reading also yields `Cubic` (a cubic key with an
   unlisted tangent mode) and `None`. The UMG profile has its own key form (§7.1).
3. **Corpus plugins.** The test sandbox `.uproject` enables MovieRenderPipeline, DMXEngine + DMXProtocol and
   SunPosition, and AnimatorKit. Tests that need a plugin skip with a note when it is off.
   UEAssetDoc itself depends only on modules that are on by default.
4. **Template and Camera Animation Sequences: in scope**, with fixtures as their corpus.
5. **Director Blueprint: editable**, by hosting the Blueprint profile's components inside the sequence.
6. **Control Rig tracks.** Sequences read and preserve them through the generic channel proxy
   (`Sequencer.ControlRigTrack` read warning). Authoring belongs to the Control Rig design, on top of this
   design's channel codec.
7. **Binding identity.** A binding is identified by its `Name` among its siblings. `Id` (the GUID digits) is emitted
   only where siblings share a name. It is also emitted for a name containing `/` or starting with `#`, because
   binding paths could not express such a name.
8. **Dependents: warn only.** Deleting or recreating a binding that other packages reference reports
   `Sequencer.DependentsAffected` and never edits the other package.
9. **Apply while the sequence is open in Sequencer: applied in place** (§5.4), not refused: the open Sequencers refresh
   after a success and after a rollback.
10. **Module / owner.** The new module is `AssetDocumentSequencer`, provider `seq`, with owner
    `EAssetDocumentPrimitiveOwner::Cinematics` = **12**. Its core dependencies are engine modules (MovieScene, MovieSceneTracks,
    LevelSequence, UniversalObjectLocator, Sequencer, BlueprintGraph, CinematicCamera); the plugin dependencies
    TemplateSequence, LevelSequenceEditor and SequencerScripting are listed in `UEAssetDoc.uplugin`.
11. **Unresolvable locators.** A locator that does not resolve in the open world is only reported: Inspect
    shows `Resolved: false`, and status reports it as `Info`. A syntactically invalid locator is an error at apply,
    `Sequencer.InvalidLocator`.

### 8.1 Read: implementation notes

- **Corpus location.** The corpus template folders are copied into the sandbox at their own `/Game` paths, as
  creating a project from the template does (`/Game/ArchvisProject/...`, not
  `/Game/AssetDocumentTests/Sequencer/<Template>/...`). The sequences' locators and sub-sequence references name
  those paths, so this is the only way they resolve. Tests remove only what they copied.
- **`list_types` kinds.** The kinds are `sequencer_track`, `sequencer_section` and `sequencer_binding`, with
  underscores like the other provider kinds (`ikrig_solver`, `metasound_node`) instead of dots.
- **Parameter sections.** Read and written in the §3.5.3 `Parameters` form (§8.3).
- **Fixed binding IDs.** A binding reference stored as a fixed ID (not sequence-relative) is written
  `{"Binding": "<path>", "Fixed": true}`. An invalid (empty) ID is `null`.
- **Sparse comparison bases.** The spawnable `Template` is sparse against the class default object and the
  component archetypes. Section `Properties` are sparse against the section class default object.
- **Not read.** Locator resolve flags, the legacy `SpawnableObjectBindingID` of possessables and `ObjectsToLabels`
  are not read; they are derived or editor-only.
- **Status scope.** Status checks this sequence only (the Sequence Validator recurses into subsequences). Locator
  resolution appears only in Inspect.
- **Unnamed channels.** A channel without a metadata name is keyed `Value`. A second channel with the same name is
  keyed `<name>#2`, with a `Sequencer.DuplicateChannelName` read warning.
- **Derived template fields.** `PostLoad` recomputes some component fields from others (a cine camera's
  `FieldOfView` / `AspectRatio` from filmback and lens), so a loaded spawnable template differs from its component
  archetype in them and the `Template` lists them. They are stable across loads. A template created in memory
  by an apply has not been loaded yet, so the apply excludes such derived fields (§8.4, Template baseline).
- **Spawnable children in Inspect.** A component binding under a spawnable resolves against the spawnable's saved
  actor template (the spawned actor exists only during playback). Those locator rows carry `SpawnableTemplate: true`.
- **Change detection.** Apply runs only when Diff found a change. Preflight and apply compare the Body read by
  `seq.read.v1`.

### 8.2 Core authoring: implementation notes

- **Two primitives, op batches.** `seq.edit.v1` takes `{ops: [...]}` and runs them in one `FScopedTransaction` inside
  `FAssetDocumentEditorNonInteractiveScope` (not one transaction per op: Core clears the undo history that references
  the package after every apply anyway, §5.3). The ops are `settings`, `binding.add / remove / rename / set / order`,
  `track.add / remove / set / order`, `section.add / set / remove`, `channel.append`, `folders.set`, `markers.set`,
  `metadata.set` (the §4.4 list merged: `binding.set` carries kind, locators, template, custom binding and tags;
  folders and markers are written as whole lists). The result lists `changed`, the objects created per op (track and
  section object names, so the next batch can address them) and `dependents`. `seq.status.v1` returns the status
  record (§5.1) the apply compares before and after.
- **Planning (Python, `level_sequence_apply.py`).** One read with GUIDs and object names, then three batches:
  structure (settings, binding deletes / renames / adds, track deletes / adds), content (binding kinds and content,
  binding order, track Details, sections in place / deleted from the end / appended, track order), and the rest (dense
  channel chunks, folders, markers, metadata). A rate change (`DisplayRate` or `TickResolution`) runs first and the
  sequence is read again, because every frame of the document is in the new rates. Dense channels are split into
  `channel.append` chunks of at most 1500 key nodes.
- **Track identity on create.** A new track whose document entry has `Id` is renamed to it (when the name is free),
  so a created copy of a sequence keeps the object names the source's document showed.
- **Default spawn track.** Comparison and planning use a normal form in which a spawnable without a listed `Spawn`
  track has the editor's default one (`{"Type": "Spawn", "Sections": [{"Channels": {"Value": {"Default": true}}}]}`)
  at index 0. A live spawnable without any Spawn track keeps that state when the document does not list one.
- **Stated tangents.** The write's tangent rule: `User` / `Break` keys keep their tangents; every other key starts
  without stored tangents and `AutoSetTangents` computes them, as for keys the editor adds. Stored tangents that rule
  would not reproduce (saved by an older Sequencer, or left by key-menu edits: a `Linear` / `Constant` key keeps the
  leave tangent that shapes a following cubic segment, `GSequencerLinearCubicInterpolation`) are read as **stated
  tangents**: the read simulates the rule on a copy of the channel and lists `Arrive` / `Leave` (and weights) on the
  keys whose stored tangents differ, repeating until stable (`SmartAuto` iterates from neighbours' tangents). The write
  keeps stated tangents after `AutoSetTangents`, so the round trip reproduces the stored curve exactly (corpus
  `Seq_CineCameraActor02`); for that reason a curve channel's `PostEditChange` (which is `AutoSetTangents`, what the
  key editors call) is not called after the write. An author drops `Arrive` / `Leave` from such a key to let Sequencer compute them.
  (A channel written in several `channel.append` chunks keeps its stated tangents across chunks, §8.3.)
- **Negative zero.** The read writes `-0.0` as `0` (a document number `-0` parses back as the integer 0).
- **Property tracks without a property.** `Fade` is a float property track the engine binds itself; a property track
  class whose path is empty stays empty instead of being refused.
- **Validation outcome.** The status before and after the edits decides: errors the sequence did not have fail the
  apply (`Sequencer.ValidationFailed`, Core rolls back); earlier errors (`Sequencer.ValidationError`) and warnings
  (`Sequencer.ValidationWarning`) come back as warnings.
- **Rollback.** Core's package snapshot plus the rebuilder `AssetDocumentSequencer.Sequence` (`MarkAsChanged`,
  compiled data reset). Behaviour test `Apply.Rollback` adds a binding and changes a marker before each failure.
- **Read-only sequences.** A sequence marked `ReadOnly` is unlocked for the edit and locked again (the document's
  `Settings.ReadOnly` decides the final state); locked sections are unlocked for their write and locked again.
- **Open editor.** When the sequence, or a sequence that reaches it through sub / shot sections, is an edited asset of
  `UAssetEditorSubsystem`, the apply lands in place and the open Sequencers refresh (§5.4).
- **Metadata author.** A new sequence gets the creating user as `MetaData.Author` from the factory, as in the editor;
  the round-trip comparison leaves `Author` out (§6.2 (4)).

### 8.3 Remaining tracks: implementation notes

- **Parameter sections by reflection (§3.5.3).** Every stored `TArray` of a struct holding channels is a parameter
  set (Control Rig sections excepted: Control Rig design), so material, MPC, component / primitive material, custom primitive data,
  particle / Niagara parameters and plugin sections such as DMX (fixture patch → function channels, nested
  `Parameters`) share one reader / writer. The write rebuilds the arrays from the list and then rebuilds the channel
  proxy with `PostEditImport` (what a paste of the section runs; the proxy points into the arrays). Custom primitive
  data names are first float indices checked against the proxy's rule (it asserts otherwise); a refused write empties
  the sets before the proxy rebuild so no half-written element reaches it (Core rolls the package back).
- **`parameters.append`.** A `Parameters` list larger than one op (DMX `LS_FixtureDemo`: 34 patches, ~11k nodes)
  continues with `parameters.append {track, section, entries, parameter?}` ops that add entries at the end of their
  sets (the owner at the `parameter` index path; nested lists continue after the op adding their owner).
  `channel.append` takes the same index path for a parameter's channel. Key chunks are sized by their written form
  (key objects or columns), so every op stays under the per-call node limit.
- **Stated tangents across chunks.** Dense channels are written in time order; each `channel.append` carries `stated`
  (`[start, count]` runs of the keys already written whose stated tangents must survive the append's
  `AutoSetTangents`). The write snapshots and restores those keys. Test: 1000-key Slomo and 400-key MPC channels with
  stated and `User` tangents (`Apply.TrackKinds`).
- **Instanced objects.** Instanced, persistent subobject properties (conditions, custom time-warp getters,
  plugin objects) read as `{Class, Properties}` (sparse against the class defaults; channels and
  `UMovieSceneSignedObject::Signature`, a change stamp, left out) or `null`; the same class is edited in place, another
  class is a new object owned by the section / track (`RF_Transactional`), as the Details class picker does.
- **Time warp.** `FMovieSceneTimeWarpVariant` reads as a number (play rate) or one of `{FixedFrame}`, `{FrameRate}`,
  `{Loop}`, `{Clamp}`, `{LoopFloat}`, `{ClampFloat}`, `{Custom: {Class, Properties}}`; a custom getter's curve
  channels are section channels. A changed variant rebuilds the channel proxy.
- **Conditions.** Track and section `ConditionContainer` are instanced objects (above); per-row conditions are the
  track's `RowConditions` `{"<row>": condition}` (row metadata).
- **WholeSectionRanges on the document's bounds.** The validator checks `Start` (inclusive) and `End` (exclusive) as the
  document lists them; a section ending inclusively on a whole frame (DMX sections) ends one tick after it, and the
  warning names it on the source and on a copy alike.
- **Preserved content.** Remote binding references no sub / shot section reaches and channel override registries stay
  untouched by an update that rewrites their sections; section and node groups listed as extracted are kept as well
  (`Apply.Preserved`).
- **Dependents.** Blueprint "Get Sequence Binding" nodes are found as package referencers like parent sequences
  (`Apply.DependentsBlueprint`).

### 8.4 Template and Camera Animation Sequences: implementation notes

- **Two registrations, one route.** `profile.template_sequence.v1` and `profile.camera_animation_sequence.v1` share
  route `seq_profile` and provider `seq` (profile ids and class paths are unique per registration). Create uses the
  generic factory lifecycle: the Template Sequence factory gets `BoundActorClass` from a `factory_context` handle
  (`Root.Class`, resolved with base `Actor`), as the New Template Sequence dialog sets it; the Camera Animation
  Sequence factory takes no context (it always makes a Cine Camera Actor root).
- **`Root` instead of `Bindings`.** The root is legacy spawnable 0 (`GetRootObjectBindingID`); component bindings are
  possessables parented to it and listed as its child possessables. The Python profile converts `Root` to a one-entry
  binding tree (kinds `Root` / `Component`) and reuses the Level Sequence plan over `/Body/Root`, so tracks, sections,
  channels, folders and markers have exactly the Level Sequence semantics. The root's GUID is not authored (the editor
  replaces it on a class change); it is matched by `Name` / `RenamedFrom`, and `Name` omitted keeps it.
- **Change bound class (`template.root`).** A raw editor-twin op, run alone before the plan when `Root.Class` differs:
  spawner `CreateNewSpawnableType` → `MakeUniqueSpawnableName` → `AddSpawnable` → `PreEditChange(BoundActorClass)` →
  `MoveBindingContents` → `RemoveSpawnable` → `PostEditChangeProperty`, as `FTemplateSequenceEditorUtil::
  ChangeActorBinding`. Deviations: no preview actor, viewport placement (`SetupDefaultsForSpawnable`) or camera lock
  (`NewCameraAdded`), which are viewport conveniences; the default Spawn track's object id and the root's folder entry
  follow the new GUID (the editor leaves them on the removed GUID); the old root's display-name override is cleared. A
  sequence with more than one spawnable is refused (`Sequencer.UnsupportedBindingShape`). Test: `Template.ChangeClass`.
- **Component names.** `BoundActorComponents` is written by `binding.set` (`Component`; omitted = the binding name, as
  the editor names the possessable after the component; `""` = none) and checked by status against the root class's
  native components and Blueprint construction-script nodes.
- **Camera Animation Sequences.** Root classes must derive from `ACameraActor` (`Sequencer.CameraClassRequired`, also in
  status). Behaviour: `Template.CameraShake` plays a document-created sequence through `USequenceCameraShakePattern`.
- **Template baseline.** The class default object and the component archetypes do not hold the derived fields of an
  instance: a Cine Camera's `FieldOfView` / `AspectRatio` are recomputed by PostLoad and by edit-change notifications.
  A `Template` is therefore sparse against a fresh transient actor of the class with its components' edit-change
  notifications sent (`FTemplateBaseline`), which is what both a written and a reloaded template hold. Test: `Apply.CineCameraSpawnable`. A listed value equal to that baseline is not stored and
  reads back omitted (the section says to list only differences).

### 8.5 Director, events, groups: implementation notes

- **Hosted Blueprint.** The graph module keeps a registry of hosted Blueprints (`RegisterAssetDocumentHostedBlueprint
  (ULevelSequence::StaticClass(), resolver)`); the `blueprint` and `graph` providers resolve a Level Sequence handle to
  its Director Blueprint (`Graph.NoHostedBlueprint` when it has none), so `blueprint.member.*` and `graph.*` run on the
  Director inside the sequence's package snapshot, transaction and Undo, and the Blueprint rebuilder restores it on
  rollback (`FHostedBlueprintRebuilder`). Python reaches it by `context.handoff(source_handle=<sequence handle>,
  target_provider_id="blueprint" | "graph")`. The Blueprint profile's own components do the work
  (`sequencer_director.py`: members, dispatchers, renames, K2 graph executor, class defaults); their `/Body/<key>` paths
  are rebased to `/Body/Director/<key>`.
- **Region shape.** `Director` holds the Blueprint profile's keys (`ImplementedInterfaces`, `Variables`,
  `ClassDefaults`, `EventDispatchers`, `UbergraphPages`, `FunctionGraphs`, `MacroGraphs`), each optional (omitted =
  kept), ; `ParentClass`, `Components`, `Timelines`, `BlueprintType` are
  refused (`Sequencer.InvalidBody`). `{}` creates the Director (`director.create`:
  `FMovieSceneSequenceEditor::GetOrCreateDirectorBlueprint`), `null` removes it last (`director.remove`, refused with
  `Sequencer.DirectorInUse` while keys or director bindings call it). The facts header (`Blueprint`, `Class`) is a read
  fact for the profile (self class of the K2 adapter).
- **Order.** The Director is written first (members, graphs, class defaults), the live sequence is read again, then the
  sequence plan runs, so event keys and director bindings can name endpoints the same document adds; a key whose
  endpoint the document deletes is rewritten and fails with `Sequencer.UnknownEventEndpoint` (rollback). Graph renames
  keep the endpoint node (graphs match by `GraphGuid`, then name), so a key that follows the renamed endpoint keeps the
  same node. The compile runs in the profile's repair phase (`compile_if_needed`): compiler errors fail the apply
  (`Graph.FinalizeFailed` under `/Body/Director`) when the document lists `Director`; otherwise they are warnings
  (`Sequencer.DirectorCompileFailed`).
- **Event keys.** `{Frame, Event: {Endpoint?, BoundObjectPin?, Payload?}}` (`Endpoint` names a function graph or a
  custom event). Written with `FMovieSceneEventUtils::SetEndpoint` and the
  payload map; repeater sections' `Event` is the same object. The event track's `SupportsType` names only the legacy
  `UMovieSceneEventSection`; the profile also takes `Trigger` / `Repeater` (the track editor's menus), so event
  sections always carry `Class`.
- **Director bindings.** `FMovieSceneDynamicBinding` values (the "Replaceable from Director Blueprint" binding's
  `DynamicBinding`) are `{Endpoint, ResolveParamsPin?, Payload?}` inside the custom binding's `Properties`, written with
  `FMovieSceneDynamicBindingUtils::SetEndpoint`.
- **Section groups.** The header field `Group` (numbers from 1; names only, one namespace over `Bindings` and `Tracks`,
  `Root` and `Tracks` for Template Sequences). Diff compares the partition (which sections share a group), not the
  numbers; a group needs two sections (`Sequencer.InvalidBody`). Written last by `section_groups.set` (ungroup all,
  then `UMovieScene::GroupSections` per group); regions the document omits keep their live sections in the partition.
  `Sequencer.UnsupportedRemoteBinding` applies only to IDs no sub / shot section reaches.
- **Node groups.** `Groups: [{Name, Members}]` with members `{Folder}`, `{Binding}`, `{Track, Node?}` or `{Raw}`,
  composed into the Sequencer's outliner paths (folder names, binding GUIDs, track object names). Members compare as a
  set. A `Raw` path names nothing the document has; a track whose object name a Raw path would now name (a created
  track gets the next free `<Class>_<n>` name) is renamed so the path stays unresolved, as it was in the source
  (`LS_FixtureDemo`).
- **Remote binding references.** `{"Binding": <path inside the sub sequence>, "Sequence": <section path> | [outer, ...,
  inner]}`, the sequence ID the binding picker composes (`FSequenceBindingTree`), depth ≤ 4. Tracks whose values hold
  such references are written after the others, and `track.order` runs before section content, so a reference may
  name a shot section created by the same document.
- **Not authored.** Level Instance contents (out of scope); the Director's `Timelines` / components (not offered by the
  Blueprint editor for a non-actor Blueprint).
