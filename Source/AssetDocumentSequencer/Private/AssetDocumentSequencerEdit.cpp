// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// seq.edit.v1: edits of a Level Sequence as Sequencer's UI makes them (design §4.2), without an open Sequencer (an open
// sequence is refused, §5.4). Each call is one FScopedTransaction with Modify() on every touched object; the data
// calls are the ones the UI's actions make (UMovieScene, FMovieSceneBindingReferences, UMovieSceneTrack /
// UMovieSceneSection setters, the channel data interface with AutoSetTangents). Values use the document forms of design
// §3.3 (display frames, binding paths) through AssetDocumentSequencerDecode.cpp.
//
// Args {ops: [...]}, each {op, ...}; bindings by GUID (digits), tracks by object name, sections by object name:
//   settings {values}                        Settings fields listed in values (sparse): TickResolution first (the
//                                            editor's tick-resolution change: MigrateFrameTimes), then the rates, then
//                                            the ranges in the new rates, locks and flags; SelectionRange null = a
//                                            new sequence's, CustomClock null = nothing (it goes with Clock)
//   binding.add {guid, name, parent?}        a possessable with that GUID (no references yet)
//   binding.remove {guid}                    the tree's Delete: children first, tracks, references, folder entries
//   binding.rename {guid, name}              F2: FMovieScenePossessable::SetName + UMovieScene::SetObjectDisplayName
//   binding.set {guid, entry}                entry: {Class?, Bind | Spawnable | CustomBinding | References} (the kind
//                                            and its content, converting the kind when it changes) and/or {Tags}
//   binding.order {guids}                    SortingOrder of siblings in this order (the tree's drag reorder)
//   track.add {binding?, type, property?, name?, object?}
//   track.remove {track} | track.set {track, entry} | track.order {tracks}
//   section.add {track, entry} | section.set {track, section, entry} | section.remove {track, section}
//   channel.append {track, section, channel, keys, parameter?, stated?}  more keys of a dense channel (columnar chunk)
//   parameters.append {track, section, entries, parameter?}  more Parameters entries (of the section, or of the entry at
//                                            the parameter index path) when the list is too large for one op
//   folders.set {folders, bindings, tracks}  the whole folder tree: [{Path, Color?}], [{guid, folder}], [{track, folder}]
//   markers.set {markers}                    the whole marked-frame list
//   metadata.set {values}                    Author / Notes (the listed ones; "" clears)
//   template.root {class}                    a Template / Camera Animation Sequence's bound actor class: a new root
//                                            spawnable of that class with the old root's tracks and component bindings
//                                            (the Details panel's class picker; AssetDocumentSequencerTemplateRoot.cpp)
//   director.create {} | director.remove {}  the Level Sequence's Director Blueprint (Sequencer's "Open Director
//                                            Blueprint"; removal refused while events or director bindings call it)
//   section_groups.set {groups}              every section group: [[{track, section}, ...]] (Ungroup all, Group each)
//   groups.set {groups}                      the node groups [{Name, Members}] (AssetDocumentSequencerGroups.cpp)
// In a template sequence the root spawnable is a binding too (rename, order, binding.set {Root, Tags}) and its
// component bindings take binding.set {Class?, Component, Tags?}.
// Result {changed, created: [{op, object}] (tracks and sections the ops added, by op index), dependents: [{package,
// bindings}]} (packages that reference the sequence when bindings were removed:
// Sequencer.DependentsAffected, warn only, decision §8.8).

#include "AssetDocumentSequencerEdit.h"

#include "AssetDocumentEditorNonInteractiveScope.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Bindings/MovieSceneCustomBinding.h"
#include "Bindings/MovieSceneSpawnableActorBinding.h"
#include "Components/ActorComponent.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneBindingReferences.h"
#include "MovieSceneClock.h"
#include "MovieSceneFolder.h"
#include "MovieSceneMarkedFrame.h"
#include "MovieSceneMetaData.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSequence.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneTimeHelpers.h"
#include "MovieSceneTrack.h"
#include "ScopedTransaction.h"
#include "UniversalObjectLocator.h"
#include "UniversalObjectLocatorStringParams.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "AssetDocumentSequencerEdit"

namespace UE::AssetDocument::Graph::Sequencer::Edit
{
const FSequenceModel& FEdit::Model()
{
	if (!bModelValid)
	{
		CachedModel = FSequenceModel();
		FString Error;
		BuildModel(Sequence, CachedModel, Error);
		bModelValid = true;
	}
	return CachedModel;
}

void FEdit::NoteCreated(const UObject& Object)
{
	FObjectBuilder Row;
	Row.AddInt(TEXT("op"), CurrentOp);
	Row.AddString(TEXT("object"), Object.GetName());
	Created.Add(Row.Build());
}

bool ReadGuid(FReader& Op, const TCHAR* Key, FGuid& Out)
{
	FString Text;
	if (!Op.String(Key, Text))
	{
		return false;
	}
	if (!FGuid::Parse(Text, Out) || !Out.IsValid())
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("%s is a binding GUID (32 hex digits)."), Key), Key);
	}
	return true;
}

UMovieSceneTrack* FindTrackByName(UMovieScene& MovieScene, const FString& Name)
{
	if (UMovieSceneTrack* CameraCut = MovieScene.GetCameraCutTrack(); CameraCut != nullptr && CameraCut->GetName() == Name)
	{
		return CameraCut;
	}
	for (UMovieSceneTrack* Track : MovieScene.GetTracks())
	{
		if (Track != nullptr && Track->GetName() == Name)
		{
			return Track;
		}
	}
	for (const FMovieSceneBinding& Binding : static_cast<const UMovieScene&>(MovieScene).GetBindings())
	{
		for (UMovieSceneTrack* Track : Binding.GetTracks())
		{
			if (Track != nullptr && Track->GetName() == Name)
			{
				return Track;
			}
		}
	}
	return nullptr;
}

namespace SequencerEditLocal
{
bool IsNumber(const FValue& Value)
{
	return Value.Kind == EAssetDocumentRuntimeValueKind::Integer || Value.Kind == EAssetDocumentRuntimeValueKind::Number;
}

} // namespace

FTimeBase TimeOf(const UMovieScene& MovieScene)
{
	FTimeBase Time;
	Time.Tick = MovieScene.GetTickResolution();
	Time.Display = MovieScene.GetDisplayRate();
	return Time;
}

bool CheckFields(FReader& Reader, const FValue& Object, std::initializer_list<const TCHAR*> Allowed, const TCHAR* What)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return Reader.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("%s is an object."), What));
	}
	for (const FString& Key : Object.ObjectKeys)
	{
		bool bKnown = false;
		for (const TCHAR* Name : Allowed)
		{
			bKnown |= Key == Name;
		}
		if (!bKnown)
		{
			TArray<FString> Names;
			for (const TCHAR* Name : Allowed)
			{
				Names.Add(Name);
			}
			return Reader.Fail(TEXT("Graph.UnknownField"), FString::Printf(TEXT("%s takes %s; '%s' is not one of them."), What,
				*FString::Join(Names, TEXT(", ")), *Key), Escape(Key));
		}
	}
	return true;
}

namespace SequencerEditLocal
{
bool DecodeRate(FReader& Reader, const FValue& Value, const FString& Key, FFrameRate& Out)
{
	const FValue* Numerator = Value.Kind == EAssetDocumentRuntimeValueKind::Object ? FindField(Value, TEXT("Numerator")) : nullptr;
	const FValue* Denominator = Value.Kind == EAssetDocumentRuntimeValueKind::Object ? FindField(Value, TEXT("Denominator")) : nullptr;
	if (Numerator == nullptr || Denominator == nullptr || Numerator->Kind != EAssetDocumentRuntimeValueKind::Integer
		|| Denominator->Kind != EAssetDocumentRuntimeValueKind::Integer || Numerator->IntegerValue <= 0 || Denominator->IntegerValue <= 0
		|| Numerator->IntegerValue > MAX_int32 || Denominator->IntegerValue > MAX_int32)
	{
		return Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("A frame rate is {\"Numerator\": <positive integer>, \"Denominator\": <positive integer>}."),
			Key);
	}
	Out = FFrameRate(static_cast<int32>(Numerator->IntegerValue), static_cast<int32>(Denominator->IntegerValue));
	return true;
}

