// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// seq.edit.v1 track, section and channel ops (AssetDocumentSequencerEdit.cpp has the call and the other ops). What the
// Sequencer UI does for each (design §4.2):
//   track.add       the "+ Track" menus: UMovieScene::AddTrack (binding / global) or AddCameraCutTrack, then the track
//                   editor's setup (property tracks: SetPropertyNameAndPath; Spawn: SetObjectId; nameable: SetDisplayName);
//                   {binding?, type, property?, name?, object?} (object: the document's track Id, kept when free)
//   track.remove    the tree's Delete (UMovieScene::RemoveTrack / RemoveCameraCutTrack, folder entries)
//   track.set       Details of the track (mute, muted rows, row conditions, the class's own fields), rename
//   track.order     the tree's drag reorder (SortingOrder)
//   section.add     "Add Section" (UMovieSceneTrack::CreateNewSection, or the section class the track takes), then
//                   section.set on it before UMovieSceneTrack::AddSection
//   section.set     the section's Details and context menu (range, row, priority, active, locked, blend, easing, pre/post
//                   roll, when finished, the class's own fields) and its channels (keys, defaults, extrapolation)
//   section.remove  Delete
//   section.set's Parameters: the "+ Parameter" menus of parameter sections (the parameter arrays rebuilt, then the
//                   channel proxy, as a paste of the section does)
//   channel.append  more keys of a channel (the columnar chunks of a dense channel); {parameter?, stated?}: the channel
//                   of a parameter entry (index path), the earlier keys whose stated tangents are kept ([start, count]
//                   runs)
//   parameters.append  more Parameters entries (of the section or of the entry at the parameter index path) when the
//                   list is too large for one op: added at the end of their sets, then the channel proxy rebuilt
// A section entry is complete: a field it omits takes the section class default, a channel it omits is cleared.

#include "AssetDocumentSequencerEdit.h"

#include "Channels/MovieSceneEvent.h"
#include "EntitySystem/MovieScenePropertyBinding.h"
#include "Generators/MovieSceneEasingFunction.h"
#include "MovieScene.h"
#include "MovieSceneFolder.h"
#include "MovieSceneNameableTrack.h"
#include "MovieSceneSection.h"
#include "MovieSceneSequence.h"
#include "MovieSceneTrack.h"
#include "Conditions/MovieSceneCondition.h"
#include "Sections/MovieSceneSubSection.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Tracks/MovieSceneSpawnTrack.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph::Sequencer::Edit
{
namespace
{
bool CheckKeys(FReader& Reader, const FValue& Object, std::initializer_list<const TCHAR*> Allowed, const TCHAR* What)
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

UMovieSceneTrack* RequireTrack(FEdit& Edit, FReader& Op)
{
	FString Name;
	if (!Op.String(TEXT("track"), Name))
	{
		return nullptr;
	}
	UMovieSceneTrack* Track = FindTrackByName(Edit.MovieScene, Name);
	if (Track == nullptr)
	{
		Op.Fail(TEXT("Sequencer.UnknownTrack"), FString::Printf(TEXT("The sequence has no track '%s'."), *Name), TEXT("track"));
	}
	return Track;
}

UMovieSceneSection* RequireSection(FReader& Op, UMovieSceneTrack& Track)
{
	FString Name;
	if (!Op.String(TEXT("section"), Name))
	{
		return nullptr;
	}
	for (UMovieSceneSection* Section : Track.GetAllSections())
	{
		if (Section != nullptr && Section->GetName() == Name)
		{
			return Section;
		}
	}
	Op.Fail(TEXT("Sequencer.UnknownSection"), FString::Printf(TEXT("Track %s has no section '%s'."), *Track.GetName(), *Name), TEXT("section"));
	return nullptr;
}

bool SetProperty(UMovieSceneTrack& Track, const FString& Path, FReader& Op, const FString& Key)
{
	UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(&Track);
	if (PropertyTrack == nullptr)
	{
		return Path.IsEmpty() || Op.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("A %s track has no Property."),
			*TrackTypeToken(*Track.GetClass())), Key);
	}
	if (Path.IsEmpty())
	{
		// A property track class without a property of its own (Fade: a float track the engine binds itself) stays so.
		return PropertyTrack->GetPropertyPath().IsNone() || Op.Fail(TEXT("Sequencer.InvalidValue"), TEXT("A property track has Property (the property path on the bound object, e.g. "
			"FieldOfView or Settings.Value)."), Key);
	}
	const FMovieScenePropertyBinding Binding = FMovieScenePropertyBinding::FromPath(Path);
	if (Binding.PropertyName.IsNone())
	{
		return Op.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("'%s' is not a property path."), *Path), Key);
	}
	if (PropertyTrack->GetPropertyPath() != FName(*Path))
	{
		PropertyTrack->SetPropertyNameAndPath(Binding.PropertyName, Path);
	}
	return true;
}

bool SetName(UMovieSceneTrack& Track, const FString& Name, FReader& Op, const FString& Key)
{
	UMovieSceneNameableTrack* Nameable = Cast<UMovieSceneNameableTrack>(&Track);
	if (Nameable == nullptr)
	{
		return Name.IsEmpty() || Op.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("A %s track has no Name."),
			*TrackTypeToken(*Track.GetClass())), Key);
	}
#if WITH_EDITORONLY_DATA
	static const FTextProperty* DisplayName = FindFProperty<FTextProperty>(UMovieSceneNameableTrack::StaticClass(), TEXT("DisplayName"));
	if (DisplayName != nullptr && DisplayName->GetPropertyValue_InContainer(Nameable).ToString() != Name)
	{
		Nameable->SetDisplayName(Name.IsEmpty() ? FText::GetEmpty() : FText::FromString(Name));
	}
