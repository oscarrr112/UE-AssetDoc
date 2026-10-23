// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// seq.read.v1: the authored state of a Level Sequence (design §2.1, §3) as document-ready records. The sequence is read
// as stored (after the engine's PostLoad upgrades, design §7.1); nothing is rebuilt or written.
//
// Args {offset?, defaults_at?[document paths]}. Records (paged under the value budget), in document order:
//   {kind: "header", settings, folders[], markers[], director, metadata, counts{}}
//   {kind: "binding", id, parent, entry: {Name, Id?, Class?, Bind? | Spawnable? | CustomBinding? | References?, Tags?, Folder?}}
//                                       (every binding, depth first in Sequencer's order; Python builds the tree)
//   Template / Camera Animation Sequences (design §3.4.3): one root binding
//   {Name, Id?, Root: {Class, Template?, Ownership?, ContinuouslyRespawn?, NetAddressableName?, LevelName?, SpawnTransform?},
//   Tags?, Folder?} (the root spawnable; Class is the bound actor class) and component bindings below it
//   {Name, Id?, Class?, Component, Tags?} (Component: the component the binding locates by name, "" for none).
//   {kind: "track", binding, object, entry: {Type, Property?, Name?, Id?, Folder?, Muted?, MutedRows?, Properties?}}
//                                       (global tracks first, then per binding; binding "" = global)
//   {kind: "section", object, entry: {Class?, Start?, End?, Row?, ..., Properties?, Channels, Event?}}   (of the last track)
//                                       (object: the track's / section's object name, how seq.edit.v1 addresses them)
//   {kind: "keys" | "columns", channel, ...}                     (keys of the last section's channel; AssetDocumentSequencerChannels.cpp)
//   {kind: "warning", code, message, path}                       (content shown raw / kept as it is, design §3.9)
//   {kind: "default", path, value}                               (defaults_at: the default of a listed path whose field a
//                                                                 sparse object leaves out; Verify compares an authored
//                                                                 default with it)
// Frames are display frames of the sequence (design §3.3); binding references are binding paths (§3.4.2).

#include "AssetDocumentSequencerPrimitives.h"

