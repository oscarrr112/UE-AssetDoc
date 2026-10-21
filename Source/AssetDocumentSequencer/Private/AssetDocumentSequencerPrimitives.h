// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentGraphPrimitives.h"
#include "Misc/FrameNumber.h"
#include "Misc/FrameRate.h"
#include "UObject/StrongObjectPtr.h"

struct FUniversalObjectLocator;
class AActor;
class UActorComponent;
class FJsonObject;
class IAssetDocumentDerivedStateRebuilder;
class UMovieScene;
class UMovieSceneFolder;
class UMovieSceneSection;
class UMovieSceneSequence;
class UMovieSceneTrack;
class UBlueprint;
struct FMovieSceneDynamicBinding;
struct FMovieSceneEvent;
struct FMovieSceneSequenceID;
struct FMovieSceneObjectBindingID;
struct FMovieSceneSpawnable;
struct FOptionalMovieSceneBlendType;

// Sequencer primitives (canonical provider "seq", owner Cinematics) of the Level Sequence profile
// (docs/architecture/asset-document-sequencer-design.md §4.4). Same conventions as the rig and physics providers:
// one "asset" handle, value DTOs with schema_version 1, errors as Graph.* / Sequencer.* codes. Phase S2: read, status
// and the core authoring edits (seq.edit.v1).
namespace UE::AssetDocument::Graph::Sequencer
{
inline const TCHAR* ProviderId = TEXT("seq");

TConstArrayView<FAssetDocumentPrimitiveDescriptor> GetDescriptors();
TConstArrayView<FBinding> GetBindings();

bool Fail(FCall& Call, FStringView Code, FStringView Message, FStringView Path = TEXTVIEW("/"));

// Shortest decimal that reads back as the same float32 (0.1, not 0.10000000149011612).
double ShortestFloat(double Value);

// A name as a document string ("" for None).
FValue MakeName(FName Name);

// Document path segment (JSON pointer escaping of ~ and /).
FString Escape(const FString& Name);

// Pages records under the runtime value budget: {total, next_offset, records}.
void AddRecordPage(TArray<FValue>& Records, int64 Offset, FObjectBuilder& Result);

// ---- time (design §3.3) --------------------------------------------------------------------------------------------

// The two rates of one movie scene: times are stored in ticks, the document states display frames.
struct FTimeBase
{
	FFrameRate Tick = FFrameRate(24000, 1);
	FFrameRate Display = FFrameRate(30, 1);
};

// Ticks as display frames: an integer when the frame is whole, else the shortest decimal that converts back to the same
// tick (FFrameRate::TransformTime display -> tick, rounded to the nearest tick, as the profile's apply does).
FValue MakeFrames(int64 Ticks, const FTimeBase& Time);
// {"Numerator", "Denominator"}.
FValue MakeRate(const FFrameRate& Rate);
// The display frame a tick lands on is whole.
bool IsWholeFrame(int64 Ticks, const FTimeBase& Time);

// ---- the sequence model (AssetDocumentSequencerModel.cpp) ----------------------------------------------------------

// One binding (possessable) of the movie scene with its document identity (design §3.4.2).
struct FBindingInfo
{
	FGuid Guid;
	FGuid Parent;
	FString Name;          // the Sequencer's label (UMovieScene::GetObjectDisplayName)
	FString Segment;       // binding path segment: Name, or #<Id> when Id is needed
	FString Path;          // binding path from the root ("Camera/CameraComponent")
	FString DocumentPath;  // "/Body/Bindings/0/Children/1"
	bool bNeedsId = false; // a sibling has the same name (or the name cannot be a path segment)
	bool bRoot = false;    // the root spawnable of a Template / Camera Animation Sequence (design §3.4.3)
	TArray<FGuid> Children;
};

struct FTrackInfo
{
	const UMovieSceneTrack* Track = nullptr;
	FGuid Binding;          // invalid for global tracks
	FString DocumentPath;   // "/Body/Tracks/2", "/Body/Bindings/0/Tracks/1"
	FString Type;           // type token (§3.5.1) or class path
	FString Property;       // property tracks: PropertyBinding.PropertyPath
	FString Name;           // nameable tracks: the display name ("" when unnamed)
	bool bNeedsId = false;  // (Type, Property, Name) repeats in its scope
};

struct FFolderInfo
{
	FString Path;
	const UMovieSceneFolder* Folder = nullptr;
};

struct FSequenceModel
{
	const UMovieSceneSequence* Sequence = nullptr;
	const UMovieScene* MovieScene = nullptr;
	FTimeBase Time;
	// A Template / Camera Animation Sequence: its document has one root binding (the first spawnable, Body.Root) whose
	// children are component bindings located by name (UTemplateSequence::BoundActorComponents).
	bool bTemplate = false;
	FGuid Root;
	TArray<FGuid> Roots;              // root bindings in document order
	TArray<FGuid> Order;              // every binding in document order (depth first)
	TMap<FGuid, FBindingInfo> Bindings;
	TArray<FGuid> Orphans;            // object bindings outside the document (legacy spawnables, bindings outside a template's root,
	                                  // stale entries)
	TArray<FTrackInfo> Tracks;        // global tracks first (camera cut first), then per binding in document order
	TMap<const UMovieSceneTrack*, int32> TrackIndex;
	TArray<FFolderInfo> Folders;      // document order (depth first, sorting order)
	TMap<FGuid, FString> BindingFolders;
	TMap<const UMovieSceneTrack*, FString> TrackFolders;
	// Section group number of each grouped section (groups numbered by their first section in Tracks order).
	TMap<const UMovieSceneSection*, int32> SectionGroups;