#endif
	return true;
}

// ---- section entry ---------------------------------------------------------------------------------------------------

const FProperty* FindEventProperty(const UClass& Class)
{
	for (TFieldIterator<FProperty> It(&Class); It; ++It)
	{
		const FStructProperty* Struct = CastField<FStructProperty>(*It);
		if (Struct != nullptr && Struct->Struct == FMovieSceneEvent::StaticStruct())
		{
			return *It;
		}
	}
	return nullptr;
}

bool FrameField(FEdit& Edit, FReader& Reader, const FValue& Entry, const TCHAR* Key, int32 Default, int32& Out)
{
	Out = Default;
	const FValue* Value = FindField(Entry, Key);
	if (Value == nullptr)
	{
		return true;
	}
	FDecodeContext Context{Edit.Model(), TimeOf(Edit.MovieScene), &Edit.MovieScene, Reader, Key};
	return DecodeFrames(*Value, Context, Out);
}

bool IntField(FReader& Reader, const FValue& Entry, const TCHAR* Key, int32 Default, int32& Out)
{
	Out = Default;
	const FValue* Value = FindField(Entry, Key);
	if (Value == nullptr)
	{
		return true;
	}
	if (Value->Kind != EAssetDocumentRuntimeValueKind::Integer || Value->IntegerValue < MIN_int32 || Value->IntegerValue > MAX_int32)
	{
		return Reader.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("%s is an integer."), Key), Key);
	}
	Out = static_cast<int32>(Value->IntegerValue);
	return true;
}

bool BoolValue(FReader& Reader, const FValue& Entry, const TCHAR* Key, bool Default, bool& Out)
{
	Out = Default;
	const FValue* Value = FindField(Entry, Key);
	if (Value == nullptr)
	{
		return true;
	}
	if (Value->Kind != EAssetDocumentRuntimeValueKind::Boolean)
	{
		return Reader.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("%s is true or false."), Key), Key);
	}
	Out = Value->BooleanValue;
	return true;
}

bool WriteEaseFunction(FEdit& Edit, FReader& Reader, UMovieSceneSection& Section, const UMovieSceneSection& Defaults, bool bIn, const FValue* Authored)
{
	const TCHAR* Key = bIn ? TEXT("InFunction") : TEXT("OutFunction");
	TScriptInterface<IMovieSceneEasingFunction>& Slot = bIn ? Section.Easing.EaseIn : Section.Easing.EaseOut;
	UObject* DefaultFunction = (bIn ? Defaults.Easing.EaseIn : Defaults.Easing.EaseOut).GetObject();
	UClass* Class = DefaultFunction != nullptr ? DefaultFunction->GetClass() : nullptr;
	const FValue Empty = MakeEmptyObject();
	FReader FunctionReader = Reader.Child(Key, Authored != nullptr ? *Authored : Empty);
	const FValue* Properties = nullptr;
	if (Authored != nullptr)
	{
		if (!CheckKeys(FunctionReader, *Authored, {TEXT("Class"), TEXT("Properties")}, Key))
		{
			return false;
		}
		const FValue* ClassPath = FindField(*Authored, TEXT("Class"));
		Class = ClassPath != nullptr ? LoadObject<UClass>(nullptr, *ClassPath->StringValue, nullptr, LOAD_NoWarn | LOAD_Quiet) : nullptr;
		if (Class == nullptr || Class->HasAnyClassFlags(CLASS_Abstract) || !Class->ImplementsInterface(UMovieSceneEasingFunction::StaticClass()))
		{
			return FunctionReader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("An easing function is {Class: <a class implementing "
				"MovieSceneEasingFunction>, Properties?}."), TEXT("Class"));
		}
		Properties = FindField(*Authored, TEXT("Properties"));
	}
	if (Class == nullptr)
	{
		Slot = nullptr;
		return true;
	}
	UObject* Function = Slot.GetObject();
	if (Function == nullptr || Function->GetClass() != Class)
	{
		// The easing menu's function choice: a new function object of that class on the section.
		Function = NewObject<UObject>(&Section, Class, NAME_None, RF_Transactional);
		Slot.SetObject(Function);
		Slot.SetInterface(Cast<IMovieSceneEasingFunction>(Function));
	}
	Function->Modify();
	const UObject* Baseline = DefaultFunction != nullptr && DefaultFunction->GetClass() == Class ? DefaultFunction : Class->GetDefaultObject();
	FDecodeContext Context{Edit.Model(), TimeOf(Edit.MovieScene), Function, FunctionReader, TEXT("Properties")};
	return DecodeSparseObject(*Class, Properties != nullptr ? *Properties : Empty, Function, Baseline,
		[](const FProperty& Property) { return Property.HasAnyPropertyFlags(CPF_Edit); }, Context);
}

bool WriteEasing(FEdit& Edit, FReader& Reader, UMovieSceneSection& Section, const UMovieSceneSection& Defaults, const FValue* Easing)
{
	const FValue Empty = MakeEmptyObject();
	const FValue& Object = Easing != nullptr ? *Easing : Empty;
	FReader EasingReader = Reader.Child(TEXT("Easing"), Object);
	if (!CheckKeys(EasingReader, Object, {TEXT("In"), TEXT("Out"), TEXT("InFunction"), TEXT("OutFunction")}, TEXT("Easing")))
	{
		return false;
	}
	int32 In = 0;
	int32 Out = 0;
	if (!FrameField(Edit, EasingReader, Object, TEXT("In"), 0, In) || !FrameField(Edit, EasingReader, Object, TEXT("Out"), 0, Out))
	{
		return false;
	}
	Section.Easing.bManualEaseIn = FindField(Object, TEXT("In")) != nullptr;
	Section.Easing.ManualEaseInDuration = Section.Easing.bManualEaseIn ? In : Defaults.Easing.ManualEaseInDuration;
	Section.Easing.bManualEaseOut = FindField(Object, TEXT("Out")) != nullptr;
	Section.Easing.ManualEaseOutDuration = Section.Easing.bManualEaseOut ? Out : Defaults.Easing.ManualEaseOutDuration;
	return WriteEaseFunction(Edit, EasingReader, Section, Defaults, true, FindField(Object, TEXT("InFunction")))
		&& WriteEaseFunction(Edit, EasingReader, Section, Defaults, false, FindField(Object, TEXT("OutFunction")));
}