#include "Bindings/MovieSceneCustomBinding.h"
#include "Bindings/MovieSceneSpawnableActorBinding.h"
#include "Channels/MovieSceneEvent.h"
#include "Channels/MovieSceneSectionChannelOverrideRegistry.h"
#include "Components/ActorComponent.h"
#include "Conditions/MovieSceneCondition.h"
#include "EdGraph/EdGraph.h"
#include "EditorCategoryUtils.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBindingReferences.h"
#include "MovieSceneClock.h"
#include "MovieSceneFolder.h"
#include "MovieSceneMarkedFrame.h"
#include "MovieSceneMetaData.h"
#include "MovieSceneNameableTrack.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSection.h"
#include "MovieSceneSequence.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneTrack.h"
#include "TemplateSequence.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "UniversalObjectLocator.h"
#include "UniversalObjectLocatorStringParams.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph::Sequencer
{
namespace SequencerReadLocal
{
bool IsDeclaredBy(const FProperty& Property, std::initializer_list<const UClass*> Bases)
{
	const UClass* Owner = Property.GetOwnerClass();
	for (const UClass* Base : Bases)
	{
		for (const UClass* Class = Base; Class != nullptr; Class = Class->GetSuperClass())
		{
			if (Owner == Class)
			{
				return true;
			}
		}
	}
	return false;
}

FValue MakeRange(const TRange<FFrameNumber>& Range, const FTimeBase& Time)
{
	FObjectBuilder Object;
	if (Range.GetLowerBound().IsClosed())
	{
		const int32 Lower = Range.GetLowerBoundValue().Value + (Range.GetLowerBound().IsExclusive() ? 1 : 0);
		Object.Add(TEXT("Start"), MakeFrames(Lower, Time));
	}
	if (Range.GetUpperBound().IsClosed())
	{
		const int32 Upper = Range.GetUpperBoundValue().Value + (Range.GetUpperBound().IsInclusive() ? 1 : 0);
		Object.Add(TEXT("End"), MakeFrames(Upper, Time));
	}
	return Object.Build();
}

FValue MakeColor(const FColor& Color)
{
	FObjectBuilder Object;
	Object.AddInt(TEXT("R"), Color.R);
	Object.AddInt(TEXT("G"), Color.G);
	Object.AddInt(TEXT("B"), Color.B);
	Object.AddInt(TEXT("A"), Color.A);
	return Object.Build();
}

FValue MakeLinearColor(const FLinearColor& Color)
{
	FObjectBuilder Object;
	Object.Add(TEXT("R"), MakeNumber(ShortestFloat(Color.R)));
	Object.Add(TEXT("G"), MakeNumber(ShortestFloat(Color.G)));
	Object.Add(TEXT("B"), MakeNumber(ShortestFloat(Color.B)));
	Object.Add(TEXT("A"), MakeNumber(ShortestFloat(Color.A)));
	return Object.Build();
}

FString EnumToken(const UEnum* Enum, int64 Value)
{
	if (Enum == nullptr)
	{
		return FString::Printf(TEXT("%lld"), Value);
	}
	return Enum->GetNameStringByValue(Value);
}

// ---- header --------------------------------------------------------------------------------------------------------

FValue ReadSettings(const FSequenceModel& Model, FWarnings& Warnings)
{
	const UMovieScene& MovieScene = *Model.MovieScene;
	const UMovieScene& Defaults = *GetDefault<UMovieScene>();
	FObjectBuilder Settings;
	Settings.Add(TEXT("DisplayRate"), MakeRate(MovieScene.GetDisplayRate()));
	Settings.Add(TEXT("TickResolution"), MakeRate(MovieScene.GetTickResolution()));
	Settings.AddString(TEXT("Evaluation"), MovieScene.GetEvaluationType() == EMovieSceneEvaluationType::FrameLocked ? TEXT("FrameLocked") : TEXT("WithSubFrames"));
	if (MovieScene.GetClockSource() != Defaults.GetClockSource())
	{
		Settings.AddString(TEXT("Clock"), EnumToken(StaticEnum<EUpdateClockSource>(), static_cast<int64>(MovieScene.GetClockSource())));
		if (const UObject* Custom = MovieScene.GetCustomClock())
		{
			FEncodeContext Context{Model, Model.Time, Warnings, TEXT("/Body/Settings/CustomClock")};
			FObjectBuilder Clock;
			Clock.AddString(TEXT("Class"), Custom->GetClass()->GetPathName());
			Clock.Add(TEXT("Properties"), EncodeSparseObject(*Custom->GetClass(), Custom, Custom->GetClass()->GetDefaultObject(),
				[](const FProperty& Property) { return Property.HasAnyPropertyFlags(CPF_Edit); }, Context));
			Settings.Add(TEXT("CustomClock"), Clock.Build());
		}
	}
	Settings.Add(TEXT("PlaybackRange"), MakeRange(MovieScene.GetPlaybackRange(), Model.Time));
	const TRange<FFrameNumber> Selection = MovieScene.GetSelectionRange();
	if (!Selection.IsEmpty() && Selection != Defaults.GetSelectionRange())
	{
		Settings.Add(TEXT("SelectionRange"), MakeRange(Selection, Model.Time));
	}
	if (MovieScene.IsPlaybackRangeLocked())
	{
		Settings.AddBool(TEXT("PlaybackRangeLocked"), true);
	}
	if (MovieScene.IsReadOnly())
	{
		Settings.AddBool(TEXT("ReadOnly"), true);
	}
	if (MovieScene.AreMarkedFramesLocked())
	{
		Settings.AddBool(TEXT("MarkersLocked"), true);
	}
	if (MovieScene.GetGloballyShowMarkedFrames())
	{
		Settings.AddBool(TEXT("ShowMarkers"), true);
	}
	return Settings.Build();
}

FValue ReadFolders(const FSequenceModel& Model)
{
	TArray<FValue> Folders;
#if WITH_EDITORONLY_DATA
	const FColor DefaultColor = GetDefault<UMovieSceneFolder>()->GetFolderColor();
	for (const FFolderInfo& Folder : Model.Folders)
	{
		FObjectBuilder Entry;
		Entry.AddString(TEXT("Path"), Folder.Path);
		if (Folder.Folder->GetFolderColor() != DefaultColor)
		{
			Entry.Add(TEXT("Color"), MakeColor(Folder.Folder->GetFolderColor()));
		}
		Folders.Add(Entry.Build());
	}
#endif
	return MakeArray(MoveTemp(Folders));
}

FValue ReadMarkers(const FSequenceModel& Model)
{
	TArray<FMovieSceneMarkedFrame> Marks = Model.MovieScene->GetMarkedFrames();
	Marks.StableSort([](const FMovieSceneMarkedFrame& A, const FMovieSceneMarkedFrame& B) { return A.FrameNumber < B.FrameNumber; });
	TArray<FValue> Markers;
	for (const FMovieSceneMarkedFrame& Mark : Marks)
	{
		FObjectBuilder Entry;
		Entry.Add(TEXT("Frame"), MakeFrames(Mark.FrameNumber.Value, Model.Time));
		if (!Mark.Label.IsEmpty())
		{
			Entry.AddString(TEXT("Label"), Mark.Label);
		}
#if WITH_EDITORONLY_DATA
		if (!Mark.Comment.IsEmpty())
		{
			Entry.AddString(TEXT("Comment"), Mark.Comment);
		}
		if (Mark.bUseCustomColor)
		{
			Entry.Add(TEXT("Color"), MakeLinearColor(Mark.CustomColor));
		}
#endif
		if (Mark.bIsDeterminismFence)
		{
			Entry.AddBool(TEXT("DeterminismFence"), true);
		}
		if (Mark.bIsInclusiveTime)
		{
			Entry.AddBool(TEXT("Inclusive"), true);
		}
		Markers.Add(Entry.Build());
	}
	return MakeArray(MoveTemp(Markers));
}

FValue ReadMetaData(const UMovieSceneSequence& Sequence)
{
	FObjectBuilder Object;
	if (const ULevelSequence* Level = Cast<ULevelSequence>(&Sequence))
	{
		if (const UMovieSceneMetaData* MetaData = Level->FindMetaData<UMovieSceneMetaData>())
		{
			if (!MetaData->GetAuthor().IsEmpty())
			{
				Object.AddString(TEXT("Author"), MetaData->GetAuthor());
			}
			if (!MetaData->GetNotes().IsEmpty())
			{
				Object.AddString(TEXT("Notes"), MetaData->GetNotes());
			}
		}
	}
	return Object.Build();
}

// ---- bindings ------------------------------------------------------------------------------------------------------

FString PossessedClassPath(const FMovieScenePossessable& Possessable)
{
	static const FSoftClassProperty* Property = FindFProperty<FSoftClassProperty>(FMovieScenePossessable::StaticStruct(), TEXT("PossessedObjectClass"));
	if (Property == nullptr)
	{
		return FString();
	}
	return Property->GetPropertyValue_InContainer(&Possessable).ToSoftObjectPath().ToString();
}

} // namespace

bool LocatorRoundTrips(const FUniversalObjectLocator& Locator, const FString& Text)
{
	FUniversalObjectLocator Parsed;
	const UE::UniversalObjectLocator::FParseStringParams Params;
	return Parsed.TryParseString(Text, Params).bSuccess && Parsed == Locator;
}

// The locator's string with its payloads as written (some engines percent-encode them; the parser reads both forms).
FString LocatorString(const FUniversalObjectLocator& Locator)
{
	TStringBuilder<512> Text;
	Locator.ToString(Text);
	FString Result = Text.ToString();
	if (Result.Contains(TEXT("%")))
	{
		FString Decoded = FGenericPlatformHttp::UrlDecode(Result);
		if (LocatorRoundTrips(Locator, Decoded))
		{
			Result = MoveTemp(Decoded);
		}
	}
	return Result;
}

bool IsComponentReference(const FProperty& Property)
{
	const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property);
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		Object = CastField<FObjectPropertyBase>(Array->Inner);
	}
	return Object != nullptr && (Property.HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference)
		|| (Object->PropertyClass != nullptr && Object->PropertyClass->IsChildOf(UActorComponent::StaticClass())));
}

bool IsTemplateField(const UClass& Class, const FProperty& Property)
{
	if (!Property.HasAnyPropertyFlags(CPF_Edit) || Property.HasAnyPropertyFlags(CPF_EditConst) || IsComponentReference(Property))
	{
		return false;
	}
	const FString& Category = Property.GetMetaData(TEXT("Category"));
	return Category.IsEmpty() || !FEditorCategoryUtils::IsCategoryHiddenFromClass(&Class, Category);
}