// {Start?, End?} display frames -> [Start, End) in ticks (an omitted bound is open).
bool DecodeRange(FEdit& Edit, FReader& Reader, const FValue& Value, const FString& Key, bool bClosed, TRange<FFrameNumber>& Out)
{
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Object || !CheckFields(Reader, Value, {TEXT("Start"), TEXT("End")}, TEXT("A range")))
	{
		return Value.Kind == EAssetDocumentRuntimeValueKind::Object ? false
			: Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("A range is {\"Start\": <frame>, \"End\": <frame>}."), Key);
	}
	FDecodeContext Context{Edit.Model(), TimeOf(Edit.MovieScene), &Edit.MovieScene, Reader, Key};
	const FValue* Start = FindField(Value, TEXT("Start"));
	const FValue* End = FindField(Value, TEXT("End"));
	if (bClosed && (Start == nullptr || End == nullptr))
	{
		return Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("The playback range has Start and End."), Key);
	}
	TRangeBound<FFrameNumber> Lower = TRangeBound<FFrameNumber>::Open();
	TRangeBound<FFrameNumber> Upper = TRangeBound<FFrameNumber>::Open();
	int32 StartTicks = 0;
	int32 EndTicks = 0;
	if (Start != nullptr)
	{
		Context.Path = Key + TEXT("/Start");
		if (!DecodeFrames(*Start, Context, StartTicks))
		{
			return false;
		}
		Lower = TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(StartTicks));
	}
	if (End != nullptr)
	{
		Context.Path = Key + TEXT("/End");
		if (!DecodeFrames(*End, Context, EndTicks))
		{
			return false;
		}
		Upper = TRangeBound<FFrameNumber>::Exclusive(FFrameNumber(EndTicks));
	}
	if (Start != nullptr && End != nullptr && EndTicks < StartTicks)
	{
		return Reader.Fail(TEXT("Sequencer.InvalidRange"), TEXT("End is before Start."), Key);
	}
	Out = TRange<FFrameNumber>(Lower, Upper);
	return true;
}

bool BoolField(FReader& Reader, const FValue& Value, const FString& Key, bool& Out)
{
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Boolean)
	{
		return Reader.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("%s is true or false."), *Key), Key);
	}
	Out = Value.BooleanValue;
	return true;
}

// ---- settings --------------------------------------------------------------------------------------------------------

bool OpSettings(FEdit& Edit, FReader& Op)
{
	const FValue* Values = nullptr;
	if (!Op.Object(TEXT("values"), Values))
	{
		return false;
	}
	FReader Reader = Op.Child(TEXT("values"), *Values);
	if (!CheckFields(Reader, *Values, {TEXT("DisplayRate"), TEXT("TickResolution"), TEXT("Evaluation"), TEXT("Clock"), TEXT("CustomClock"),
		TEXT("PlaybackRange"), TEXT("SelectionRange"), TEXT("PlaybackRangeLocked"), TEXT("ReadOnly"), TEXT("MarkersLocked"), TEXT("ShowMarkers")},
		TEXT("Settings")))
	{
		return false;
	}
	UMovieScene& MovieScene = Edit.MovieScene;
	MovieScene.Modify();
	Edit.bChanged = true;
	if (const FValue* Tick = FindField(*Values, TEXT("TickResolution")))
	{
		FFrameRate Rate;
		if (!DecodeRate(Reader, *Tick, TEXT("TickResolution"), Rate))
		{
			return false;
		}
		const FFrameRate Old = MovieScene.GetTickResolution();
		if (Rate != Old)
		{
			// The editor's "Advanced > Tick Resolution" (SSequencerTimePanel): every time migrates, nothing moves.
			UE::MovieScene::TimeHelpers::MigrateFrameTimes(Old, Rate, &MovieScene);
			MovieScene.SetTickResolutionDirectly(Rate);
			MovieScene.SetReadOnly(false);
		}
	}
	if (const FValue* Display = FindField(*Values, TEXT("DisplayRate")))
	{
		FFrameRate Rate;
		if (!DecodeRate(Reader, *Display, TEXT("DisplayRate"), Rate))
		{
			return false;
		}
		MovieScene.SetDisplayRate(Rate);
	}
	if (const FValue* Evaluation = FindField(*Values, TEXT("Evaluation")))
	{
		if (Evaluation->StringValue == TEXT("FrameLocked"))
		{
			MovieScene.SetEvaluationType(EMovieSceneEvaluationType::FrameLocked);
		}
		else if (Evaluation->StringValue == TEXT("WithSubFrames"))
		{
			MovieScene.SetEvaluationType(EMovieSceneEvaluationType::WithSubFrames);
		}
		else
		{
			return Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("Evaluation is WithSubFrames or FrameLocked."), TEXT("Evaluation"));
		}
	}
	if (const FValue* Clock = FindField(*Values, TEXT("Clock")))
	{
		const int64 Source = StaticEnum<EUpdateClockSource>()->GetValueByNameString(Clock->StringValue);
		if (Clock->Kind != EAssetDocumentRuntimeValueKind::String || Source == INDEX_NONE)
		{
			return Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("Clock is a clock source (Tick, Platform, Audio, RelativeTimecode, Timecode, "
				"PlayEveryFrame, Custom)."), TEXT("Clock"));
		}
		MovieScene.SetClockSource(static_cast<EUpdateClockSource>(Source));
	}
	// null (a listed Settings that leaves CustomClock out): the clock object goes with Clock, nothing to write.
	if (const FValue* Custom = FindField(*Values, TEXT("CustomClock")); Custom != nullptr && Custom->Kind != EAssetDocumentRuntimeValueKind::Null)
	{
		FReader ClockReader = Reader.Child(TEXT("CustomClock"), *Custom);
		const FValue* ClassPath = Custom->Kind == EAssetDocumentRuntimeValueKind::Object ? FindField(*Custom, TEXT("Class")) : nullptr;
		UClass* Class = ClassPath != nullptr ? LoadObject<UClass>(nullptr, *ClassPath->StringValue, nullptr, LOAD_NoWarn | LOAD_Quiet) : nullptr;
		if (!CheckFields(ClockReader, *Custom, {TEXT("Class"), TEXT("Properties")}, TEXT("CustomClock")))
		{
			return false;
		}
		if (Class == nullptr || !Class->IsChildOf(UMovieSceneClock::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
		{
			return ClockReader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("CustomClock.Class is a concrete movie scene clock class."), TEXT("Class"));
		}
		UMovieSceneClock* Instance = Cast<UMovieSceneClock>(MovieScene.GetCustomClock());
		if (Instance == nullptr || Instance->GetClass() != Class)
		{
			Instance = NewObject<UMovieSceneClock>(&MovieScene, Class, NAME_None, RF_Transactional);
			MovieScene.SetCustomClock(Instance);
		}
		Instance->Modify();
		const FValue* Properties = FindField(*Custom, TEXT("Properties"));
		const FValue Empty = MakeEmptyObject();
		FDecodeContext Context{Edit.Model(), TimeOf(MovieScene), Instance, ClockReader, TEXT("Properties")};
		if (!DecodeSparseObject(*Class, Properties != nullptr ? *Properties : Empty, Instance, Class->GetDefaultObject(),
			[](const FProperty& Property) { return Property.HasAnyPropertyFlags(CPF_Edit); }, Context))
		{
			return false;
		}
	}
	if (const FValue* Playback = FindField(*Values, TEXT("PlaybackRange")))
	{
		TRange<FFrameNumber> Range;
		if (!DecodeRange(Edit, Reader, *Playback, TEXT("PlaybackRange"), true, Range))
		{
			return false;
		}
		MovieScene.SetPlaybackRange(Range);
	}
	if (const FValue* Selection = FindField(*Values, TEXT("SelectionRange")); Selection != nullptr && Selection->Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		// null (a listed Settings that leaves SelectionRange out): a new sequence's selection range.
#if WITH_EDITORONLY_DATA
		MovieScene.SetSelectionRange(GetDefault<UMovieScene>()->GetSelectionRange());
#endif
	}
	else if (Selection != nullptr)
	{
		TRange<FFrameNumber> Range;
		if (!DecodeRange(Edit, Reader, *Selection, TEXT("SelectionRange"), false, Range))
		{
			return false;
		}
#if WITH_EDITORONLY_DATA
		MovieScene.SetSelectionRange(Range);
#endif
	}
	bool bFlag = false;
	if (const FValue* Locked = FindField(*Values, TEXT("PlaybackRangeLocked")))
	{
		if (!BoolField(Reader, *Locked, TEXT("PlaybackRangeLocked"), bFlag))
		{
			return false;
		}
		MovieScene.SetPlaybackRangeLocked(bFlag);
	}
	if (const FValue* ReadOnly = FindField(*Values, TEXT("ReadOnly")))
	{
		if (!BoolField(Reader, *ReadOnly, TEXT("ReadOnly"), Edit.bReadOnly))
		{
			return false;
		}
	}
	if (const FValue* MarkersLocked = FindField(*Values, TEXT("MarkersLocked")))
	{
		if (!BoolField(Reader, *MarkersLocked, TEXT("MarkersLocked"), bFlag))
		{
			return false;
		}
		MovieScene.SetMarkedFramesLocked(bFlag);
	}
	if (const FValue* Show = FindField(*Values, TEXT("ShowMarkers")))
	{
		if (!BoolField(Reader, *Show, TEXT("ShowMarkers"), bFlag))
		{
			return false;
		}
#if WITH_EDITORONLY_DATA
		MovieScene.SetGloballyShowMarkedFrames(bFlag);
#endif
	}
	Edit.Invalidate();
	return true;
}

