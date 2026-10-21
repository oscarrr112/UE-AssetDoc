# Sequencer Profile (Level and Template Sequences)

Classes: /Script/LevelSequence.LevelSequence, /Script/TemplateSequence.TemplateSequence, /Script/TemplateSequence.CameraAnimationSequence

Exact profile for Level Sequences (Python runtime, `seq_profile`). `extract_asset_document`, `inspect_asset_document_target`, `diff_asset_document` and `apply_asset_document_file` (Create and Update) work: `Settings`, `Bindings`, `Tracks`, `Folders`, `Markers`, `Groups`, `Director` and `MetaData` are written as Sequencer's own edits (tree rename / delete / reorder, "+ Track", section and key edits, "Convert to Possessable / Spawnable", folder and marker edits, section "Group", the Group Manager, Advanced > Tick Resolution, "Open Director Blueprint" and the Blueprint editor).

```jsonc
"Body": {
  "Settings": {"DisplayRate": {"Numerator": 30, "Denominator": 1}, "TickResolution": {"Numerator": 24000, "Denominator": 1},
               "Evaluation": "WithSubFrames", "PlaybackRange": {"Start": 0, "End": 150}},
  "Bindings": [
    {"Name": "ShotCam", "Folder": "Cameras",
     "Spawnable": {"Class": "/Script/CinematicCamera.CineCameraActor", "Template": {"Components": {"CameraComponent": {"CurrentAperture": 4}}}},
     "Tracks": [{"Type": "Spawn", "Sections": [{"Channels": {"Value": {"Default": true}}}]},
                {"Type": "Transform", "Sections": [{"Start": 0, "End": 150, "Group": 1, "Channels": {
                  "Location.X": {"Keys": [{"Frame": 0, "Value": -600}, {"Frame": 150, "Value": -250, "Interp": "Linear"}]},
                  "Location.Z": {"Default": 160}}}]}],
     "Children": [{"Name": "CameraComponent", "Class": "/Script/CinematicCamera.CineCameraComponent",
                   "Bind": ["uobj://subobj?payload0=CameraComponent"],
                   "Tracks": [{"Type": "Float", "Property": "CurrentFocalLength", "Sections": [...]}]}]},
    {"Name": "SM_Door", "Class": "/Script/Engine.StaticMeshActor",
     "Bind": ["uobj://actor?payload0=/Game/Maps/Lobby.Lobby:PersistentLevel.SM_Door_3"],
     "Tracks": [{"Type": "Event", "Sections": [{"Class": "Trigger", "Start": 0, "End": 150, "Channels": {"Value": {"Keys": [
                  {"Frame": 60, "Event": {"Endpoint": "OpenDoor", "BoundObjectPin": "Door", "Payload": {"Speed": "2.5"}}}]}}}]}]}],
  "Tracks": [{"Type": "CameraCut", "Sections": [{"Start": 0, "End": 150, "Properties": {"CameraBindingID": {"Binding": "ShotCam"}}}]},
             {"Type": "Fade", "Sections": [{"Start": 0, "End": 150, "Group": 1, "Channels": {"Value": {"Keys": [{"Frame": 0, "Value": 1, "Interp": "Linear"}, {"Frame": 15, "Value": 0, "Interp": "Linear"}]}}}]}],
  "Folders": [{"Path": "Cameras"}, {"Path": "Set/Props", "Color": {"R": 80, "G": 160, "B": 80, "A": 255}}],
  "Markers": [{"Frame": 60, "Label": "Door opens"}],
  "Groups": [{"Name": "Hero", "Members": [{"Binding": "ShotCam"}, {"Folder": "Cameras"}, {"Track": "/Body/Tracks/1"}]}],
  "Director": {"Variables": [...], "FunctionGraphs": [{"Name": "OpenDoor",
                 "Signature": {"Inputs": [{"Name": "Door", "Type": "object:/Script/Engine.Actor"}, {"Name": "Speed", "Type": "float"}]},
                 "Nodes": [...], "Links": [...]}]},
  "MetaData": {"Author": "...", "Notes": "..."}
}
```