FTemplateBaseline::FTemplateBaseline(UClass& ActorClass)
	: Class(&ActorClass)
{
	if (ActorClass.IsChildOf(AActor::StaticClass()) && !ActorClass.HasAnyClassFlags(CLASS_Abstract))
	{
		Fresh.Reset(NewObject<AActor>(GetTransientPackage(), &ActorClass, NAME_None, RF_Transient));
		// The notifications a written template gets (WriteTemplate) and a loaded one's PostLoad recompute derived
		// fields from the stored ones; a new instance still holds its archetype's (a Cine Camera's FieldOfView and
		// AspectRatio from the class default object's construction).
		TArray<UActorComponent*> Components;
		Fresh->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (Component != nullptr)
			{
				Component->PostEditChange();
			}
		}
		Fresh->PostEditChange();
	}
}

const UObject* FTemplateBaseline::Actor() const
{
	return Fresh.IsValid() ? static_cast<const UObject*>(Fresh.Get()) : Class->GetDefaultObject();
}

const UObject* FTemplateBaseline::Component(const UActorComponent& Component) const
{
	if (Fresh.IsValid())
	{
		TArray<UActorComponent*> Components;
		Fresh->GetComponents(Components);
		for (const UActorComponent* Candidate : Components)
		{
			if (Candidate != nullptr && Candidate->GetFName() == Component.GetFName() && Candidate->GetClass() == Component.GetClass())
			{
				return Candidate;
			}
		}
	}
	return Component.GetArchetype();
}

namespace SequencerReadLocal
{
// Aliases a read adds for the fields it encodes under other names, dropped when it is done.
struct FScopedDefaultsAliases
{
	explicit FScopedDefaultsAliases(FWarnings& InWarnings)
		: Warnings(InWarnings), Count(InWarnings.DefaultsAliases.Num())
	{
	}
	~FScopedDefaultsAliases()
	{
		Warnings.DefaultsAliases.SetNum(Count);
	}
	FScopedDefaultsAliases(const FScopedDefaultsAliases&) = delete;
	FScopedDefaultsAliases& operator=(const FScopedDefaultsAliases&) = delete;

	FWarnings& Warnings;
	int32 Count;
};

// The spawnable's saved default state (design §3.4.3): actor fields and per-component fields that differ from a fresh
// actor of the class (FTemplateBaseline).
FValue ReadTemplate(AActor& Template, FEncodeContext& Context)
{
	const FTemplateBaseline Baseline(*Template.GetClass());
	FObjectBuilder Object;
	FEncodeContext ActorContext{Context.Model, Context.Time, Context.Warnings, Context.Path + TEXT("/Properties")};
	const UClass& ActorClass = *Template.GetClass();
	FValue Properties = EncodeSparseObject(ActorClass, &Template, Baseline.Actor(),
		[&ActorClass](const FProperty& Property) { return IsTemplateField(ActorClass, Property); }, ActorContext);
	if (!Properties.ObjectKeys.IsEmpty())
	{
		Object.Add(TEXT("Properties"), MoveTemp(Properties));
	}
	TArray<UActorComponent*> Components;
	Template.GetComponents(Components);
	Components.Sort([](const UActorComponent& A, const UActorComponent& B) { return A.GetName() < B.GetName(); });
	FObjectBuilder ComponentObject;
	bool bAny = false;
	for (const UActorComponent* Component : Components)
	{
		const UObject* Defaults = Component != nullptr && Component->GetArchetype() != nullptr ? Baseline.Component(*Component) : nullptr;
		if (Defaults == nullptr)
		{
			continue;
		}
		FEncodeContext ComponentContext{Context.Model, Context.Time, Context.Warnings,
			Context.Path + TEXT("/Components/") + Escape(Component->GetName())};
		const UClass& ComponentClass = *Component->GetClass();
		FValue Fields = EncodeSparseObject(ComponentClass, Component, Defaults,
			[&ComponentClass](const FProperty& Property) { return IsTemplateField(ComponentClass, Property); }, ComponentContext);
		if (!Fields.ObjectKeys.IsEmpty())
		{
			ComponentObject.Add(Component->GetName(), MoveTemp(Fields));
			bAny = true;
		}
	}
	if (bAny)
	{
		Object.Add(TEXT("Components"), ComponentObject.Build());
	}
	return Object.Build();
}

FValue ReadCustomBinding(const UMovieSceneCustomBinding& Binding, FEncodeContext& Context)
{
	FObjectBuilder Object;
	Object.AddString(TEXT("Class"), Binding.GetClass()->GetPathName());
	FEncodeContext Inner{Context.Model, Context.Time, Context.Warnings, Context.Path + TEXT("/Properties")};
	Object.Add(TEXT("Properties"), EncodeSparseObject(*Binding.GetClass(), &Binding, Binding.GetClass()->GetDefaultObject(),
		[](const FProperty& Property) { return !IsComponentReference(Property); }, Inner));
	return Object.Build();
}

FValue ReadSpawnable(UMovieSceneSpawnableActorBinding& Binding, FEncodeContext& Context)
{
	const TConstArrayView<TPair<const TCHAR*, const TCHAR*>> Known = SpawnableKnownFields();
	FObjectBuilder Object;
	AActor* Template = Cast<AActor>(Binding.GetObjectTemplate());
	if (Template != nullptr)
	{
		Object.AddString(TEXT("Class"), Template->GetClass()->GetPathName());
		FEncodeContext TemplateContext{Context.Model, Context.Time, Context.Warnings, Context.Path + TEXT("/Template")};
		FValue Saved = ReadTemplate(*Template, TemplateContext);
		if (!Saved.ObjectKeys.IsEmpty())
		{
			Object.Add(TEXT("Template"), MoveTemp(Saved));
		}
	}
	FEncodeContext FieldContext{Context.Model, Context.Time, Context.Warnings, Context.Path};
	// The fields are encoded at the spawnable's path under their own names; the document lists the known ones under
	// its names and the others under Properties.
	const FScopedDefaultsAliases Aliases(Context.Warnings);
	for (const TPair<const TCHAR*, const TCHAR*>& Entry : Known)
	{
		Context.Warnings.DefaultsAliases.Emplace(Context.Path + TEXT("/") + Entry.Value, Context.Path + TEXT("/") + Entry.Key);
	}
	Context.Warnings.DefaultsAliases.Emplace(Context.Path + TEXT("/Properties"), Context.Path);
	FValue Fields = EncodeSparseObject(*Binding.GetClass(), &Binding, Binding.GetClass()->GetDefaultObject(),
		[](const FProperty& Property) { return Property.GetFName() != TEXT("ActorTemplate") && !IsComponentReference(Property); }, FieldContext);
	FObjectBuilder Others;
	bool bOthers = false;
	for (int32 Index = 0; Index < Fields.ObjectKeys.Num(); ++Index)
	{
		const TPair<const TCHAR*, const TCHAR*>* Mapped = nullptr;
		for (const TPair<const TCHAR*, const TCHAR*>& Entry : Known)
		{
			Mapped = Fields.ObjectKeys[Index] == Entry.Key ? &Entry : Mapped;
		}
		if (Mapped != nullptr)
		{
			Object.Add(Mapped->Value, Fields.Elements[Index]);
		}
		else
		{
			Others.Add(Fields.ObjectKeys[Index], Fields.Elements[Index]);
			bOthers = true;
		}
	}
	if (bOthers)
	{
		Object.Add(TEXT("Properties"), Others.Build());
	}
	return Object.Build();
}

// A template's root spawnable (design §3.4.3): the bound actor class, the saved template and the spawnable's fields
// that differ from a new spawnable.
FValue ReadRoot(const FSequenceModel& Model, const FMovieSceneSpawnable& Spawnable, FEncodeContext& Context)
{
	FObjectBuilder Object;
	const UTemplateSequence* Template = Cast<UTemplateSequence>(Model.Sequence);
	AActor* Actor = Cast<AActor>(const_cast<UObject*>(Spawnable.GetObjectTemplate()));
	FString Class = Template != nullptr ? Template->BoundActorClass.ToSoftObjectPath().ToString() : FString();
	if (Class.IsEmpty() && Actor != nullptr)
	{
		Class = Actor->GetClass()->GetPathName();
	}
	Object.AddString(TEXT("Class"), Class);
	if (Actor != nullptr)
	{
		FEncodeContext TemplateContext{Context.Model, Context.Time, Context.Warnings, Context.Path + TEXT("/Template")};
		FValue Saved = ReadTemplate(*Actor, TemplateContext);
		if (!Saved.ObjectKeys.IsEmpty())
		{
			Object.Add(TEXT("Template"), MoveTemp(Saved));
		}
	}
	const FMovieSceneSpawnable Defaults = RootSpawnableDefaults(Actor);
	const TConstArrayView<TPair<const TCHAR*, const TCHAR*>> Known = RootSpawnableFields();
	auto Mapped = [&Known](const FString& Field) -> const TCHAR*
	{
		for (const TPair<const TCHAR*, const TCHAR*>& Entry : Known)
		{
			if (Field == Entry.Value)
			{
				return Entry.Key;
			}
		}
		return nullptr;
	};
	const FScopedDefaultsAliases Aliases(Context.Warnings);
	for (const TPair<const TCHAR*, const TCHAR*>& Entry : Known)
	{
		Context.Warnings.DefaultsAliases.Emplace(Context.Path + TEXT("/") + Entry.Key, Context.Path + TEXT("/") + Entry.Value);
	}
	FValue Fields = EncodeSparseObject(*FMovieSceneSpawnable::StaticStruct(), &Spawnable, &Defaults,
		[&Mapped](const FProperty& Property) { return Mapped(Property.GetName()) != nullptr; }, Context);
	for (int32 Index = 0; Index < Fields.ObjectKeys.Num(); ++Index)
	{
		Object.Add(Mapped(Fields.ObjectKeys[Index]), Fields.Elements[Index]);
	}
	return Object.Build();
}

void AddTags(FObjectBuilder& Entry, const TArray<FName>& Values)
{
	if (!Values.IsEmpty())
	{
		TArray<FString> Tags;
		for (const FName& Tag : Values)
		{
			Tags.Add(Tag.ToString());
		}
		Entry.Add(TEXT("Tags"), MakeStringArray(Tags));
	}
}

void AddBindingRecord(const FSequenceModel& Model, const FBindingInfo& Info, FObjectBuilder& Entry, TArray<FValue>& Records)
{
	if (!Info.Parent.IsValid() || !Model.Bindings.Contains(Info.Parent))
	{
		if (const FString* Folder = Model.BindingFolders.Find(Info.Guid))
		{
			Entry.AddString(TEXT("Folder"), *Folder);
		}
	}
	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("binding"));
	Record.AddString(TEXT("id"), Info.Guid.ToString(EGuidFormats::Digits));
	Record.AddString(TEXT("parent"), Model.Bindings.Contains(Info.Parent) ? Info.Parent.ToString(EGuidFormats::Digits) : FString());
	Record.Add(TEXT("entry"), Entry.Build());
	Records.Add(Record.Build());
}