// ---- bindings --------------------------------------------------------------------------------------------------------

FMovieScenePossessable* RequirePossessable(FEdit& Edit, FReader& Op, const FGuid& Guid)
{
	FMovieScenePossessable* Possessable = Edit.MovieScene.FindPossessable(Guid);
	if (Possessable == nullptr)
	{
		Op.Fail(TEXT("Sequencer.UnknownBinding"), FString::Printf(TEXT("The sequence has no binding %s."), *Guid.ToString(EGuidFormats::Digits)),
			TEXT("guid"));
	}
	return Possessable;
}

bool OpBindingAdd(FEdit& Edit, FReader& Op)
{
	FGuid Guid;
	FString Name;
	FString ParentText;
	if (!ReadGuid(Op, TEXT("guid"), Guid) || !Op.String(TEXT("name"), Name) || !Op.String(TEXT("parent"), ParentText, false))
	{
		return false;
	}
	if (Edit.MovieScene.FindPossessable(Guid) != nullptr || Edit.MovieScene.FindBinding(Guid) != nullptr)
	{
		return Op.Fail(TEXT("Sequencer.BindingExists"), TEXT("A binding with this GUID exists."), TEXT("guid"));
	}
	FGuid Parent;
	if (!ParentText.IsEmpty() && (!FGuid::Parse(ParentText, Parent)
		|| (Edit.MovieScene.FindPossessable(Parent) == nullptr && (!Edit.Model().bTemplate || Parent != Edit.Model().Root))))
	{
		return Op.Fail(TEXT("Sequencer.UnknownBinding"), TEXT("The parent binding does not exist."), TEXT("parent"));
	}
	if (Edit.Model().bTemplate && !Parent.IsValid())
	{
		return Op.Fail(TEXT("Sequencer.InvalidBody"), TEXT("A template sequence's bindings are component bindings below its root (Body.Root.Children)."),
			TEXT("parent"));
	}
	Edit.MovieScene.Modify();
	// UMovieScene::AddPossessable(Name, Class), then the document's GUID (what external content references) through
	// ReplacePossessable, which moves the possessable and its object binding to that GUID.
	const FGuid Added = Edit.MovieScene.AddPossessable(Name, nullptr);
	FMovieScenePossessable Possessable(Name, nullptr);
	Possessable.SetGuid(Guid);
	Edit.MovieScene.ReplacePossessable(Added, Possessable);
	if (Parent.IsValid())
	{
		Edit.MovieScene.FindPossessable(Guid)->SetParent(Parent, &Edit.MovieScene);
		// A component binding of a template's root spawnable (UTemplateSequence::FindOrAddBinding).
		if (FMovieSceneSpawnable* Spawnable = Edit.MovieScene.FindSpawnable(Parent))
		{
			Spawnable->AddChildPossessable(Guid);
		}
	}
	Edit.bChanged = true;
	Edit.Invalidate();
	return true;
}

void RemoveBindingTree(FEdit& Edit, const FGuid& Guid, const FString& Label)
{
	// Children first (the tree's Delete removes a binding with its component bindings).
	TArray<FGuid> Children;
	for (int32 Index = 0; Index < Edit.MovieScene.GetPossessableCount(); ++Index)
	{
		const FMovieScenePossessable& Possessable = Edit.MovieScene.GetPossessable(Index);
		if (Possessable.GetParent() == Guid && Possessable.GetGuid() != Guid)
		{
			Children.Add(Possessable.GetGuid());
		}
	}
	for (const FGuid& Child : Children)
	{
		RemoveBindingTree(Edit, Child, Label);
	}
#if WITH_EDITORONLY_DATA
	TArray<UMovieSceneFolder*> Folders;
	GetMovieSceneFoldersRecursive(Edit.MovieScene.GetRootFolders(), Folders);
	for (UMovieSceneFolder* Folder : Folders)
	{
		if (Folder != nullptr && Folder->GetChildObjectBindings().Contains(Guid))
		{
			Folder->Modify();
			Folder->RemoveChildObjectBinding(Guid);
		}
	}
#endif
	Edit.MovieScene.SetObjectDisplayName(Guid, FText::GetEmpty());
	const FMovieScenePossessable* Possessable = Edit.MovieScene.FindPossessable(Guid);
	if (FMovieSceneSpawnable* Spawnable = Possessable != nullptr ? Edit.MovieScene.FindSpawnable(Possessable->GetParent()) : nullptr)
	{
		Spawnable->RemoveChildPossessable(Guid);
	}
	Edit.MovieScene.RemovePossessable(Guid);
	Edit.Sequence.UnbindPossessableObjects(Guid);
}

bool OpBindingRemove(FEdit& Edit, FReader& Op)
{
	FGuid Guid;
	if (!ReadGuid(Op, TEXT("guid"), Guid) || RequirePossessable(Edit, Op, Guid) == nullptr)
	{
		return false;
	}
	const FBindingInfo* Info = Edit.Model().FindBinding(Guid);
	const FString Label = Info != nullptr ? Info->Path : Guid.ToString(EGuidFormats::Digits);
	Edit.Sequence.Modify();
	Edit.MovieScene.Modify();
	RemoveBindingTree(Edit, Guid, Label);
	Edit.RemovedBindings.Add(Label);
	Edit.bChanged = true;
	Edit.Invalidate();
	return true;
}

bool OpBindingRename(FEdit& Edit, FReader& Op)
{
	FGuid Guid;
	FString Name;
	if (!ReadGuid(Op, TEXT("guid"), Guid) || !Op.String(TEXT("name"), Name))
	{
		return false;
	}
	// A template's root spawnable is renamed as Sequencer renames a spawnable (FMovieSceneSpawnable::SetName).
	FMovieSceneSpawnable* Spawnable = Edit.Model().bTemplate && Guid == Edit.Model().Root ? Edit.MovieScene.FindSpawnable(Guid) : nullptr;
	FMovieScenePossessable* Possessable = Spawnable == nullptr ? RequirePossessable(Edit, Op, Guid) : nullptr;
	if (Possessable == nullptr && Spawnable == nullptr)
	{
		return false;
	}
	Edit.MovieScene.Modify();
	if (Spawnable != nullptr)
	{
		Spawnable->SetName(Name);
	}
	else
	{
		Possessable->SetName(Name);
	}
	Edit.MovieScene.SetObjectDisplayName(Guid, FText::FromString(Name));
	Edit.bChanged = true;
	Edit.Invalidate();
	return true;
}

FString LocatorText(const FUniversalObjectLocator& Locator)
{
	TStringBuilder<512> Text;
	Locator.ToString(Text);
	return Text.ToString();
}

bool ParseLocator(FReader& Reader, const FValue& Value, const FString& Key, FUniversalObjectLocator& Out)
{
	if (Value.Kind != EAssetDocumentRuntimeValueKind::String)
	{
		return Reader.Fail(TEXT("Sequencer.InvalidLocator"), TEXT("A locator is a string (uobj://...)."), Key);
	}
	const UE::UniversalObjectLocator::FParseStringParams Params;
	if (!Out.TryParseString(Value.StringValue, Params).bSuccess)
	{
		return Reader.Fail(TEXT("Sequencer.InvalidLocator"), FString::Printf(TEXT("'%s' is not a valid object locator (FUniversalObjectLocator string "
			"form, e.g. uobj://actor?payload0=/Game/Maps/Lobby.Lobby:PersistentLevel.SM_Door_3; the level tool set returns them)."),
			*Value.StringValue), Key);
	}
	return true;
}

} // namespace