bool WriteBlend(FReader& Reader, UMovieSceneSection& Section, const FValue& Entry)
{
	static const FStructProperty* Property = FindFProperty<FStructProperty>(UMovieSceneSection::StaticClass(), TEXT("BlendType"));
	if (Property == nullptr)
	{
		return Reader.Fail(TEXT("Sequencer.EditFailed"), TEXT("UMovieSceneSection::BlendType is not reflected in this engine version."), TEXT("Blend"));
	}
	FOptionalMovieSceneBlendType Blend = NewSectionBlend(Section);
	if (const FValue* Value = FindField(Entry, TEXT("Blend")))
	{
		if (Value->Kind == EAssetDocumentRuntimeValueKind::Null)
		{
			Blend = FOptionalMovieSceneBlendType();
		}
		else
		{
			const int64 Type = Value->Kind == EAssetDocumentRuntimeValueKind::String
				? StaticEnum<EMovieSceneBlendType>()->GetValueByNameString(Value->StringValue) : INDEX_NONE;
			if (Type == INDEX_NONE || !Section.GetSupportedBlendTypes().Contains(static_cast<EMovieSceneBlendType>(Type)))
			{
				TArray<FString> Names;
				for (const EMovieSceneBlendType Supported : Section.GetSupportedBlendTypes())
				{
					Names.Add(StaticEnum<EMovieSceneBlendType>()->GetNameStringByValue(static_cast<int64>(Supported)));
				}
				return Reader.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("Blend is one of %s (or null) for this section."),
					*FString::Join(Names, TEXT(", "))), TEXT("Blend"));
			}
			Blend = FOptionalMovieSceneBlendType(static_cast<EMovieSceneBlendType>(Type));
		}
	}
	*Property->ContainerPtrToValuePtr<FOptionalMovieSceneBlendType>(&Section) = Blend;
	return true;
}

// The inner sequence a sub / shot section will play (Properties.SubSequence as written; omitted is none): its rates are
// the time base of the section's offsets (design §3.5.4).
UMovieSceneSequence* InnerSequence(UMovieSceneSection& Section, const FValue* Properties)
{
	const FObjectProperty* SubSequence = FindFProperty<FObjectProperty>(Section.GetClass(), TEXT("SubSequence"));
	if (SubSequence == nullptr)
	{
		return nullptr;
	}
	if (const FValue* Authored = Properties != nullptr ? FindField(*Properties, TEXT("SubSequence")) : nullptr)
	{
		return Authored->Kind == EAssetDocumentRuntimeValueKind::String && !Authored->StringValue.IsEmpty()
			? LoadObject<UMovieSceneSequence>(nullptr, *Authored->StringValue, nullptr, LOAD_NoWarn | LOAD_Quiet) : nullptr;
	}
	return nullptr;
}