void AddBinding(const FSequenceModel& Model, const FBindingInfo& Info, TArray<FValue>& Records, FWarnings& Warnings)
{
	UMovieScene& MovieScene = *const_cast<UMovieScene*>(Model.MovieScene);
	const FMovieScenePossessable* Possessable = MovieScene.FindPossessable(Info.Guid);
	FObjectBuilder Entry;
	Entry.AddString(TEXT("Name"), Info.Name);
	if (Info.bNeedsId)
	{
		Entry.AddString(TEXT("Id"), Info.Guid.ToString(EGuidFormats::Digits));
	}
	if (Model.bTemplate)
	{
		if (Info.bRoot)
		{
			const FMovieSceneSpawnable& Spawnable = *MovieScene.FindSpawnable(Info.Guid);
			FEncodeContext RootContext{Model, Model.Time, Warnings, Info.DocumentPath};
			Entry.Add(TEXT("Root"), ReadRoot(Model, Spawnable, RootContext));
			AddTags(Entry, Spawnable.Tags);
		}
		else if (Possessable != nullptr)
		{
			if (!PossessedClassPath(*Possessable).IsEmpty())
			{
				Entry.AddString(TEXT("Class"), PossessedClassPath(*Possessable));
			}
			Entry.AddString(TEXT("Component"), TemplateComponentName(*Model.Sequence, Info.Guid));
			AddTags(Entry, Possessable->Tags);
		}
		AddBindingRecord(Model, Info, Entry, Records);
		return;
	}
	const FMovieSceneBindingReferences* References = Model.Sequence->GetBindingReferences();
	const TArrayView<const FMovieSceneBindingReference> Refs = References != nullptr ? References->GetReferences(Info.Guid)
		: TArrayView<const FMovieSceneBindingReference>();
	int32 Custom = 0;
	for (const FMovieSceneBindingReference& Reference : Refs)
	{
		Custom += Reference.CustomBinding != nullptr ? 1 : 0;
	}
	FEncodeContext Context{Model, Model.Time, Warnings, Info.DocumentPath};
	if (Custom == 0)
	{
		if (Possessable != nullptr && !PossessedClassPath(*Possessable).IsEmpty())
		{
			Entry.AddString(TEXT("Class"), PossessedClassPath(*Possessable));
		}
		TArray<FValue> Locators;
		for (int32 Index = 0; Index < Refs.Num(); ++Index)
		{
			const FString Text = LocatorString(Refs[Index].Locator);
			if (!LocatorRoundTrips(Refs[Index].Locator, Text))
			{
				Warnings.Add(TEXT("Sequencer.UnsupportedLocator"), FString::Printf(TEXT("The locator '%s' does not parse back to the same "
					"locator; it is shown as text and kept as it is."), *Text), FString::Printf(TEXT("%s/Bind/%d"), *Info.DocumentPath, Index));
			}
			Locators.Add(MakeString(Text));
		}
		Entry.Add(TEXT("Bind"), MakeArray(MoveTemp(Locators)));
	}
	else if (Custom == 1 && Refs.Num() == 1)
	{
		UMovieSceneCustomBinding* Binding = Refs[0].CustomBinding;
		UMovieSceneSpawnableActorBinding* Spawnable = Cast<UMovieSceneSpawnableActorBinding>(Binding);
		if (Spawnable != nullptr && Spawnable->GetClass() == UMovieSceneSpawnableActorBinding::StaticClass())
		{
			FEncodeContext SpawnableContext{Model, Model.Time, Warnings, Info.DocumentPath + TEXT("/Spawnable")};
			Entry.Add(TEXT("Spawnable"), ReadSpawnable(*Spawnable, SpawnableContext));
		}
		else
		{
			FEncodeContext CustomContext{Model, Model.Time, Warnings, Info.DocumentPath + TEXT("/CustomBinding")};
			Entry.Add(TEXT("CustomBinding"), ReadCustomBinding(*Binding, CustomContext));
		}
	}
	else
	{
		Warnings.Add(TEXT("Sequencer.UnsupportedBindingShape"), TEXT("This binding mixes locators and custom bindings (or has several custom "
			"bindings); its references are shown raw and kept as they are."), Info.DocumentPath);
		TArray<FValue> Raw;
		for (int32 Index = 0; Index < Refs.Num(); ++Index)
		{
			FObjectBuilder Row;
			if (Refs[Index].CustomBinding != nullptr)
			{
				FEncodeContext CustomContext{Model, Model.Time, Warnings, FString::Printf(TEXT("%s/References/%d/CustomBinding"), *Info.DocumentPath, Index)};
				Row.Add(TEXT("CustomBinding"), ReadCustomBinding(*Refs[Index].CustomBinding, CustomContext));
			}
			else
			{
				Row.AddString(TEXT("Locator"), LocatorString(Refs[Index].Locator));
			}
			Raw.Add(Row.Build());
		}
		Entry.Add(TEXT("References"), MakeArray(MoveTemp(Raw)));
	}
	if (Possessable != nullptr)
	{
		AddTags(Entry, Possessable->Tags);
	}
	AddBindingRecord(Model, Info, Entry, Records);
}