Template Sequences and Camera Animation Sequences use the same document with `Root` in place of `Bindings`: see "Template Sequences".

## Applying

- Omitted region = kept. A present `Bindings`, `Tracks`, `Folders`, `Markers` is complete (unlisted items are deleted). A present `Settings` / `MetaData` is complete except the timing fields: an omitted `Clock`, `CustomClock`, `SelectionRange`, flag, `Author` or `Notes` takes a new sequence's value (deleting it resets it); omitted `DisplayRate`, `TickResolution`, `Evaluation`, `PlaybackRange` are kept (Diff notes this as `Sequencer.OmittedKept`, Info). A track / section / binding `Properties` and a spawnable `Template` list what differs from the defaults: a field not listed is set back to its default. In a listed binding, omitted `Tracks` / `Children` / `Tags` mean none; in a listed section, omitted header fields take the section class defaults and omitted channels are cleared.
- Bindings match by `Id`, else `Name`, else `RenamedFrom` among their siblings. Rename with `"Name": "New", "RenamedFrom": "Old"`: the GUID stays, so camera cuts, Blueprint binding pins and level overrides keep working (update `{"Binding": ...}` paths in the document to the new name). A binding that is not matched is new (new GUID); a deleted binding that other packages reference gives the warning `Sequencer.DependentsAffected`, and an `Id` that cannot be reused gives `Sequencer.BindingRecreated`.
- Changing `Bind` / `Spawnable` / `CustomBinding` converts the binding in place (GUID, tracks and children kept). A spawnable needs `Class`; without a listed `Spawn` track it gets the editor's default (spawned for the whole sequence). `Template` is the actor's state as "Save Default State" leaves it, against a fresh actor of the class. Its fields are the ones the class's Details panel edits: a Cine Camera's `FieldOfView`, `AspectRatio` and `CurrentHorizontalFOV` are not (they follow `CurrentFocalLength` and the filmback). Extract lists only values that differ from that actor; a listed default is not stored (it reads back omitted) and the post-save check and Diff compare it with the default.
- Tracks match by `Id`, else `Type` + `Property` + `Name` in order within their scope; sections by position (extra ones are deleted from the end, new ones appended). List order is the tree order; the camera cut track is always first (Sequencer pins it; listed elsewhere it is moved first and `/Body/Tracks/<n>` references follow). A new section needs `Start` and `End` unless its track allows infinite sections (`Sequencer.SectionRangeRequired`); its `Class` cannot change (`Sequencer.SectionClassChanged`: delete and add).
- Rates: changing `TickResolution` migrates every stored time (frames in the document do not change). Changing `DisplayRate` is applied first and every frame in the document is read in the new rate, as Sequencer shows them.
- A sub / shot section whose `SubSequence` plays this sequence (directly or deeper) is refused (`Sequencer.CircularSubsequence`). Track types the sequence does not support are refused (`Sequencer.TrackNotSupported`).
- The apply fails if the sequence gets errors it did not have (`Sequencer.ValidationFailed`, see Compile); earlier errors and warnings come back as warnings. Any failure rolls the whole apply back.
- Apply works with the sequence open in Sequencer, or played by a sequence open there: Sequencer shows the applied (or rolled-back) sequence. After an apply, Undo history that touches the sequence is cleared.
- Codes at document paths: `Sequencer.UnknownBinding` (a `{"Binding": ...}` path or parent that does not exist), `Sequencer.InvalidLocator`, `Sequencer.InvalidBindingClass`, `Sequencer.InvalidSpawnableClass`, `Sequencer.UnknownComponent` (template component name), `Sequencer.UnknownTrackType`, `Sequencer.UnknownChannel`, `Sequencer.InvalidKey`, `Sequencer.InvalidValue`, `Sequencer.InvalidRange`, `Sequencer.InvalidFolder` (a `Folder` not in `Folders`), `Sequencer.FieldNotEditable`, `Sequencer.InvalidBody`.

Example: rename the camera, add a focus pull on its component and a marker (other regions omitted = kept; `Bindings` and `Markers` are complete):

