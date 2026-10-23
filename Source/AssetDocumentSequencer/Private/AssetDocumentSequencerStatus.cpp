// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// The read status of a Level Sequence ("Compile" of Extract / Inspect, design §5.1). A sequence has no compile button;
// the status is the profile's structural checks plus checks for the four rules of the editor's Sequence Validator
// (its runner is not exported, so the checks run here for this sequence; subsequences are checked when they are
// extracted themselves):
//   Errors:   a property track whose property path does not resolve on the bound class (Sequencer.UnknownProperty);
//             a camera cut / attach binding reference naming a binding the sequence does not have; a sub-sequence
//             cycle (the movie-scene compiler has no cycle guard).
//   Warnings: UnassignedBindingsAndAssets (a possessable with no object reference, a camera cut / attach without a
//             binding, a sub / shot section without a sequence, audio without a sound, skeletal animation without an
//             animation), DuplicateKeys, WholeSectionRanges, SectionAlignments (section bounds within two display frames
//             of a camera cut bound); an event without an endpoint.
// Template / Camera Animation Sequences (design §3.4.3) add: Errors: a bound actor class that is not an actor class a
// template can spawn (a camera actor class for a Camera Animation Sequence); Warnings: no root, a root whose template
// is not of the bound actor class, a component binding that names no component or one the root's class does not have.
// Status: UpToDate, UpToDateWithWarnings or Error. Messages carry document paths. Never changes the asset; locator
// resolution (world dependent) is reported by Inspect only.

#include "AssetDocumentSequencerPrimitives.h"