// ---- tracks and sections -------------------------------------------------------------------------------------------

const FName SectionHeaderFields[] = {TEXT("SectionRange"), TEXT("RowIndex"), TEXT("OverlapPriority"), TEXT("bIsActive"), TEXT("bIsLocked"),
	TEXT("BlendType"), TEXT("Easing"), TEXT("PreRollFrames"), TEXT("PostRollFrames"), TEXT("EvalOptions"), TEXT("bSupportsInfiniteRange")};
// Derived or editor-view state of section classes (design §2.2).
const FName SectionDerivedFields[] = {TEXT("InitialCameraCutTransform"), TEXT("bHasInitialCameraCutTransform"), TEXT("ThumbnailReferenceOffset"),
	TEXT("OverrideRegistry"), TEXT("ChannelsUsedBitmap") /* custom primitive data: derived from its parameters */};
const FName TrackOwnedFields[] = {TEXT("DisplayName"), TEXT("bIsEvalDisabled"), TEXT("RowsDisabled"), TEXT("LocalRowsDisabled"),
	TEXT("TrackRowMetadata"), TEXT("SortingOrder"), TEXT("PropertyBinding"), TEXT("bSupportsDefaultSections"),
	TEXT("ObjectGuid") /* UMovieSceneSpawnTrack: its binding's GUID (SetObjectId when the track is added) */};

template <int32 N>
bool IsListed(const FProperty& Property, const FName (&Names)[N])
{
	for (const FName& Name : Names)
	{
		if (Property.GetFName() == Name)
		{
			return true;
		}
	}
	return false;
}