bool WriteSectionEntry(FEdit& Edit, FReader& Reader, UMovieSceneSection& Section, const FValue& Entry, bool bNew)
{
	if (!CheckKeys(Reader, Entry, {TEXT("Class"), TEXT("Start"), TEXT("End"), TEXT("Row"), TEXT("OverlapPriority"), TEXT("Active"), TEXT("Locked"),
		TEXT("Blend"), TEXT("Easing"), TEXT("PreRoll"), TEXT("PostRoll"), TEXT("WhenFinished"), TEXT("Properties"), TEXT("Event"), TEXT("Unsupported"),
		TEXT("Channels"), TEXT("Parameters")}, TEXT("A section")))
	{
		return false;
	}
	const UMovieSceneSection& Defaults = *CastChecked<UMovieSceneSection>(Section.GetClass()->GetDefaultObject());
	if (const FValue* Unsupported = FindField(Entry, TEXT("Unsupported")))
	{
		if (Unsupported->StringValue != TEXT("ChannelOverrides"))
		{
			return Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("Unsupported is \"ChannelOverrides\" (kept as it is)."), TEXT("Unsupported"));
		}
	}
	if (!bNew && FindField(Entry, TEXT("Class")) != nullptr)
	{
		UMovieSceneTrack* Track = Section.GetTypedOuter<UMovieSceneTrack>();
		const FString Token = Track != nullptr ? SectionClassToken(*Section.GetClass(), *Track->GetClass()) : FString();
		const FString Authored = FindField(Entry, TEXT("Class"))->StringValue;
		if (Authored != Token && Authored != Section.GetClass()->GetPathName())
		{
			return Reader.Fail(TEXT("Sequencer.SectionClassChanged"), TEXT("A section keeps its class; a section of another class is a new "
				"section."), TEXT("Class"));
		}
	}
	bool bLocked = false;
	bool bActive = true;
	int32 Row = 0;
	int32 Priority = 0;
	int32 PreRoll = 0;
	int32 PostRoll = 0;
	if (!BoolValue(Reader, Entry, TEXT("Locked"), Defaults.IsLocked(), bLocked) || !BoolValue(Reader, Entry, TEXT("Active"), Defaults.IsActive(), bActive)
		|| !IntField(Reader, Entry, TEXT("Row"), Defaults.GetRowIndex(), Row) || !IntField(Reader, Entry, TEXT("OverlapPriority"),
		Defaults.GetOverlapPriority(), Priority) || !FrameField(Edit, Reader, Entry, TEXT("PreRoll"), Defaults.GetPreRollFrames(), PreRoll)
		|| !FrameField(Edit, Reader, Entry, TEXT("PostRoll"), Defaults.GetPostRollFrames(), PostRoll))
	{
		return false;
	}
	Section.Modify();
	Section.SetIsLocked(false);

	// Range: [Start, End), an omitted bound is open (infinite), which only sections that support it take.
	const FValue* Start = FindField(Entry, TEXT("Start"));
	const FValue* End = FindField(Entry, TEXT("End"));
	const TRange<FFrameNumber> Current = Section.GetRange();
	if (!Section.GetSupportsInfiniteRange() && ((Start == nullptr && (bNew || Current.GetLowerBound().IsClosed()))
		|| (End == nullptr && (bNew || Current.GetUpperBound().IsClosed()))))
	{
		return Reader.Fail(TEXT("Sequencer.SectionRangeRequired"), FString::Printf(TEXT("A %s has Start and End (it cannot be infinite)."),
			*Section.GetClass()->GetName()), Start == nullptr ? TEXT("Start") : TEXT("End"));
	}
	int32 StartTicks = 0;
	int32 EndTicks = 0;
	if (!FrameField(Edit, Reader, Entry, TEXT("Start"), 0, StartTicks) || !FrameField(Edit, Reader, Entry, TEXT("End"), 0, EndTicks))
	{
		return false;
	}
	if (Start != nullptr && End != nullptr && EndTicks < StartTicks)
	{
		return Reader.Fail(TEXT("Sequencer.InvalidRange"), TEXT("End is before Start."), TEXT("End"));
	}
	const TRange<FFrameNumber> Range(Start != nullptr ? TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(StartTicks)) : TRangeBound<FFrameNumber>::Open(),
		End != nullptr ? TRangeBound<FFrameNumber>::Exclusive(FFrameNumber(EndTicks)) : TRangeBound<FFrameNumber>::Open());
	Section.SetRange(Range);
	Section.SetRowIndex(Row);
	Section.SetOverlapPriority(Priority);
	Section.SetIsActive(bActive);
	Section.SetPreRollFrames(PreRoll);
	Section.SetPostRollFrames(PostRoll);
	if (!WriteBlend(Reader, Section, Entry) || !WriteEasing(Edit, Reader, Section, Defaults, FindField(Entry, TEXT("Easing"))))
	{
		return false;
	}
	EMovieSceneCompletionMode Completion = Defaults.EvalOptions.CompletionMode;
	if (const FValue* WhenFinished = FindField(Entry, TEXT("WhenFinished")))
	{
		const int64 Mode = WhenFinished->Kind == EAssetDocumentRuntimeValueKind::String
			? StaticEnum<EMovieSceneCompletionMode>()->GetValueByNameString(WhenFinished->StringValue) : INDEX_NONE;
		if (Mode == INDEX_NONE)
		{
			return Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("WhenFinished is KeepState, RestoreState or ProjectDefault."), TEXT("WhenFinished"));
		}
		Completion = static_cast<EMovieSceneCompletionMode>(Mode);
	}
	Section.EvalOptions.CompletionMode = Completion;

	// Properties: the class's own fields (sub-section offsets in the inner sequence's time).
	const FValue Empty = MakeEmptyObject();
	const FValue* Properties = FindField(Entry, TEXT("Properties"));
	bool bProxyChanged = false;
	FDecodeContext Context{Edit.Model(), TimeOf(Edit.MovieScene), &Section, Reader, TEXT("Properties"), &bProxyChanged};
	UMovieSceneSequence* Inner = InnerSequence(Section, Properties);
	if (Inner != nullptr)
	{
		if (!Edit.Sequence.IsSubSequenceCompatible(*Inner))
		{
			return Reader.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("A %s cannot play %s as a sub-sequence."),
				*Edit.Sequence.GetClass()->GetName(), *Inner->GetPathName()), TEXT("Properties/SubSequence"));
		}
		if (Inner == &Edit.Sequence || SequenceReaches(Inner, &Edit.Sequence))
		{
			return Reader.Fail(TEXT("Sequencer.CircularSubsequence"), FString::Printf(TEXT("%s plays %s (directly or through its own "
				"sub-sequences), so it cannot be a sub-sequence of it."), *Inner->GetPathName(), *Edit.Sequence.GetPathName()),
				TEXT("Properties/SubSequence"));
		}
		if (const UMovieScene* InnerScene = Inner->GetMovieScene())
		{
			Context.Time = TimeOf(*InnerScene);
		}
	}
	const FProperty* EventProperty = FindEventProperty(*Section.GetClass());
	if (!DecodeSparseObject(*Section.GetClass(), Properties != nullptr ? *Properties : Empty, &Section, &Defaults,
		[EventProperty](const FProperty& Property) { return &Property != EventProperty && IsSectionPropertyField(Property); }, Context))
	{
		return false;
	}
	if (EventProperty != nullptr)
	{
		FMovieSceneEvent& Event = *EventProperty->ContainerPtrToValuePtr<FMovieSceneEvent>(&Section);
		const FValue* Authored = FindField(Entry, TEXT("Event"));
		FWarnings Warnings;
		FEncodeContext EncodeContext{Edit.Model(), TimeOf(Edit.MovieScene), Warnings, FString()};
		if (Authored == nullptr || !SameValue(*Authored, EncodeEvent(Event, EncodeContext)))
		{
			FDecodeContext EventContext{Edit.Model(), TimeOf(Edit.MovieScene), &Section, Reader, TEXT("Event")};
			if (!DecodeValue(*EventProperty, Authored != nullptr ? *Authored : Empty, &Event, EventContext))
			{
				return false;
			}
		}
	}
	else if (FindField(Entry, TEXT("Event")) != nullptr)
	{
		return Reader.Fail(TEXT("Sequencer.FieldNotEditable"), FString::Printf(TEXT("A %s has no Event."), *Section.GetClass()->GetName()), TEXT("Event"));
	}
	Section.PostEditChange();
	if (bProxyChanged)
	{
		// A time warp or instanced object the channel proxy pointed into was replaced (custom time-warp curves are channels).
		static_cast<UObject&>(Section).PostEditImport();
	}

	// Parameters (parameter sections): the parameter arrays rebuilt from the entries, then the channel proxy.
	{
		FDecodeContext ParameterContext{Edit.Model(), TimeOf(Edit.MovieScene), &Section, Reader, TEXT("Parameters")};
		if (!WriteParameters(Section, FindField(Entry, TEXT("Parameters")), ParameterContext))
		{
			return false;
		}
	}

	// Channels: listed channels written, the others cleared.
	const FValue* Channels = FindField(Entry, TEXT("Channels"));
	if (Channels != nullptr && Channels->Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("Channels is an object of channel name -> channel."), TEXT("Channels"));
	}
	TSet<FString> Listed;
	for (int32 Index = 0; Channels != nullptr && Index < Channels->ObjectKeys.Num(); ++Index)
	{
		Listed.Add(Channels->ObjectKeys[Index]);
	}
	ClearChannels(Section, Listed);
	for (int32 Index = 0; Channels != nullptr && Index < Channels->ObjectKeys.Num(); ++Index)
	{
		FDecodeContext ChannelContext{Edit.Model(), TimeOf(Edit.MovieScene), &Section, Reader, TEXT("Channels/") + Escape(Channels->ObjectKeys[Index])};
		if (!WriteChannel(Section, Channels->ObjectKeys[Index], Channels->Elements[Index], false, ChannelContext))
		{
			return false;
		}
	}
	Section.SetIsLocked(bLocked);
	return true;
}
} // namespace