	const FBindingInfo* FindBinding(const FGuid& Guid) const { return Bindings.Find(Guid); }
	FString SectionPath(const UMovieSceneSection& Section) const;
};

// The sequence classes this provider documents: Level Sequences and Template Sequences (Camera Animation Sequences are
// Template Sequences).
bool IsDocumentedSequence(const UObject& Asset);

// False with an error for a sequence without a movie scene.
bool BuildModel(const UMovieSceneSequence& Sequence, FSequenceModel& OutModel, FString& OutError);

// Type token of a track class (design §3.5.1): "Transform", "Float", "Event", ...; the class path for other classes.
FString TrackTypeToken(const UClass& TrackClass);
// The track class of a type token or class path (nullptr when there is none).
UClass* TrackClassOf(const FString& Type);
// The section class a track of TrackClass takes for a section class token (SectionClassToken's inverse), or nullptr.
UClass* SectionClassOf(const FString& Token, const UClass& TrackClass);
// The section class token (only written for tracks that take several section classes): "Trigger", "Repeater",
// "CinematicShot", ...; the class path outside the engine's MovieScene modules.
FString SectionClassToken(const UClass& SectionClass, const UClass& TrackClass);
// The track takes more than one section class (event Trigger / Repeater, sub / shot sections).
bool HasSeveralSectionClasses(const UClass& TrackClass);
// The track takes sections of this class: UMovieSceneTrack::SupportsType, and for event tracks the Trigger / Repeater
// sections the event track editor's "Add Trigger / Repeater Section" menus create (SupportsType names only the legacy
// UMovieSceneEventSection).
bool TakesSectionClass(const UMovieSceneTrack& TrackDefaults, UClass* SectionClass);
// The blend type a new section of this class gets in this section's track: the camera cut, sub / shot, CVar and
// template sequence sections set Absolute in their constructor when the track supports it, which their class default
// (no track) cannot. A loaded section is constructed in its track too, so a blend left at the class default's (unset)
// is not saved and comes back as this value (the former KI-17): the read omits Blend and the edit resets it to this value.
FOptionalMovieSceneBlendType NewSectionBlend(const UMovieSceneSection& Section);
// Non-abstract, non-deprecated subclasses of Base that are loaded (native and loaded Blueprint classes).
TArray<UClass*> ConcreteSubclasses(const UClass& Base);

// ---- values (AssetDocumentSequencerValues.cpp) ---------------------------------------------------------------------

// Read-side diagnostics: {kind: "warning", code, message, path} records.
struct FWarnings
{
	TArray<FValue> Records;
	void Add(const TCHAR* Code, const FString& Message, const FString& Path);
	// Document paths whose default value the read reports (seq.read.v1 "defaults_at", Verify): a sparse object leaves
	// out a field at its default, so an authored default reads back absent. EncodeSparseObject adds
	// {kind: "default", path, value} for each one under a field it leaves out (AddDefault).
	TSet<FString> DefaultsAt;
	// Document path prefix -> the path the encoder writes it at, for fields a read renames (a spawnable's
	// SpawnOwnership is the document's Ownership): AddDefaultsAt finds a requested default under its encoder path and
	// reports it at the document path.
	TArray<TPair<FString, FString>> DefaultsAliases;
	FString EncoderPathOf(const FString& DocumentPath) const;
	void AddDefault(const FString& Path, const FValue& Value);
};

// Encoding context of one value: the model (binding paths), the time base frame numbers are in, the warnings sink
// and the document path of the value.
struct FEncodeContext
{
	const FSequenceModel& Model;
	FTimeBase Time;
	FWarnings& Warnings;
	FString Path;
};

// A document value of one property value (design §3.3, §7.1 adapters): FFrameNumber -> display frames,
// FMovieSceneObjectBindingID -> {"Binding": path} (raw {Guid, SequenceID, ParentIndex} + Sequencer.UnsupportedRemoteBinding
// for a reference into another sequence), FMovieSceneTimeWarpVariant -> one of {"PlayRate": x}, {"FixedFrame": frames},
// {"FrameRate": rate}, {"Loop": frames}, {"Clamp": frames}, {"LoopFloat": x}, {"ClampFloat": x}, {"Custom": instanced
// object}, FMovieSceneEvent -> the event shape, instanced objects {Class, Properties?}, engine structs member by member,
// everything else through the Editor typed value codec. A value the codec
// cannot represent is {"Unsupported": <export text>} with Sequencer.UnsupportedValue. Never fails.
FValue EncodeValue(const FProperty& Property, const void* ValuePtr, FEncodeContext& Context);

// The fields of Object that differ from Defaults (null: every field), in declaration order, filtered by Include.
// Engine structs compare member by member. Empty object when nothing differs.
FValue EncodeSparseObject(const UStruct& Struct, const void* Value, const void* Defaults, TFunctionRef<bool(const FProperty&)> Include,
	FEncodeContext& Context);

// A stored field: not transient, deprecated, or a delegate.
bool IsStoredField(const FProperty& Property);
// The property (or a member of it, at any depth) holds a Sequencer channel (channels are read through the channel proxy).
bool HoldsChannel(const FProperty& Property);
// The property refers to sections (Sections arrays, SectionToKey): owned by the document's Sections list.
bool RefersToSections(const FProperty& Property);

// The struct holds a frame number, binding ID, time warp, event or instanced object at some depth (encoded member by
// member).
bool ContainsAdaptedValue(const UScriptStruct* Struct);

// An instanced object reference (conditions, custom time warps): owned by the object that holds it and written as
// {"Class": path, "Properties"?: sparse against the class defaults} or null; its channels are the section's channels.
bool IsInstancedObjectProperty(const FProperty& Property);
// The fields of an instanced object in its Properties: stored fields that are not channels.
bool IsSubobjectField(const FProperty& Property);
FValue EncodeSubobject(const UObject* Object, FEncodeContext& Context);

FValue EncodeBindingId(const FMovieSceneObjectBindingID& Id, FEncodeContext& Context);
FValue EncodeEvent(const FMovieSceneEvent& Event, FEncodeContext& Context);

// Endpoint name of an event / director binding endpoint node: a function graph's name or a custom event's name.
FString EndpointName(const UObject* Endpoint);
// The endpoint node an event / director binding calls while it is still in its Blueprint (nullptr once its graph was
// removed, e.g. FBlueprintEditorUtils::RemoveGraph renames the graph out of the Blueprint, or the node was deleted).
const UObject* LiveEndpoint(const TWeakObjectPtr<UObject>& Endpoint);

// ---- decode (AssetDocumentSequencerDecode.cpp) ---------------------------------------------------------------------

// Decoding context of one written value: the model (binding paths), the time base frame numbers are in, the object
// that owns the value (text ids, object pickers), the reader errors go to and the value's path below the reader.
struct FDecodeContext
{
	const FSequenceModel& Model;
	FTimeBase Time;
	UObject* Owner = nullptr;
	FReader& Reader;
	FString Path;
	// Set when a decoded value replaced a time warp or instanced object the section's channel proxy may point into
	// (the caller rebuilds the proxy).
	bool* ProxyChanged = nullptr;
};

// Display frames -> ticks (FFrameRate::TransformTime display -> tick, rounded to the nearest tick).
int64 TicksOf(double Frames, const FTimeBase& Time);
bool DecodeFrames(const FValue& Value, FDecodeContext& Context, int32& OutTicks);
// The binding a binding path names ("A/B", or "#<Id>" segments).
bool ResolveBindingPath(const FSequenceModel& Model, const FString& Path, FGuid& OutGuid);
bool DecodeBindingId(const FValue& Value, FMovieSceneObjectBindingID& Out, FDecodeContext& Context);

// An engine struct written member by member (not a core struct, not one of the adapted structs, not a channel).
bool IsMemberwiseStruct(const FProperty& Property);
// A channel field: a channel, a fixed array / container of channels or of structs holding channels (written through
// the channel proxy by name). A single struct that holds channels is not one: its other members are values.
bool IsChannelField(const FProperty& Property);
// EncodeValue's inverse onto ValuePtr (an edit of the value there).
bool DecodeValue(const FProperty& Property, const FValue& Value, void* ValuePtr, FDecodeContext& Context);
// EncodeSparseObject's inverse, complete: listed fields are decoded (onto the default value), omitted fields take
// their default value (Defaults: null keeps them); unknown or filtered fields fail with Sequencer.FieldNotEditable.
bool DecodeSparseObject(const UStruct& Struct, const FValue& Object, void* Memory, const void* Defaults, TFunctionRef<bool(const FProperty&)> Include,
	FDecodeContext& Context);
// Structural equality of two document values (numbers as float32 or exact).
bool SameValue(const FValue& A, const FValue& B);

// Scratch memory for one property value.
struct FPropertyValueBuffer
{
	explicit FPropertyValueBuffer(const FProperty& InProperty);
	~FPropertyValueBuffer();
	FPropertyValueBuffer(const FPropertyValueBuffer&) = delete;
	FPropertyValueBuffer& operator=(const FPropertyValueBuffer&) = delete;