void SetPossessedClass(FMovieScenePossessable& Possessable, const FString& Path)
{
#if WITH_EDITORONLY_DATA
	static const FSoftClassProperty* Property = FindFProperty<FSoftClassProperty>(FMovieScenePossessable::StaticStruct(), TEXT("PossessedObjectClass"));
	if (Property != nullptr)
	{
		Property->SetPropertyValue_InContainer(&Possessable, FSoftObjectPtr(FSoftObjectPath(Path)));
	}
#endif
}

namespace SequencerEditLocal
{
bool WriteBind(FEdit& Edit, FReader& Entry, const FValue& Bind, const FGuid& Guid)
{
	if (Bind.Kind != EAssetDocumentRuntimeValueKind::Array)
	{
		return Entry.Fail(TEXT("Sequencer.InvalidValue"), TEXT("Bind is an array of locator strings."), TEXT("Bind"));
	}
	ConvertToPossessable(Edit, Guid);
	FMovieSceneBindingReferences* References = Edit.Sequence.GetBindingReferences();
	if (References == nullptr)
	{
		return Bind.Elements.IsEmpty() || Entry.Fail(TEXT("Sequencer.UnsupportedBindingShape"), TEXT("This sequence has no binding references."),
			TEXT("Bind"));
	}
	TArray<FUniversalObjectLocator> Wanted;
	for (int32 Index = 0; Index < Bind.Elements.Num(); ++Index)
	{
		FUniversalObjectLocator Locator;
		if (!ParseLocator(Entry, Bind.Elements[Index], FString::Printf(TEXT("Bind/%d"), Index), Locator))
		{
			return false;
		}
		Wanted.Add(MoveTemp(Locator));
	}
	const TArrayView<const FMovieSceneBindingReference> Current = References->GetReferences(Guid);
	bool bSame = Current.Num() == Wanted.Num();
	for (int32 Index = 0; bSame && Index < Wanted.Num(); ++Index)
	{
		bSame = Current[Index].CustomBinding == nullptr && Current[Index].Locator == Wanted[Index];
	}
	if (bSame)
	{
		return true;
	}
	// Assign / Add / Remove selected actors: the references in binding-index order.
	Edit.Sequence.Modify();
	References->RemoveBinding(Guid);
	for (FUniversalObjectLocator& Locator : Wanted)
	{
		References->AddBinding(Guid, MoveTemp(Locator));
	}
	Edit.bChanged = true;
	return true;
}

// The spawnable's template (design §3.4.3): actor fields sparse against the class defaults, components against their
// archetypes; then each template object's PostEditChange (what Details editing does; derived fields such as a cine
// camera's field of view are recomputed as on load).
} // namespace

bool WriteTemplate(FEdit& Edit, FReader& Reader, AActor& Template, const FValue* Saved)
{
	const FValue Empty = MakeEmptyObject();
	const FValue& Object = Saved != nullptr ? *Saved : Empty;
	if (!CheckFields(Reader, Object, {TEXT("Properties"), TEXT("Components")}, TEXT("Template")))
	{
		return false;
	}
	Template.Modify();
	// Omitted fields take a fresh actor's values (what the read side compares with).
	const FTemplateBaseline Baseline(*Template.GetClass());
	const FValue* Properties = FindField(Object, TEXT("Properties"));
	FDecodeContext Context{Edit.Model(), TimeOf(Edit.MovieScene), &Template, Reader, TEXT("Properties")};
	const UClass& ActorClass = *Template.GetClass();
	if (!DecodeSparseObject(ActorClass, Properties != nullptr ? *Properties : Empty, &Template, Baseline.Actor(),
		[&ActorClass](const FProperty& Property) { return IsTemplateField(ActorClass, Property); }, Context))
	{
		return false;
	}
	const FValue* Components = FindField(Object, TEXT("Components"));
	if (Components != nullptr && Components->Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("Components is an object of component name -> fields."), TEXT("Components"));
	}
	TArray<UActorComponent*> Owned;
	Template.GetComponents(Owned);
	if (Components != nullptr)
	{
		for (const FString& Name : Components->ObjectKeys)
		{
			if (!Owned.ContainsByPredicate([&Name](const UActorComponent* Component) { return Component != nullptr && Component->GetName() == Name; }))
			{
				TArray<FString> Names;
				for (const UActorComponent* Component : Owned)
				{
					if (Component != nullptr)
					{
						Names.Add(Component->GetName());
					}
				}
				return Reader.Fail(TEXT("Sequencer.UnknownComponent"), FString::Printf(TEXT("A %s template has no component '%s'. Components: %s."),
					*Template.GetClass()->GetName(), *Name, *FString::Join(Names, TEXT(", "))), TEXT("Components/") + Escape(Name));
			}
		}
	}
	for (UActorComponent* Component : Owned)
	{
		const UObject* Defaults = Component != nullptr && Component->GetArchetype() != nullptr ? Baseline.Component(*Component) : nullptr;
		if (Defaults == nullptr)
		{
			continue;
		}
		const FValue* Fields = Components != nullptr ? FindField(*Components, Component->GetName()) : nullptr;
		Component->Modify();
		FDecodeContext ComponentContext{Edit.Model(), TimeOf(Edit.MovieScene), Component, Reader, TEXT("Components/") + Escape(Component->GetName())};
		const UClass& ComponentClass = *Component->GetClass();
		if (!DecodeSparseObject(ComponentClass, Fields != nullptr ? *Fields : Empty, Component, Defaults,
			[&ComponentClass](const FProperty& Property) { return IsTemplateField(ComponentClass, Property); }, ComponentContext))
		{
			return false;
		}
	}
	for (UActorComponent* Component : Owned)
	{
		if (Component != nullptr)
		{
			Component->PostEditChange();
		}
	}
	Template.PostEditChange();
	return true;
}