void RemoveTrack(FEdit& Edit, UMovieSceneTrack& Track)
{
#if WITH_EDITORONLY_DATA
	TArray<UMovieSceneFolder*> Folders;
	GetMovieSceneFoldersRecursive(Edit.MovieScene.GetRootFolders(), Folders);
	for (UMovieSceneFolder* Folder : Folders)
	{
		if (Folder != nullptr && Folder->GetChildTracks().Contains(&Track))
		{
			Folder->Modify();
			Folder->RemoveChildTrack(&Track);
		}
	}
#endif
	Edit.MovieScene.Modify();
	if (Edit.MovieScene.GetCameraCutTrack() == &Track)
	{
		Edit.MovieScene.RemoveCameraCutTrack();
	}
	else
	{
		Edit.MovieScene.RemoveTrack(Track);
	}
	Edit.bChanged = true;
	Edit.Invalidate();
}

bool OpTrackAdd(FEdit& Edit, FReader& Op)
{
	FString Type;
	FString BindingText;
	FString Property;
	FString Name;
	FString ObjectName;
	if (!Op.String(TEXT("type"), Type) || !Op.String(TEXT("binding"), BindingText, false) || !Op.String(TEXT("property"), Property, false)
		|| !Op.String(TEXT("name"), Name, false) || !Op.String(TEXT("object"), ObjectName, false))
	{
		return false;
	}
	UClass* Class = TrackClassOf(Type);
	if (Class == nullptr || Class->HasAnyClassFlags(CLASS_Abstract))
	{
		return Op.Fail(TEXT("Sequencer.UnknownTrackType"), FString::Printf(TEXT("'%s' is not a track type (a type token such as Transform, Float, "
			"Audio, or a track class path; list_types kind sequencer_track lists them)."), *Type), TEXT("type"));
	}
	if (!UMovieScene::IsTrackClassAllowed(Class) || Edit.Sequence.IsTrackSupported(Class) == ETrackSupport::NotSupported)
	{
		return Op.Fail(TEXT("Sequencer.TrackNotSupported"), FString::Printf(TEXT("A %s does not take %s tracks."), *Edit.Sequence.GetClass()->GetName(),
			*Type), TEXT("type"));
	}
	FGuid Binding;
	if (!BindingText.IsEmpty() && (!FGuid::Parse(BindingText, Binding) || Edit.MovieScene.FindBinding(Binding) == nullptr))
	{
		return Op.Fail(TEXT("Sequencer.UnknownBinding"), TEXT("The sequence has no such binding."), TEXT("binding"));
	}
	Edit.MovieScene.Modify();
	UMovieSceneTrack* Track = nullptr;
	if (Class->IsChildOf(UMovieSceneCameraCutTrack::StaticClass()))
	{
		if (Binding.IsValid() || Edit.MovieScene.GetCameraCutTrack() != nullptr)
		{
			return Op.Fail(TEXT("Sequencer.TrackNotSupported"), TEXT("A sequence has one camera cut track, outside the bindings."), TEXT("type"));
		}
		Track = Edit.MovieScene.AddCameraCutTrack(Class);
	}
	else
	{
		Track = Binding.IsValid() ? Edit.MovieScene.AddTrack(Class, Binding) : Edit.MovieScene.AddTrack(Class);
	}
	if (Track == nullptr)
	{
		return Op.Fail(TEXT("Sequencer.TrackNotSupported"), FString::Printf(TEXT("The movie scene refused a %s track."), *Type), TEXT("type"));
	}
	if (Property.IsEmpty() && Type == TEXT("Transform"))
	{
		Property = TEXT("Transform");
	}
	if (!SetProperty(*Track, Property, Op, TEXT("property")) || !SetName(*Track, Name, Op, TEXT("name")))
	{
		return false;
	}
	if (UMovieSceneSpawnTrack* Spawn = Cast<UMovieSceneSpawnTrack>(Track))
	{
		Spawn->SetObjectId(Binding);
	}
	// The document's track Id (the object name Extract shows for tracks that Type / Property / Name do not identify) is
	// kept when the name is free, so the next Extract shows the same Id.
	FText InvalidReason;
	if (!ObjectName.IsEmpty() && ObjectName != Track->GetName() && FName(*ObjectName).IsValidObjectName(InvalidReason)
		&& StaticFindObjectFast(UObject::StaticClass(), &Edit.MovieScene, FName(*ObjectName)) == nullptr)
	{
		Track->Rename(*ObjectName, nullptr, REN_DontCreateRedirectors);
	}
	Edit.NoteCreated(*Track);
	Edit.bChanged = true;
	Edit.Invalidate();
	return true;
}

