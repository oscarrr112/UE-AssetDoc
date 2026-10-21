# MetaSound profile design

Status: M0–M3 implemented (M0 `bab75ed7` / `f37bc9ca`, M1 `303163ed` / `d8593988`, M2 `0e035c53`, M3 `eea54864`);
M4: guide, Audio Analyzer nodes, format fixtures and the support matrix
[asset-document-metasound-matrix.md](asset-document-metasound-matrix.md) awaiting approval; the black-box MCP trial
(§6.4) is still open. Design written 2026-10-06.
Governing rules: [design rules](asset-document-design-rules.md) §3 (rollback = functional
equivalence), §3.1 (Undo history), §4 (editor-equivalent code, raw paths allowed when the editor does it that way)
and §5 (support matrix). The reference is the **MetaSound editor** (UE 5.7, `Engine/Plugins/Runtime/Metasound`),
the MetaSound document builder and the Content Browser. Legacy generators are not a reference. Acceptance is
editor fidelity on real engine/plugin content, free edits, and an AI writing MetaSounds from scratch.

Source paths below are relative to `Engine/Plugins/Runtime/Metasound/Source/` unless they start with `Engine/`.

## 1. Classes in scope

| Class | Scope | Reason |
|---|---|---|
| `/Script/MetasoundEngine.MetaSoundSource` | **Exact profile** `profile.metasound_source.v1` | The playable MetaSound (`USoundWaveProcedural` + `FMetasoundAssetBase` + `IMetaSoundDocumentInterface`). The graph lives in `RootMetasoundDocument`; the sound settings are ordinary Details properties. |
| `/Script/MetasoundEngine.MetaSoundPatch` | **Exact profile** `profile.metasound_patch.v1` | Same document model, no sound settings, used as a node in other MetaSounds. |
| Presets (Source or Patch whose `RootGraph.PresetOptions.bIsPreset` is true) | **Same two profiles**, `Body.Preset` | A preset is not a class. It is a Source/Patch whose graph the frontend regenerates from the referenced MetaSound on every registration (`FAutoUpdateRootGraph::Transform` → `FRebuildPresetRootGraph`). Only its input default overrides and the reference are authored. |
| Graph pages (`FMetasoundFrontendGraphClass::PagedGraphs`, per-page input defaults) | **Same profiles**, `Body.Graphs.<Page>`, `Inputs.<n>.PageDefaults` | Pages are still "Pages (Experimental)" in `UMetaSoundSettings`, but the editor edits them (`FMetasoundPagesDetailCustomization`) and the data is in every asset (the default page). The one-page case must be first-class; extra pages come in a later phase (§7). |
| `UMetaSoundBuilderDocument` | Out | Transient object of the builder API; never an asset. |
| `UMetaSoundSettings`, `UMetasoundEditorSettings` (pages, quality, audition page) | Out | Project and editor settings (config), not assets. `Inspect` reports the page names and quality names a document may use. |
| `UMetasoundEditorGraph` and its nodes, `UMetasoundEditorGraphMemberDefault*` instances held by the editor graph | Out (derived) | The editor graph is `UPROPERTY(Transient)` (`UMetaSoundSource::EditorGraph`) and is rebuilt from the frontend document (`FGraphBuilder::SynchronizeGraph`). The member-default objects that persist are in the document (§2). |
| Other audio assets (SoundWave, SoundCue, SoundClass, Submix, Attenuation, WaveTableBank, AudioBus, Quartz, Harmonix MIDI assets, ...) | Out (generic profile) | Not MetaSound documents. SoundCue is another graph asset and would need its own design. |

The shipped engine content has **no** presets, no extra pages, no Quad/5.1/7.1 output and no
`UE.Attenuation` / `UE.Spatialization` interface. Fidelity for those comes from content that the tests build
with the editor's own builder API first (§6).

## 2. Editor data model: authored vs derived

The editor writes everything through `FMetaSoundFrontendDocumentBuilder` (MetasoundFrontend, public, `METASOUNDFRONTEND_API`), obtained with
`FDocumentBuilderRegistry::FindOrBeginBuilding(asset)` / `UMetaSoundEditorSubsystem::FindOrBeginBuilding`. The EdGraph
is a **pull-based view**: `FEditor::Tick` → `FGraphBuilder::SynchronizeGraph` rebuilds comments, members, output
nodes, nodes and connections from the document when the document's `ModifyContext` says it changed. So the
frontend document is the single source of truth, and the profile edits it, not the EdGraph.

### Authored (Extract must emit, Apply must write)

`UMetaSoundSource/Patch::RootMetasoundDocument` (`FMetasoundFrontendDocument`, `MetasoundFrontendDocument.h`):

- `Interfaces` (`TSet<FMetasoundFrontendVersion>`): declared interfaces. Which ones a user may add or remove is per
  class (`FMetasoundFrontendInterfaceUClassOptions::bIsModifiable`). On a Source, `UE.Source` and the
  `UE.OutputFormat.*` interfaces are not modifiable. The output format follows the `OutputFormat` Details property.
- `RootGraph.Metadata`: `DisplayName`, `Description`, `Author`, `Keywords`, `CategoryHierarchy`, `AccessFlags`
  (`Referenceable`, `Deprecated`). The `MetaSound` Details category edits these through property handles
  (`MetasoundDetailCustomization.cpp`).
- `RootGraph.Interface.Inputs[]` (`FMetasoundFrontendClassInput`): `Name`, `TypeName`, `AccessType`
  (Reference, or Value for a "Constructor" input), `Metadata.DisplayName/Description/SortOrderIndex/bIsAdvancedDisplay`,
  `Defaults[]` (one literal per page ID). `Outputs[]` hold the same fields without defaults.
- `RootGraph.PagedGraphs[]` (`FMetasoundFrontendGraph`, one per page). Each holds:
  - `Nodes[]` (`ID`, `ClassID` → `Dependencies[]` entry, `Name`, node `Interface`, `InputLiterals` (pin defaults),
    `Configuration` (`TInstancedStruct<FMetaSoundFrontendNodeConfiguration>`), `ClassInterfaceOverride`,
    `Style.Display.Locations` (`TMap<FGuid EdGraphNodeGuid, FVector2D>`, one entry since document version 1.12),
    `Style.Display.Comment/bCommentVisible`, `Style.bUnconnectedPinsHidden`);
  - `Edges[]` (`FromNodeID/FromVertexID/ToNodeID/ToVertexID`);
  - `Variables[]` (`Name`, `TypeName`, `Literal`, `DisplayName`, `Description`, plus the IDs of the variable node,
    mutator, accessors and deferred accessors);
  - `Style.Comments` (`TMap<FGuid, FMetaSoundFrontendGraphComment>`: comment, colour, position, size, font size,
    depth, colour bubble, move mode).
- `RootGraph.PresetOptions` (`bIsPreset`, `InputsInheritingDefault`).
- `Metadata.MemberMetadata` (`TMap<FGuid MemberID, UMetaSoundFrontendMemberMetadata*>`): the editor's
  `UMetasoundEditorGraphMemberDefault<Type>` subobjects. Their **widget and range settings are authored**
  (`ClampDefault`, `Range`, `WidgetType`, `WidgetOrientation`, `WidgetUnitValueType`, volume settings). Their
  per-page `Defaults` arrays are `Transient` mirrors of the class input defaults.

Details properties of the asset (generic property admission): on `UMetaSoundSource` everything the Details panel
shows from `USoundBase` / `USoundWave` / `USoundWaveProcedural` (attenuation, concurrency, sound class, submix sends,
virtualization, looping, ...) plus `OutputFormat`, `QualitySetting`, `BlockRateOverride` and `SampleRateOverride`.
A Patch has no Details content beyond the document.