#include "AssetDocumentGraphValue.h"
#include "Channels/MovieSceneEvent.h"
#include "Dom/JsonObject.h"
#include "Bindings/MovieSceneCustomBinding.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBindingReferences.h"
#include "GameFramework/Actor.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSection.h"
#include "MovieSceneSpawnable.h"
#include "TemplateSequence.h"
#include "MovieSceneSequence.h"
#include "MovieSceneTrack.h"
#include "Sections/MovieScene3DConstraintSection.h"
#include "Sections/MovieSceneAudioSection.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneSkeletalAnimationSection.h"
#include "Sections/MovieSceneSubSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph::Sequencer
{
namespace
{
struct FStatusMessage
{
	FString Severity;
	FString Rule;
	FString Message;
	FString Path;
	FString Source;
};

FString SectionLabel(const FSequenceModel& Model, const UMovieSceneSection& Section)
{
	const UMovieSceneTrack* Track = Section.GetTypedOuter<UMovieSceneTrack>();
	const int32* Index = Track != nullptr ? Model.TrackIndex.Find(Track) : nullptr;
	const FString Type = Index != nullptr ? Model.Tracks[*Index].Type : FString(TEXT("Track"));
	return FString::Printf(TEXT("%s section %s"), *Type, *Model.SectionPath(Section));
}

// The property path of a property track on Class ("Struct.Member", array elements "Name[2]"); a path that names a
// Set<Name> function (the Sequencer keys setter-only properties) also resolves.
bool ResolvesOn(const UClass& Class, const FString& Path)
{
	TArray<FString> Parts;
	Path.ParseIntoArray(Parts, TEXT("."));
	const UStruct* Struct = &Class;
	for (int32 Index = 0; Index < Parts.Num(); ++Index)
	{
		FString Name = Parts[Index];
		int32 Bracket = INDEX_NONE;
		if (Name.FindChar(TEXT('['), Bracket))
		{
			Name.LeftInline(Bracket);
		}
		const FProperty* Property = Struct != nullptr ? Struct->FindPropertyByName(FName(*Name)) : nullptr;
		if (Property == nullptr)
		{
			return Index == Parts.Num() - 1 && Struct == &Class && Class.FindFunctionByName(FName(*(TEXT("Set") + Name))) != nullptr;
		}
		if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
		{
			Property = Array->Inner;
		}
		const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
		Struct = StructProperty != nullptr ? StructProperty->Struct : nullptr;
	}
	return true;
}

const UClass* BoundClass(const FSequenceModel& Model, const FGuid& Guid)
{
	UMovieScene& MovieScene = *const_cast<UMovieScene*>(Model.MovieScene);
	const FMovieScenePossessable* Possessable = MovieScene.FindPossessable(Guid);
	if (Possessable == nullptr)
	{
		const FMovieSceneSpawnable* Spawnable = MovieScene.FindSpawnable(Guid);
		const UObject* Template = Spawnable != nullptr ? Spawnable->GetObjectTemplate() : nullptr;
		return Template != nullptr ? Template->GetClass() : nullptr;
	}
	const FMovieSceneBindingReferences* References = Model.Sequence->GetBindingReferences();
	if (References != nullptr)
	{
		for (const FMovieSceneBindingReference& Reference : References->GetReferences(Guid))
		{
			if (Reference.CustomBinding != nullptr)
			{
				return Reference.CustomBinding->GetBoundObjectClass();
			}
		}
	}
#if WITH_EDITORONLY_DATA
	return Possessable->GetPossessedObjectClass();
#else
	return nullptr;
#endif
}

void CheckReference(const FSequenceModel& Model, const UMovieSceneSection& Section, const FMovieSceneObjectBindingID& Id, const TCHAR* What,
	TArray<FStatusMessage>& Out)
{
	const FString Path = Model.SectionPath(Section);
	if (!Id.IsValid())
	{
		Out.Add({TEXT("Warning"), TEXT("UnassignedBindingsAndAssets"), FString::Printf(TEXT("%s: the %s has no binding."), *SectionLabel(Model, Section), What),
			Path + TEXT("/Properties"), TEXT("SequenceValidator")});
		return;
	}
	if (Id.GetRelativeSequenceID() == MovieSceneSequenceID::Root && Model.FindBinding(Id.GetGuid()) == nullptr)
	{
		Out.Add({TEXT("Error"), FString(), FString::Printf(TEXT("%s: the %s names binding %s, which the sequence does not have."),
			*SectionLabel(Model, Section), What, *Id.GetGuid().ToString(EGuidFormats::Digits)), Path + TEXT("/Properties"), TEXT("Sequencer")});
	}
}

bool ReachesSequence(const UMovieSceneSequence* From, const UMovieSceneSequence* Target, TSet<const UMovieSceneSequence*>& Visited)
{
	if (From == nullptr || Visited.Contains(From))
	{
		return false;
	}
	Visited.Add(From);
	const UMovieScene* MovieScene = From->GetMovieScene();
	if (MovieScene == nullptr)
	{
		return false;
	}
	for (const UMovieSceneSection* Section : MovieScene->GetAllSections())
	{
		if (const UMovieSceneSubSection* Sub = Cast<UMovieSceneSubSection>(Section))
		{
			const UMovieSceneSequence* Inner = Sub->GetSequence();
			if (Inner == Target || ReachesSequence(Inner, Target, Visited))
			{
				return true;
			}
		}
	}
	return false;
}

void CheckEvent(const FSequenceModel& Model, const FMovieSceneEvent& Event, const FString& Path, const FString& Label, TArray<FStatusMessage>& Out)
{
#if WITH_EDITORONLY_DATA
	if (LiveEndpoint(Event.WeakEndpoint) == nullptr)
	{
		Out.Add({TEXT("Warning"), TEXT("UnassignedBindingsAndAssets"), FString::Printf(TEXT("%s: an event has no endpoint (it calls nothing)."), *Label),
			Path, TEXT("Sequencer")});
	}
#endif
}

void CheckSection(const FSequenceModel& Model, const UMovieSceneSection& Section, const TArray<int64>& CutTimes, TArray<FStatusMessage>& Out)
{
	const FString Path = Model.SectionPath(Section);
	const FString Label = SectionLabel(Model, Section);
	auto Unassigned = [&](const TCHAR* What)
	{
		Out.Add({TEXT("Warning"), TEXT("UnassignedBindingsAndAssets"), FString::Printf(TEXT("%s has no %s."), *Label, What), Path + TEXT("/Properties"),
			TEXT("SequenceValidator")});
	};
	if (const UMovieSceneAudioSection* Audio = Cast<UMovieSceneAudioSection>(&Section))
	{
		if (Audio->GetSound() == nullptr)
		{
			Unassigned(TEXT("sound"));
		}
	}
	else if (const UMovieSceneCameraCutSection* Cut = Cast<UMovieSceneCameraCutSection>(&Section))
	{
		CheckReference(Model, Section, Cut->GetCameraBindingID(), TEXT("camera cut"), Out);
	}
	else if (const UMovieScene3DConstraintSection* Constraint = Cast<UMovieScene3DConstraintSection>(&Section))
	{
		CheckReference(Model, Section, Constraint->GetConstraintBindingID(), TEXT("constraint"), Out);
	}
	else if (const UMovieSceneSubSection* Sub = Cast<UMovieSceneSubSection>(&Section))
	{
		if (Sub->GetSequence() == nullptr)
		{
			Unassigned(TEXT("sequence"));
		}
		else
		{
			TSet<const UMovieSceneSequence*> Visited;
			if (Sub->GetSequence() == Model.Sequence || ReachesSequence(Sub->GetSequence(), Model.Sequence, Visited))
			{
				Out.Add({TEXT("Error"), FString(), FString::Printf(TEXT("%s: %s contains this sequence (a sub-sequence cycle)."), *Label,
					*Sub->GetSequence()->GetPathName()), Path + TEXT("/Properties/SubSequence"), TEXT("Sequencer")});
			}
		}
	}
	else if (const UMovieSceneSkeletalAnimationSection* Animation = Cast<UMovieSceneSkeletalAnimationSection>(&Section))
	{
		if (Animation->GetAnimation() == nullptr)
		{
			Unassigned(TEXT("animation"));
		}
	}

	// Events: repeater sections hold one event, trigger sections an event channel.
	for (TFieldIterator<FStructProperty> It(Section.GetClass()); It; ++It)
	{
		if (It->Struct == FMovieSceneEvent::StaticStruct())
		{
			CheckEvent(Model, *It->ContainerPtrToValuePtr<FMovieSceneEvent>(&Section), Path + TEXT("/Event"), Label, Out);
		}
	}

	// DuplicateKeys.
	ForEachChannelKeyTimes(Section, [&](const FString& Channel, TConstArrayView<FFrameNumber> Times)
	{
		TSet<int32> Seen;
		int32 Duplicates = 0;
		for (const FFrameNumber& Time : Times)
		{
			bool bAlready = false;
			Seen.Add(Time.Value, &bAlready);
			Duplicates += bAlready ? 1 : 0;
		}
		if (Duplicates > 0)
		{
			Out.Add({TEXT("Warning"), TEXT("DuplicateKeys"), FString::Printf(TEXT("%s: channel %s contains %d duplicate key(s)."), *Label, *Channel, Duplicates),
				Path + TEXT("/Channels/") + Escape(Channel), TEXT("SequenceValidator")});
		}
	});

	// WholeSectionRanges (skeletal animation sections may end between frames). The bounds are the document's Start
	// (inclusive) and End (exclusive): a section ending inclusively on a whole frame ends one tick after it.
	const TRange<FFrameNumber> Range = Section.GetRange();
	const int32 LowerTicks = Range.HasLowerBound() ? Range.GetLowerBoundValue().Value + (Range.GetLowerBound().IsExclusive() ? 1 : 0) : 0;
	const int32 UpperTicks = Range.HasUpperBound() ? Range.GetUpperBoundValue().Value + (Range.GetUpperBound().IsInclusive() ? 1 : 0) : 0;
	if (Range.HasLowerBound() && !IsWholeFrame(LowerTicks, Model.Time))
	{
		Out.Add({TEXT("Warning"), TEXT("WholeSectionRanges"), FString::Printf(TEXT("%s: lower bound is not on a whole frame."), *Label), Path + TEXT("/Start"),
			TEXT("SequenceValidator")});
	}
	if (Range.HasUpperBound() && !Section.IsA<UMovieSceneSkeletalAnimationSection>() && !IsWholeFrame(UpperTicks, Model.Time))
	{
		Out.Add({TEXT("Warning"), TEXT("WholeSectionRanges"), FString::Printf(TEXT("%s: upper bound is not on a whole frame."), *Label), Path + TEXT("/End"),
			TEXT("SequenceValidator")});
	}

	// SectionAlignments: a bound within two display frames of a camera cut bound, but not on it.
	if (!CutTimes.IsEmpty() && !Section.IsA<UMovieSceneCameraCutSection>())
	{
		const int64 Threshold = FFrameRate::TransformTime(FFrameTime(2), Model.Time.Display, Model.Time.Tick).RoundToFrame().Value;
		auto Near = [&](int64 Time, const TCHAR* Key)
		{
			for (const int64 Cut : CutTimes)
			{
				const int64 Distance = FMath::Abs(Cut - Time);
				if (Distance != 0 && Distance <= Threshold)
				{
					Out.Add({TEXT("Warning"), TEXT("SectionAlignments"), FString::Printf(TEXT("%s: %s is close to a camera cut bound but not on it."), *Label,
						Key), Path + TEXT("/") + Key, TEXT("SequenceValidator")});
					return;
				}
			}
		};
		if (Range.HasLowerBound())
		{
			Near(LowerTicks, TEXT("Start"));
		}
		if (Range.HasUpperBound())
		{
			Near(UpperTicks, TEXT("End"));
		}
	}
}

void DescribeTemplate(const FSequenceModel& Model, TArray<FStatusMessage>& Out)
{
	const UTemplateSequence* Template = Cast<UTemplateSequence>(Model.Sequence);
	UMovieScene& MovieScene = *const_cast<UMovieScene*>(Model.MovieScene);
	if (Template == nullptr)
	{
		return;
	}
	const FString ClassPath = Template->BoundActorClass.ToSoftObjectPath().ToString();
	FString Code;
	FString Message;
	if (!ClassPath.IsEmpty() && !CheckRootClass(*Template, Template->BoundActorClass.LoadSynchronous(), ClassPath, Code, Message))
	{
		Out.Add({TEXT("Error"), FString(), FString::Printf(TEXT("%s (%s)"), *Message, *Code), TEXT("/Body/Root/Class"), TEXT("Sequencer")});
	}
	if (!Model.Root.IsValid())
	{
		Out.Add({TEXT("Warning"), TEXT("UnassignedBindingsAndAssets"), TEXT("The template has no root (no bound actor class): it animates nothing "
			"until Body.Root.Class is set."), TEXT("/Body"), TEXT("Sequencer")});
		return;
	}
	const FMovieSceneSpawnable* Spawnable = MovieScene.FindSpawnable(Model.Root);
	const AActor* Actor = Spawnable != nullptr ? Cast<AActor>(Spawnable->GetObjectTemplate()) : nullptr;
	if (Actor == nullptr || (!ClassPath.IsEmpty() && Actor->GetClass()->GetPathName() != ClassPath))
	{
		Out.Add({TEXT("Warning"), FString(), FString::Printf(TEXT("The root spawns %s but the bound actor class is %s."),
			Actor != nullptr ? *Actor->GetClass()->GetPathName() : TEXT("nothing"), ClassPath.IsEmpty() ? TEXT("not set") : *ClassPath),
			TEXT("/Body/Root/Class"), TEXT("Sequencer")});
	}
	for (const FGuid& Guid : Model.Order)
	{
		const FBindingInfo& Info = Model.Bindings[Guid];
		if (Info.bRoot)
		{
			continue;
		}
		const FString Component = TemplateComponentName(*Template, Guid);
		if (Component.IsEmpty())
		{
			Out.Add({TEXT("Warning"), TEXT("UnassignedBindingsAndAssets"), FString::Printf(TEXT("Binding %s names no component (unbound)."), *Info.Path),
				Info.DocumentPath + TEXT("/Component"), TEXT("SequenceValidator")});
		}
		else if (Actor != nullptr && Info.Parent == Model.Root && FindTemplateComponentClass(*Actor, FName(*Component)) == nullptr)
		{
			Out.Add({TEXT("Warning"), TEXT("UnassignedBindingsAndAssets"), FString::Printf(TEXT("Binding %s names component '%s', which %s does not "
				"have (unbound)."), *Info.Path, *Component, *Actor->GetClass()->GetName()), Info.DocumentPath + TEXT("/Component"), TEXT("SequenceValidator")});
		}
	}
}

void Describe(const FSequenceModel& Model, TArray<FStatusMessage>& Out)
{
	DescribeTemplate(Model, Out);
	const FMovieSceneBindingReferences* References = Model.Sequence->GetBindingReferences();
	for (const FGuid& Guid : Model.Order)
	{
		const FBindingInfo& Info = Model.Bindings[Guid];
		if (References != nullptr && References->GetReferences(Guid).IsEmpty())
		{
			Out.Add({TEXT("Warning"), TEXT("UnassignedBindingsAndAssets"), FString::Printf(TEXT("Binding %s has no object reference (unbound)."), *Info.Path),
				Info.DocumentPath + TEXT("/Bind"), TEXT("SequenceValidator")});
		}
	}
	for (const FTrackInfo& Track : Model.Tracks)
	{
		if (Track.Property.IsEmpty() || !Track.Binding.IsValid() || Track.Track->IsA<UMovieScene3DTransformTrack>())
		{
			continue;
		}
		const UClass* Class = BoundClass(Model, Track.Binding);
		if (Class != nullptr && !ResolvesOn(*Class, Track.Property))
		{
			Out.Add({TEXT("Error"), FString(), FString::Printf(TEXT("Property '%s' does not resolve on %s (Sequencer.UnknownProperty)."), *Track.Property,
				*Class->GetPathName()), Track.DocumentPath + TEXT("/Property"), TEXT("Sequencer")});
		}
	}
	TArray<int64> CutTimes;
	if (const UMovieSceneTrack* CameraCut = Model.MovieScene->GetCameraCutTrack())
	{
		for (const UMovieSceneSection* Section : CameraCut->GetAllSections())
		{
			const TRange<FFrameNumber> Range = Section != nullptr ? Section->GetRange() : TRange<FFrameNumber>::Empty();
			if (Range.HasLowerBound())
			{
				CutTimes.AddUnique(Range.GetLowerBoundValue().Value);
			}
			if (Range.HasUpperBound())
			{
				CutTimes.AddUnique(Range.GetUpperBoundValue().Value);
			}
		}
	}
	for (const FTrackInfo& Track : Model.Tracks)
	{
		for (const UMovieSceneSection* Section : Track.Track->GetAllSections())
		{
			if (Section != nullptr)
			{
				CheckSection(Model, *Section, CutTimes, Out);
			}
		}
	}
}
} // namespace

bool SequenceReaches(const UMovieSceneSequence* From, const UMovieSceneSequence* Target)
{
	TSet<const UMovieSceneSequence*> Visited;
	return From != nullptr && (From == Target || ReachesSequence(From, Target, Visited));
}

bool DescribeStatusValue(const UObject& Asset, FValue& OutStatus)
{
	const UMovieSceneSequence* Sequence = Cast<UMovieSceneSequence>(&Asset);
	if (Sequence == nullptr || !IsDocumentedSequence(Asset))
	{
		return false;
	}
	FSequenceModel Model;
	FString Error;
	TArray<FStatusMessage> Messages;
	if (!BuildModel(*Sequence, Model, Error))
	{
		Messages.Add({TEXT("Error"), FString(), Error, TEXT("/Body"), TEXT("Sequencer")});
	}
	else
	{
		Describe(Model, Messages);
	}
	bool bError = false;
	bool bWarning = false;
	TArray<FValue> Rows;
	for (const FStatusMessage& Message : Messages)
	{
		bError |= Message.Severity == TEXT("Error");
		bWarning |= Message.Severity == TEXT("Warning");
		FObjectBuilder Row;
		Row.AddString(TEXT("Severity"), Message.Severity);
		if (!Message.Rule.IsEmpty())
		{
			Row.AddString(TEXT("Rule"), Message.Rule);
		}
		Row.AddString(TEXT("Message"), Message.Message);
		if (!Message.Path.IsEmpty())
		{
			Row.AddString(TEXT("Path"), Message.Path);
		}
		Row.AddString(TEXT("Source"), Message.Source);
		Rows.Add(Row.Build());
	}
	FObjectBuilder Object;
	Object.AddString(TEXT("Status"), bError ? TEXT("Error") : bWarning ? TEXT("UpToDateWithWarnings") : TEXT("UpToDate"));
	Object.Add(TEXT("Messages"), MakeArray(MoveTemp(Rows)));
	OutStatus = Object.Build();
	return true;
}

TSharedPtr<FJsonObject> DescribeStatus(const UObject& Asset)
{
	FValue Status;
	if (!DescribeStatusValue(Asset, Status))
	{
		return nullptr;
	}
	const TSharedPtr<FJsonValue> Json = ValueToJson(Status);
	return Json.IsValid() ? Json->AsObject() : nullptr;
}
} // namespace UE::AssetDocument::Graph::Sequencer