bool OpTrackRemove(FEdit& Edit, FReader& Op)
{
	UMovieSceneTrack* Track = RequireTrack(Edit, Op);
	if (Track == nullptr)
	{
		return false;
	}
	RemoveTrack(Edit, *Track);
	return true;
}

// Row conditions (the track row's Details: FMovieSceneTrackRowMetadata::ConditionContainer): {"<row>": condition}; rows
// not listed have none. A row's condition of the same class is edited in place.
bool WriteRowConditions(FEdit& Edit, UMovieSceneTrack& Track, FReader& Entry, const FValue* RowConditions)
{
	static const FMapProperty* RowMetadata = FindFProperty<FMapProperty>(UMovieSceneTrack::StaticClass(), TEXT("TrackRowMetadata"));
	static const FObjectProperty* ConditionProperty = FindFProperty<FObjectProperty>(FMovieSceneConditionContainer::StaticStruct(), TEXT("Condition"));
	if (RowMetadata == nullptr || ConditionProperty == nullptr)
	{
		return RowConditions == nullptr || Entry.Fail(TEXT("Sequencer.FieldNotEditable"), TEXT("Track row metadata is not available."),
			TEXT("RowConditions"));
	}
	TMap<int32, FMovieSceneTrackRowMetadata>& Rows = *RowMetadata->ContainerPtrToValuePtr<TMap<int32, FMovieSceneTrackRowMetadata>>(&Track);
	if (RowConditions != nullptr && RowConditions->Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return Entry.Fail(TEXT("Sequencer.InvalidValue"), TEXT("RowConditions is an object of row index -> condition {Class, Properties?}."),
			TEXT("RowConditions"));
	}
	TMap<int32, UMovieSceneCondition*> Written;
	for (int32 Index = 0; RowConditions != nullptr && Index < RowConditions->ObjectKeys.Num(); ++Index)
	{
		const FString& Key = RowConditions->ObjectKeys[Index];
		const FString Path = TEXT("RowConditions/") + Escape(Key);
		int32 RowIndex = INDEX_NONE;
		if (Key.IsEmpty() || Key.Len() > 9 || !Key.IsNumeric() || Key.Contains(TEXT(".")) || Key.Contains(TEXT("-")) || Key.Contains(TEXT("+")))
		{
			return Entry.Fail(TEXT("Sequencer.InvalidValue"), TEXT("A row condition's key is the row index (\"0\", \"1\", ...)."), Path);
		}
		RowIndex = FCString::Atoi(*Key);
		if (Written.Contains(RowIndex))
		{
			return Entry.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("Row %d has two conditions."), RowIndex), Path);
		}
		const FMovieSceneTrackRowMetadata* Current = Rows.Find(RowIndex);
		FPropertyValueBuffer Value(*ConditionProperty);
		ConditionProperty->SetObjectPropertyValue(Value.Memory, Current != nullptr ? Current->ConditionContainer.Condition.Get() : nullptr);
		FDecodeContext Context{Edit.Model(), TimeOf(Edit.MovieScene), &Track, Entry, Path};
		if (!DecodeValue(*ConditionProperty, RowConditions->Elements[Index], Value.Memory, Context))
		{
			return false;
		}
		Written.Add(RowIndex, Cast<UMovieSceneCondition>(ConditionProperty->GetObjectPropertyValue(Value.Memory)));
	}
	Rows.Reset();
	for (const TPair<int32, UMovieSceneCondition*>& Pair : Written)
	{
		if (Pair.Value != nullptr)
		{
			Rows.Add(Pair.Key).ConditionContainer.Condition = Pair.Value;
		}
	}
	return true;
}

