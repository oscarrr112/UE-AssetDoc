// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentUMGPrimitiveProvider.h"
#include "AssetDocumentUMGProviderTestSupport.h"
#include "Fixtures/AssetDocumentUMGProviderTestFixture.h"
#include "Provider/AssetDocumentUMGPrimitiveCatalog.h"
#include "Provider/AssetDocumentUMGValueDto.h"

#include "Animation/WidgetAnimation.h"
#include "Blueprint/UserWidget.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Components/CanvasPanel.h"
#include "Engine/Blueprint.h"
#include "Misc/AutomationTest.h"
#include "MovieScene.h"
#include "MovieSceneSequence.h"
#include "MovieSceneTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "WidgetBlueprint.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentUMGWidgetAnimationProviderTestsPrivate
{
const FAssetDocumentPrimitiveDescriptor* FindWidgetAnimationDescriptor(FStringView PrimitiveId)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : GetAssetDocumentUMGPrimitiveDescriptors())
	{
		if (Descriptor.PrimitiveId == PrimitiveId)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

void AddWidgetAnimationArgument(FAssetDocumentPrimitiveInvocation& Invocation, FString Name, FAssetDocumentOpaqueHandle Handle)
{
	Invocation.HandleArguments.Add({MoveTemp(Name), MoveTemp(Handle)});
}

const FAssetDocumentRuntimeValueDto* FindWidgetAnimationField(
	const FAssetDocumentRuntimeValueDto& Value,
	const TCHAR* Name)
{
	for (int32 Index = 0; Index < Value.ObjectKeys.Num(); ++Index)
	{
		if (Value.ObjectKeys[Index] == Name && Value.Elements.IsValidIndex(Index))
		{
			return &Value.Elements[Index];
		}
	}
	return nullptr;
}

bool HasStringField(
	const FAssetDocumentRuntimeValueDto& Value,
	const TCHAR* Name,
	const FString& Expected)
{
	const FAssetDocumentRuntimeValueDto* Field =
		FindWidgetAnimationField(Value, Name);
	return Field != nullptr
		&& Field->Kind == EAssetDocumentRuntimeValueKind::String
		&& Field->StringValue == Expected;
}

bool HasIntegerField(
	const FAssetDocumentRuntimeValueDto& Value,
	const TCHAR* Name,
	int64 Expected)
{
	const FAssetDocumentRuntimeValueDto* Field =
		FindWidgetAnimationField(Value, Name);
	return Field != nullptr
		&& Field->Kind == EAssetDocumentRuntimeValueKind::Integer
		&& Field->IntegerValue == Expected;
}

bool HasNumberField(
	const FAssetDocumentRuntimeValueDto& Value,
	const TCHAR* Name,
	double Expected)
{
	const FAssetDocumentRuntimeValueDto* Field =
		FindWidgetAnimationField(Value, Name);
	return Field != nullptr
		&& Field->Kind == EAssetDocumentRuntimeValueKind::Number
		&& FMath::IsNearlyEqual(Field->NumberValue, Expected);
}

bool HasResultHandle(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const TCHAR* Name)
{
	return Outcome.HandleResults.ContainsByPredicate([Name](const FAssetDocumentPrimitiveHandleResult& Result)
	{
		return Result.Name == Name && !Result.Handles.IsEmpty();
	});
}

FAssetDocumentPrimitiveInvocation MakeQueryInvocation(
	const FAssetDocumentPrimitiveDescriptor& Query,
	const FAssetDocumentUMGProviderTestContext& Context,
	FAssetDocumentUMGProviderTestHandleResolver& Handles,
	UWidgetBlueprint* Blueprint)
{
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentUMGProviderTestInvocation(Query, Context);
	AddWidgetAnimationArgument(Invocation, TEXT("widget_blueprint"), Handles.AddObjectHandle(
		Blueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly));
	Invocation.ValueArguments = MakeAssetDocumentUMGObject({
		{TEXT("animation_name"), MakeAssetDocumentUMGString(TEXT("Fade"))}});
	return Invocation;
}

void InvalidateAllFixtureHandles(
	FAssetDocumentUMGProviderTestHandleResolver& Handles,
	const FAssetDocumentUMGProviderTestContext& Context,
	EAssetDocumentHandleInvalidationReason Reason)
{
	FAssetDocumentHandleInvalidation Event;
	Event.Reason = Reason;
	Event.RequestId = Context.RequestId;
	Event.TransactionId = Context.TransactionId;
	Event.RuntimeRevision = Context.RuntimeRevision;
	Event.ProviderId = Context.ProviderId;
	Handles.Invalidate(Event);
}
} // namespace AssetDocumentUMGWidgetAnimationProviderTestsPrivate


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUMGWidgetAnimationProviderTest,
	"UEAssetDoc.UMGProvider.WidgetAnimationLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUMGWidgetAnimationProviderTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentUMGWidgetAnimationProviderTestsPrivate;
	const FAssetDocumentPrimitiveDescriptor* Query = FindWidgetAnimationDescriptor(TEXT("umg.animation.query.v1"));
	TestNotNull(TEXT("WidgetAnimation query is callable."), Query);
	if (Query == nullptr)
	{
		return false;
	}
	FAssetDocumentUMGProviderTestFixture Fixture;
	TestTrue(TEXT("The transient WidgetBlueprint fixture is available."), Fixture.CreateTransientWidgetBlueprint());
	UWidgetBlueprint* Blueprint = Fixture.GetWidgetBlueprint();
	UWidgetAnimation* Animation = Fixture.AddFixtureAnimation(TEXT("Fade"));
	TestNotNull(TEXT("The fixture owns a WidgetAnimation."), Animation);
	if (Blueprint == nullptr || Animation == nullptr)
	{
		return false;
	}
	UMovieSceneSequence& Sequence = *Animation;
	UMovieScene* MovieScene = Sequence.GetMovieScene();
	TestNotNull(TEXT("The fixture animation owns a MovieScene."), MovieScene);
	if (MovieScene == nullptr)
	{
		return false;
	}

	// Test-owned fixture facts use public MovieScene and WidgetAnimation APIs only.
	MovieScene->SetTickResolutionDirectly(FFrameRate(24000, 1));
	MovieScene->SetDisplayRate(FFrameRate(24, 1));
	MovieScene->SetPlaybackRange(FFrameNumber(24), 48);
	const FGuid PossessableGuid = MovieScene->AddPossessable(
		TEXT("AnimatedPanel"), UCanvasPanel::StaticClass());
	TestTrue(TEXT("The public fixture possessable is valid."), PossessableGuid.IsValid());
	UUserWidget* PreviewRoot =
		Fixture.CreatePreviewRoot(TEXT("AnimationPreview"));
	UCanvasPanel* AnimatedPanel = PreviewRoot
		? Cast<UCanvasPanel>(
			Fixture.CreatePreviewWidget(
				UCanvasPanel::StaticClass(),
				TEXT("AnimatedPanel"),
				*PreviewRoot))
		: nullptr;
	TestNotNull(
		TEXT("The public fixture preview root is valid."),
		PreviewRoot);
	TestNotNull(TEXT("The public fixture widget is valid."), AnimatedPanel);
	if (!PossessableGuid.IsValid()
		|| PreviewRoot == nullptr
		|| AnimatedPanel == nullptr)
	{
		return false;
	}
	Animation->BindPossessableObject(
		PossessableGuid, *AnimatedPanel, PreviewRoot);
	TestEqual(TEXT("The public fixture creates one animation binding."), Animation->GetBindings().Num(), 1);
	UMovieSceneFloatTrack* Track = Cast<UMovieSceneFloatTrack>(
		MovieScene->AddTrack(UMovieSceneFloatTrack::StaticClass(), PossessableGuid));
	TestNotNull(TEXT("The public fixture creates a float track."), Track);
	if (Track == nullptr)
	{
		return false;
	}
	Track->SetPropertyNameAndPath(TEXT("RenderOpacity"), TEXT("RenderOpacity"));
	UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(Track->CreateNewSection());
	TestNotNull(TEXT("The public fixture creates a float section."), Section);
	if (Section == nullptr)
	{
		return false;
	}
	static_cast<UMovieSceneTrack*>(Track)->AddSection(
		UMovieSceneTrack::FSectionParameter(*Section));
	Section->SetRange(TRange<FFrameNumber>::Inclusive(FFrameNumber(24), FFrameNumber(71)));
	AddKeyToChannel(&Section->GetChannel(), FFrameNumber(24), 0.25f, EMovieSceneKeyInterpolation::Linear);
	AddKeyToChannel(&Section->GetChannel(), FFrameNumber(48), 0.75f, EMovieSceneKeyInterpolation::Auto);

	FAssetDocumentUMGProviderTestContext Context;
	FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
	FAssetDocumentPrimitiveInvocation Invocation = MakeQueryInvocation(*Query, Context, Handles, Blueprint);
	const FAssetDocumentPrimitiveOutcome Outcome = CreateAssetDocumentUMGPrimitiveProvider()->Invoke(Invocation, Handles, nullptr);
	TestEqual(TEXT("The query succeeds with fixture animation facts."), Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	const FAssetDocumentRuntimeValueDto* Animations =
		FindWidgetAnimationField(
			Outcome.Value, TEXT("animations"));
	TestNotNull(TEXT("The query returns the fixture animation array."), Animations);
	if (Animations == nullptr || !Animations->Elements.IsValidIndex(0))
	{
		return false;
	}
	const FAssetDocumentRuntimeValueDto& Facts = Animations->Elements[0];
	const FAssetDocumentRuntimeValueDto* StartTime =
		FindWidgetAnimationField(Facts, TEXT("start_time"));
	const FAssetDocumentRuntimeValueDto* EndTime =
		FindWidgetAnimationField(Facts, TEXT("end_time"));
	const FAssetDocumentRuntimeValueDto* DisplayRateNumerator =
		FindWidgetAnimationField(
			Facts, TEXT("display_rate_numerator"));
	const FAssetDocumentRuntimeValueDto* DisplayRateDenominator =
		FindWidgetAnimationField(
			Facts, TEXT("display_rate_denominator"));
	const FAssetDocumentRuntimeValueDto* PlaybackStart =
		FindWidgetAnimationField(
			Facts, TEXT("playback_start_frame"));
	const FAssetDocumentRuntimeValueDto* PlaybackEnd =
		FindWidgetAnimationField(
			Facts, TEXT("playback_end_frame"));
	TestTrue(TEXT("The raw start time is preserved."), StartTime != nullptr && HasNumberField(Facts, TEXT("start_time"), Animation->GetStartTime()));
	TestTrue(TEXT("The raw end time is preserved."), EndTime != nullptr && HasNumberField(Facts, TEXT("end_time"), Animation->GetEndTime()));
	TestTrue(TEXT("The raw display rate is preserved."), DisplayRateNumerator != nullptr && DisplayRateDenominator != nullptr
		&& HasIntegerField(Facts, TEXT("display_rate_numerator"), 24)
		&& HasIntegerField(Facts, TEXT("display_rate_denominator"), 1));
	TestTrue(TEXT("The raw playback range is preserved."), PlaybackStart != nullptr && PlaybackEnd != nullptr
		&& HasIntegerField(Facts, TEXT("playback_start_frame"), 24)
		&& HasIntegerField(Facts, TEXT("playback_end_frame"), 72));
	const FAssetDocumentRuntimeValueDto* Possessables =
		FindWidgetAnimationField(Facts, TEXT("possessables"));
	const FAssetDocumentRuntimeValueDto* Bindings =
		FindWidgetAnimationField(Facts, TEXT("bindings"));
	const FAssetDocumentRuntimeValueDto* Tracks =
		FindWidgetAnimationField(Facts, TEXT("tracks"));
	TestTrue(TEXT("The possessable fact is preserved."), Possessables != nullptr && Possessables->Elements.Num() == 1
		&& HasStringField(Possessables->Elements[0], TEXT("guid"), PossessableGuid.ToString()));
	TestTrue(TEXT("The public WidgetAnimation binding fact is preserved."), Bindings != nullptr && Bindings->Elements.Num() == 1
		&& HasStringField(Bindings->Elements[0], TEXT("possessable_guid"), PossessableGuid.ToString())
		&& HasStringField(Bindings->Elements[0], TEXT("widget_name"), TEXT("AnimatedPanel")));
	TestTrue(TEXT("The public float track fact is preserved."), Tracks != nullptr && Tracks->Elements.Num() == 1
		&& HasStringField(Tracks->Elements[0], TEXT("name"), Track->GetName()));
	const FAssetDocumentRuntimeValueDto* Sections = Tracks != nullptr && Tracks->Elements.IsValidIndex(0)
		? FindWidgetAnimationField(
			Tracks->Elements[0], TEXT("sections"))
		: nullptr;
	const FAssetDocumentRuntimeValueDto* Channels = Sections != nullptr && Sections->Elements.IsValidIndex(0)
		? FindWidgetAnimationField(
			Sections->Elements[0], TEXT("channels"))
		: nullptr;
	const FAssetDocumentRuntimeValueDto* Keys = Channels != nullptr && Channels->Elements.IsValidIndex(0)
		? FindWidgetAnimationField(
			Channels->Elements[0], TEXT("keys"))
		: nullptr;
	TestTrue(TEXT("The public section and float channel facts are preserved."), Sections != nullptr && Sections->Elements.Num() == 1
		&& Channels != nullptr && Channels->Elements.Num() == 1
		&& HasStringField(Channels->Elements[0], TEXT("name"), TEXT("float")));
	TestTrue(TEXT("The public float channel key facts are preserved."), Keys != nullptr && Keys->Elements.Num() == 2
		&& HasIntegerField(Keys->Elements[0], TEXT("frame"), 24)
		&& HasNumberField(Keys->Elements[0], TEXT("value"), 0.25)
		&& HasIntegerField(Keys->Elements[1], TEXT("frame"), 48)
		&& HasNumberField(Keys->Elements[1], TEXT("value"), 0.75));
	TestTrue(TEXT("The query issues animation, track, section, and channel handles."),
		HasResultHandle(Outcome, TEXT("widget_animation"))
		&& HasResultHandle(Outcome, TEXT("movie_scene_track"))
		&& HasResultHandle(Outcome, TEXT("movie_scene_section"))
		&& HasResultHandle(Outcome, TEXT("movie_scene_channel")));
	TestTrue(TEXT("The WidgetBlueprint fixture has no Blueprint Timeline."), Blueprint->Timelines.IsEmpty());
	TestNull(TEXT("The Create candidate is not a descriptor."), FindWidgetAnimationDescriptor(TEXT("UMG-WIDGET-ANIMATION-CREATE-v1")));
	TestNull(TEXT("The rejected broad binding candidate is not a descriptor."), FindWidgetAnimationDescriptor(TEXT("UMG-ANIMATION-BINDING-APPLY-DELTA-v1")));
	FAssetDocumentPrimitiveInvocation DeleteInvocation = Invocation;
	DeleteInvocation.PrimitiveId = TEXT("UMG-WIDGET-ANIMATION-DELETE-v1");
	const FAssetDocumentPrimitiveOutcome DeleteOutcome = CreateAssetDocumentUMGPrimitiveProvider()->Invoke(DeleteInvocation, Handles, nullptr);
	TestTrue(TEXT("WidgetAnimation Delete is an exact stable blocker."), DeleteOutcome.Diagnostics.ContainsByPredicate(
		[](const FAssetDocumentRuntimeDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("AD.BLOCK.UMG.ANIMATION_DELETE_EVIDENCE"); }));
	for (const EAssetDocumentHandleInvalidationReason Reason : {
		EAssetDocumentHandleInvalidationReason::PackageUnload,
		EAssetDocumentHandleInvalidationReason::ProviderUnload})
	{
		FAssetDocumentUMGProviderTestContext StaleContext;
		FAssetDocumentUMGProviderTestHandleResolver StaleHandles(StaleContext);
		FAssetDocumentPrimitiveInvocation StaleInvocation = MakeQueryInvocation(*Query, StaleContext, StaleHandles, Blueprint);
		InvalidateAllFixtureHandles(StaleHandles, StaleContext, Reason);
		const FAssetDocumentPrimitiveOutcome StaleOutcome = CreateAssetDocumentUMGPrimitiveProvider()->Invoke(StaleInvocation, StaleHandles, nullptr);
		TestEqual(TEXT("Asset or provider unload makes query input handles fail closed."), StaleOutcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	}
	FAssetDocumentUMGProviderTestContext GenerationContext;
	FAssetDocumentUMGProviderTestHandleResolver GenerationHandles(GenerationContext);
	FAssetDocumentPrimitiveInvocation GenerationInvocation = MakeQueryInvocation(*Query, GenerationContext, GenerationHandles, Blueprint);
	GenerationHandles.AdvanceProviderGeneration();
	TestEqual(TEXT("Provider generation makes query input handles fail closed."),
		CreateAssetDocumentUMGPrimitiveProvider()->Invoke(GenerationInvocation, GenerationHandles, nullptr).Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);
	FAssetDocumentUMGProviderTestContext RevisionContext;
	FAssetDocumentUMGProviderTestHandleResolver RevisionHandles(RevisionContext);
	FAssetDocumentPrimitiveInvocation RevisionInvocation = MakeQueryInvocation(*Query, RevisionContext, RevisionHandles, Blueprint);
	++RevisionInvocation.RuntimeRevision;
	TestEqual(TEXT("Runtime revision changes make query input handles fail closed."),
		CreateAssetDocumentUMGPrimitiveProvider()->Invoke(RevisionInvocation, RevisionHandles, nullptr).Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Fixture.DestroyTransientWidgetBlueprint();
	return true;
}

#endif