void AddSection(const FSequenceModel& Model, const FTrackInfo& Track, const UMovieSceneSection& Section, int32 Index, TArray<FValue>& Records,
	FWarnings& Warnings)
{
	const UMovieSceneSection& Defaults = *CastChecked<UMovieSceneSection>(Section.GetClass()->GetDefaultObject());
	const FString Path = FString::Printf(TEXT("%s/Sections/%d"), *Track.DocumentPath, Index);
	FObjectBuilder Entry;
	if (HasSeveralSectionClasses(*Track.Track->GetClass()))
	{
		Entry.AddString(TEXT("Class"), SectionClassToken(*Section.GetClass(), *Track.Track->GetClass()));
	}
	const TRange<FFrameNumber> Range = Section.GetRange();
	FValue Bounds = MakeRange(Range, Model.Time);
	for (int32 Field = 0; Field < Bounds.ObjectKeys.Num(); ++Field)
	{
		Entry.Add(Bounds.ObjectKeys[Field], Bounds.Elements[Field]);
	}
	if (Section.GetRowIndex() != 0)
	{
		Entry.AddInt(TEXT("Row"), Section.GetRowIndex());
	}
	if (Section.GetOverlapPriority() != 0)
	{
		Entry.AddInt(TEXT("OverlapPriority"), Section.GetOverlapPriority());
	}
	if (Section.IsActive() != Defaults.IsActive())
	{
		Entry.AddBool(TEXT("Active"), Section.IsActive());
	}
	if (Section.IsLocked() != Defaults.IsLocked())
	{
		Entry.AddBool(TEXT("Locked"), Section.IsLocked());
	}
	if (const int32* Group = Model.SectionGroups.Find(&Section))
	{
		Entry.AddInt(TEXT("Group"), *Group);
	}
	const FOptionalMovieSceneBlendType Blend = Section.GetBlendType();
	const FOptionalMovieSceneBlendType DefaultBlend = NewSectionBlend(Section);
	if (Blend.IsValid() != DefaultBlend.IsValid() || (Blend.IsValid() && Blend.Get() != DefaultBlend.Get()))
	{
		Entry.Add(TEXT("Blend"), Blend.IsValid() ? MakeString(EnumToken(StaticEnum<EMovieSceneBlendType>(), static_cast<int64>(Blend.Get())))
			: MakeNull());
	}
	{
		FObjectBuilder Easing;
		bool bEasing = false;
		if (Section.Easing.bManualEaseIn)
		{
			Easing.Add(TEXT("In"), MakeFrames(Section.Easing.ManualEaseInDuration, Model.Time));
			bEasing = true;
		}
		if (Section.Easing.bManualEaseOut)
		{
			Easing.Add(TEXT("Out"), MakeFrames(Section.Easing.ManualEaseOutDuration, Model.Time));
			bEasing = true;
		}
		auto AddFunction = [&](const TCHAR* Key, const UObject* Function, const UObject* DefaultFunction)
		{
			if (Function == nullptr)
			{
				return;
			}
			FEncodeContext Context{Model, Model.Time, Warnings, Path + TEXT("/Easing/") + Key};
			const UObject* Baseline = DefaultFunction != nullptr && DefaultFunction->GetClass() == Function->GetClass()
				? DefaultFunction : Function->GetClass()->GetDefaultObject();
			FValue Fields = EncodeSparseObject(*Function->GetClass(), Function, Baseline, [](const FProperty& Property) { return Property.HasAnyPropertyFlags(CPF_Edit); },
				Context);
			if (DefaultFunction != nullptr && DefaultFunction->GetClass() == Function->GetClass() && Fields.ObjectKeys.IsEmpty())
			{
				return;
			}
			FObjectBuilder Object;
			Object.AddString(TEXT("Class"), Function->GetClass()->GetPathName());
			if (!Fields.ObjectKeys.IsEmpty())
			{
				Object.Add(TEXT("Properties"), MoveTemp(Fields));
			}
			Easing.Add(Key, Object.Build());
			bEasing = true;
		};
		AddFunction(TEXT("InFunction"), Section.Easing.EaseIn.GetObject(), Defaults.Easing.EaseIn.GetObject());
		AddFunction(TEXT("OutFunction"), Section.Easing.EaseOut.GetObject(), Defaults.Easing.EaseOut.GetObject());
		if (bEasing)
		{
			Entry.Add(TEXT("Easing"), Easing.Build());
		}
	}
	if (Section.GetPreRollFrames() != 0)
	{
		Entry.Add(TEXT("PreRoll"), MakeFrames(Section.GetPreRollFrames(), Model.Time));
	}
	if (Section.GetPostRollFrames() != 0)
	{
		Entry.Add(TEXT("PostRoll"), MakeFrames(Section.GetPostRollFrames(), Model.Time));
	}
	if (Section.EvalOptions.CompletionMode != Defaults.EvalOptions.CompletionMode)
	{
		Entry.AddString(TEXT("WhenFinished"), EnumToken(StaticEnum<EMovieSceneCompletionMode>(), static_cast<int64>(Section.EvalOptions.CompletionMode)));
	}

	// Properties (the section class's own fields; sub-section offsets are in the inner sequence's time, design §3.5.4).
	FEncodeContext Context{Model, Model.Time, Warnings, Path + TEXT("/Properties")};
	if (const FObjectProperty* SubSequence = FindFProperty<FObjectProperty>(Section.GetClass(), TEXT("SubSequence")))
	{
		const UMovieSceneSequence* Inner = Cast<UMovieSceneSequence>(SubSequence->GetObjectPropertyValue_InContainer(&Section));
		if (Inner != nullptr && Inner->GetMovieScene() != nullptr)
		{
			Context.Time.Tick = Inner->GetMovieScene()->GetTickResolution();
			Context.Time.Display = Inner->GetMovieScene()->GetDisplayRate();
		}
	}
	const FProperty* EventProperty = nullptr;
	FValue Properties = EncodeSparseObject(*Section.GetClass(), &Section, &Defaults, [&EventProperty](const FProperty& Property)
	{
		const FStructProperty* Struct = CastField<FStructProperty>(&Property);
		if (Struct != nullptr && Struct->Struct == FMovieSceneEvent::StaticStruct())
		{
			EventProperty = &Property;
			return false;
		}
		return IsSectionPropertyField(Property);
	}, Context);
	if (!Properties.ObjectKeys.IsEmpty())
	{
		Entry.Add(TEXT("Properties"), MoveTemp(Properties));
	}
	if (EventProperty != nullptr)
	{
		FEncodeContext EventContext{Model, Model.Time, Warnings, Path + TEXT("/Event")};
		Entry.Add(TEXT("Event"), EncodeEvent(*EventProperty->ContainerPtrToValuePtr<FMovieSceneEvent>(&Section), EventContext));
	}
	if (const FObjectProperty* Registry = FindFProperty<FObjectProperty>(Section.GetClass(), TEXT("OverrideRegistry")))
	{
		if (Registry->GetObjectPropertyValue_InContainer(&Section) != nullptr)
		{
			Warnings.Add(TEXT("Sequencer.UnsupportedChannelOverride"), TEXT("This section has procedural channel overrides (channel override "
				"registry); they are kept as they are and cannot be written yet."), Path);
			Entry.AddString(TEXT("Unsupported"), TEXT("ChannelOverrides"));
		}
	}
	FObjectBuilder Channels;
	TArray<FValue> KeyRecords;
	FEncodeContext ChannelContext{Model, Model.Time, Warnings, Path};
	ReadChannels(Section, ChannelContext, Channels, KeyRecords);
	Entry.Add(TEXT("Channels"), Channels.Build());
	if (UsesParameters(Section))
	{
		FValue Parameters = ReadParameters(Section, ChannelContext, KeyRecords);
		if (!Parameters.Elements.IsEmpty())
		{
			Entry.Add(TEXT("Parameters"), MoveTemp(Parameters));
		}
	}

	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("section"));
	Record.AddString(TEXT("object"), Section.GetName());
	Record.Add(TEXT("entry"), Entry.Build());
	Records.Add(Record.Build());
	Records.Append(MoveTemp(KeyRecords));
}