bool OpTrackSet(FEdit& Edit, FReader& Op)
{
	UMovieSceneTrack* Track = RequireTrack(Edit, Op);
	const FValue* EntryValue = nullptr;
	if (Track == nullptr || !Op.Object(TEXT("entry"), EntryValue))
	{
		return false;
	}
	FReader Entry = Op.Child(TEXT("entry"), *EntryValue);
	if (!CheckKeys(Entry, *EntryValue, {TEXT("Type"), TEXT("Property"), TEXT("Name"), TEXT("Id"), TEXT("Folder"), TEXT("Muted"), TEXT("MutedRows"),
		TEXT("RowConditions"), TEXT("Properties")}, TEXT("A track")))
	{
		return false;
	}
	Track->Modify();
	if (const FValue* Property = FindField(*EntryValue, TEXT("Property")))
	{
		if (!SetProperty(*Track, Property->StringValue, Entry, TEXT("Property")))
		{
			return false;
		}
	}
	const FValue* Name = FindField(*EntryValue, TEXT("Name"));
	if (!SetName(*Track, Name != nullptr ? Name->StringValue : FString(), Entry, TEXT("Name")))
	{
		return false;
	}
	bool bMuted = false;
	if (!BoolValue(Entry, *EntryValue, TEXT("Muted"), false, bMuted))
	{
		return false;
	}
	Track->SetEvalDisabled(bMuted);
	static const FArrayProperty* RowsDisabled = FindFProperty<FArrayProperty>(UMovieSceneTrack::StaticClass(), TEXT("RowsDisabled"));
	if (RowsDisabled != nullptr)
	{
		TArray<int32> Rows;
		if (const FValue* MutedRows = FindField(*EntryValue, TEXT("MutedRows")))
		{
			if (MutedRows->Kind != EAssetDocumentRuntimeValueKind::Array)
			{
				return Entry.Fail(TEXT("Sequencer.InvalidValue"), TEXT("MutedRows is an array of row indices."), TEXT("MutedRows"));
			}
			for (int32 Index = 0; Index < MutedRows->Elements.Num(); ++Index)
			{
				const FValue& Row = MutedRows->Elements[Index];
				if (Row.Kind != EAssetDocumentRuntimeValueKind::Integer || Row.IntegerValue < 0 || Row.IntegerValue > MAX_int32)
				{
					return Entry.Fail(TEXT("Sequencer.InvalidValue"), TEXT("A muted row is a row index."), FString::Printf(TEXT("MutedRows/%d"), Index));
				}
				Rows.AddUnique(static_cast<int32>(Row.IntegerValue));
			}
		}
		*RowsDisabled->ContainerPtrToValuePtr<TArray<int32>>(Track) = MoveTemp(Rows);
	}
	if (!WriteRowConditions(Edit, *Track, Entry, FindField(*EntryValue, TEXT("RowConditions"))))
	{
		return false;
	}
	const FValue Empty = MakeEmptyObject();
	const FValue* Properties = FindField(*EntryValue, TEXT("Properties"));
	FDecodeContext Context{Edit.Model(), TimeOf(Edit.MovieScene), Track, Entry, TEXT("Properties")};
	if (!DecodeSparseObject(*Track->GetClass(), Properties != nullptr ? *Properties : Empty, Track, Track->GetClass()->GetDefaultObject(),
		&IsTrackPropertyField, Context))
	{
		return false;
	}
	if (Properties != nullptr && !Properties->ObjectKeys.IsEmpty())
	{
		Track->PostEditChange();
	}
	Edit.bChanged = true;
	Edit.Invalidate();
	return true;
}

bool OpTrackOrder(FEdit& Edit, FReader& Op)
{
	const FValue* Tracks = nullptr;
	if (!Op.Array(TEXT("tracks"), Tracks))
	{
		return false;
	}
	for (int32 Index = 0; Index < Tracks->Elements.Num(); ++Index)
	{
		UMovieSceneTrack* Track = FindTrackByName(Edit.MovieScene, Tracks->Elements[Index].StringValue);
		if (Track == nullptr)
		{
			return Op.Fail(TEXT("Sequencer.UnknownTrack"), FString::Printf(TEXT("The sequence has no track '%s'."), *Tracks->Elements[Index].StringValue),
				FString::Printf(TEXT("tracks/%d"), Index));
		}
#if WITH_EDITORONLY_DATA
		if (Track->GetSortingOrder() != Index)
		{
			Track->Modify();
			Track->SetSortingOrder(Index);
		}
#endif
	}
	Edit.bChanged = true;
	Edit.Invalidate();
	return true;
}

bool OpSectionAdd(FEdit& Edit, FReader& Op)
{
	UMovieSceneTrack* Track = RequireTrack(Edit, Op);
	const FValue* EntryValue = nullptr;
	if (Track == nullptr || !Op.Object(TEXT("entry"), EntryValue))
	{
		return false;
	}
	FReader Entry = Op.Child(TEXT("entry"), *EntryValue);
	UMovieSceneSection* Section = nullptr;
	Track->Modify();
	if (const FValue* ClassToken = FindField(*EntryValue, TEXT("Class")))
	{
		UClass* Class = SectionClassOf(ClassToken->StringValue, *Track->GetClass());
		if (Class == nullptr)
		{
			return Entry.Fail(TEXT("Sequencer.InvalidSectionClass"), FString::Printf(TEXT("A %s track takes no '%s' sections (list_types kind "
				"sequencer_section lists the section classes of each track)."), *TrackTypeToken(*Track->GetClass()), *ClassToken->StringValue),
				TEXT("Class"));
		}
		Section = NewObject<UMovieSceneSection>(Track, Class, NAME_None, RF_Transactional);
	}
	else
	{
		Section = Track->CreateNewSection();
	}
	if (Section == nullptr)
	{
		return Entry.Fail(TEXT("Sequencer.InvalidSectionClass"), TEXT("The track did not create a section."));
	}
	if (!WriteSectionEntry(Edit, Entry, *Section, *EntryValue, true))
	{
		return false;
	}
	Track->AddSection(*Section);
	Track->UpdateEasing();
	Edit.NoteCreated(*Section);
	Edit.bChanged = true;
	Edit.Invalidate();
	return true;
}