### Derived (never emitted, recomputed by the editor's own paths)

| State | Recomputed by |
|---|---|
| `EditorGraph` (transient) and all EdGraph nodes/pins, node error messages | `FGraphBuilder::SynchronizeGraph` / `ValidateGraph` when an editor opens or ticks |
| Hidden graph-input nodes (class type `Input`, `ID == ClassInput.NodeID`) and their edges to input template nodes | the member itself; `FInputNodeTemplate::CreateNode` / `Inject` |
| The output vertex node of every output on every page (it always exists; it is *visible* only with a location) | `AddGraphOutput`, page creation |
| The variable's own node (class type `Variable`) and the edges of the variable stack (variable → mutator → accessors) | `AddGraphVariable*Node`, `SpliceVariableNodeFromStack` |
| `Dependencies[]` (class snapshots of the classes used by nodes), the node's own `Interface` copy | `AddNodeByClassName` / `AddNode`, `RemoveUnusedDependencies`, `SynchronizeDependencyMetadata`, auto-update |
| Node **minor** versions | Auto-update during every editor registration (`FMetasoundAssetBase::UpdateAndRegisterForExecution` → `VersionDependencies` → `FAutoUpdateRootGraph`) |
| A preset's whole root graph, interfaces and inherited defaults | `FRebuildPresetRootGraph` on registration |
| `RootGraph.Metadata.ClassName` (GUID name, the registry key other MetaSounds reference), `Version`, `Type` | `InitDocument` on create, `GenerateNewClassName` on duplicate (`FAssetHelper::PostDuplicate`) |
| Document `Metadata.Version`, `IdCounter`, `ChangeID`s, `ModifyContext`, `Style.bIsGraphEditable`, `bMessageNodeUpdated`, `EdgeStyles` | versioning on load (`VersionAsset` in `Serialize`), the builder |
| `ReferencedAssetClassKeys`, `ReferencedAssetClassObjects`, `ReferenceAssetClassCache` | `RebuildReferencedAssetClasses` during registration |
| `OutputFormat`/`NumChannels` agreement with the output-format interface, `QualitySettingGuid`, `SampleRate` | `ConformObjectToDocument`, `PostEditChangeQualitySettings` |
| `AssetClassID`, `RegistryInputTypes`, `RegistryOutputTypes`, `bIsPreset` UPROPERTYs (deprecated) and the deprecated `Graph` | asset registry tags (`FMetaSoundAssetClassInfo::ExportToContext`); migration only |
| Frontend registry entry, runtime `FGraph`, operator caches | `RegisterGraphWithFrontend` |

Two editor behaviours shape the read path:

1. **Versioning on load.** Every editor load runs `VersionAsset` in `Serialize` without dirtying the package. The
   in-memory document is therefore already at the current document version when the profile reads it.
2. **Editor-open normalisation.** Opening the MetaSound editor runs `VersionAsset` and
   `FInputNodeTemplate::Inject(Builder, bForceNodeCreation=false)` (`MetasoundEditor.cpp:1168`). On older assets
   this adds input template nodes for connected inputs, so the graph shows "Get <Input>" nodes. The profile's read
   presents the asset as the editor shows it after opening (§3.3).

## 3. Document shape

### 3.1 Example (a small, complete one-shot stereo Source)

```jsonc
{
  "SchemaVersion": 1, "Target": "/Game/Audio/MSS_Blip", "Class": "/Script/MetasoundEngine.MetaSoundSource",
  "Action": "CreateOrUpdate",
  "Properties": {"OutputFormat": "Stereo", "AttenuationSettings": "/Game/Audio/ATT_Small.ATT_Small"},
  "Body": {
    "Interfaces": ["UE.Source.OneShot"],
    "Metadata": {"Description": "Short pitched blip", "Keywords": ["ui"], "Referenceable": true},
    "Inputs": {
      "Pitch": {"Type": "Float", "Default": 0.0, "SortOrder": 0,
                "Widget": {"Range": {"X": -12, "Y": 12}, "ClampDefault": true, "WidgetType": "Slider"}},
      "Wave":  {"Type": "WaveAsset", "Default": "/Game/Audio/SW_Blip.SW_Blip", "Constructor": true}
    },
    "Outputs": {},
    "Graphs": {
      "Default": {
        "Nodes": {
          "OnPlay": {"Class": "Input", "Member": "UE.Source.OnPlay", "Position": [-500, 0]},
          "Wave":   {"Class": "Input", "Member": "Wave",  "Position": [-500, 120]},
          "Pitch":  {"Class": "Input", "Member": "Pitch", "Position": [-500, 200]},
          "Player": {"Class": "UE.Wave Player.Stereo", "Position": [-200, 0],
                     "Inputs": {"Play": "OnPlay", "Wave Asset": "Wave", "Pitch Shift": "Pitch"},
                     "Defaults": {"Loop": false}},
          "Env":    {"Class": "AD Envelope.AD Envelope.Audio", "Position": [-200, 300],
                     "Inputs": {"Trigger": "OnPlay"}, "Defaults": {"Decay Time": 0.4}},
          "MulL":   {"Class": "UE.Multiply.Audio", "Position": [100, 0],
                     "Inputs": {"PrimaryOperand": "Player.Out Left", "AdditionalOperands": "Env.Out Envelope"}},
          "MulR":   {"Class": "UE.Multiply.Audio", "Position": [100, 150],
                     "Inputs": {"PrimaryOperand": "Player.Out Right", "AdditionalOperands": "Env.Out Envelope"}},
          "OutL":   {"Class": "Output", "Member": "UE.OutputFormat.Stereo.Audio:0", "Position": [400, 0],
                     "Inputs": {"UE.OutputFormat.Stereo.Audio:0": "MulL"}},
          "OutR":   {"Class": "Output", "Member": "UE.OutputFormat.Stereo.Audio:1", "Position": [400, 150],
                     "Inputs": {"UE.OutputFormat.Stereo.Audio:1": "MulR"}},
          "Done":   {"Class": "Output", "Member": "UE.Source.OneShot.OnFinished", "Position": [400, 300],
                     "Inputs": {"UE.Source.OneShot.OnFinished": "Player.On Finished"}}
        },
        "Variables": {},
        "Comments": {
          "Note": {"Text": "Envelope shapes both channels", "Position": [-250, 260], "Size": [700, 260]}
        }
      }
    }
  }
}
```

Class and pin names are the 5.7 registry's (`MetasoundWavePlayerNode.cpp`, `MetasoundADEnvelopeNode.cpp`,
`MetasoundMathNodes.cpp`, the output-format and source interfaces). The AI reads them from `list_types` /
`inspect` (§3.6), never from memory.

### 3.2 Body regions

All regions follow the core rule: **omitted = keep, present = complete state**. Deviations are listed.

