// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Editor-made Level Sequences with content the document has no form for.
// UEAssetDoc.Tolerant.LevelSequence: a transform section with procedural channel overrides (the channel context menu's
//   "Override With > Double Perlin Noise"). It reads as the ChannelOverrides marker with a warning, its own extract
//   re-applies as a no-op, an edit of the same section keeps the overrides, and an edited marker is refused.
// UEAssetDoc.Tolerant.LevelSequenceOrphan: a legacy spawnable (the pre-5.4 spawnable form the upgrade could not
//   convert) is not part of the document. Extract warns about it, the extract re-applies as a no-op, and an edit keeps it.

#include "AssetDocumentTolerantTestSupport.h"

#include "Channels/IMovieSceneChannelOverrideProvider.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneDoublePerlinNoiseChannelContainer.h"
#include "Channels/MovieSceneSectionChannelOverrideRegistry.h"
#include "GameFramework/Actor.h"
#include "LevelSequence.h"
#include "Misc/ScopeExit.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentTolerantSequencerTestsLocal
{
// A new Level Sequence as the Content Browser's "Level Sequence" makes it, 30 fps, frames 0-150.
ULevelSequence* NewSequence(const AssetDocumentTolerantTestSupport::FTarget& Target)
{
	ULevelSequence* Sequence = Target.Create<ULevelSequence>();
	Sequence->Initialize();
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	MovieScene->SetDisplayRate(FFrameRate(30, 1));
	MovieScene->SetTickResolutionDirectly(FFrameRate(24000, 1));
	MovieScene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(150 * 800)));
	return Sequence;
}

UMovieSceneSection* TransformSection(ULevelSequence* Sequence)
{
	UMovieScene* MovieScene = Sequence != nullptr ? Sequence->GetMovieScene() : nullptr;
	if (MovieScene == nullptr || MovieScene->GetPossessableCount() == 0)
	{
		return nullptr;
	}
	const UMovieScene3DTransformTrack* Track = MovieScene->FindTrack<UMovieScene3DTransformTrack>(MovieScene->GetPossessable(0).GetGuid());
	return Track != nullptr && !Track->GetAllSections().IsEmpty() ? Track->GetAllSections()[0] : nullptr;
}

int32 OverrideCount(UMovieSceneSection* Section)
{
	IMovieSceneChannelOverrideProvider* Provider = Cast<IMovieSceneChannelOverrideProvider>(Section);
	const UMovieSceneSectionChannelOverrideRegistry* Registry = Provider != nullptr ? Provider->GetChannelOverrideRegistry(false) : nullptr;
	return Registry != nullptr ? Registry->NumChannels() : 0;
}