bool OpSectionSet(FEdit& Edit, FReader& Op)
{
	UMovieSceneTrack* Track = RequireTrack(Edit, Op);
	UMovieSceneSection* Section = Track != nullptr ? RequireSection(Op, *Track) : nullptr;
	const FValue* EntryValue = nullptr;
	if (Section == nullptr || !Op.Object(TEXT("entry"), EntryValue))
	{
		return false;
	}
	FReader Entry = Op.Child(TEXT("entry"), *EntryValue);
	Track->Modify();
	if (!WriteSectionEntry(Edit, Entry, *Section, *EntryValue, false))
	{
		return false;
	}
	Track->UpdateEasing();
	Edit.bChanged = true;
	return true;
}

bool OpSectionRemove(FEdit& Edit, FReader& Op)
{
	UMovieSceneTrack* Track = RequireTrack(Edit, Op);
	UMovieSceneSection* Section = Track != nullptr ? RequireSection(Op, *Track) : nullptr;
	if (Section == nullptr)
	{
		return false;
	}
	Track->Modify();
	Track->RemoveSection(*Section);
	Track->UpdateEasing();
	Edit.bChanged = true;
	Edit.Invalidate();
	return true;
}

// The op's parameter: the index path of a Parameters entry.
static bool ParameterPath(FReader& Op, const FValue* Parameter, TArray<int32>& OutPath)
{
	for (int32 Index = 0; Parameter != nullptr && Index < Parameter->Elements.Num(); ++Index)
	{
		const FValue& Element = Parameter->Elements[Index];
		if (Element.Kind != EAssetDocumentRuntimeValueKind::Integer || Element.IntegerValue < 0 || Element.IntegerValue > MAX_int32)
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("parameter is the index path of a Parameters entry."), TEXT("parameter"));
		}
		OutPath.Add(static_cast<int32>(Element.IntegerValue));
	}
	return true;
}

bool OpChannelAppend(FEdit& Edit, FReader& Op)
{
	UMovieSceneTrack* Track = RequireTrack(Edit, Op);
	UMovieSceneSection* Section = Track != nullptr ? RequireSection(Op, *Track) : nullptr;
	FString Channel;
	const FValue* Keys = nullptr;
	if (Section == nullptr || !Op.String(TEXT("channel"), Channel) || !Op.Object(TEXT("keys"), Keys))
	{
		return false;
	}
	const bool bLocked = Section->IsLocked();
	Section->Modify();
	Section->SetIsLocked(false);
	// stated: [start, count] runs of the keys already in the channel whose stated tangents are kept.
	TArray<int32> Stated;
	const FValue* StatedRuns = nullptr;
	if (!Op.Array(TEXT("stated"), StatedRuns, false))
	{
		return false;
	}
	for (int32 Index = 0; StatedRuns != nullptr && Index < StatedRuns->Elements.Num(); ++Index)
	{
		const FValue& Run = StatedRuns->Elements[Index];
		if (Run.Kind != EAssetDocumentRuntimeValueKind::Array || Run.Elements.Num() != 2 || Run.Elements[0].Kind != EAssetDocumentRuntimeValueKind::Integer
			|| Run.Elements[1].Kind != EAssetDocumentRuntimeValueKind::Integer || Run.Elements[0].IntegerValue < 0 || Run.Elements[1].IntegerValue < 0
			|| Run.Elements[0].IntegerValue + Run.Elements[1].IntegerValue > MAX_int32)
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("stated is an array of [start, count] key runs."), FString::Printf(TEXT("stated/%d"), Index));
		}
		for (int64 Key = Run.Elements[0].IntegerValue; Key < Run.Elements[0].IntegerValue + Run.Elements[1].IntegerValue; ++Key)
		{
			Stated.Add(static_cast<int32>(Key));
		}
	}
	const FValue* Parameter = nullptr;
	TArray<int32> Path;
	if (!Op.Array(TEXT("parameter"), Parameter, false) || !ParameterPath(Op, Parameter, Path))
	{
		return false;
	}
	FDecodeContext Context{Edit.Model(), TimeOf(Edit.MovieScene), Section, Op, TEXT("keys")};
	if (Parameter != nullptr)
	{
		FName TypeName;
		void* Memory = nullptr;
		FString Error;
		if (!FindParameterChannel(*Section, Path, Channel, TypeName, Memory, Error))
		{
			return Op.Fail(TEXT("Sequencer.UnknownChannel"), Error, TEXT("channel"));
		}
		if (!WriteChannelValue(TypeName, Memory, *Keys, true, Stated, Context))
		{
			return false;
		}
	}
	else if (!WriteChannel(*Section, Channel, *Keys, true, Context, Stated))
	{
		return false;
	}
	Section->SetIsLocked(bLocked);
	Edit.bChanged = true;
	return true;
}
bool OpParametersAppend(FEdit& Edit, FReader& Op)
{
	UMovieSceneTrack* Track = RequireTrack(Edit, Op);
	UMovieSceneSection* Section = Track != nullptr ? RequireSection(Op, *Track) : nullptr;
	const FValue* Entries = nullptr;
	const FValue* Parameter = nullptr;
	TArray<int32> Path;
	if (Section == nullptr || !Op.Array(TEXT("entries"), Entries) || !Op.Array(TEXT("parameter"), Parameter, false) || !ParameterPath(Op, Parameter, Path))
	{
		return false;
	}
	const bool bLocked = Section->IsLocked();
	Section->Modify();
	Section->SetIsLocked(false);
	FDecodeContext Context{Edit.Model(), TimeOf(Edit.MovieScene), Section, Op, TEXT("entries")};
	if (!AppendParameters(*Section, Path, *Entries, Context))
	{
		return false;
	}
	Section->SetIsLocked(bLocked);
	Edit.bChanged = true;
	return true;
}
} // namespace UE::AssetDocument::Graph::Sequencer::Edit
