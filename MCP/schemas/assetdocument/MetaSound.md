# MetaSound Profile

Classes: /Script/MetasoundEngine.MetaSoundSource, /Script/MetasoundEngine.MetaSoundPatch

Exact profiles for MetaSound Sources and Patches (Python runtime, `audio_profile`). `extract_asset_document`, `inspect_asset_document_target` and `diff_asset_document` work on every MetaSound. `apply_asset_document_file` creates and edits Sources, Patches and their presets, with their graph pages (see Authoring and Pages).

The Body is the MetaSound as the MetaSound editor shows it after opening the asset (the input nodes the editor adds when it opens a MetaSound are included). Reading never registers the MetaSound, marks it dirty or leaves objects behind.

Body shape of an extract: part `body_shape`. Pages: part `pages`.

- `Interfaces`: the declared interfaces a user can add or remove in the editor (`UE.Source.OneShot`, `UE.Attenuation`, ...). Fixed ones (`UE.Source`, a Source's output format) are not listed; Inspect lists all with versions (`Interfaces[{Name, Version, Modifiable, Registered}]`) and the ones this class may add (`AvailableInterfaces`).
- `Metadata`: `DisplayName`, `Description`, `Author`, `Keywords`, `Categories`, `Referenceable`, `Deprecated` (the Details panel's class metadata).
- `Inputs` / `Outputs`: user members `{"<name>": {Type, Default?, PageDefaults?, Constructor?, DisplayName?, Description?, SortOrder, Advanced?, Widget?}}` (outputs have no defaults). Interface members (`UE.Source.OnPlay`, ...) appear only when a user-editable field (`Default`, `PageDefaults`, `SortOrder`, `Advanced`, `Widget`) differs from the interface. `Type` is the MetaSound data type (`Float`, `Int32`, `Bool`, `Trigger`, `Time`, `Audio`, `String`, `WaveAsset`, `Enum:EStereoPannerLaw`, `Float:Array`, ...). `Default` is the default page's value; `PageDefaults` `{"<Page>": value}` the other pages. `Widget` holds the member's Details settings that differ from their defaults (a Float input's `WidgetType` defaults to the editor preference Default Input Widget Type, `RadialSlider`; on UE 5.7 the MetaSound editor sets it to that again whenever it opens the MetaSound, so another `WidgetType` lasts only until then; UE 5.8 keeps it).
- `Graphs`: one entry per page (`Default` is the default page; other pages are the project's MetaSound page settings). `Nodes` is keyed by a document-local key: the member name for `Input` / `Output` / `Get` / `GetDelayed` / `Set` nodes, otherwise the class name part, with `_2`, `_3`, ... in node-ID order. `Id` is the node's durable identity. `Class` is `Input`, `Output`, `Get`, `GetDelayed`, `Set`, a MetaSound asset path (a Patch used as a node) or the node class name (`UE.Sine.Audio`, `UE.Reroute`, `UE.Audio Analyzer`: the editor-only analyzer the editor offers on an audio output, one input `Value`, removed from the sound that plays); `Version` appears when the major version is not 1. A node without an editor location is `"Hidden": true`.
- Connections are written on the consuming node: `Inputs: {"<input pin>": "<source>"}` with `<key>` (a node with one output) or `<key>.<output pin>`; `Input:<member>` is a member read directly on a page the editor shows without input nodes (read only: Apply needs an `Input` node, `MetaSound.InvalidSource`). `Defaults` lists the input values that differ from the node class default; `Configuration` `{Type, Properties}` the node's configuration (the node Details' General settings, e.g. a Harmonix `HarmonixNodes.TapeSS` node's `NumChannels`) where it differs from the configuration a new node of the class gets; `Comment`, `CommentVisible`, `HideUnconnectedPins` the node's own comment and display settings.
- Values follow the pin's data type: numbers, booleans, strings, object paths, enum entry names, arrays as JSON arrays; `Trigger` and `Audio` have no value.
- `Variables` `{"<name>": {Type, Default?, DisplayName?, Description?}}` and `Comments` (comment boxes) are per page.
- Presets: `Preset: {"Parent": "<MetaSound path>"}` and no `Graphs` / `Interfaces` / `Outputs` (they follow the parent); `Inputs` lists only the overridden inputs, each as `{"Default": value}`.
- `Properties` are the asset's Details settings (`OutputFormat`, `AttenuationSettings`, ...). The document payload (`RootMetasoundDocument`) is the Body; registration, versioning and cache properties, and a Source's `bLooping` / `Duration` (they follow `Interfaces`: without `UE.Source.OneShot` it loops), are not authored (`MetaSound.NotAuthoredProperty`).

Discovery: `list_types` with `kind: "metasound_node"` (node classes with version, category and pins `"Name: Type (constructor) = default"`; `Template: true` rows are placed as `UE.Reroute`, `UE.Audio Analyzer` or `Input` nodes; a family differing only in a data type is one row, `<T>` in its class and pins standing for each of its `Variants`: `Array.Random Get.<T>:Array` with `Float` is `Array.Random Get.Float:Array`), `"metasound_datatype"` (data types: literal kind, array and constructor support, enum entries) and `"metasound_interface"` (interfaces with members and the classes that may use them); `name` filters by name (substring or `*` wildcard), `path` by prefix.

## Authoring (apply)

Apply makes the edits the MetaSound editor makes (document builder, Members panel, Interfaces panel, Details panel), then registers the graph as the editor does after an edit and validates it. The document is the whole graph: nodes, connections, members, variables and comments that are not in it are removed.

- Ids are optional. A new node or comment without `Id` gets one derived from its key, so applying the same document twice changes nothing. An entry matches an existing node by `Id`, else by key (same `Class` and `Member`); `Output` nodes match by `Member`.
- Omitted means keep for: `Position` (a new node without one is placed left of the node it feeds, clear of the others), member `Default` and `SortOrder`, variable `Default`, the fields of interface members, and comment fields (except `ColorBubble`, which defaults to false). Omitted means the default for: user member `DisplayName`, `Description`, `Advanced`, `Constructor` and `Widget`; variable `DisplayName` and `Description`. `Inputs`, `Defaults` and the node's own comment are complete: a pin left out is disconnected, or reset to the class default.
- Rename a member with `RenamedFrom: "<old name>"` on the new key (its nodes follow; update their `Member`). Changing an input's `Type` resets its default and widget, as the editor does; connections from it must match the new type.
- `Variables` with `Get`, `GetDelayed` and `Set` nodes (`Member` is the variable; `Set` takes input `Value`, `Get` / `GetDelayed` connect as `<key>.Value`). A MetaSound asset path as `Class` adds it as a node (it must be `Referenceable`, as the editor's asset drop requires). `UE.Reroute` takes its type from its connections.
- Validation: errors the edit introduces fail the apply with `MetaSound.ValidationFailed` (at the node's document path) and roll it back; errors that were already there come back as `MetaSound.ValidationError` warnings, editor warnings as `MetaSound.ValidationWarning`. Operator build errors the edit introduces fail it the same way with `MetaSound.BuildFailed`; old ones are `MetaSound.BuildError` warnings.

```jsonc
// A new one-shot source: no ids; positions optional.
{"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundSource", "Target": "/Game/Audio/MSS_Blip", "Action": "Create",
 "Properties": {"OutputFormat": "Mono"},
 "Body": {"Interfaces": ["UE.Source.OneShot"],
  "Inputs": {"Pitch": {"Type": "Float", "Default": 440.0, "Widget": {"Range": {"X": 100, "Y": 2000}}}},
  "Graphs": {"Default": {"Nodes": {
    "Play":  {"Class": "Input", "Member": "UE.Source.OnPlay"},
    "Pitch": {"Class": "Input", "Member": "Pitch"},
    "Osc":   {"Class": "UE.Sine.Audio", "Inputs": {"Frequency": "Pitch"}},
    "Env":   {"Class": "AD Envelope.AD Envelope.Audio", "Inputs": {"Trigger": "Play"}, "Defaults": {"Decay Time": 0.3}},
    "Amp":   {"Class": "UE.Multiply.Audio", "Inputs": {"PrimaryOperand": "Osc", "AdditionalOperands": "Env.Out Envelope"}},
    "Out":   {"Class": "Output", "Member": "UE.OutputFormat.Mono.Audio:0", "Inputs": {"UE.OutputFormat.Mono.Audio:0": "Amp"}},
    "Done":  {"Class": "Output", "Member": "UE.Source.OneShot.OnFinished", "Inputs": {"UE.Source.OneShot.OnFinished": "Env.On Done"}}}}}}}
```