void AddTrack(const FSequenceModel& Model, const FTrackInfo& Info, TArray<FValue>& Records, FWarnings& Warnings)
{
	const UMovieSceneTrack& Track = *Info.Track;
	FObjectBuilder Entry;
	Entry.AddString(TEXT("Type"), Info.Type);
	if (!Info.Property.IsEmpty() && !(Info.Type == TEXT("Transform") && Info.Property == TEXT("Transform")))
	{
		Entry.AddString(TEXT("Property"), Info.Property);
	}
	if (!Info.Name.IsEmpty())
	{
		Entry.AddString(TEXT("Name"), Info.Name);
	}
	if (Info.bNeedsId)
	{
		Entry.AddString(TEXT("Id"), Track.GetName());
	}
	if (!Info.Binding.IsValid())
	{
		if (const FString* Folder = Model.TrackFolders.Find(&Track))
		{
			Entry.AddString(TEXT("Folder"), *Folder);
		}
	}
	if (Track.IsEvalDisabled(false))
	{
		Entry.AddBool(TEXT("Muted"), true);
	}
	static const FArrayProperty* RowsDisabled = FindFProperty<FArrayProperty>(UMovieSceneTrack::StaticClass(), TEXT("RowsDisabled"));
	if (RowsDisabled != nullptr)
	{
		const TArray<int32>& Rows = *RowsDisabled->ContainerPtrToValuePtr<TArray<int32>>(&Track);
		if (!Rows.IsEmpty())
		{
			TArray<FValue> Values;
			for (const int32 Row : Rows)
			{
				Values.Add(MakeInt(Row));
			}
			Entry.Add(TEXT("MutedRows"), MakeArray(MoveTemp(Values)));
		}
	}
	// Row conditions (the track row's Details): {"<row>": condition} by row.
	static const FMapProperty* RowMetadata = FindFProperty<FMapProperty>(UMovieSceneTrack::StaticClass(), TEXT("TrackRowMetadata"));
	if (RowMetadata != nullptr)
	{
		const TMap<int32, FMovieSceneTrackRowMetadata>& Rows = *RowMetadata->ContainerPtrToValuePtr<TMap<int32, FMovieSceneTrackRowMetadata>>(&Track);
		TArray<int32> Indices;
		for (const TPair<int32, FMovieSceneTrackRowMetadata>& Pair : Rows)
		{
			if (Pair.Value.ConditionContainer.Condition != nullptr)
			{
				Indices.Add(Pair.Key);
			}
		}
		Indices.Sort();
		FObjectBuilder Conditions;
		for (const int32 Row : Indices)
		{
			FEncodeContext RowContext{Model, Model.Time, Warnings, FString::Printf(TEXT("%s/RowConditions/%d"), *Info.DocumentPath, Row)};
			Conditions.Add(FString::FromInt(Row), EncodeSubobject(Rows[Row].ConditionContainer.Condition, RowContext));
		}
		if (!Indices.IsEmpty())
		{
			Entry.Add(TEXT("RowConditions"), Conditions.Build());
		}
	}
	FEncodeContext Context{Model, Model.Time, Warnings, Info.DocumentPath + TEXT("/Properties")};
	FValue Properties = EncodeSparseObject(*Track.GetClass(), &Track, Track.GetClass()->GetDefaultObject(), &IsTrackPropertyField, Context);
	if (!Properties.ObjectKeys.IsEmpty())
	{
		Entry.Add(TEXT("Properties"), MoveTemp(Properties));
	}
	if (Track.GetClass()->GetName().Contains(TEXT("ControlRig")))
	{
		Warnings.Add(TEXT("Sequencer.ControlRigTrack"), TEXT("Control Rig tracks are read through the generic channel proxy; authoring them "
			"belongs to the Control Rig profile."), Info.DocumentPath);
	}
	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("track"));
	Record.AddString(TEXT("binding"), Info.Binding.IsValid() ? Info.Binding.ToString(EGuidFormats::Digits) : FString());
	Record.AddString(TEXT("object"), Track.GetName());
	Record.Add(TEXT("entry"), Entry.Build());
	Records.Add(Record.Build());
	const TArray<UMovieSceneSection*>& Sections = Track.GetAllSections();
	for (int32 Index = 0; Index < Sections.Num(); ++Index)
	{
		if (Sections[Index] != nullptr)
		{
			AddSection(Model, Info, *Sections[Index], Index, Records, Warnings);
		}
	}
}

void AddOrphanWarnings(const FSequenceModel& Model, FWarnings& Warnings)
{
	for (const FGuid& Orphan : Model.Orphans)
	{
		Warnings.Add(TEXT("Sequencer.UnsupportedBindingShape"), FString::Printf(TEXT("Object binding %s is not part of the document (%s); its tracks "
			"are not read and are kept as they are."), *Orphan.ToString(EGuidFormats::Digits), Model.bTemplate
			? TEXT("a template sequence documents its root spawnable and the bindings below it") : TEXT("a legacy spawnable or a stale entry")),
			Model.bTemplate ? TEXT("/Body/Root") : TEXT("/Body/Bindings"));
	}
}
} // namespace

TConstArrayView<TPair<const TCHAR*, const TCHAR*>> RootSpawnableFields()
{
	static const TPair<const TCHAR*, const TCHAR*> Known[] = {
		{TEXT("Ownership"), TEXT("Ownership")}, {TEXT("ContinuouslyRespawn"), TEXT("bContinuouslyRespawn")},
		{TEXT("NetAddressableName"), TEXT("bNetAddressableName")}, {TEXT("LevelName"), TEXT("LevelName")},
		{TEXT("SpawnTransform"), TEXT("SpawnTransform")}};
	return Known;
}

FString TemplateComponentName(const UMovieSceneSequence& Sequence, const FGuid& Guid)
{
	const UTemplateSequence* Template = Cast<UTemplateSequence>(&Sequence);
	const FName* Name = Template != nullptr ? Template->BoundActorComponents.Find(Guid) : nullptr;
	return Name != nullptr && !Name->IsNone() ? Name->ToString() : FString();
}

TConstArrayView<TPair<const TCHAR*, const TCHAR*>> SpawnableKnownFields()
{
	static const TPair<const TCHAR*, const TCHAR*> Known[] = {
		{TEXT("SpawnOwnership"), TEXT("Ownership")}, {TEXT("bContinuouslyRespawn"), TEXT("ContinuouslyRespawn")},
		{TEXT("bNetAddressableName"), TEXT("NetAddressableName")}, {TEXT("LevelName"), TEXT("LevelName")}};
	return Known;
}