```jsonc
"Bindings": [
  {"Name": "HeroCam", "RenamedFrom": "ShotCam", "Folder": "Cameras", "Spawnable": {...as extracted...},
   "Tracks": [{"Type": "Spawn", "Sections": [{"Channels": {"Value": {"Default": true}}}]}, {"Type": "Transform", "Sections": [...]}],
   "Children": [{"Name": "CameraComponent", "Class": "/Script/CinematicCamera.CineCameraComponent", "Bind": ["uobj://subobj?payload0=CameraComponent"],
                 "Tracks": [{"Type": "Float", "Property": "FocusSettings.ManualFocusDistance",
                             "Sections": [{"Start": 0, "End": 150, "Channels": {"Value": {"Keys": [{"Frame": 0, "Value": 400}, {"Frame": 90, "Value": 150, "Interp": "Auto"}]}}}]}]}]},
  {"Name": "SM_Door", ...as extracted...}],
"Markers": [{"Frame": 60, "Label": "Door opens"}, {"Frame": 90, "Label": "Focus on door"}]
```

## Time

- Every time (key `Frame`, section `Start` / `End`, ranges, markers, pre/post roll, easing) is in **display frames** of `Settings.DisplayRate`, the numbers Sequencer shows. Sub-frame keys are fractions (`100.5`); Extract writes the shortest decimal that converts back to the same tick of `TickResolution`.
- Exception: a sub / shot section's `Properties.Parameters` offsets (`StartFrameOffset`, `EndFrameOffset`, ...) are in the **inner** sequence's frames.
- `Start` is inclusive, `End` exclusive; an omitted bound is open (infinite).

## Bindings