TSharedPtr<FJsonObject> Section(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonValue> Value = AssetDocumentTolerantTestSupport::At(Document, TEXT("/Body/Bindings/0/Tracks/0/Sections/0"));
	return Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantLevelSequenceTest, "UEAssetDoc.Tolerant.LevelSequence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantLevelSequenceTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantSequencerTestsLocal;
	const FTarget Target(TEXT("LS_Tolerant"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	ULevelSequence* Sequence = NewSequence(Target);
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	const FGuid Prop = MovieScene->AddPossessable(TEXT("Prop"), AActor::StaticClass());
	UMovieScene3DTransformTrack* Track = MovieScene->AddTrack<UMovieScene3DTransformTrack>(Prop);
	UMovieSceneSection* Transform = Track != nullptr ? Track->CreateNewSection() : nullptr;
	if (!TestNotNull(TEXT("transform section"), Transform))
	{
		return false;
	}
	Track->AddSection(*Transform);
	Transform->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(150 * 800)));
	// The channel context menu's "Override With > Double Perlin Noise" on Location.X.
	IMovieSceneChannelOverrideProvider* Provider = Cast<IMovieSceneChannelOverrideProvider>(Transform);
	const TArrayView<const FMovieSceneChannelMetaData> MetaData = Transform->GetChannelProxy().GetMetaData<FMovieSceneDoubleChannel>();
	FMovieSceneDoubleChannel* LocationX = Transform->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0);
	if (!TestTrue(TEXT("override provider"), Provider != nullptr && MetaData.Num() > 0 && LocationX != nullptr))
	{
		return false;
	}
	const FName Overridden = MetaData[0].Name;
	UMovieSceneSectionChannelOverrideRegistry* Registry = Provider->GetChannelOverrideRegistry(true);
	UMovieSceneDoublePerlinNoiseChannelContainer* Noise = NewObject<UMovieSceneDoublePerlinNoiseChannelContainer>(Registry, NAME_None, RF_Transactional);
	Noise->InitializeOverride(LocationX);
	Registry->AddChannel(Overridden, Noise);
	Provider->OnChannelOverridesChanged();
	MovieScene->MarkAsChanged();
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const FString Marker = TEXT("/Body/Bindings/0/Tracks/0/Sections/0/ChannelOverrides");
	FExpected Expected;
	Expected.Warnings = {{TEXT("Sequencer.Unrepresented"), Marker}};
	Expected.Markers = {Marker};
	if (!TestTolerantRoundTrip(*this, Target, Expected, TEXT("Level Sequence")).IsValid())
	{
		return false;
	}

	if (TestTrue(TEXT("the overridden section changes"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		if (const TSharedPtr<FJsonObject> Entry = Section(Document))
		{
			Entry->SetNumberField(TEXT("OverlapPriority"), 5);
		}
	}, TEXT("Edit")).IsSuccess()))
	{
		UMovieSceneSection* Edited = TransformSection(Target.Get<ULevelSequence>());
		TestTrue(TEXT("the section was rewritten"), Edited != nullptr && Edited->GetOverlapPriority() == 5);
		TestEqual(TEXT("the channel override is kept"), OverrideCount(Edited), 1);
		TestStillShows(*this, Target, TEXT("Sequencer.Unrepresented"), Marker, TEXT("After edit"));
	}

	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		if (const TSharedPtr<FJsonObject> Entry = Section(Document))
		{
			Entry->GetObjectField(TEXT("ChannelOverrides"))->SetStringField(TEXT("Unsupported"), TEXT("Noise on Location.X"));
		}
	}, TEXT("Edited marker")), TEXT("Sequencer.UnrepresentedValue"), Marker, TEXT("Edited marker"));
	TestEqual(TEXT("the refused apply left the overrides"), OverrideCount(TransformSection(Target.Get<ULevelSequence>())), 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantLevelSequenceOrphanTest, "UEAssetDoc.Tolerant.LevelSequenceOrphan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantLevelSequenceOrphanTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantSequencerTestsLocal;
	const FTarget Target(TEXT("LS_TolerantOrphan"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	ULevelSequence* Sequence = NewSequence(Target);
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	// A legacy spawnable: an object template on the movie scene itself, as sequences saved before custom bindings hold.
	AActor* Template = NewObject<AActor>(MovieScene, AActor::StaticClass(), TEXT("LegacyProp"), RF_Transactional | RF_ArchetypeObject | RF_Public);
	MovieScene->AddSpawnable(TEXT("LegacyProp"), *Template);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	FExpected Expected;
	Expected.Warnings = {{TEXT("Sequencer.UnsupportedBindingShape"), TEXT("/Body/Bindings")}};
	if (!TestTolerantRoundTrip(*this, Target, Expected, TEXT("Legacy spawnable")).IsValid())
	{
		return false;
	}

	if (TestTrue(TEXT("a marker is added"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		TArray<TSharedPtr<FJsonValue>> Marks;
		const TSharedPtr<FJsonObject> Mark = MakeShared<FJsonObject>();
		Mark->SetNumberField(TEXT("Frame"), 30);
		Mark->SetStringField(TEXT("Label"), TEXT("Cue"));
		Marks.Add(MakeShared<FJsonValueObject>(Mark));
		Document->GetObjectField(TEXT("Body"))->SetArrayField(TEXT("Markers"), Marks);
	}, TEXT("Edit")).IsSuccess()))
	{
		const ULevelSequence* Edited = Target.Get<ULevelSequence>();
		const UMovieScene* After = Edited != nullptr ? Edited->GetMovieScene() : nullptr;
		TestTrue(TEXT("the marker was written"), After != nullptr && After->GetMarkedFrames().Num() == 1);
		TestTrue(TEXT("the legacy spawnable is kept"), After != nullptr && After->GetSpawnableCount() == 1);
		TestStillShows(*this, Target, TEXT("Sequencer.UnsupportedBindingShape"), TEXT("/Body/Bindings"), TEXT("After edit"));
	}
	return !HasAnyErrors();
}

#endif