namespace SequencerEditLocal
{
bool WriteSpawnable(FEdit& Edit, FReader& Entry, const FValue& Spawnable, const FGuid& Guid)
{
	FReader Reader = Entry.Child(TEXT("Spawnable"), Spawnable);
	if (!CheckFields(Reader, Spawnable, {TEXT("Class"), TEXT("Template"), TEXT("Ownership"), TEXT("ContinuouslyRespawn"), TEXT("NetAddressableName"),
		TEXT("LevelName"), TEXT("Properties")}, TEXT("Spawnable")))
	{
		return false;
	}
	const FValue* ClassPath = FindField(Spawnable, TEXT("Class"));
	if (ClassPath == nullptr || ClassPath->Kind != EAssetDocumentRuntimeValueKind::String)
	{
		return Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("A spawnable has Class (the actor class it spawns)."), TEXT("Class"));
	}
	UClass* Class = LoadObject<UClass>(nullptr, *ClassPath->StringValue, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (Class == nullptr)
	{
		return Reader.Fail(TEXT("Sequencer.InvalidSpawnableClass"), FString::Printf(TEXT("Class '%s' does not load."), *ClassPath->StringValue),
			TEXT("Class"));
	}
	UMovieSceneCustomBinding* Custom = ConvertToCustomBinding(Edit, Guid, *UMovieSceneSpawnableActorBinding::StaticClass(), Class, Reader, TEXT("Class"));
	UMovieSceneSpawnableActorBinding* Binding = Cast<UMovieSceneSpawnableActorBinding>(Custom);
	if (Binding == nullptr)
	{
		return false;
	}
	AActor* Template = Cast<AActor>(Binding->GetObjectTemplate());
	if (Template == nullptr)
	{
		return Reader.Fail(TEXT("Sequencer.InvalidSpawnableClass"), TEXT("The spawnable has no actor template."), TEXT("Class"));
	}
	FReader TemplateReader = Reader.Child(TEXT("Template"), FindField(Spawnable, TEXT("Template")) != nullptr ? *FindField(Spawnable, TEXT("Template"))
		: Spawnable);
	if (!WriteTemplate(Edit, TemplateReader, *Template, FindField(Spawnable, TEXT("Template"))))
	{
		return false;
	}
	// The binding's own fields (Ownership, ContinuouslyRespawn, NetAddressableName, LevelName and the others in
	// Properties), complete against the binding class defaults.
	FObjectBuilder Fields;
	for (const TPair<const TCHAR*, const TCHAR*>& Known : SpawnableKnownFields())
	{
		if (const FValue* Value = FindField(Spawnable, Known.Value))
		{
			Fields.Add(Known.Key, *Value);
		}
	}
	if (const FValue* Others = FindField(Spawnable, TEXT("Properties")))
	{
		for (int32 Index = 0; Others->Kind == EAssetDocumentRuntimeValueKind::Object && Index < Others->ObjectKeys.Num(); ++Index)
		{
			Fields.Add(Others->ObjectKeys[Index], Others->Elements[Index]);
		}
	}
	Binding->Modify();
	const FValue FieldValue = Fields.Build();
	FDecodeContext Context{Edit.Model(), TimeOf(Edit.MovieScene), Binding, Reader, FString()};
	if (!DecodeSparseObject(*Binding->GetClass(), FieldValue, Binding, Binding->GetClass()->GetDefaultObject(),
		[](const FProperty& Property) { return Property.GetFName() != TEXT("ActorTemplate") && !IsComponentReference(Property); }, Context))
	{
		return false;
	}
	if (FMovieScenePossessable* Possessable = Edit.MovieScene.FindPossessable(Guid))
	{
		Possessable->SetPossessedObjectClass(Class);
	}
	Edit.bChanged = true;
	return true;
}

bool WriteCustom(FEdit& Edit, FReader& Entry, const FValue& Custom, const FGuid& Guid)
{
	FReader Reader = Entry.Child(TEXT("CustomBinding"), Custom);
	if (!CheckFields(Reader, Custom, {TEXT("Class"), TEXT("Properties")}, TEXT("CustomBinding")))
	{
		return false;
	}
	const FValue* ClassPath = FindField(Custom, TEXT("Class"));
	UClass* Class = ClassPath != nullptr ? LoadObject<UClass>(nullptr, *ClassPath->StringValue, nullptr, LOAD_NoWarn | LOAD_Quiet) : nullptr;
	if (Class == nullptr)
	{
		return Reader.Fail(TEXT("Sequencer.InvalidBindingClass"), TEXT("CustomBinding.Class is a custom binding class path."), TEXT("Class"));
	}
	UMovieSceneCustomBinding* Binding = ConvertToCustomBinding(Edit, Guid, *Class, nullptr, Reader, TEXT("Class"));
	if (Binding == nullptr)
	{
		return false;
	}
	Binding->Modify();
	const FValue Empty = MakeEmptyObject();
	const FValue* Properties = FindField(Custom, TEXT("Properties"));
	FDecodeContext Context{Edit.Model(), TimeOf(Edit.MovieScene), Binding, Reader, TEXT("Properties")};
	if (!DecodeSparseObject(*Class, Properties != nullptr ? *Properties : Empty, Binding, Class->GetDefaultObject(),
		[](const FProperty& Property) { return !IsComponentReference(Property); }, Context))
	{
		return false;
	}
	Binding->PostEditChange();
	if (FMovieScenePossessable* Possessable = Edit.MovieScene.FindPossessable(Guid))
	{
		Possessable->SetPossessedObjectClass(Binding->GetBoundObjectClass());
	}
	Edit.bChanged = true;
	return true;
}

// Mixed references (locators and custom bindings on one binding, Sequencer.UnsupportedBindingShape): rebuilt row by row.
bool WriteReferences(FEdit& Edit, FReader& Entry, const FValue& Rows, const FGuid& Guid)
{
	FMovieSceneBindingReferences* References = Edit.Sequence.GetBindingReferences();
	if (Rows.Kind != EAssetDocumentRuntimeValueKind::Array || References == nullptr)
	{
		return Entry.Fail(TEXT("Sequencer.InvalidValue"), TEXT("References is an array of {Locator} / {CustomBinding} rows."), TEXT("References"));
	}
	Edit.Sequence.Modify();
	Edit.MovieScene.Modify();
	References->RemoveBinding(Guid);
	for (int32 Index = 0; Index < Rows.Elements.Num(); ++Index)
	{
		const FValue& Row = Rows.Elements[Index];
		const FString Key = FString::Printf(TEXT("References/%d"), Index);
		if (const FValue* Locator = Row.Kind == EAssetDocumentRuntimeValueKind::Object ? FindField(Row, TEXT("Locator")) : nullptr)
		{
			FUniversalObjectLocator Parsed;
			if (!ParseLocator(Entry, *Locator, Key + TEXT("/Locator"), Parsed))
			{
				return false;
			}
			References->AddBinding(Guid, MoveTemp(Parsed));
			continue;
		}
		const FValue* Custom = Row.Kind == EAssetDocumentRuntimeValueKind::Object ? FindField(Row, TEXT("CustomBinding")) : nullptr;
		const FValue* ClassPath = Custom != nullptr && Custom->Kind == EAssetDocumentRuntimeValueKind::Object ? FindField(*Custom, TEXT("Class")) : nullptr;
		UClass* Class = ClassPath != nullptr ? LoadObject<UClass>(nullptr, *ClassPath->StringValue, nullptr, LOAD_NoWarn | LOAD_Quiet) : nullptr;
		if (Class == nullptr || !Class->IsChildOf(UMovieSceneCustomBinding::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
		{
			return Entry.Fail(TEXT("Sequencer.InvalidBindingClass"), TEXT("A reference row is {Locator} or {CustomBinding: {Class, Properties}}."), Key);
		}
		UMovieSceneCustomBinding* Binding = NewObject<UMovieSceneCustomBinding>(&Edit.MovieScene, Class,
			MakeUniqueObjectName(&Edit.MovieScene, Class, *(Class->GetName() + TEXT("_CustomBinding"))), RF_Transactional);
		const FValue Empty = MakeEmptyObject();
		const FValue* Properties = FindField(*Custom, TEXT("Properties"));
		FReader RowReader = Entry.Child(Key + TEXT("/CustomBinding"), *Custom);
		FDecodeContext Context{Edit.Model(), TimeOf(Edit.MovieScene), Binding, RowReader, TEXT("Properties")};
		if (!DecodeSparseObject(*Class, Properties != nullptr ? *Properties : Empty, Binding, Class->GetDefaultObject(),
			[](const FProperty& Property) { return !IsComponentReference(Property); }, Context))
		{
			return false;
		}
		References->AddBinding(Guid, FUniversalObjectLocator(), ELocatorResolveFlags::None, Binding);
	}
	Edit.bChanged = true;
	Edit.Invalidate();
	return true;
}

} // namespace

bool ReadTags(FReader& Entry, const FValue& Tags, TArray<FName>& Out)
{
	if (Tags.Kind != EAssetDocumentRuntimeValueKind::Array)
	{
		return Entry.Fail(TEXT("Sequencer.InvalidValue"), TEXT("Tags is an array of names."), TEXT("Tags"));
	}
	TArray<FName> Names;
	for (const FValue& Tag : Tags.Elements)
	{
		Names.Add(FName(*Tag.StringValue));
	}
	Out = MoveTemp(Names);
	return true;
}

namespace SequencerEditLocal
{
bool OpBindingSet(FEdit& Edit, FReader& Op)
{
	FGuid Guid;
	const FValue* EntryValue = nullptr;
	if (!ReadGuid(Op, TEXT("guid"), Guid) || !Op.Object(TEXT("entry"), EntryValue))
	{
		return false;
	}
	FReader Entry = Op.Child(TEXT("entry"), *EntryValue);
	if (Edit.Model().bTemplate)
	{
		return SetTemplateBinding(Edit, Op, Entry, *EntryValue, Guid);
	}
	if (!CheckFields(Entry, *EntryValue, {TEXT("Class"), TEXT("Bind"), TEXT("Spawnable"), TEXT("CustomBinding"), TEXT("References"), TEXT("Tags")},
		TEXT("binding.set entry")))
	{
		return false;
	}
	if (RequirePossessable(Edit, Op, Guid) == nullptr)
	{
		return false;
	}
	int32 Kinds = 0;
	for (const TCHAR* Key : {TEXT("Bind"), TEXT("Spawnable"), TEXT("CustomBinding"), TEXT("References")})
	{
		Kinds += FindField(*EntryValue, Key) != nullptr ? 1 : 0;
	}
	if (Kinds > 1)
	{
		return Entry.Fail(TEXT("Sequencer.InvalidBody"), TEXT("A binding is one of Bind, Spawnable or CustomBinding."));
	}
	Edit.MovieScene.Modify();
	if (const FValue* Bind = FindField(*EntryValue, TEXT("Bind")))
	{
		if (!WriteBind(Edit, Entry, *Bind, Guid))
		{
			return false;
		}
		const FValue* Class = FindField(*EntryValue, TEXT("Class"));
		SetPossessedClass(*Edit.MovieScene.FindPossessable(Guid), Class != nullptr ? Class->StringValue : FString());
	}
	else if (const FValue* Spawnable = FindField(*EntryValue, TEXT("Spawnable")))
	{
		if (!WriteSpawnable(Edit, Entry, *Spawnable, Guid))
		{
			return false;
		}
	}
	else if (const FValue* Custom = FindField(*EntryValue, TEXT("CustomBinding")))
	{
		if (!WriteCustom(Edit, Entry, *Custom, Guid))
		{
			return false;
		}
	}
	else if (const FValue* Rows = FindField(*EntryValue, TEXT("References")))
	{
		if (!WriteReferences(Edit, Entry, *Rows, Guid))
		{
			return false;
		}
	}
	else if (const FValue* Class = FindField(*EntryValue, TEXT("Class")))
	{
		SetPossessedClass(*Edit.MovieScene.FindPossessable(Guid), Class->StringValue);
	}
	// The entry is complete: no Tags = none.
	const FValue NoTags = MakeArray(TArray<FValue>());
	const FValue* Tags = FindField(*EntryValue, TEXT("Tags"));
	if (!ReadTags(Entry, Tags != nullptr ? *Tags : NoTags, Edit.MovieScene.FindPossessable(Guid)->Tags))
	{
		return false;
	}
	Edit.bChanged = true;
	Edit.Invalidate();
	return true;
}

bool OpBindingOrder(FEdit& Edit, FReader& Op)
{
	const FValue* Guids = nullptr;
	if (!Op.Array(TEXT("guids"), Guids))
	{
		return false;
	}
	Edit.MovieScene.Modify();
	for (int32 Index = 0; Index < Guids->Elements.Num(); ++Index)
	{
		FGuid Guid;
		FMovieSceneBinding* Binding = FGuid::Parse(Guids->Elements[Index].StringValue, Guid) ? Edit.MovieScene.FindBinding(Guid) : nullptr;
		if (Binding == nullptr)
		{
			return Op.Fail(TEXT("Sequencer.UnknownBinding"), TEXT("The sequence has no such binding."), FString::Printf(TEXT("guids/%d"), Index));
		}
#if WITH_EDITORONLY_DATA
		Binding->SetSortingOrder(Index);
#endif
	}
	Edit.bChanged = true;
	Edit.Invalidate();
	return true;
}

// ---- folders, markers, metadata ----------------------------------------------------------------------------------------

#if WITH_EDITORONLY_DATA
void CollectFolders(TArrayView<UMovieSceneFolder* const> Folders, const FString& Prefix, TMap<FString, UMovieSceneFolder*>& Out)
{
	for (UMovieSceneFolder* Folder : Folders)
	{
		if (Folder == nullptr)
		{
			continue;
		}
		const FString Path = Prefix.IsEmpty() ? Folder->GetFolderName().ToString() : Prefix + TEXT("/") + Folder->GetFolderName().ToString();
		Out.FindOrAdd(Path, Folder);
		CollectFolders(Folder->GetChildFolders(), Path, Out);
	}
}
#endif

bool OpFoldersSet(FEdit& Edit, FReader& Op)
{
	const FValue* Folders = nullptr;
	const FValue* Bindings = nullptr;
	const FValue* Tracks = nullptr;
	if (!Op.Array(TEXT("folders"), Folders) || !Op.Array(TEXT("bindings"), Bindings) || !Op.Array(TEXT("tracks"), Tracks))
	{
		return false;
	}
#if WITH_EDITORONLY_DATA
	UMovieScene& MovieScene = Edit.MovieScene;
	MovieScene.Modify();
	TMap<FString, UMovieSceneFolder*> Existing;
	CollectFolders(MovieScene.GetRootFolders(), FString(), Existing);
	TMap<FString, UMovieSceneFolder*> Wanted;
	TMap<FString, TArray<UMovieSceneFolder*>> Children; // parent path ("" = root) -> folders in document order
	const FColor DefaultColor = GetDefault<UMovieSceneFolder>()->GetFolderColor();
	for (int32 Index = 0; Index < Folders->Elements.Num(); ++Index)
	{
		const FValue& Row = Folders->Elements[Index];
		FReader RowReader = Op.Child(FString::Printf(TEXT("folders/%d"), Index), Row);
		if (!CheckFields(RowReader, Row, {TEXT("Path"), TEXT("Color")}, TEXT("A folder")))
		{
			return false;
		}
		const FValue* PathValue = FindField(Row, TEXT("Path"));
		const FString Path = PathValue != nullptr ? PathValue->StringValue : FString();
		if (Path.IsEmpty() || Path.StartsWith(TEXT("/")) || Path.EndsWith(TEXT("/")) || Path.Contains(TEXT("//")) || Wanted.Contains(Path))
		{
			return RowReader.Fail(TEXT("Sequencer.InvalidFolder"), FString::Printf(TEXT("'%s' is not a new folder path (names joined by '/', each folder "
				"listed once)."), *Path), TEXT("Path"));
		}
		FString Parent;
		FString Leaf = Path;
		int32 Slash = INDEX_NONE;
		if (Path.FindLastChar(TEXT('/'), Slash))
		{
			Parent = Path.Left(Slash);
			Leaf = Path.Mid(Slash + 1);
			if (!Wanted.Contains(Parent))
			{
				return RowReader.Fail(TEXT("Sequencer.InvalidFolder"), FString::Printf(TEXT("The parent folder '%s' is listed before its children."),
					*Parent), TEXT("Path"));
			}
		}
		UMovieSceneFolder* Folder = Existing.FindRef(Path);
		if (Folder == nullptr)
		{
			Folder = NewObject<UMovieSceneFolder>(&MovieScene, NAME_None, RF_Transactional);
			Folder->SetFolderName(FName(*Leaf));
		}
		Folder->Modify();
		FColor Color = DefaultColor;
		if (const FValue* ColorValue = FindField(Row, TEXT("Color")))
		{
			const FValue* R = FindField(*ColorValue, TEXT("R"));
			const FValue* G = FindField(*ColorValue, TEXT("G"));
			const FValue* B = FindField(*ColorValue, TEXT("B"));
			const FValue* A = FindField(*ColorValue, TEXT("A"));
			if (R == nullptr || G == nullptr || B == nullptr || !IsNumber(*R) || !IsNumber(*G) || !IsNumber(*B))
			{
				return RowReader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("A folder Color is {R, G, B, A?} (0-255)."), TEXT("Color"));
			}
			Color = FColor(static_cast<uint8>(FMath::Clamp<int64>(R->IntegerValue, 0, 255)), static_cast<uint8>(FMath::Clamp<int64>(G->IntegerValue, 0, 255)),
				static_cast<uint8>(FMath::Clamp<int64>(B->IntegerValue, 0, 255)),
				static_cast<uint8>(A != nullptr ? FMath::Clamp<int64>(A->IntegerValue, 0, 255) : 255));
		}
		Folder->SetFolderColor(Color);
		Wanted.Add(Path, Folder);
		Children.FindOrAdd(Parent).Add(Folder);
	}
	// The tree: each folder's child folders and the root list, in document order (sorting order = position).
	for (const TPair<FString, UMovieSceneFolder*>& Entry : Existing)
	{
		Entry.Value->Modify();
		Entry.Value->ClearChildTracks();
		Entry.Value->ClearChildObjectBindings();
		for (UMovieSceneFolder* Child : TArray<UMovieSceneFolder*>(Entry.Value->GetChildFolders()))
		{
			Entry.Value->RemoveChildFolder(Child);
		}
	}
	MovieScene.EmptyRootFolders();
	for (const TPair<FString, TArray<UMovieSceneFolder*>>& Entry : Children)
	{
		for (int32 Index = 0; Index < Entry.Value.Num(); ++Index)
		{
			UMovieSceneFolder* Folder = Entry.Value[Index];
			Folder->SetSortingOrder(Index);
			if (Entry.Key.IsEmpty())
			{
				MovieScene.AddRootFolder(Folder);
			}
			else
			{
				Wanted[Entry.Key]->AddChildFolder(Folder);
			}
		}
	}
	for (int32 Index = 0; Index < Bindings->Elements.Num(); ++Index)
	{
		const FValue& Row = Bindings->Elements[Index];
		const FValue* GuidValue = FindField(Row, TEXT("guid"));
		const FValue* FolderValue = FindField(Row, TEXT("folder"));
		FGuid Guid;
		UMovieSceneFolder* Folder = FolderValue != nullptr ? Wanted.FindRef(FolderValue->StringValue) : nullptr;
		if (GuidValue == nullptr || !FGuid::Parse(GuidValue->StringValue, Guid) || Folder == nullptr)
		{
			return Op.Fail(TEXT("Sequencer.InvalidFolder"), FString::Printf(TEXT("Folder '%s' is not listed in Folders."),
				FolderValue != nullptr ? *FolderValue->StringValue : TEXT("")), FString::Printf(TEXT("bindings/%d"), Index));
		}
		Folder->AddChildObjectBinding(Guid);
	}
	for (int32 Index = 0; Index < Tracks->Elements.Num(); ++Index)
	{
		const FValue& Row = Tracks->Elements[Index];
		const FValue* TrackValue = FindField(Row, TEXT("track"));
		const FValue* FolderValue = FindField(Row, TEXT("folder"));
		UMovieSceneTrack* Track = TrackValue != nullptr ? FindTrackByName(MovieScene, TrackValue->StringValue) : nullptr;
		UMovieSceneFolder* Folder = FolderValue != nullptr ? Wanted.FindRef(FolderValue->StringValue) : nullptr;
		if (Track == nullptr || Folder == nullptr)
		{
			return Op.Fail(TEXT("Sequencer.InvalidFolder"), FString::Printf(TEXT("Folder '%s' is not listed in Folders."),
				FolderValue != nullptr ? *FolderValue->StringValue : TEXT("")), FString::Printf(TEXT("tracks/%d"), Index));
		}
		Folder->AddChildTrack(Track);
	}
#endif
	Edit.bChanged = true;
	Edit.Invalidate();
	return true;
}

bool OpMarkersSet(FEdit& Edit, FReader& Op)
{
	const FValue* Markers = nullptr;
	if (!Op.Array(TEXT("markers"), Markers))
	{
		return false;
	}
	TArray<FMovieSceneMarkedFrame> Frames;
	for (int32 Index = 0; Index < Markers->Elements.Num(); ++Index)
	{
		const FValue& Row = Markers->Elements[Index];
		FReader RowReader = Op.Child(FString::Printf(TEXT("markers/%d"), Index), Row);
		if (!CheckFields(RowReader, Row, {TEXT("Frame"), TEXT("Label"), TEXT("Comment"), TEXT("Color"), TEXT("DeterminismFence"), TEXT("Inclusive")},
			TEXT("A marker")))
		{
			return false;
		}
		const FValue* Frame = FindField(Row, TEXT("Frame"));
		if (Frame == nullptr)
		{
			return RowReader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("A marker has Frame."));
		}
		FDecodeContext Context{Edit.Model(), TimeOf(Edit.MovieScene), &Edit.MovieScene, RowReader, TEXT("Frame")};
		int32 Ticks = 0;
		if (!DecodeFrames(*Frame, Context, Ticks))
		{
			return false;
		}
		FMovieSceneMarkedFrame Mark;
		Mark.FrameNumber = FFrameNumber(Ticks);
		if (const FValue* Label = FindField(Row, TEXT("Label")))
		{
			Mark.Label = Label->StringValue;
		}
#if WITH_EDITORONLY_DATA
		if (const FValue* Comment = FindField(Row, TEXT("Comment")))
		{
			Mark.Comment = Comment->StringValue;
		}
		if (const FValue* Color = FindField(Row, TEXT("Color")))
		{
			const FValue* R = FindField(*Color, TEXT("R"));
			const FValue* G = FindField(*Color, TEXT("G"));
			const FValue* B = FindField(*Color, TEXT("B"));
			const FValue* A = FindField(*Color, TEXT("A"));
			auto Channel = [](const FValue* Value, double Default) { return Value != nullptr && IsNumber(*Value)
				? (Value->Kind == EAssetDocumentRuntimeValueKind::Integer ? static_cast<double>(Value->IntegerValue) : Value->NumberValue) : Default; };
			Mark.bUseCustomColor = true;
			Mark.CustomColor = FLinearColor(static_cast<float>(Channel(R, 0.0)), static_cast<float>(Channel(G, 0.0)), static_cast<float>(Channel(B, 0.0)),
				static_cast<float>(Channel(A, 1.0)));
		}
#endif
		bool bFlag = false;
		if (const FValue* Fence = FindField(Row, TEXT("DeterminismFence")))
		{
			if (!BoolField(RowReader, *Fence, TEXT("DeterminismFence"), bFlag))
			{
				return false;
			}
			Mark.bIsDeterminismFence = bFlag;
		}
		if (const FValue* Inclusive = FindField(Row, TEXT("Inclusive")))
		{
			if (!BoolField(RowReader, *Inclusive, TEXT("Inclusive"), bFlag))
			{
				return false;
			}
			Mark.bIsInclusiveTime = bFlag;
		}
		Frames.Add(MoveTemp(Mark));
	}
	Frames.StableSort([](const FMovieSceneMarkedFrame& A, const FMovieSceneMarkedFrame& B) { return A.FrameNumber < B.FrameNumber; });
	// The list as written (AddMarkedFrame would invent labels for unlabelled markers; the document states them).
	static const FArrayProperty* Property = FindFProperty<FArrayProperty>(UMovieScene::StaticClass(), TEXT("MarkedFrames"));
	if (Property == nullptr)
	{
		return Op.Fail(TEXT("Sequencer.EditFailed"), TEXT("UMovieScene::MarkedFrames is not reflected in this engine version."), TEXT("markers"));
	}
	Edit.MovieScene.Modify();
	*Property->ContainerPtrToValuePtr<TArray<FMovieSceneMarkedFrame>>(&Edit.MovieScene) = MoveTemp(Frames);
	Edit.bChanged = true;
	return true;
}

bool OpMetaDataSet(FEdit& Edit, FReader& Op)
{
	const FValue* Values = nullptr;
	if (!Op.Object(TEXT("values"), Values))
	{
		return false;
	}
	FReader Reader = Op.Child(TEXT("values"), *Values);
	if (!CheckFields(Reader, *Values, {TEXT("Author"), TEXT("Notes")}, TEXT("MetaData")))
	{
		return false;
	}
	ULevelSequence* Level = Cast<ULevelSequence>(&Edit.Sequence);
	if (Level == nullptr)
	{
		return Reader.Fail(TEXT("Sequencer.UnsupportedAsset"), TEXT("MetaData is a Level Sequence region."));
	}
	Level->Modify();
	UMovieSceneMetaData* MetaData = Level->FindOrAddMetaData<UMovieSceneMetaData>();
	MetaData->Modify();
	if (const FValue* Author = FindField(*Values, TEXT("Author")))
	{
		MetaData->SetAuthor(Author->StringValue);
	}
	if (const FValue* Notes = FindField(*Values, TEXT("Notes")))
	{
		MetaData->SetNotes(Notes->StringValue);
	}
	Edit.bChanged = true;
	return true;
}

// ---- dispatch --------------------------------------------------------------------------------------------------------

bool RunOp(FEdit& Edit, FReader& Op, const FString& Kind)
{
	if (Kind == TEXT("settings")) { return OpSettings(Edit, Op); }
	if (Kind == TEXT("binding.add")) { return OpBindingAdd(Edit, Op); }
	if (Kind == TEXT("binding.remove")) { return OpBindingRemove(Edit, Op); }
	if (Kind == TEXT("binding.rename")) { return OpBindingRename(Edit, Op); }
	if (Kind == TEXT("binding.set")) { return OpBindingSet(Edit, Op); }
	if (Kind == TEXT("binding.order")) { return OpBindingOrder(Edit, Op); }
	if (Kind == TEXT("track.add")) { return OpTrackAdd(Edit, Op); }
	if (Kind == TEXT("track.remove")) { return OpTrackRemove(Edit, Op); }
	if (Kind == TEXT("track.set")) { return OpTrackSet(Edit, Op); }
	if (Kind == TEXT("track.order")) { return OpTrackOrder(Edit, Op); }
	if (Kind == TEXT("section.add")) { return OpSectionAdd(Edit, Op); }
	if (Kind == TEXT("section.set")) { return OpSectionSet(Edit, Op); }
	if (Kind == TEXT("section.remove")) { return OpSectionRemove(Edit, Op); }
	if (Kind == TEXT("channel.append")) { return OpChannelAppend(Edit, Op); }
	if (Kind == TEXT("parameters.append")) { return OpParametersAppend(Edit, Op); }
	if (Kind == TEXT("folders.set")) { return OpFoldersSet(Edit, Op); }
	if (Kind == TEXT("markers.set")) { return OpMarkersSet(Edit, Op); }
	if (Kind == TEXT("metadata.set")) { return OpMetaDataSet(Edit, Op); }
	if (Kind == TEXT("template.root")) { return OpTemplateRoot(Edit, Op); }
	if (Kind == TEXT("director.create")) { return OpDirectorCreate(Edit, Op); }
	if (Kind == TEXT("director.remove")) { return OpDirectorRemove(Edit, Op); }
	if (Kind == TEXT("section_groups.set")) { return OpSectionGroupsSet(Edit, Op); }
	if (Kind == TEXT("groups.set")) { return OpNodeGroupsSet(Edit, Op); }
	return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Unknown op '%s'."), *Kind), TEXT("op"));
}

bool CheckOpFields(FReader& Op, const FValue& Value, const FString& Kind)
{
	static const TMap<FString, TArray<FString>> Fields = {
		{TEXT("settings"), {TEXT("values")}}, {TEXT("binding.add"), {TEXT("guid"), TEXT("name"), TEXT("parent")}},
		{TEXT("binding.remove"), {TEXT("guid")}}, {TEXT("binding.rename"), {TEXT("guid"), TEXT("name")}},
		{TEXT("binding.set"), {TEXT("guid"), TEXT("entry")}}, {TEXT("binding.order"), {TEXT("guids")}},
		{TEXT("track.add"), {TEXT("binding"), TEXT("type"), TEXT("property"), TEXT("name"), TEXT("object")}}, {TEXT("track.remove"), {TEXT("track")}},
		{TEXT("track.set"), {TEXT("track"), TEXT("entry")}}, {TEXT("track.order"), {TEXT("tracks")}},
		{TEXT("section.add"), {TEXT("track"), TEXT("entry")}}, {TEXT("section.set"), {TEXT("track"), TEXT("section"), TEXT("entry")}},
		{TEXT("section.remove"), {TEXT("track"), TEXT("section")}},
		{TEXT("channel.append"), {TEXT("track"), TEXT("section"), TEXT("channel"), TEXT("keys"), TEXT("parameter"), TEXT("stated")}},
		{TEXT("parameters.append"), {TEXT("track"), TEXT("section"), TEXT("entries"), TEXT("parameter")}},
		{TEXT("folders.set"), {TEXT("folders"), TEXT("bindings"), TEXT("tracks")}}, {TEXT("markers.set"), {TEXT("markers")}},
		{TEXT("metadata.set"), {TEXT("values")}}, {TEXT("template.root"), {TEXT("class")}}, {TEXT("director.create"), {}},
		{TEXT("director.remove"), {}}, {TEXT("section_groups.set"), {TEXT("groups")}}, {TEXT("groups.set"), {TEXT("groups")}}};
	const TArray<FString>* Allowed = Fields.Find(Kind);
	if (Allowed == nullptr)
	{
		return true; // RunOp names the unknown op
	}
	for (const FString& Key : Value.ObjectKeys)
	{
		if (Key != TEXT("op") && !Allowed->Contains(Key))
		{
			return Op.Fail(TEXT("Graph.UnknownField"), FString::Printf(TEXT("'%s' takes %s; '%s' is not one of them."), *Kind,
				*FString::Join(*Allowed, TEXT(", ")), *Key), Key);
		}
	}
	return true;
}

FValue Dependents(const FEdit& Edit)
{
	TArray<FValue> Rows;
	if (Edit.RemovedBindings.IsEmpty())
	{
		return MakeArray(MoveTemp(Rows));
	}
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FName> Referencers;
	Registry.GetReferencers(Edit.Sequence.GetOutermost()->GetFName(), Referencers, UE::AssetRegistry::EDependencyCategory::Package);
	Referencers.Sort(FNameLexicalLess());
	for (const FName& Package : Referencers)
	{
		if (Package == Edit.Sequence.GetOutermost()->GetFName())
		{
			continue;
		}
		FObjectBuilder Row;
		Row.AddString(TEXT("package"), Package.ToString());
		Row.Add(TEXT("bindings"), MakeStringArray(Edit.RemovedBindings));
		Rows.Add(Row.Build());
	}
	return MakeArray(MoveTemp(Rows));
}
} // namespace
} // namespace UE::AssetDocument::Graph::Sequencer::Edit