- `Bindings` is the Sequencer tree: root bindings in tree order, component / sub-object bindings as `Children`. A binding is identified by `Name` among its siblings; Extract adds `Id` (the binding GUID) where siblings share a name.
- Exactly one of: `Bind` (possessable: one locator string per binding reference, `[]` = unbound), `Spawnable` (`Class`, `Template` with the actor's saved default state as `Properties` and per-component `Components`, sparse against a fresh actor of the class, and non-default `Ownership`, `ContinuouslyRespawn`, ...), `CustomBinding` (`{Class, Properties}`: replaceable and plugin binding types). `Class` is the possessed class.
- A reference to a binding inside values (camera cut, attach, constraint, actor-reference keys) is `{"Binding": "<path>"}`: names from the root joined by `/` (`ShotCam/CameraComponent`), `#<Id>` for a name that needs its Id; `"Fixed": true` when stored as a fixed (not sequence-relative) ID. A binding inside a sub / shot sequence (the camera cut to a shot's camera) adds `"Sequence"`: the document path of that sub / shot section (`/Body/Tracks/1/Sections/0`), or a list of section paths from the outer to the inner sequence; the binding path is inside that sequence. It may name a section the same document creates.
- "Replaceable from Director Blueprint" (and other dynamic bindings): `"CustomBinding": {"Class": "/Script/MovieSceneTracks.MovieSceneReplaceableDirectorBlueprintBinding", "Properties": {"DynamicBinding": {"Endpoint": "Door_DynamicBinding", "ResolveParamsPin"?: ..., "Payload"?: {...}}}}`; the endpoint is a Director function.
- Locators are data: no map needs to be open. Inspect reports whether each resolves in the map that is open in the editor.

## Tracks, Sections, Channels

- Track: `Type` (token, or the track class path for other tracks), `Property` (property tracks), `Name` (named tracks), `Id` (the track's object name, only when type / property / name repeat in one scope), `Folder` (global tracks), `Muted`, `MutedRows`, `RowConditions` (`{"<row index>": condition}`), `Properties` (track Details, sparse), `Sections` (in the track's order).
- Type tokens: `Transform`, `Float`, `Double`, `Bool`, `Byte`, `Enum`, `Integer`, `String`, `Text`, `Vector`, `FloatVector`, `Color`, `Rotator`, `EulerTransform`, `ObjectProperty`, `Visibility`, `Spawn`, `Attach`, `Path`, `ActorReference`, `SkeletalAnimation`, `Material`, `PrimitiveMaterial`, `CustomPrimitiveData`, `CameraShake`, `Event`, `Audio`, `CameraCut`, `Shot`, `Subsequence`, `Fade`, `Slomo`, `LevelVisibility`, `DataLayer`, `CVar`, `MaterialParameterCollection`, `TimeWarp`. `list_types` kind `sequencer_track` lists every track class the sequence supports with its token.
- Section: `Class` (only for tracks with several section classes, e.g. event `Trigger` / `Repeater`), `Start`, `End`, `Row` (omitted 0), `OverlapPriority`, `Active` (omitted true), `Locked`, `Blend`, `Easing` (`In` / `Out` durations, `InFunction` / `OutFunction`), `Group` (section group number: the sections Sequencer's "Group" joined share one; numbers are names only and span `Bindings` and `Tracks`; a group has two or more sections), `PreRoll`, `PostRoll`, `WhenFinished`, `Properties` (the section class's own fields, sparse against its defaults), `Event` (repeater sections), `Channels`, `Parameters` (parameter sections).
- `Channels` are keyed by the channel names Sequencer uses (`Location.X`, `Rotation.Z`, `Scale.Y`, ...); the single unnamed channel of spawn, fade, float, bool, event and similar sections is `Value`. A channel is `{Keys?, Default?, PreInfinity?, PostInfinity?}`; an omitted channel has no keys and no default.
- Curve keys (float / double): `{Frame, Value, Interp?, Arrive?, Leave?, ArriveWeight?, LeaveWeight?, WeightMode?}`. `Interp` is the Sequencer key menu: `SmartAuto` (omitted), `Auto`, `User`, `Break`, `Linear`, `Constant`. Tangents (value change per tick) and weights are listed for `User` / `Break` keys; Sequencer computes the others. Another key lists `Arrive` / `Leave` only when its stored tangents differ from the computed ones (saved by an older Sequencer or left by key-menu edits); they are kept as listed, and dropping them lets Sequencer compute them. `PreInfinity` / `PostInfinity` omitted = `Constant`.
- Discrete keys (bool, integer, enum by name, string, text, object path): `{Frame, Value}`. Event keys (and repeater sections' `Event`): `{Frame, Event: {Endpoint?, BoundObjectPin?, Payload?}}`: `Endpoint` names a Director function or custom event, `BoundObjectPin` its input that receives the bound object, `Payload` the other inputs' values as text (`{"Speed": "2.5"}`). A key naming a missing endpoint fails (`Sequencer.UnknownEventEndpoint`). Write each payload value as the Details panel would (`"2.5"`, `"true"`, an enum entry name, an object path); a value the input's type does not take (`"wide"` for a float) fails validate and apply with `Sequencer.InvalidPayload` naming the input and its type (director binding payloads too).
- A channel with more than 64 keys is written in columns: `{"Frames": [...], "Values": [...], "Interp": "Linear"}` (`Interp` one token when every key has it, a list otherwise, omitted when all are `SmartAuto`; `Arrive` / `Leave` / weight lists hold `null` for keys without listed tangents).

- `Parameters` (material, material parameter collection, custom primitive data, particle parameter and DMX sections: the "+ Parameter" menus): `[{Type?, Name?, Properties?, Channels?, Parameters?}]`, listed type by type in the class's order. `Type` is `Bool`, `Scalar`, `Vector2D`, `Vector`, `Color`, `Transform` (or the parameter array's name, e.g. DMX `FixturePatchChannels`), omitted when the section has one kind; `Name` is the parameter name (custom primitive data: its first float index, `"0"`..`"35"` within range); `Channels` by member name (`ParameterCurve`, `XCurve`, `RedCurve`..`AlphaCurve`, `Translation[0]`, ...); `Properties` the other fields (material layer / asset names, DMX patch); nested `Parameters` (DMX function channels). A listed section's `Parameters` is complete (omitted = none). `list_types` `sequencer_section` shows each class's parameter kinds.
- Instanced objects (conditions: track / section `Properties.ConditionContainer.Condition`, row conditions, group `SubConditions`; custom time warps) are `{"Class": "<class path>", "Properties"?: {...sparse...}}` or `null`; an object of the same class is edited in place. Condition classes: `/Script/MovieSceneTracks.MovieScenePlatformCondition` (`ValidPlatforms`), `...MovieSceneScalabilityCondition` (`Group`, `Operator`, `Level`), `/Script/MovieScene.MovieSceneGroupCondition` (`Operator`, `SubConditions: [{"Condition": ...}]`), Blueprint condition classes; `bInvert` on each.
- Time warps (time-warp section `TimeWarp`, sub-section `Parameters.TimeScale`, animation `Params.PlayRate`): one of `{"PlayRate": x}`, `{"FixedFrame": f}`, `{"FrameRate": {Numerator, Denominator}}`, `{"Loop": f}`, `{"Clamp": f}`, `{"LoopFloat": x}`, `{"ClampFloat": x}`, `{"Custom": {"Class": "/Script/MovieScene.MovieScenePlayRateCurve"}}` (or `MovieSceneTimeWarpCurve`); a custom curve's keys are the section's channels (`PlayRate`).

## Other Regions

- `Settings`: Extract writes `DisplayRate`, `TickResolution`, `Evaluation` (`WithSubFrames` | `FrameLocked`) and `PlaybackRange` always, `Clock`, `CustomClock`, `SelectionRange`, `PlaybackRangeLocked`, `ReadOnly`, `MarkersLocked`, `ShowMarkers` when not default (omitted: `Tick`, none, none, false). With `Clock: "Custom"` an omitted `CustomClock` keeps the clock object.
- `Folders`: every folder as `Path` (`Parent/Child`) in tree order; root bindings and global tracks name theirs with `Folder`.
- `Markers` sorted by frame: `{Frame, Label?, Comment?, Color?, DeterminismFence?, Inclusive?}`.
- `Groups` (the Group Manager's node groups, complete when listed): `[{Name, Members}]`, a member `{Folder}`, `{Binding}`, `{Track: "<document path>", Node?: "<category / channel>"}` or `{Raw: "<outliner path>"}` (a stored path naming nothing the document has; kept as is).
- `Director`: the Director Blueprint, in the Blueprint profile's keys (see Blueprint): `ImplementedInterfaces`, `Variables`, `ClassDefaults`, `EventDispatchers`, `UbergraphPages`, `FunctionGraphs`, `MacroGraphs` (each optional: omitted = kept, listed = complete). `{}` creates it ("Open Director Blueprint"); `null` removes it (refused while event keys or director bindings call it: `Sequencer.DirectorInUse`). No `ParentClass`, `Components` or `Timelines`. It is written before the sequence's own edits (keys can name new endpoints), renames keep the endpoint (graphs match by `GraphGuid`), and it is compiled after the apply: compiler errors fail and roll back the whole apply, diagnostics under `/Body/Director`. A graph's `"Layout": "Auto"` (see Blueprint) is reported by Diff at `/Body/Director/<section>/<n>/Layout`.
- Top-level `Properties`: other sequence Details (generic, sparse).

## Kept, Not Authored

Read warnings (Inspect `ReadWarnings`, each `{Code, Message, Path}`) mark content that is listed as found and kept untouched on update (list it as extracted; it cannot be created or changed yet): `Sequencer.UnsupportedChannelOverride` (procedural channel overrides, `"Unsupported": "ChannelOverrides"` on the section), `Sequencer.UnsupportedRemoteBinding` (a binding inside a subsequence the document cannot name, e.g. a sequence ID no sub section has: raw `{Guid, SequenceID, ParentIndex}`), `Sequencer.UnsupportedBindingShape` (mixed locator + custom binding: raw `References`), `Sequencer.UnsupportedLocator` (a locator whose text does not parse back), `Sequencer.UnsupportedChannel` (`{"Unsupported": "<channel type>"}`), `Sequencer.UnsupportedValue` (`{"Unsupported": "<export text>"}`), `Sequencer.DuplicateChannelName` (the later channel keyed `Name#2`), `Sequencer.ControlRigTrack` (read through the generic channels).

## Template Sequences

Template Sequences (`/Script/TemplateSequence.TemplateSequence`) animate one actor class; Camera Animation Sequences (`/Script/TemplateSequence.CameraAnimationSequence`) animate a camera and play as camera shakes (Sequence Camera Shake pattern) or through the camera animation subsystem. Their `Body` is `Settings`, `Root`, `Tracks`, `Folders`, `Markers`, `Groups` (no `Bindings`, `Director`, `MetaData`; section groups span `Root` and `Tracks`):

```jsonc
"Body": {
  "Settings": {"PlaybackRange": {"Start": 0, "End": 30}},
  "Root": {"Name": "CameraActor", "Class": "/Script/Engine.CameraActor",
           "Tracks": [{"Type": "Transform", "Sections": [{"Channels": {"Location.X": {"Keys": [
                        {"Frame": 0, "Value": 0, "Interp": "Linear"}, {"Frame": 30, "Value": 100, "Interp": "Linear"}]}}}]}],
           "Children": [{"Name": "CameraComponent", "Class": "/Script/Engine.CameraComponent",
                         "Tracks": [{"Type": "Float", "Property": "FieldOfView", "Sections": [...]}]}]}
}
```

- `Root` is the spawned root actor: `Class` (the bound actor class), `Template`, `Ownership`, `ContinuouslyRespawn`, `NetAddressableName`, `LevelName`, `SpawnTransform` as a `Spawnable`, plus `Name`, `Tags`, `Folder`, `Tracks`, `Children`. `Name` omitted keeps the root's name. Without a listed `Spawn` track the root gets the factory's (spawned for the whole sequence).
- Changing `Class` is the Details panel's Bound Actor Class picker: a new root of that class takes the old root's tracks, component bindings and folder; its GUID changes, as in the editor. List `Template` for the new class (the old one's does not apply).
- `Children` are component bindings: `Component` names the component of the root actor (omitted = the child's `Name`, `""` = none), `Class` the bound object class. Status warns when a component is not found on the root class.
- Create: a Template Sequence needs `Root.Class` (`Sequencer.RootClassRequired`). A Camera Animation Sequence starts with a Cine Camera Actor root and takes Camera Actor classes only (`Sequencer.CameraClassRequired`); `Sequencer.RootClassInvalid` for abstract, deprecated or non-actor classes.
- Everything else (time, tracks, sections, channels, folders, markers, matching, rollback) is as for Level Sequences; Inspect `BindingFacts` lists the root (`Kind` `Root`) and component bindings (`Kind` `Component`, resolved in the root's template).

## Inspect and Discovery

`inspect_asset_document_target` on a sequence adds `BindingFacts` (`Path`, `DocumentPath`, `Guid`, `Kind`, `Class`, `Locators[{Locator, Resolved, Object?, Class?, Actor?, SpawnableTemplate?}]` against the open map; a spawnable's component bindings resolve in its saved template, marked `SpawnableTemplate`), `AnimatableProperties` (per bound class, the properties "+ Track" offers with their track `Type`), `TrackClasses` (per track class in use: section classes, channel names and types, writable `Properties`), `DirectorEndpoints`, `References` (assets it depends on, with `Exists`), `ReadWarnings` and `Status`. `list_types` kinds: `sequencer_track` (`schema` = the sequence class), `sequencer_section` (`name` / `path` filter; channels for engine section classes), `sequencer_binding` (custom binding types).

## Compile

`Compile` / Inspect `Status`: `{Status, Messages[{Severity, Rule?, Message, Path?, Source}]}`, the Sequence Validator rules (`UnassignedBindingsAndAssets`, `DuplicateKeys`, `WholeSectionRanges`, `SectionAlignments`) and the profile's checks: errors for a camera cut / constraint naming a binding the sequence lacks, a sub-sequence cycle, a property track whose property does not resolve on the bound class; warnings for unbound possessables and events without an endpoint. `Status` is `UpToDate`, `UpToDateWithWarnings` or `Error`.