	const FProperty& Property;
	void* Memory = nullptr;
};

// ---- channels (AssetDocumentSequencerChannels.cpp) -----------------------------------------------------------------

// The channels of a section (channel proxy, metadata names; an unnamed channel is "Value"): Channels entries without
// their keys ({Default?, PreInfinity?, PostInfinity?} or {"Unsupported"} for a channel struct without reflected key
// arrays), and the key records that follow the section record:
//   {kind: "keys", channel, keys: [...]}                       (64 keys or fewer: one object per key)
//   {kind: "columns", channel, frames, values, interp?, arrive?, leave?, arrive_weight?, leave_weight?, weight_mode?}
//                                                              (more than 64 keys, chunked; Python joins and collapses)
// Curve keys: {Frame, Value, Interp?, Arrive?, Leave?, ArriveWeight?, LeaveWeight?, WeightMode?} (tangents of User /
// Break keys only); discrete keys {Frame, Value}; event keys {Frame, Event}.
void ReadChannels(const UMovieSceneSection& Section, FEncodeContext& Context, FObjectBuilder& OutChannels, TArray<FValue>& OutKeyRecords);

// Channel catalogue of a section: [{Name, Type}] in proxy order.
TArray<TSharedPtr<FJsonValue>> DescribeChannels(const UMovieSceneSection& Section);

// Writes one channel of a section by its document name (design §3.3; the editor's channel data interface): with
// bAppend false the channel is reset (keys, default) and Channel's keys, Default and extrapolation written; with bAppend
// true Channel's keys are added to the ones already there (the columnar chunks of a dense channel). Keys are
// {Keys: [...]} objects or the columnar {Frames, Values, Interp?, Arrive?, ...}. Curve channels re-run AutoSetTangents;
// Stated (appends) lists the keys already in the channel whose stated tangents are kept (the earlier chunks' Arrive /
// Leave of keys other than User / Break), so every chunk's stated tangents survive.
bool WriteChannel(UMovieSceneSection& Section, const FString& Name, const FValue& Channel, bool bAppend, FDecodeContext& Context,
	TConstArrayView<int32> Stated = {});
// The same for one channel struct in memory (a parameter's channel, AssetDocumentSequencerParameters.cpp).
bool WriteChannelValue(FName TypeName, void* Channel, const FValue& Value, bool bAppend, TConstArrayView<int32> Stated, FDecodeContext& Context);
// Reads one channel struct in memory into OutChannels[Name] and its key records (with "parameter": Parameter when not
// empty). Context.Path is the path of the object that holds Channels.
void ReadChannelValue(FName TypeName, const void* Channel, const FString& Name, TConstArrayView<int32> Parameter, FEncodeContext& Context,
	FObjectBuilder& OutChannels, TArray<FValue>& OutKeyRecords);
// A channel struct type with reflected key arrays (KeyTimes / KeyValues): it can be read and written.
bool IsSupportedChannelType(FName TypeName);

// ---- parameter sections (AssetDocumentSequencerParameters.cpp, design §3.5.3) --------------------------------------

// A section's parameter sets: every stored array of structs that hold channels (material, MPC, custom primitive data
// and particle parameter curves, DMX fixture patch channels), except Control Rig sections (read through the channel
// proxy, S4). Their channels are written in Parameters, not Channels:
//   "Parameters": [{Type?, Name?, Properties?, Channels?, Parameters?}]
// Type: the set (the array's name without "ParameterNamesAndCurves" / "ParameterInfosAndCurves": Bool, Scalar,
// Vector2D, Vector, Color, Transform; the array's name otherwise), omitted where the owner has a single set; Name: the
// parameter's name (ParameterName, or ParameterInfo.Name); Properties: the element's other fields, sparse; Channels:
// the element's channel members by field name ("Name[i]" for fixed arrays); Parameters: nested sets (DMX function
// channels of a fixture patch). Entries are listed set by set in the class's order.
bool UsesParameters(const UMovieSceneSection& Section);
// Addresses of the channels inside parameter sets (skipped by the flat Channels).
TSet<const void*> ParameterChannelAddresses(const UMovieSceneSection& Section);
// The Parameters entries and the key records of their channels ("parameter": index path of the entry).
FValue ReadParameters(const UMovieSceneSection& Section, FEncodeContext& Context, TArray<FValue>& OutKeyRecords);
// Rebuilds every parameter set from the entries (null: none) and rebuilds the channel proxy (PostEditImport, as a paste).
bool WriteParameters(UMovieSceneSection& Section, const FValue* Entries, FDecodeContext& Context);
// Adds entries at the end of their sets in the section (empty Path) or in the entry at Path: the rest of a Parameters
// list too large for one op (seq.edit.v1 parameters.append).
bool AppendParameters(UMovieSceneSection& Section, TConstArrayView<int32> Path, const FValue& Entries, FDecodeContext& Context);
// The channel Name of the parameter entry at Path (index path as in the key records).
bool FindParameterChannel(UMovieSceneSection& Section, TConstArrayView<int32> Path, const FString& Name, FName& OutType, void*& OutChannel,
	FString& OutError);
// [{Type, Name: bool, Channels: [...], Properties: [...]}] of a section class (catalog / Inspect).
TArray<TSharedPtr<FJsonValue>> DescribeParameters(const UClass& SectionClass);
// Clears a channel (no keys, no default), as an omitted channel of a listed section.
void ClearChannels(UMovieSceneSection& Section, const TSet<FString>& Keep);
// Document names of a section's channels (proxy order) and whether each is supported (has reflected key arrays).
TArray<TPair<FString, bool>> ChannelNames(const UMovieSceneSection& Section);

// Key times of every channel of a section, parameter channels included (status: duplicate keys).
void ForEachChannelKeyTimes(const UMovieSceneSection& Section, TFunctionRef<void(const FString& Channel, TConstArrayView<FFrameNumber> Times)> Visit);

// ---- sections and tracks (AssetDocumentSequencerRead.cpp) ----------------------------------------------------------

// Section / track Properties: the class's own stored fields and the editable base fields (design §3.5), not header
// fields, channels, sections, delegates or derived state.
bool IsSectionPropertyField(const FProperty& Property);
bool IsTrackPropertyField(const FProperty& Property);
// Spawnable templates and custom bindings (design §3.4.3): component references are the template's structure, not
// values; template fields are the ones the class's Details panel edits: CPF_Edit, not read-only (CPF_EditConst, a
// Cine Camera's CurrentHorizontalFOV) and not in a category the class hides (a Cine Camera component hides
// CameraSettings: FieldOfView and AspectRatio are derived from the lens and filmback).
bool IsComponentReference(const FProperty& Property);
bool IsTemplateField(const UClass& Class, const FProperty& Property);
// A binding's locator as a document string, and whether a string parses back to that locator.
FString LocatorString(const FUniversalObjectLocator& Locator);
bool LocatorRoundTrips(const FUniversalObjectLocator& Locator, const FString& Text);
// What a spawnable template's omitted fields mean: a fresh actor of the class, as Sequencer makes a spawnable's template,
// with its derived fields recomputed (edit-change notifications, as a written template gets them and as PostLoad
// recomputes them for a saved one: a Cine Camera's FieldOfView and AspectRatio; the class default object and the
// component archetypes hold other values). Transient; collected after use.
class FTemplateBaseline
{
public:
	explicit FTemplateBaseline(UClass& ActorClass);
	// The actor's defaults.
	const UObject* Actor() const;
	// The defaults of the template's component (the fresh actor's component of that name, else the archetype).
	const UObject* Component(const UActorComponent& Component) const;

private:
	TStrongObjectPtr<AActor> Fresh;
	UClass* Class = nullptr;
};
// The binding's spawnable fields: document key -> UMovieSceneSpawnableActorBinding property.
TConstArrayView<TPair<const TCHAR*, const TCHAR*>> SpawnableKnownFields();

// The template root's spawnable fields: document key -> FMovieSceneSpawnable property.
TConstArrayView<TPair<const TCHAR*, const TCHAR*>> RootSpawnableFields();
// The component a template's component binding names ("" when it names none).
FString TemplateComponentName(const UMovieSceneSequence& Sequence, const FGuid& Guid);

// ---- template sequences (AssetDocumentSequencerTemplateRoot.cpp) ---------------------------------------------------

// The fields of a new root spawnable of this template (UMovieScene::AddSpawnable: AutoSetNetAddressableName): what an
// omitted Root field means.
FMovieSceneSpawnable RootSpawnableDefaults(const UObject* Template);
// The class of the component an actor of Template's class has under Name (native default subobjects and Blueprint
// components, as the spawned actor names them), or nullptr.
UClass* FindTemplateComponentClass(const AActor& Template, FName Name);
// Class can be the bound actor class of Sequence (an actor class that is not abstract; a camera actor class for a
// Camera Animation Sequence); else false with Sequencer.RootClassInvalid / Sequencer.CameraClassRequired.
bool CheckRootClass(const UMovieSceneSequence& Sequence, const UClass* Class, const FString& Path, FString& OutCode, FString& OutMessage);

// ---- director (AssetDocumentSequencerRead.cpp, AssetDocumentSequencerDirector.cpp) --------------------------------

// Inspect: {"Blueprint": path, "Endpoints": [{Name, Kind: Function | CustomEvent, Inputs: [{Name, Type}]}]} or null.
FValue DescribeDirector(const UMovieSceneSequence& Sequence);
// seq.read.v1 header: {"Blueprint": path, "Class": generated class path} or null (the Director region itself is read
// through the Blueprint providers, which resolve the sequence's handle to its director: ResolveHostedDirector).
FValue DescribeDirectorHeader(const UMovieSceneSequence& Sequence);
// The Director Blueprint of the Level Sequence that is or contains Inside (null: none).
UBlueprint* FindDirectorBlueprint(const UObject& Inside);
// RegisterAssetDocumentHostedBlueprint resolver of ULevelSequence.
UBlueprint* ResolveHostedDirector(UObject& Host);
// An event value {Endpoint?, BoundObjectPin?, Payload?} onto Event (Context.Owner: the event section): the endpoint
// picker's FMovieSceneEventUtils::SetEndpoint on the named function / custom event of the director, the payload as the
// endpoint's Details write it. {} clears the event.
bool DecodeEvent(const FValue& Value, FMovieSceneEvent& Event, FDecodeContext& Context);
// A director binding's FMovieSceneDynamicBinding as {Endpoint?, ResolveParamsPin?, Payload?} and back (Context.Owner:
// an object of the sequence; FMovieSceneDynamicBindingUtils::SetEndpoint, function endpoints only).
FValue EncodeDynamicBinding(const FMovieSceneDynamicBinding& Binding, FEncodeContext& Context);
bool DecodeDynamicBinding(const FValue& Value, FMovieSceneDynamicBinding& Binding, FDecodeContext& Context);
// Document paths of the event sections and bindings that call the director.
TArray<FString> DirectorUsers(const UMovieSceneSequence& Sequence);

// ---- groups and remote references (AssetDocumentSequencerGroups.cpp) ------------------------------------------------

// The node groups [{Name, Members: [{Folder} | {Binding} | {Track, Node?} | {Raw}]}] in collection order.
FValue ReadNodeGroups(const FSequenceModel& Model);
// A binding reference into a sub / shot sequence (Guid in the sequence that SequenceId names, relative to Model's
// sequence) as {"Binding": path in that sequence, "Sequence": section path | [section paths]}; false when no sub
// section chain (depth 4) reaches it.
bool DescribeRemoteBinding(const FSequenceModel& Model, const FMovieSceneSequenceID& SequenceId, const FGuid& Guid, FValue& Out);
// {"Binding", "Sequence"} -> the relative binding ID (FSequenceBindingTree's sequence ID of the section chain).
bool DecodeRemoteBinding(const FValue& Value, FMovieSceneObjectBindingID& Out, FDecodeContext& Context);

// ---- status (AssetDocumentSequencerStatus.cpp) ---------------------------------------------------------------------

// The read checks of design §5.1 as a compile status: {Status, Messages[{Severity, Rule?, Message, Path?, Source}]},
// Status one of UpToDate, UpToDateWithWarnings, Error. False for other assets. Never changes the asset.
bool DescribeStatusValue(const UObject& Asset, FValue& OutStatus);
// From reaches Target through sub / shot sections (at any depth; loaded sequences only).
bool SequenceReaches(const UMovieSceneSequence* From, const UMovieSceneSequence* Target);
TSharedPtr<FJsonObject> DescribeStatus(const UObject& Asset);

// Rollback rebuilder (AssetDocumentSequencerRebuilder.cpp): after Core restores a sequence package, the movie scene is
// marked changed (evaluation recompiles) and its precompiled data dropped, as Undo does.
TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeRebuilder();

// Open-editor apply (AssetDocumentSequencerOpenEditor.cpp): the open Sequencers that show an applied sequence (as their
// root or a sub-sequence) are refreshed as after their Undo.
void RegisterOpenEditorHandler();
void UnregisterOpenEditorHandler();

// ---- catalog (AssetDocumentSequencerCatalog.cpp) -------------------------------------------------------------------

bool ListTrackTypes(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError);
bool ListSectionTypes(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError);
bool ListBindingTypes(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError);

// ---- handlers (read: AssetDocumentSequencerRead.cpp; edit and status: AssetDocumentSequencerEdit*.cpp) ---------------

bool HandleRead(FCall& Call, FObjectBuilder& Result);
bool HandleEdit(FCall& Call, FObjectBuilder& Result);
bool HandleStatus(FCall& Call, FObjectBuilder& Result);
bool HandleInspect(FCall& Call, FObjectBuilder& Result);
bool HandleCatalog(FCall& Call, FObjectBuilder& Result);
} // namespace UE::AssetDocument::Graph::Sequencer