namespace UE::AssetDocument::Graph::Sequencer
{
bool HandleEdit(FCall& Call, FObjectBuilder& Result)
{
	using namespace Edit::SequencerEditLocal;
	using namespace Edit;
	const FValue* Ops = nullptr;
	if (!Call.Args.OnlyFields({TEXT("ops")}) || !Call.Args.Array(TEXT("ops"), Ops))
	{
		return false;
	}
	UMovieSceneSequence* Sequence = Cast<UMovieSceneSequence>(&Call.Asset);
	UMovieScene* MovieScene = Sequence != nullptr ? Sequence->GetMovieScene() : nullptr;
	if (MovieScene == nullptr)
	{
		return Fail(Call, TEXT("Sequencer.UnsupportedAsset"), TEXT("seq.edit.v1 edits Level and Template Sequences."), TEXT("/HandleArguments/asset"));
	}
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	const FScopedTransaction Transaction(LOCTEXT("EditSequence", "Edit Sequence"));
	FEdit Edit(*Sequence, *MovieScene);
	Edit.bReadOnly = MovieScene->IsReadOnly();
	if (Edit.bReadOnly && !Ops->Elements.IsEmpty())
	{
		MovieScene->Modify();
		MovieScene->SetReadOnly(false);
	}
	bool bOk = true;
	for (int32 Index = 0; Index < Ops->Elements.Num() && bOk; ++Index)
	{
		FReader Op = Call.Args.Child(FString::Printf(TEXT("ops/%d"), Index), Ops->Elements[Index]);
		Edit.CurrentOp = Index;
		FString Kind;
		if (!Op.IsObject())
		{
			bOk = Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("Each op is an object."));
			break;
		}
		bOk = Op.String(TEXT("op"), Kind) && CheckOpFields(Op, Ops->Elements[Index], Kind) && RunOp(Edit, Op, Kind);
		if (bOk && NonInteractive.WasInteractionRefused())
		{
			NonInteractive.DiscardCreatedPackages();
			bOk = Op.Fail(TEXT("Sequencer.InteractiveEditRefused"), TEXT("The edit tried to open an editor dialog, which an MCP request cannot "
				"answer; the edit was refused."));
		}
	}
	if (MovieScene->IsReadOnly() != Edit.bReadOnly)
	{
		MovieScene->SetReadOnly(Edit.bReadOnly);
	}
	if (!bOk)
	{
		return false;
	}
	if (Edit.bChanged)
	{
		MovieScene->MarkAsChanged();
		Sequence->MarkPackageDirty();
	}
	Result.AddBool(TEXT("changed"), Edit.bChanged);
	Result.Add(TEXT("dependents"), Dependents(Edit));
	Result.Add(TEXT("created"), MakeArray(MoveTemp(Edit.Created)));
	return true;
}

bool HandleStatus(FCall& Call, FObjectBuilder& Result)
{
	if (!Call.Args.OnlyFields({}))
	{
		return false;
	}
	FValue Status;
	if (!DescribeStatusValue(Call.Asset, Status))
	{
		return Fail(Call, TEXT("Sequencer.UnsupportedAsset"), TEXT("seq.status.v1 reads Level and Template Sequences."), TEXT("/HandleArguments/asset"));
	}
	Result.Add(TEXT("compile"), MoveTemp(Status));
	return true;
}
} // namespace UE::AssetDocument::Graph::Sequencer

#undef LOCTEXT_NAMESPACE