bool IsSectionPropertyField(const FProperty& Property)
{
	using namespace SequencerReadLocal;
	if (!IsStoredField(Property) || IsChannelField(Property) || RefersToSections(Property) || IsListed(Property, SectionHeaderFields)
		|| IsListed(Property, SectionDerivedFields))
	{
		return false;
	}
	if (IsDeclaredBy(Property, {UMovieSceneSection::StaticClass()}))
	{
		return Property.HasAnyPropertyFlags(CPF_Edit) && !Property.HasAnyPropertyFlags(CPF_EditConst);
	}
	return true;
}

bool IsTrackPropertyField(const FProperty& Property)
{
	using namespace SequencerReadLocal;
	if (!IsStoredField(Property) || IsChannelField(Property) || RefersToSections(Property) || IsListed(Property, TrackOwnedFields))
	{
		return false;
	}
	if (IsDeclaredBy(Property, {UMovieSceneTrack::StaticClass(), UMovieSceneNameableTrack::StaticClass(), UMovieScenePropertyTrack::StaticClass()}))
	{
		return Property.HasAnyPropertyFlags(CPF_Edit) && !Property.HasAnyPropertyFlags(CPF_EditConst);
	}
	return true;
}

FValue DescribeDirector(const UMovieSceneSequence& Sequence)
{
	const ULevelSequence* Level = Cast<ULevelSequence>(&Sequence);
	const UBlueprint* Blueprint = Level != nullptr ? Level->GetDirectorBlueprint() : nullptr;
	if (Blueprint == nullptr)
	{
		return MakeNull();
	}
	auto Inputs = [](const UK2Node_EditablePinBase& Node)
	{
		TArray<FValue> Pins;
		for (const TSharedPtr<FUserPinInfo>& Pin : Node.UserDefinedPins)
		{
			if (!Pin.IsValid())
			{
				continue;
			}
			FObjectBuilder Type;
			Type.AddString(TEXT("Category"), Pin->PinType.PinCategory.ToString());
			if (!Pin->PinType.PinSubCategory.IsNone())
			{
				Type.AddString(TEXT("SubCategory"), Pin->PinType.PinSubCategory.ToString());
			}
			if (const UObject* Object = Pin->PinType.PinSubCategoryObject.Get())
			{
				Type.AddString(TEXT("SubCategoryObject"), Object->GetPathName());
			}
			FObjectBuilder Row;
			Row.Add(TEXT("Name"), MakeName(Pin->PinName));
			Row.Add(TEXT("Type"), Type.Build());
			Pins.Add(Row.Build());
		}
		return MakeArray(MoveTemp(Pins));
	};
	TArray<FValue> Endpoints;
	for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		for (const UEdGraphNode* Node : Graph != nullptr ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
		{
			if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				FObjectBuilder Row;
				Row.AddString(TEXT("Name"), EndpointName(Entry));
				Row.AddString(TEXT("Kind"), TEXT("Function"));
				Row.Add(TEXT("Inputs"), Inputs(*Entry));
				Endpoints.Add(Row.Build());
			}
		}
	}
	for (const UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		for (const UEdGraphNode* Node : Graph != nullptr ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
		{
			if (const UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Node))
			{
				FObjectBuilder Row;
				Row.AddString(TEXT("Name"), EndpointName(Event));
				Row.AddString(TEXT("Kind"), TEXT("CustomEvent"));
				Row.Add(TEXT("Inputs"), Inputs(*Event));
				Endpoints.Add(Row.Build());
			}
		}
	}
	FObjectBuilder Director;
	Director.AddString(TEXT("Blueprint"), Blueprint->GetPathName());
	Director.Add(TEXT("Endpoints"), MakeArray(MoveTemp(Endpoints)));
	return Director.Build();
}

bool HandleRead(FCall& Call, FObjectBuilder& Result)
{
	using namespace SequencerReadLocal;
	int64 Offset = 0;
	const FValue* DefaultsAt = nullptr;
	if (!Call.Args.OnlyFields({TEXT("offset"), TEXT("defaults_at")}) || !Call.Args.Int(TEXT("offset"), Offset, false)
		|| !Call.Args.Array(TEXT("defaults_at"), DefaultsAt, false))
	{
		return false;
	}
	if (Offset < 0)
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("offset must be >= 0."), TEXT("offset"));
	}
	FWarnings Warnings;
	if (DefaultsAt != nullptr)
	{
		for (const FValue& Path : DefaultsAt->Elements)
		{
			if (Path.Kind != EAssetDocumentRuntimeValueKind::String)
			{
				return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("defaults_at lists document paths."), TEXT("defaults_at"));
			}
			Warnings.DefaultsAt.Add(Path.StringValue);
		}
	}
	const UMovieSceneSequence* Sequence = Cast<UMovieSceneSequence>(&Call.Asset);
	if (Sequence == nullptr)
	{
		return Fail(Call, TEXT("Sequencer.UnsupportedAsset"), TEXT("seq.read.v1 reads Level and Template Sequences."), TEXT("/HandleArguments/asset"));
	}
	FSequenceModel Model;
	FString Error;
	if (!BuildModel(*Sequence, Model, Error))
	{
		return Fail(Call, TEXT("Sequencer.ReadFailed"), Error);
	}
	TArray<FValue> Records;
	{
		FObjectBuilder Header;
		Header.AddString(TEXT("kind"), TEXT("header"));
		Header.Add(TEXT("settings"), ReadSettings(Model, Warnings));
		Header.Add(TEXT("folders"), ReadFolders(Model));
		Header.Add(TEXT("markers"), ReadMarkers(Model));
		Header.Add(TEXT("director"), DescribeDirectorHeader(*Sequence));
		Header.Add(TEXT("groups"), ReadNodeGroups(Model));
		Header.Add(TEXT("metadata"), ReadMetaData(*Sequence));
		FObjectBuilder Counts;
		Counts.AddInt(TEXT("bindings"), Model.Order.Num());
		Counts.AddInt(TEXT("tracks"), Model.Tracks.Num());
		Header.Add(TEXT("counts"), Counts.Build());
		Records.Add(Header.Build());
	}
	for (const FGuid& Guid : Model.Order)
	{
		AddBinding(Model, Model.Bindings[Guid], Records, Warnings);
	}
	for (const FTrackInfo& Track : Model.Tracks)
	{
		AddTrack(Model, Track, Records, Warnings);
	}
	AddOrphanWarnings(Model, Warnings);
	Records.Append(MoveTemp(Warnings.Records));
	AddRecordPage(Records, Offset, Result);
	return true;
}
} // namespace UE::AssetDocument::Graph::Sequencer