| Region | Shape | Notes |
|---|---|---|
| `Interfaces` | array of interface names | The **user-modifiable** declared interfaces of this class (Source: `UE.Source.OneShot`, `UE.Source.StartTime`, `UE.Attenuation`, `UE.Spatialization`, `UE.Source.Orientation`, `UE.Listener.Orientation`, plugin interfaces; Patch: also `UE.OutputFormat.*`, `UE.InputFormat.*`). Non-modifiable ones (`UE.Source`, a Source's output format) are derived and listed by Inspect. The version is derived: adding a name declares its highest registered version, an existing name keeps its declared version. |
| `Metadata` | `{DisplayName?, Description?, Author?, Keywords?, Categories?, Referenceable?, Deprecated?}` | The class metadata the Details panel edits. Values are complete (omitted field = new-asset value, with `Author` = the editor's default author on create). |
| `Inputs` | `{"<name>": {Type, Default?, PageDefaults?, Constructor?, DisplayName?, Description?, SortOrder?, Advanced?, Widget?, RenamedFrom?}}` | Complete over **user** inputs. Interface members (`UE.Source.OnPlay`, ...) are implied by `Interfaces`; they may be listed to set the fields the editor lets a user change (`Default`, `PageDefaults`, `SortOrder`, `Advanced`, `Widget`), are not removed when omitted, and are extracted only when those fields differ from the interface definition. `Type` is the MetaSound data type name (`Float`, `Int32`, `Bool`, `Trigger`, `Time`, `Audio`, `String`, `WaveAsset`, `Enum:EStereoPannerLaw`, `Float:Array`, ...). `Default` is the default-page literal; `PageDefaults` `{"<PageName>": value}` is complete over the other pages. `Widget` holds the Details values of the member's literal-metadata object (`UMetasoundEditorGraphMemberDefault<Type>`), sparse, through the typed value codec. |
| `Outputs` | `{"<name>": {Type, Constructor?, DisplayName?, Description?, SortOrder?, Advanced?, RenamedFrom?}}` | Same rules as `Inputs`, without defaults. |
| `Graphs` | `{"<PageName>": {Nodes, Variables, Comments}}` | `Default` is the default page (`Metasound::Frontend::DefaultPageName`); other names resolve through `UMetaSoundSettings` page settings (`MetaSound.UnknownPage` lists the configured pages). A present `Graphs` is the complete page set (a page not listed is removed, `RemoveGraphPage`); `Default` cannot be removed. Inside a page `Nodes`, `Variables`, `Comments` follow omitted = keep / present = complete. |
| `Graphs.<P>.Nodes` | `{"<key>": Node}` | See §3.3. |
| `Graphs.<P>.Variables` | `{"<name>": {Type, Default?, DisplayName?, Description?, RenamedFrom?}}` | Variables are per page in the frontend (`FMetasoundFrontendGraph::Variables`). |
| `Graphs.<P>.Comments` | `{"<key>": {Id?, Text, Position, Size, Color?, FontSize?, ColorBubble?, MoveMode?}}` | Comment boxes (`Style.Comments`). |
| `Preset` | `{"Parent": "<MetaSound object path>"}` or absent / `null` | See §3.7. |

`Properties` (top-level, sparse, generic admission) carry the asset's Details values. Body-owned and refused
there: `RootMetasoundDocument`/`RootMetaSoundDocument` (→ Body), `ReferencedAssetClassKeys`,
`ReferencedAssetClassObjects`, `ReferenceAssetClassCache`, `QualitySettingGuid`, `EditorGraph`, `Graph` and the
deprecated tag properties (`MetaSound.NotAuthoredProperty`, with the reason).

### 3.3 Nodes, identity and keys

```jsonc
"<key>": {
  "Class": "UE.Sine.Audio" | "/Game/Audio/MSP_Mix.MSP_Mix" | "Input" | "Output" | "Get" | "GetDelayed" | "Set"
           | "UE.Reroute" | "UE.Audio Analyzer" | ...,
  "Member": "<input / output / variable name>",          // member nodes only
  "Version": 2,                                         // major version, omitted = 1
  "Id": "8C6A0E2F4B1D47E8A1F30B9C5D7E6F10",            // frontend node ID (extract writes it)
  "Position": [x, y],                                   // omitted = keep (new: column layout like Material)
  "Hidden": true,                                       // node without an editor location (extract only emits true)
  "Inputs":   {"<input pin>": "<key>" | "<key>.<output pin>"},   // complete: omitted pin = unconnected
  "Defaults": {"<input pin>": <literal>},              // complete: omitted pin = class default
  "Configuration": {"Type": "/Script/Module.Struct", "Properties": {...}},
  "Comment": "text", "CommentVisible": true, "HideUnconnectedPins": true
}
```

**Class references.** A registered node class is its full class name `Namespace.Name.Variant` (the form
`FMetasoundFrontendClassName::Parse` reads and `GetFullName` writes; an empty variant drops the last part). A
MetaSound asset used as a node (a "subgraph") is its object path; Apply resolves it to the asset's class name and
checks what the editor checks on drop and paste: it is a MetaSound, it has the `Referenceable` access flag, and
adding it does not create a reference cycle (`FMetasoundAssetBase::AddingReferenceCausesLoop` →
`MetaSound.ReferenceCycle`). Extract maps asset classes back to object paths through `IMetaSoundAssetManager`.
`Input`, `Output`, `Get`, `GetDelayed`, `Set` are the member nodes the Members panel places, with `Member` naming
the input, output or variable. `Version` is the **major** version; minor versions are derived (the editor
auto-updates them on registration). A new node gets the highest minor version of that major, as the palette does
(`FindClassWithHighestMinorVersion`). A changed `Version` on an existing key is the editor's "Update Node" for that
node (§4). An unknown class fails with `MetaSound.UnknownNodeClass` and close names; a class without
`Referenceable`, or deprecated for new nodes, fails as the palette filters it.

**Keys and `Id`.** The frontend identifies nodes by GUID (`FMetasoundFrontendNode::ID`). Node IDs survive save,
reload and Content Browser duplication (duplication only renames the root class, `PostDuplicate` →
`GenerateNewClassName`), so the GUID is the durable identity. Keys are only document-local names used by
`Inputs` references. The rules, chosen so that an extract, an AI-written document, and an edited extract all
re-apply as no-ops:

1. **Extract** writes every node's `Id` and gives it a readable key: the member name for member nodes
   (`Pitch`, `Out_UE.OutputFormat.Stereo.Audio_0` sanitised), otherwise the class `Name` part (`Sine`, `Multiply`,
   `Wave_Player`) with `_2`, `_3`, ... in node-ID order. Keys use letters, digits, `_` and `-` (no `.`), so
   `"<key>.<pin>"` splits at the first `.` even though pin names contain dots and spaces.
2. **Apply** resolves an entry to an existing node by `Id` when it is given (it must exist on that page). Without
   an `Id` it uses `FGuid::NewDeterministicGuid("AssetDocument.MetaSound.Node:" + key)`. If no node has that ID, it
   creates one with exactly that ID (`AddNodeByClassName(..., InNodeID)` and its siblings take the ID). An AI
   document without `Id` fields therefore keeps matching the nodes it created. Extract then shows them under
   class-derived keys plus `Id`, and that extract also applies as a no-op. This is the same contract the Blueprint
   profile gives (`Id` local, `NodeGuid` metadata).
3. A different `Class` (or `Member`) on a resolved node is delete + create, as in Material.
4. Member nodes: an `Input` entry is an **input template node** (`FInputNodeTemplate`), never the hidden input
   node. An `Output` entry *is* the output's vertex node: at most one per output per page
   (`MetaSound.DuplicateOutputNode`). Removing it removes its location and edges, as the editor's delete does
   (the vertex node itself is kept because every page must have it). `Get`/`GetDelayed`/`Set` are the variable's
   accessor, deferred accessor and mutator nodes; at most one `Set` per variable (the editor's paste rule).
5. The editor location key (the EdGraph `NodeGuid` stored in `Locations`) is not authored. New locations use
   `FGuid::NewDeterministicGuid(NodeID + ":Location")`, so repeated applies are byte-stable.
6. Comments use the same `Id` / deterministic-ID scheme (`FindOrAddGraphComment(CommentID)`).
7. Members (inputs, outputs, variables) are identified by name. `RenamedFrom` keeps the member, its node IDs,
   edges and literal metadata (`SetGraphInputName` / `SetGraphOutputName` / `SetGraphVariableName`). Any other
   new name is remove + add.

**Normalised read.** Extract and the read at the start of Apply both present the document the way the editor
shows it after opening. They run `FInputNodeTemplate::Inject(bForceNodeCreation=false)` on a
`UMetaSoundBuilderDocument` copy (`UMetaSoundBuilderDocument::Create(const IMetaSoundDocumentInterface&)`) inside
`FDocumentIDGenerator::FScopeDeterminism(true)`. Deterministic IDs come from the document's own `IdCounter`, so
the template nodes Extract shows have the same IDs that Apply's first step creates on the real asset. If that
step changes anything, it is reported as a change of the apply, as when a human opens the asset and saves it. A
node with no location (for example, interface members added by `ModifyInterfaces`, or nodes made with the
builder API) is extracted with `"Hidden": true`. Writing `Hidden: true` on a new node creates it without a
location, and omitting it on an existing hidden node gives it a column position.

### 3.4 Pins, connections and values

- Pin names are the vertex names (`FMetasoundFrontendVertex::Name`) as the registry class declares them (`Frequency`,
  `Out Envelope`, `UE.OutputFormat.Mono.Audio:0`), not the localised display names. `Inputs` is complete: each
  input pin holds at most one edge (as in the frontend). A source is `"<key>"` (the node's only output: member
  nodes, reroutes) or `"<key>.<output pin>"`.
- Connections are checked like the editor's `CanCreateConnection`: data type and access type
  (`FMetaSoundFrontendDocumentBuilder::IsValidEdge` → `EInvalidEdgeReason`: `MismatchedDataType`,
  `MismatchedAccessType` "constructor input pins can only be connected to constructor output pins") and loops
  (the EdGraph DFS `ConnectionCausesLoop`, done here on the frontend edges). Errors: `MetaSound.IncompatibleConnection`
  (lists the converter node classes the registry offers for the two types, `GetPossibleConverterNodes`; the
  editor would insert one automatically, a document names it explicitly), `MetaSound.ConnectionCycle`,
  `MetaSound.PinNotFound` (lists the node's pins).
- `Defaults` are the node's `InputLiterals`. A value equal to the class default is not extracted, and writing it
  removes the literal (`RemoveNodeInputDefault`, the pin's "reset to default").
- **Literal codec** (by the pin's or member's data type, through `IDataTypeRegistry`): `Bool` → JSON bool; `Int32`
  → integer; `Float`, `Time`, ... → number (float32 shortest form); `String` → string; enum data types (`Enum:...`,
  `IEnumDataTypeInterface`) → the entry name (`"Linear"`), an integer is accepted; UObject proxy types
  (`WaveAsset`, `MIDIAsset`, ...) → object path or `null`; `<Type>:Array` → JSON array of the element form;
  `Trigger` and `Audio` take no literal. The literal is stored in the data type's preferred literal type, as
  `FGraphBuilder::GetPinLiteral` does. Comparison is semantic (same `Metasound::FLiteral` for the data type), so
  an old asset that stored an `Integer` literal for a `Float` pin does not diff.

### 3.5 Omitted vs present, per region

| Item | Omitted | Present |
|---|---|---|
| `Interfaces`, `Inputs`, `Outputs`, `Graphs`, page `Nodes`/`Variables`/`Comments` | keep | complete; unlisted user entries removed |
| Interface members inside `Inputs`/`Outputs` | interface definition values | listed fields set |
| Node `Position` | keep (new node: placed in columns) | set |
| Node `Inputs` / `Defaults` | empty (no edges / class defaults) | complete |
| Node `Configuration` | the class's default configuration | set (sparse values over the default struct) |
| Member `Default`, `SortOrder`, `Advanced`, `DisplayName`, `Description`, `Widget` | new-member value (interface value for interface members) | set |
| `Preset` | keep (preset stays preset) | `{Parent}` makes/keeps a preset; `null` converts from preset |

### 3.6 Discovery (read-only additions, allowed by §2 of the direction reset)

- `list_types` gains `kind: "metasound_node"` (registered node classes: `Class`, `Version`, `DisplayName`,
  `Category`, `Deprecated?`, and each pin as `"Name: Type"` / `"Name: Type = default"`), `kind: "metasound_datatype"`
  (name, literal form, enum entries, array/constructor support) and `kind: "metasound_interface"` (name, version,
  members, modifiable for which class). The source is `ISearchEngine::FindAllClasses` / `FindAllInterfaces` and
  `IDataTypeRegistry`. Results are paged and filtered with `name` / `path` as today.
- `inspect_asset_document_target` on a MetaSound returns the extract plus `Pins` per node (inputs and outputs with
  types, access and class defaults), `Interfaces` with versions and modifiability, `Pages` (configured page names),
  `Referencers` (loaded MetaSounds that use this one as a node), and the validation status (§5).

### 3.7 Presets

`Body.Preset = {"Parent": "/Game/Audio/MSS_Base.MSS_Base"}`. For a preset, `Graphs`, `Interfaces` and `Outputs`
are derived from the parent and are refused if authored (`MetaSound.PresetGraphDerived`). In `Inputs` only
`Default`/`PageDefaults` may be given. **A listed input is an override; an unlisted input inherits the parent
default** (`PresetOptions.InputsInheritingDefault` = the unlisted names, `SetGraphInputInheritsDefault`). `Metadata`
and `Properties` stay authorable.

- Create a preset (`Action: Create` with `Preset`): the generic factory lifecycle sets the factory's
  `ReferencedMetaSoundObject` (the Content Browser's restricted-mode "Create Preset"; the generic factory context
  falls back to the factory's single settable `UObject` field). `UMetaSoundEditorSubsystem::InitAsset`
  then runs `ConvertToPreset` and copies `OutputFormat`. The factory context comes from `Body.Preset.Parent`, as the
  StateTree profile does with `Body.Schema.Class`; the parent must be a MetaSound of the same class
  (`MetaSound.InvalidPresetParent`).
- Overrides (M2): a listed input with a `Default` is `SetGraphInputDefault` (which also clears its inheritance), a
  listed input without one stops inheriting (`SetGraphInputInheritsDefault(false)`, the Details override checkbox),
  an unlisted overriding input inherits again (`SetGraphInputInheritsDefault(true)`; registration's preset rebuild
  restores the parent's value).
- `Preset: null` on a preset: `UMetaSoundBuilderBase::ConvertFromPreset` (the editor's command). With `Graphs` /
  `Interfaces` / `Outputs` in the Body the regular phases run on the converted graph; otherwise the overrides are
  applied first. Another parent is refused (`MetaSound.PresetParentChanged`).
- `Preset` added to an existing non-preset: refused (`MetaSound.CannotConvertToPreset`, decision §9.4).

### 3.8 Free edits → operations

| Free edit | Document change | Operation (§4) |
|---|---|---|
| Insert a node into a connection | new key; re-point the downstream `Inputs` pin to it, set its own `Inputs` | create, connect ×2 |
| Delete a node | remove the key (its edges go) | `RemoveNode` + `RemoveUnusedDependencies` |
| Reconnect | change a source string | `RemoveEdgeToNodeInput` + `AddEdge` |
| Disconnect | remove the pin from `Inputs` | `RemoveEdgeToNodeInput` |
| Replace a node with another class | change `Class` on the key (connections re-listed) | delete + create |
| Rename an input / output / variable | new name + `RenamedFrom` | `SetGraphInputName` / ... |
| Change a member type | `Type` | `SetGraphInputDataType` (variables: remove + re-add with nodes, as the editor) |
| Reorder members | `SortOrder` | `SetGraphInputSortOrderIndex` / `SetGraphOutputSortOrderIndex` |
| Add a "Get" of an input in another place | another `Input` node with the same `Member` | `FInputNodeTemplate::CreateNode` |
| Update a node's major version | `Version` | the editor's Update Node (§4) |
| Move / comment / collapse pins | `Position`, `Comment`, `HideUnconnectedPins` | `SetNodeLocation`, `SetNodeComment`, `SetNodeUnconnectedPinsHidden` |
| Change channel layout of a Source | `Properties.OutputFormat` (and the output nodes' `Member`s) | Details edit → `PostEditChangeOutputFormat` |

## 4. Editor-equivalent operations

### 4.1 Mapping

The editor's pattern for every edit is `FScopedTransaction` → `MetaSound.Modify()` (and `Graph->Modify()`) →
builder calls → often `RegisterGraphWithFrontend`. The builder never opens transactions itself. The profile does
the same: one transaction per edit primitive call, `Asset->Modify()` and, for page changes, the builder object's
`Modify()`. Registration happens once at the end, in the compile primitive. Undo history then follows §3.1:
a successful apply clears history that references the asset.

| Editor action (entry point) | Frontend call the profile makes | Notes |
|---|---|---|
| New Source / Patch (`UMetaSoundSourceFactory` / `UMetaSoundFactory::FactoryCreateNew` → `UMetaSoundEditorSubsystem::InitAsset`) | generic factory lifecycle (no context needed), then the graph edits | `InitAsset`: `InitDocument` (default interfaces: Source = `UE.Source`, `UE.Source.OneShot`, `UE.OutputFormat.Mono`), `InitNodeLocations`, `Inject(force)`, `SetAuthor`, register. A `Create` document whose `Interfaces` omits OneShot removes it in the same apply. |
| Create Preset (`ExecuteCreateMetaSoundPreset`, restricted mode) | factory with `ReferencedMetaSoundObject` | §3.7 |
| Convert From Preset (`FEditor::ConvertFromPreset`) | `UMetaSoundBuilderBase::ConvertFromPreset` | |
| Palette: node (`FMetasoundGraphSchemaAction_NewNode::PerformAction` → `FGraphBuilder::AddExternalNode`) | `AddNodeByClassName(ClassName, Major, NodeID, &PageID)` + `SetNodeLocation` | `TryConnectNewNodeToMatchingDataTypePin` (auto-wire to the dragged pin) is a UI convenience; documents list connections explicitly |
| Drop an asset / subgraph node (`DroppedAssetsOnGraph`) | `UMetaSoundBuilderBase::AddNode(asset)` equivalent: `AddNodeByClassName` of the asset class | `Referenceable` + loop + asset reference filter checks |
| Reroute (`FMetasoundGraphSchemaAction_NewReroute`) | `AddNodeByTemplate(FRerouteNodeTemplate, Params)` | |
| Get input (`FMetasoundGraphSchemaAction_NewInput`, member drag) | `FInputNodeTemplate::CreateNode(Builder, InputName, &PageID)` | |
| Place output (`FMetasoundGraphSchemaAction_NewOutput`) | `SetNodeLocation` on the output vertex node | |
| Variable Get / Get Delayed / Set (`FMetasoundGraphSchemaAction_NewVariableNode`) | `AddGraphVariableAccessorNode` / `...DeferredAccessorNode` / `...MutatorNode` | |
| Comment box (`FMetasoundGraphSchemaAction_NewComment`, comment node edits) | `FindOrAddGraphComment` / field writes / `RemoveGraphComment` | |
| Move (`SMetaSoundGraphNode::MoveTo`) | `SetNodeLocation(NodeID, Loc, &LocationGuid, &PageID)` | |
| Delete (`FEditor::DeleteSelectedNodes` → `RemoveFromDocument`) | `RemoveNode` + `RemoveUnusedDependencies`; output: `RemoveEdges` + `RemoveNodeLocation` | `CanUserDeleteNode` rules: required interface outputs and non-editable graphs refuse (`MetaSound.NodeNotDeletable`) |
| Connect (`TryCreateConnection` → `FGraphBuilder::ConnectNodes`) | `IsValidEdge` + loop check, `RemoveEdgeToNodeInput` + `AddEdge` | |
| Break link (`BreakPinLinks` → `DisconnectPinVertex`) | `RemoveEdgeToNodeInput` / `RemoveEdgesFromNodeOutput` | |
| Pin default (`UMetasoundEditorGraphNode::PinDefaultValueChanged`) | `SetNodeInputDefault` / `RemoveNodeInputDefault` | |
| Members panel: add input / output / variable (`FEditor::OnAddButtonClickedOnSection`) | `AddGraphInput` / `AddGraphOutput` / `AddGraphVariable` | |
| Member Details: name, display name, description, type, constructor, advanced, sort order | `SetGraphInput/OutputName`, `...DisplayName`, `...Description`, `...DataType`, `SetGraphInputAccessType`, `...AdvancedDisplay`, `...SortOrderIndex` | The editor's sort-order lambda still uses the controller API (`SetSortOrderIndexForInput`). The builder method writes the same field. |
| Member default value (literal object `PostEditChangeProperty` → `UpdateFrontendDefaultLiteral`) | `SetGraphInputDefault(Name, Literal, &PageID)` / `SetGraphInputDefaults` / `SetGraphVariableDefault` | |
| Member widget / range (literal object Details) | `UMetaSoundEditorSubsystem::FindOrCreateGraphInputMetadata`, then Modify, PreEditChange, write, PostEditChangeProperty on that object | The object is a document subobject (`MemberMetadata`) |
| Delete member (`DeleteSelectedInterfaceItems`) | `ClearMemberMetadata` + `RemoveGraphInput` / `RemoveGraphOutput` / `RemoveGraphVariable` | |
| Interfaces panel (`FMetasoundInterfacesDetailCustomization`) | `ModifyInterfaces(FModifyInterfaceOptions(remove, add))` with `bSetDefaultNodeLocations=false` | non-modifiable → `MetaSound.InterfaceNotModifiable` |
| `OutputFormat` Details | generic property write (PostEditChangeProperty runs `SetFormat` + `ConformObjectToDocument`) | Properties are applied before the Body, so the Body refers to the new output names |
| Class metadata Details (`RootGraph.Metadata` property handles) | `SetDisplayName`, `SetDescription`, `SetAuthor`, `SetKeywords`, `SetCategoryHierarchy`, `Set/AddAccessFlags` | |
| Node configuration Details (`FMetasoundNodeDetailCustomization`) | `SetNodeConfiguration` + `UpdateNodeInterfaceFromConfiguration` | pins may change; connections to removed pins go, as in the editor |
| Node comment bubble (`SMetaSoundGraphNode::OnCommentTextCommitted/OnCommentBubbleToggled`) | `SetNodeComment` / `SetNodeCommentVisible` | |
| Update Node (`FEditor::UpdateSelectedNodeClasses`) | the same controller call the editor uses: `INodeController::ReplaceWithVersion`, then `RemoveUnreferencedDependencies` + `SynchronizeDependencyMetadata` | Controller API: public, used by the 5.7 editor. Isolated in one file with a behaviour test (§4 of the reset). |
| Pages (`FMetasoundPagesDetailCustomization`) | `AddGraphPage(PageID, bDuplicateLastGraph=true, bSetAsBuildGraph=false)` / `RemoveGraphPage` | The page's authored content is then reconciled like any page |
| Per-page input default (`MetasoundDefaultLiteralCustomization`) | `SetGraphInputDefaults(Name, all pages)` | |
| Preset "override inherited default" | `SetGraphInputInheritsDefault` | |

Not reproduced, by design: the editor's audition/preview, the focused page (`SetFocusedPage` / `BuildPageID` is
editor view state; every op passes the page explicitly), automatic converter insertion and auto-wiring (documents
are explicit), copy/paste (a document is the copy format).

### 4.2 New C++ primitives vs generic admission

Generic property admission covers the asset's Details properties, including `OutputFormat` and `QualitySetting`
(PostEditChange runs the editor's follow-ups). Nothing in the document can go through generic admission. The
document is one struct property whose Details customisation is the whole editor, and a MetaSound cannot be
scratch-cloned (`Scratch.UnsupportedSource`, direction reset §4.1). So a new provider is needed, following the
Material pattern (one provider, read / edit / compile primitives, Python orchestrates):

- Module **`AssetDocumentAudio`** (Editor, PostDefault), owner `EAssetDocumentPrimitiveOwner::Audio` (new value),
  provider id `audio`. It is table-driven via `CreateAssetDocumentAssetTablePrimitiveProvider`, like
  `ai.state_tree.*`. Dependencies: `MetasoundFrontend`, `MetasoundEngine`, `MetasoundGraphCore`,
  `MetasoundEditor`, `AudioExtensions`, `UnrealEd`, `EditorSubsystem`, `AssetDocumentEditor`,
  `AssetDocumentGraph`, `Json`. The plugin descriptor gains a dependency on the `Metasound` plugin (enabled by
  default). Niagara's design should make the same module-per-plugin choice so neither profile forces the other's
  plugin on a project.
- `audio.metasound.read.v1`: the normalised read (§3.3) as paged records:
  - header: class, preset parent, interfaces with version and modifiability, metadata, page names;
  - members, with page defaults and the reflected widget values of the literal metadata;
  - per page: nodes (ID, class name, type, major and minor version, location or hidden, literals by vertex
    name, configuration type and values, comment), edges by node ID and vertex name, variables, comments;
  - on request, class facts for given class names: pins with type, access and class default, configuration
    struct, new-configuration values;
  - data type facts: literal form and enum entries.
- `audio.metasound.edit.v1`: an op list executed with the asset's `FMetaSoundFrontendDocumentBuilder` inside
  one transaction:
  - `interfaces`, `metadata`;
  - `input.add/remove/rename/set`, `output.*`, `variable.*`, `member.widget`;
  - `page.add/remove`;
  - `node.create` (class / asset / template / member, with ID and page), `node.delete`, `node.set` (location,
    hidden, defaults, comment, configuration), `node.update_version`;
  - `connect` (source or null);
  - `comment.set/remove`;
  - `preset.convert_from/convert_to`, `preset.inherits`.

  Op errors carry `/ops/<n>/...` paths that Python maps back to document paths, as in Material. The builder
  methods are public API, so only `node.update_version` (controller) and the literal-metadata Details write
  (PreEditChange / PostEditChangeProperty on a document subobject) are "editor-way raw" code. They go in their
  own file with a behaviour test.
- `audio.metasound.compile.v1`: "Register and validate" (§5).
- `audio.metasound.catalog.v1`: the `list_types` kinds (§3.6).
- A derived-state rebuilder (`AssetDocumentAudio.MetaSound`) for rollback (§5.3).

Python: `profiles/metasound.py` (regions, keys/IDs, ops planning, diff), `profiles/metasound_codec.py` (literal
codec, class-name and pin parsing). Reused components: `data_table_common.PropertiesRegion` for `Properties`,
`generic_factory_lifecycle` with a factory-context hook for presets, the `SNAPSHOT_COVERED` preflight (no
scratch). Op planning order, as in Material:

1. Properties (`OutputFormat` decides output names).
2. `Preset` conversion.
3. `Interfaces`.
4. Member adds and renames.
5. Pages.
6. Node deletes, then creates.
7. Node sets (configuration before connections, because it can change pins).
8. Connections, in source-depth order (reroutes resolve their access type from their source).
9. Member removals and comments.
10. Compile.

## 5. Compile, validation and rollback

### 5.1 What "compile" is for a MetaSound

MetaSounds have no compile button. After an edit, the editor:

1. registers the graph with the frontend (`FGraphBuilder::RegisterGraphWithFrontend` →
   `FMetasoundAssetBase::UpdateAndRegisterForExecution`). This rebuilds referenced asset classes, registers
   dependencies, **auto-updates node minor versions and rebuilds preset graphs** (`VersionDependencies`), caches
   registry metadata and registers the graph class. When open editors reference this asset, those assets are
   registered too;
2. validates the editor graph (`FGraphBuilder::ValidateGraph`, per node `Validate`) and shows messages on nodes:
   class not found ("Asset may have not been saved, deleted or is not loaded"), interface mismatch with the
   registry class, orphaned pins, "referenced asset class contains error(s)", template nodes missing required
   connections (`HasRequiredConnections`, for example the OneShot `On Finished` warning), more than one location
   (info), duplicate node IDs (warning).

Operator build errors (`IOperatorBuildError`) never reach the editor UI. They are logged when a generator is
built on play (`GeneratorBuilder::LogBuildErrors`).

`audio.metasound.compile.v1` does the same:

1. Register: `UMetaSoundEditorSubsystem::RegisterGraphWithFrontend(Asset, bForceViewSync=true)` (public; the
   private `FGraphBuilder` cannot be linked). Then `ConformObjectToDocument`, and wait for async graph
   registration (`INodeClassRegistry::WaitForAsyncGraphRegistration`).
2. Validate without an editor graph. The checks of `UMetasoundEditorGraphExternalNode::Validate` and the template
   nodes' `HasRequiredConnections` are derived from public builder queries (`FindDependency`,
   `ISearchEngine::FindClassWithHighestMinorVersion`, `DiffAgainstRegistryInterface`, `INodeTemplate`,
   `IsValidEdge`). They live in their own file, and a behaviour test compares them with the messages the real
   editor shows when the fidelity assets are opened in the MetaSound editor during automation.
3. Optional build check (phase 3): `FFrontendGraphBuilder::CreateGraph(document, path, PageOrder)` (MetasoundFrontend,
   public) and the operator builder (`IOperatorBuilder::BuildGraphOperator`, MetasoundGraphCore, public) with the
   project's default operator settings. The `FBuildErrorArray` entries map to node IDs. This catches errors a
   human only sees in the log when pressing Play.

Result DTO: `{status: ok|errors, messages[{severity, message, page, node_id, vertex?}], registered: bool, auto_updated_nodes[]}`.

### 5.2 Failure policy and status shape

As in Material: the before-state's validation messages are read first (no registration). **New errors** fail the
apply with `MetaSound.ValidationFailed` (or `MetaSound.BuildFailed`) at the document path of the node
(`/Body/Graphs/<page>/Nodes/<key>`) and roll back. Errors the asset already had come back as warnings
(`MetaSound.ValidationError`), and warnings and infos come back with the successful result
(`MetaSound.ValidationWarning`). Auto-updated nodes are reported as `MetaSound.NodeAutoUpdated` (info).

Extract / Apply / Inspect carry `Compile` in the Blueprint shape, via a compile-status describer registered with
Core (`AssetDocumentCompileStatus.h`):

```json
{"Status": "UpToDateWithWarnings",
 "Messages": [{"Severity": "Warning", "Message": "\"On Finished\" should be connected for OneShot MetaSound sources...",
               "Graph": "Default", "Node": "Done", "NodeGuid": "8C6A...", "Path": "/Body/Graphs/Default/Nodes/Done"}],
 "Registered": true}
```

`Status`: `UpToDate`, `UpToDateWithWarnings`, `Error`, `Unknown` (never registered in this session; Extract does
not register).

### 5.3 Rollback

- Existing asset: Core's package snapshot. The rebuilder then does what the editor's undo does
  (`FAssetHelper::PostEditUndo`): it calls the asset's public `PostEditUndo()` (`SetForceRefreshViews`,
  `IDocumentBuilderRegistry::ReloadBuilder(ClassName)`, `RegisterGraphWithFrontend`) and `ConformObjectToDocument`.
  Registration on the restored document can auto-update minor versions again. That is the same state the editor
  would show and is not "worse" under §3.
- Created asset: the factory rollback deletes the package. Before that, the lifecycle calls
  `FDocumentBuilderRegistry::FinishBuilding(ClassName, AssetPath, bForceUnregisterNodeClass=true)`. Destruction
  runs `OnNotifyBeginDestroy` → `UnregisterGraphWithFrontend`. A test checks that no builder, asset manager or node
  registry entry is left behind (§3 item 4).
- Referencing assets: registration can re-register loaded MetaSounds that use this one as a node (open editors).
  As with User Defined Struct dependents (§3.2 of the reset), loaded referencers with unsaved changes refuse the
  apply (`MetaSound.DependentHasUnsavedChanges`), and referencers the editor marks dirty are reported
  (`MetaSound.DependentsUpdated`). On rollback they are re-registered after the target is restored.
- Open editor (M2, decision §9.3): the MetaSound editor works on the real asset, and its Tick resyncs its graph
  from `ModifyContext`, which the builder methods fill; the compile registers with `bForceViewSync`. The edit
  primitive modifies the open editor's graph with the asset as the editor's own edits do, and the snapshot rebuilder
  treats objects under an `UEdGraph` (the transient editor graph) as derived. Test
  `UEAssetDoc.MetaSound.OpenEditor`: an apply shows in the open graph, a failed apply rolls back,
  editor undo / redo keep the asset whole.

## 6. Fidelity acceptance plan

### 6.1 Content (MetaSounds shipped with the UE 5.7 install: `/Metasound`, Harmonix and the TP_UEIntro_BP template; none under `Engine/Content`)

Test sandbox: enable `Harmonix` (experimental, **not** enabled by default; it pulls in MusicEnvironment) and copy
`Templates/TP_UEIntro_BP/Content/DemoTemplate/Metasounds` to `/Game/DemoTemplate/Metasounds`. Formats: N = paged
document (5.5+), L = legacy (serialized editor graph, upgraded on load), M = between.

| # | Asset | Why |
|---|---|---|
| 1 | `/Metasound/DefaultAssets/Sources/MSS_PlayRandom_Oneshot` (N) | canonical Epic source: Stereo, OneShot, reroute, comments, array nodes |
| 2 | `/Metasound/DefaultAssets/Sources/MSS_PlayRandom_Loop` (N) | same graph without OneShot (interface removal) |
| 3 | `/Game/DemoTemplate/Metasounds/MS_TuningFork` (N) | largest (311 KB); the only one with variables (get / set / delayed), arrays, class interface override |
| 4 | `/Game/DemoTemplate/Metasounds/MS_Opera` (N) | Mono, filters, mixers, comments, enums, no dependencies |
| 5 | `/Harmonix/Tests/HarmonixMetasound/FusionSamplerNode/MS_Test_FusionSampler_SynSamples` (M) | smallest source, asset reference |
| 6 | `/Harmonix/MetaSounds/MSP_MidiNoteAdsr` (L) | smallest patch, legacy format |
| 7 | `/Harmonix/Tests/HarmonixMetasound/MS_MIDIStreamSineGenerator` (N) | patch used by 8 sources |
| 8 | `/Harmonix/Tests/HarmonixMetasound/MS_TriggerPulseSineGenerator` (N) | patch with trigger and time inputs |
| 9 | `/Harmonix/Tests/HarmonixMetasound/MidiPlayerNode/MS_Test_MidiPlayerNode_Simple` (L) | small legacy source using a patch |
| 10 | `.../MidiPlayerNode/MS_Test_MidiPlayerNode_Metronome_MidiClockTriggers` (L) | 42 nodes, two patches, mixers, Int32 array |
| 11 | `.../MidiPlayerNode/MS_Test_MidiPlayerNode_Metronome_AuthoredTimeSigChanges` (L) | legacy comment node, two patches |
| 12 | `.../StepSequenceNode/MS_Test_StepSequencePlayer_PageChanges` (L) | 49 nodes, largest legacy source |
| 13 | `/Harmonix/Examples/Sources/MSS_MIDISequencePlayer` (N) | Stereo, full `UE.Source` trigger set |
| 14 | `/Harmonix/Tests/HarmonixMetasoundMusicClockComponent/MSS_MetronomeClockGenerator` (N) | looping Mono, interface override |
| 15 | `.../MidiPlayerNode/MS_Test_MidiPlayerNode_Metronome_TempoChanges` (L, optional) | `Trigger Route (Float, 6)` |

Content not shipped, built by the test with the builder API the editor uses (then saved, reloaded and used as input):

- a preset of #4 with two overridden defaults;
- a patch preset;
- a two-page Source (test-project `UMetaSoundSettings` page `Low`, page defaults);
- a 5.1 / 7.1 Source;
- a Source with `UE.Attenuation` + `UE.Spatialization`;
- a Patch with `UE.InputFormat.Stereo` / `UE.OutputFormat.Stereo`;
- a node with a non-default configuration.

### 6.2 What is compared

For each asset:

1. Duplicate it into `/Game` and save.
2. Extract it.
3. Apply the extract as a new asset (`Create`).
4. Compare:
   - **document**: the two extracts equal, except that `Id`s stay equal because they are carried;
   - **frontend document**, normalised: the same nodes by ID with the same class names and major versions,
     locations, literals, configurations, edges, variables, comments, members with defaults per page, interfaces,
     preset options and member-metadata widget values. Ignored: the root `ClassName`, `IdCounter`, `ChangeID`s,
     location-map keys, `Dependencies` order, literal storage type, minor versions (both sides auto-update on
     registration, so they match whenever the registry is the same);
   - **validation**: the compile status and messages are equal;
   - **runtime graph** (cheap): `FFrontendGraphBuilder::CreateGraph` succeeds for both, with equal vertex
     interfaces and node/edge counts, and the operator build gives the same error list;
   - **audio, where deterministic**: for sources without random nodes or external timing, render 1 s through the
     operator with the same inputs and compare buffers (bit-exact or −120 dB).
5. Re-apply: no-op.
6. Reload the package from disk and extract again: equal.

### 6.3 Free edits (document vs editor API on two duplicates of #1 and #4)

The same edits are made through a document and through the editor's calls (the `UMetaSoundBuilderBase` /
`UMetaSoundEditorSubsystem` methods the schema actions and Details callbacks call, listed in §4.1). The
frontend documents and validation results must come out equal:

- insert a `UE.State Variable Filter.Audio` between the wave player and the output;
- delete a node;
- reconnect an output to another output pin;
- add, rename and remove an input with two `Get` nodes;
- change an input's type (edges to incompatible pins go);
- change a page-default value;
- add a variable with Get, Set and Get Delayed;
- add and edit a comment;
- move nodes;
- `OutputFormat` Mono → Stereo, with the output nodes re-pointed;
- remove and re-add `UE.Source.OneShot`;
- change a node's configuration;
- Update Node on an outdated major version;
- Convert From Preset;
- override, then un-override, a preset default.

Undo test: human edit, save, AI apply, Ctrl+Z behaviour as §3.1 (history cleared when it references the asset).

### 6.4 AI from scratch

- **Python test:** one `Create` builds a stereo one-shot synth. It has `Pitch` / `Cutoff` / `Wave` inputs with
  widget ranges, a Wave Player, an AD envelope, a filter, a reroute, a variable counter with Get/Set, a comment
  and `OneShot` with `On Finished` wired. It validates with no errors and plays (a generator is built). Then:
  - a patch is made, and a second source uses it as a node;
  - a preset of the first source overrides `Cutoff`;
  - errors (bad class, bad pin, type mismatch, constructor-to-reference connection, cycle, preset graph
    authoring, unknown page) fail at their document paths and roll back.
- **Black-box trial** (using only the MCP tools and the schema section): "a looping wind
  ambience with a random-gust LFO and a volume input", "an impact one-shot that picks a random wave from an array
  input", and "add a lowpass to an existing MetaSound and expose its cutoff".

## 7. Risks, unknowns and phases

### 7.1 Risks and unknowns

| Risk | Effect | Answer |
|---|---|---|
| Registration mutates the document (auto-update of minor versions, preset rebuild, `RebuildReferencedAssetClasses`) | Extract after an apply can differ from what was written; a no-op check that registers would never be a no-op | Minor versions, dependencies and preset graphs are derived, not authored. A no-op apply never registers. Extract never registers. Fidelity compares after both sides have registered. |
| Input template injection on old (L/M) assets | Extract before and after the first apply would differ | The normalised read with deterministic IDs (§3.3). Test: extract L assets, apply the extract to the same asset → no-op apart from the injection change, which is reported once. |
| `FGraphBuilder` (sync, validation, unique names) is private | Validation must be derived | Separate file plus a behaviour test against the real editor's node messages on the fidelity set |
| Controller API (`ReplaceWithVersion`, sort order) is soft-deprecated | May go away in 5.8 | One isolated file. The builder has `SetGraphInputSortOrderIndex`; Update Node falls back to delete + create keeping edges by vertex name if the controller path goes |
| Experimental 5.7 surface: node configuration (`TInstancedStruct`), `ClassInterfaceOverride`, `EAutoUpdateEligibility` (`UE_EXPERIMENTAL`), polymorphic data types, pages | Version-specific code | Configuration through the typed value codec, overrides derived (from the configuration), pages behind phase 3. Pin the matrix to 5.7. |
| Literal storage types differ between old assets and new writes | False fidelity diffs | Semantic literal comparison by data type |
| Async: referenced assets load asynchronously after `PostLoad`; graph registration may be async | Edits or validation on half-loaded state | Wait as `PreSaveAsset` does (async loads) and `WaitForAsyncGraphRegistration` before validation |
| Harmonix is experimental and off by default; Template content is not mounted | Fidelity set needs a prepared sandbox | `ue.sh` sandbox recipe enables Harmonix and copies the template folder; tests skip with a note when absent |
| Harmonix Fusion patches without an audio device (`-NoSound`) | `UFusionPatch::CreateProxyData` hits `check(SoundWaveProxy)` (HarmonixDsp `KeyzoneSettings.cpp:48`) and the editor crashes | Our paths never register such a graph: Create / Update of a MetaSound whose literals (or whose referenced MetaSounds', e.g. a preset's parent) hold a Fusion patch are refused with `MetaSound.AudioDeviceRequired` (M2). The engine itself triggers the crash when the first MetaSound editor of a session opens (its asset-tag priming registers the loaded MetaSounds) while such a MetaSound is loaded: the open-editor test unloads them first |
| Editors opened during the validation behaviour test, audio device / `-NoSound` | Flaky automation | Validation test runs only with the normal renderer and audio (as Material); the profile itself needs no audio device (`OutputFormat`/quality edits do not touch the device) |
| Referencing MetaSounds re-registered by an edit of a patch | Side effects on other packages | Dependents rule (§5.3), reported as warnings |
| Plugin dependency on `Metasound` | Projects that disable MetaSound | MetaSound is enabled by default; the module lives alone, so a later "optional plugin" split only touches `AssetDocumentAudio` |
| Performance | Registry has ~1000 classes; graphs ≤ ~300 KB | Class facts are fetched only for classes used in the document; `list_types` is paged. Reads are one document copy, cheap. |

### 7.2 Phases (each independently mergeable)

| Phase | Content | Acceptance |
|---|---|---|
| **M0 read** | `AssetDocumentAudio` module, provider, `read` + `catalog` primitives, Python Extract / Inspect / Diff for both classes (Apply still refused with a `MetaSound.ReadOnly` diagnostic replacing `Scratch.UnsupportedSource`), `list_types` kinds, compile-status describer (validation without registration) | Extract is stable (twice equal, equal after reload) on all 15 fidelity assets; Inspect pins match the registry; Body-owned properties are not emitted as `Properties` |
| **M1 graph apply** | edit + compile primitives, create through the factory, `Interfaces`, `Metadata`, `Inputs`/`Outputs` (default page), `Graphs.Default` (all node kinds, connections, defaults, comments, variables), `Properties` incl. `OutputFormat`, rollback rebuilder, open-editor refusal, dependents rule | §6.2 on all non-preset single-page assets; §6.3 free edits except configuration / Update Node / presets; §6.4 Python test (without preset); rollback (failed validation, failed connection, failed create leaves no registry entry); Undo test |
| **M2 presets and details** | `Preset` (create through factory context, convert from (to: refused, §9.4), overrides), node `Configuration`, `Version` (Update Node), member `Widget` metadata | builder-made preset and configuration fixtures pass §6.2; the remaining §6.3 edits; preset part of §6.4 |
| **M3 pages and build check** | non-default pages, `PageDefaults`, operator build check in compile | two-page fixture passes §6.2 with page settings in the sandbox; build-error fixture fails at the node path |
| **M4 AI surface** | `MCP/schemas/assetdocument/MetaSound.md` checked against the code, `list_types` description, gap closure (Audio Analyzer nodes, §6.1 format fixtures as test `Formats`), support matrix. Open-editor apply was lifted in M2 (tests `OpenEditor`, `PagesOpenEditor`) | matrix doc [asset-document-metasound-matrix.md](asset-document-metasound-matrix.md) approved by the user (2026-10-18); the trial tasks of §6.4 done through MCP only (AI trial 8, passed) |

## 8. Open questions for the user

1. **Pages**: they are labelled experimental in 5.7 and need project page settings. Is M3 (multi-page authoring)
   wanted in batch 2, or should the profile read pages (extract shows them) and refuse page edits with a stable
   diagnostic until a project needs them?
2. **Harmonix**: 13 of the 15 fidelity assets need the experimental Harmonix plugin enabled in the test sandbox.
   Is enabling it in the test project acceptable, or should fidelity rely on the two `/Metasound` sources, the
   two template assets and builder-made fixtures?
3. **Open editor**: refuse applies while the asset is open in the MetaSound editor (as Material does) until M4,
   or invest earlier in live apply, since the MetaSound editor, unlike the Material editor, edits the real asset?
4. **Convert to preset on an existing asset**: the editor has no menu for it (only the builder API and "Create
   Preset" on a new asset). Admit it (`Preset` added on Update), or require a new asset?

## 9. Decisions (2026-10-06, user)

1. Pages: multi-page editing is in batch 2, phase M3 (M0–M2 single page first).
2. Harmonix: enabled only in the test project (sandbox .uproject); the UEAssetDoc plugin does not depend on it, and tests that need Harmonix content skip with a note when it is not enabled.
3. Applying while the asset is open in the MetaSound editor moves to M2 (refresh the open editor; editor Undo tested); M1 refuses with `MetaSound.OpenInEditor`.
4. `Preset` on an existing non-preset asset is refused (the editor cannot convert an asset into a preset); presets are created with `Create`, and an existing preset's content stays editable.
