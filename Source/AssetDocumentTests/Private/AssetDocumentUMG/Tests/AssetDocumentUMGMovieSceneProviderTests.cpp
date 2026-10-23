// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentUMGPrimitiveProvider.h"
#include "AssetDocumentUMGProviderTestSupport.h"
#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/WidgetAnimation.h"
#include "WidgetBlueprint.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Components/Widget.h"
#include "Fixtures/AssetDocumentUMGProviderTestFixture.h"
#include "Misc/AutomationTest.h"
#include "Misc/ITransaction.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneSequence.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "Provider/AssetDocumentUMGValueDto.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieSceneFloatTrack.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentUMGMovieSceneProviderTestsPrivate
{
class FAssetDocumentUMGTestUndoTransaction final : public ITransaction
{
public:
	virtual void BeginOperation() override {}
	virtual void EndOperation() override {}
	virtual void Finalize() override {}
	virtual void Apply() override {}
	virtual FTransactionContext GetContext() const override
	{
		return {};
	}
	virtual bool IsTransient() const override { return false; }
	virtual bool ContainsPieObjects() const override { return false; }
	virtual void SaveArray(
		UObject*, FScriptArray*, int32, int32, int32, int32, uint32,
		STRUCT_DC, STRUCT_AR, STRUCT_DTOR) override
	{
	}
	virtual void SaveObject(UObject*) override {}
	virtual void StoreUndo(
		UObject*, TUniquePtr<FChange>) override
	{
	}
	virtual void SetPrimaryObject(UObject*) override {}
	virtual void SnapshotObject(
		UObject*, TArrayView<const FProperty*>) override
	{
	}
	virtual bool ContainsObject(const UObject*) const override
	{
		return false;
	}
};

class FAssetDocumentUMGTestUndoScope final
{
public:
	explicit FAssetDocumentUMGTestUndoScope(bool bProvideTransaction)
		: Previous(GUndo)
	{
		GUndo = bProvideTransaction ? &Transaction : nullptr;
	}

	~FAssetDocumentUMGTestUndoScope()
	{
		GUndo = Previous;
	}

private:
	FAssetDocumentUMGTestUndoTransaction Transaction;
	ITransaction* Previous = nullptr;
};

const FAssetDocumentPrimitiveDescriptor* FindMovieSceneDescriptor(
	IAssetDocumentPrimitiveProvider& Provider,
	const TCHAR* PrimitiveId)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor :
		Provider.DescribePrimitives())
	{
		if (Descriptor.PrimitiveId == PrimitiveId)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

FAssetDocumentPrimitiveInvocation MakeInvocation(
	IAssetDocumentPrimitiveProvider& Provider,
	const TCHAR* PrimitiveId,
	const FAssetDocumentUMGProviderTestContext& Context,
	FAssetDocumentRuntimeValueDto&& Value,
	TArray<FAssetDocumentPrimitiveHandleArgument>&& Handles)
{
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindMovieSceneDescriptor(Provider, PrimitiveId);
	check(Descriptor != nullptr);
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentUMGProviderTestInvocation(
			*Descriptor, Context);
	Invocation.ValueArguments = MoveTemp(Value);
	Invocation.HandleArguments = MoveTemp(Handles);
	return Invocation;
}

FAssetDocumentPrimitiveHandleArgument MakeHandle(
	const TCHAR* Name,
	const FAssetDocumentOpaqueHandle& Handle)
{
	return {Name, Handle};
}

FAssetDocumentOpaqueHandle FindResultHandle(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const TCHAR* Name,
	int32 Index = 0)
{
	for (const FAssetDocumentPrimitiveHandleResult& Result :
		Outcome.HandleResults)
	{
		if (Result.Name == Name && Result.Handles.IsValidIndex(Index))
		{
			return Result.Handles[Index];
		}
	}
	return {};
}

void MovieSceneRecordDiagnostics(
	FAutomationTestBase& Test,
	const TCHAR* Operation,
	const FAssetDocumentPrimitiveOutcome& Outcome)
{
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic :
		Outcome.Diagnostics)
	{
		Test.AddInfo(FString::Printf(
			TEXT("%s diagnostic: %s %s"),
			Operation,
			*Diagnostic.Code,
			*Diagnostic.Message));
	}
}

bool MovieSceneHasDiagnostic(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	FStringView Code)
{
	return Outcome.Diagnostics.ContainsByPredicate(
		[Code](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			return FStringView(Diagnostic.Code) == Code;
		});
}

FAssetDocumentRuntimeValueDto* FindMutableField(
	FAssetDocumentRuntimeValueDto& Object,
	FStringView Name)
{
	const int32 Index = Object.ObjectKeys.IndexOfByPredicate(
		[Name](const FString& Key)
		{
			return FStringView(Key) == Name;
		});
	return Object.Elements.IsValidIndex(Index)
		? &Object.Elements[Index]
		: nullptr;
}

FAssetDocumentRuntimeValueDto MakeKey(
	int32 Frame,
	double Value,
	const TCHAR* Interpolation)
{
	return MakeAssetDocumentUMGObject({
		{TEXT("frame"), MakeAssetDocumentUMGInteger(Frame)},
		{TEXT("value"), MakeAssetDocumentUMGNumber(Value)},
		{TEXT("interpolation"),
			MakeAssetDocumentUMGString(Interpolation)}
	});
}

FAssetDocumentRuntimeValueDto MakeChannel(
	double DefaultValue,
	TArray<FAssetDocumentRuntimeValueDto>&& Keys)
{
	return MakeAssetDocumentUMGObject({
		{TEXT("default_value"),
			MakeAssetDocumentUMGNumber(DefaultValue)},
		{TEXT("keys"),
			MakeAssetDocumentUMGArray(MoveTemp(Keys))}
	});
}

FAssetDocumentRuntimeValueDto MakeFloatInput(
	const FGuid& Guid,
	const TCHAR* PropertyName,
	const TCHAR* PropertyPath)
{
	return MakeAssetDocumentUMGObject({
		{TEXT("possessable_guid"),
			MakeAssetDocumentUMGString(
				Guid.ToString(EGuidFormats::DigitsWithHyphens))},
		{TEXT("property_name"),
			MakeAssetDocumentUMGString(PropertyName)},
		{TEXT("property_path"),
			MakeAssetDocumentUMGString(PropertyPath)},
		{TEXT("section_start_frame"),
			MakeAssetDocumentUMGInteger(0)},
		{TEXT("section_end_frame"),
			MakeAssetDocumentUMGInteger(30)},
		{TEXT("default_value"),
			MakeAssetDocumentUMGNumber(0.25)},
		{TEXT("keys"),
			MakeAssetDocumentUMGArray({
				MakeKey(0, 0.0, TEXT("linear")),
				MakeKey(15, 1.0, TEXT("auto")),
				MakeKey(29, 0.5, TEXT("constant"))})}
	});
}

FAssetDocumentRuntimeValueDto MakeTransformInput(
	const FGuid& Guid)
{
	const TCHAR* Names[] = {
		TEXT("translation_x"), TEXT("translation_y"), TEXT("angle"),
		TEXT("scale_x"), TEXT("scale_y"),
		TEXT("shear_x"), TEXT("shear_y")};
	FAssetDocumentRuntimeValueDto Channels;
	Channels.Kind = EAssetDocumentRuntimeValueKind::Object;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Names); ++Index)
	{
		Channels.ObjectKeys.Add(Names[Index]);
		Channels.Elements.Add(MakeChannel(
			Index == 3 || Index == 4 ? 1.0 : 0.0,
			{MakeKey(
				Index + 1,
				static_cast<double>(Index),
				Index % 2 == 0
					? TEXT("user")
					: TEXT("break"))}));
	}
	return MakeAssetDocumentUMGObject({
		{TEXT("possessable_guid"),
			MakeAssetDocumentUMGString(
				Guid.ToString(EGuidFormats::DigitsWithHyphens))},
		{TEXT("property_name"),
			MakeAssetDocumentUMGString(TEXT("RenderTransform"))},
		{TEXT("property_path"),
			MakeAssetDocumentUMGString(TEXT("RenderTransform"))},
		{TEXT("section_start_frame"),
			MakeAssetDocumentUMGInteger(0)},
		{TEXT("section_end_frame"),
			MakeAssetDocumentUMGInteger(60)},
		{TEXT("channels"), MoveTemp(Channels)}
	});
}

bool BuildAnimationFixture(
	FAutomationTestBase& Test,
	FAssetDocumentUMGProviderTestFixture& Fixture,
	UWidgetAnimation*& OutAnimation,
	UMovieScene*& OutMovieScene,
	FGuid& OutPossessable)
{
	if (!Test.TestTrue(
		TEXT("transient WidgetBlueprint fixture"),
		Fixture.CreateTransientWidgetBlueprint()))
	{
		return false;
	}
	OutAnimation = Fixture.AddFixtureAnimation(TEXT("Pulse"));
	Test.TestNotNull(TEXT("test-only animation fixture"), OutAnimation);
	if (OutAnimation == nullptr)
	{
		return false;
	}
	UMovieSceneSequence& Sequence = *OutAnimation;
	OutMovieScene = Sequence.GetMovieScene();
	Test.TestNotNull(TEXT("test-only MovieScene fixture"), OutMovieScene);
	if (OutMovieScene == nullptr)
	{
		return false;
	}
	OutPossessable = OutMovieScene->AddPossessable(
		TEXT("Widget"), UWidget::StaticClass());
	return Test.TestTrue(
		TEXT("possessable fixture authority"),
		OutPossessable.IsValid());
}

FAssetDocumentHandleValidationContext MakeChannelValidationContext(
	const FAssetDocumentUMGProviderTestContext& Context,
	EAssetDocumentRuntimeRequestPhase Phase)
{
	FAssetDocumentHandleValidationContext Validation;
	Validation.RequestId = Context.RequestId;
	Validation.TransactionId = Context.TransactionId;
	Validation.RuntimeRevision = Context.RuntimeRevision;
	Validation.ProviderId = Context.ProviderId;
	Validation.Phase = Phase;
	Validation.RequiredKind = EAssetDocumentHandleKind::Channel;
	Validation.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	return Validation;
}

FAssetDocumentPrimitiveOutcome InvokeTrackRemove(
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentUMGProviderTestContext& Context,
	FAssetDocumentUMGProviderTestHandleResolver& Handles,
	FAssetDocumentUMGProviderTestTransaction& Transaction,
	const FAssetDocumentOpaqueHandle& AnimationHandle,
	const FAssetDocumentOpaqueHandle& TrackHandle,
	const FAssetDocumentOpaqueHandle* SectionHandle,
	const TCHAR* Target)
{
	TArray<FAssetDocumentPrimitiveHandleArgument> HandleArguments = {
		MakeHandle(TEXT("widget_animation"), AnimationHandle),
		MakeHandle(TEXT("movie_scene_track"), TrackHandle)};
	if (SectionHandle != nullptr)
	{
		HandleArguments.Add(
			MakeHandle(
				TEXT("movie_scene_section"), *SectionHandle));
	}
	return Provider.Invoke(
		MakeInvocation(
			Provider,
			TEXT("umg.movie_scene.track_section.remove.v1"),
			Context,
			MakeAssetDocumentUMGObject({
				{TEXT("target"),
					MakeAssetDocumentUMGString(Target)}}),
			MoveTemp(HandleArguments)),
		Handles,
		&Transaction);
}
} // namespace AssetDocumentUMGMovieSceneProviderTestsPrivate


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUMGMovieSceneLifecycleTest,
	"UEAssetDoc.UMGProvider.MovieSceneLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUMGMovieSceneLifecycleTest::RunTest(
	const FString& Parameters)
{
	using namespace AssetDocumentUMGMovieSceneProviderTestsPrivate;
	(void)Parameters;
	FAssetDocumentUMGTestUndoScope UnrealUndo(true);
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentUMGPrimitiveProvider();
	FAssetDocumentUMGProviderTestFixture Fixture;
	UWidgetAnimation* Animation = nullptr;
	UMovieScene* MovieScene = nullptr;
	FGuid PossessableGuid;
	if (!BuildAnimationFixture(
		*this, Fixture, Animation,
		MovieScene, PossessableGuid))
	{
		return false;
	}
	FAssetDocumentUMGProviderTestContext Context;
	FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
	FAssetDocumentUMGProviderTestTransaction Transaction(Context);
	FAssetDocumentOpaqueHandle AnimationHandle =
		Handles.AddObjectHandle(
			Animation,
			EAssetDocumentHandleKind::Subobject,
			EAssetDocumentHandleAccess::Mutable);
	const FString InitialDigest = Fixture.CaptureDigest();

	FAssetDocumentRuntimeValueDto DuplicateFrames =
		MakeFloatInput(
			PossessableGuid,
			TEXT("DuplicateFrames"),
			TEXT("DuplicateFrames"));
	FAssetDocumentRuntimeValueDto* DuplicateKeys =
		FindMutableField(DuplicateFrames, TEXT("keys"));
	if (!TestNotNull(TEXT("duplicate key fixture"), DuplicateKeys))
	{
		return false;
	}
	DuplicateKeys->Elements.Add(
		MakeKey(15, 2.0, TEXT("linear")));
	const FAssetDocumentPrimitiveOutcome DuplicateFrameOutcome =
		Provider->Invoke(
			MakeInvocation(
				Provider.Get(),
				TEXT("umg.movie_scene.float_track.add.v1"),
				Context,
				MoveTemp(DuplicateFrames),
				{MakeHandle(
					TEXT("widget_animation"),
					AnimationHandle)}),
			Handles,
			&Transaction);
	TestTrue(
		TEXT("duplicate key frames fail before mutation"),
		DuplicateFrameOutcome.Status
				== EAssetDocumentPrimitiveOutcomeStatus::Failed
			&& MovieSceneHasDiagnostic(
				DuplicateFrameOutcome,
				TEXT("AD.UMG.PROVIDER.INVALID_INVOCATION"))
			&& Fixture.CaptureDigest() == InitialDigest);

	FAssetDocumentRuntimeValueDto UnknownChannel =
		MakeTransformInput(PossessableGuid);
	FAssetDocumentRuntimeValueDto* UnknownChannels =
		FindMutableField(UnknownChannel, TEXT("channels"));
	if (!TestNotNull(TEXT("unknown channel fixture"), UnknownChannels))
	{
		return false;
	}
	UnknownChannels->ObjectKeys.Add(TEXT("translation_z"));
	UnknownChannels->Elements.Add(
		MakeChannel(0.0, {MakeKey(2, 3.0, TEXT("linear"))}));
	const FAssetDocumentPrimitiveOutcome UnknownChannelOutcome =
		Provider->Invoke(
			MakeInvocation(
				Provider.Get(),
				TEXT("umg.movie_scene.transform_track.add.v1"),
				Context,
				MoveTemp(UnknownChannel),
				{MakeHandle(
					TEXT("widget_animation"),
					AnimationHandle)}),
			Handles,
			&Transaction);
	TestTrue(
		TEXT("unknown transform channel fails before mutation"),
		UnknownChannelOutcome.Status
				== EAssetDocumentPrimitiveOutcomeStatus::Failed
			&& MovieSceneHasDiagnostic(
				UnknownChannelOutcome,
				TEXT("AD.UMG.PROVIDER.INVALID_INVOCATION"))
			&& Fixture.CaptureDigest() == InitialDigest);

	FAssetDocumentRuntimeValueDto UnsupportedValueType =
		MakeFloatInput(
			PossessableGuid,
			TEXT("UnsupportedValueType"),
			TEXT("UnsupportedValueType"));
	FAssetDocumentRuntimeValueDto* UnsupportedDefault =
		FindMutableField(
			UnsupportedValueType, TEXT("default_value"));
	if (!TestNotNull(TEXT("unsupported type fixture"), UnsupportedDefault))
	{
		return false;
	}
	*UnsupportedDefault =
		MakeAssetDocumentUMGString(TEXT("not-a-number"));
	const FAssetDocumentPrimitiveOutcome UnsupportedTypeOutcome =
		Provider->Invoke(
			MakeInvocation(
				Provider.Get(),
				TEXT("umg.movie_scene.float_track.add.v1"),
				Context,
				MoveTemp(UnsupportedValueType),
				{MakeHandle(
					TEXT("widget_animation"),
					AnimationHandle)}),
			Handles,
			&Transaction);
	TestTrue(
		TEXT("unsupported channel value type fails before mutation"),
		UnsupportedTypeOutcome.Status
				== EAssetDocumentPrimitiveOutcomeStatus::Failed
			&& MovieSceneHasDiagnostic(
				UnsupportedTypeOutcome,
				TEXT("AD.UMG.PROVIDER.INVALID_INVOCATION"))
			&& Fixture.CaptureDigest() == InitialDigest);

	const FAssetDocumentPrimitiveDescriptor* FloatDescriptor =
		FindMovieSceneDescriptor(
			Provider.Get(),
			TEXT("umg.movie_scene.float_track.add.v1"));
	if (!TestNotNull(TEXT("float descriptor"), FloatDescriptor))
	{
		return false;
	}
	FAssetDocumentPrimitiveInvocation TimelineInvocation =
		MakeAssetDocumentUMGProviderTestInvocation(
			*FloatDescriptor, Context);
	TimelineInvocation.PrimitiveId =
		TEXT("blueprint.timeline.float_track.add.v1");
	TimelineInvocation.ValueArguments =
		MakeFloatInput(
			PossessableGuid,
			TEXT("Timeline"),
			TEXT("Timeline"));
	TimelineInvocation.HandleArguments = {
		MakeHandle(
			TEXT("widget_animation"),
			AnimationHandle)};
	const FAssetDocumentPrimitiveOutcome TimelineOutcome =
		Provider->Invoke(
			TimelineInvocation, Handles, &Transaction);
	TestTrue(
		TEXT("Blueprint Timeline identity is not a P-UMG primitive."),
		TimelineOutcome.Status
				== EAssetDocumentPrimitiveOutcomeStatus::Failed
			&& MovieSceneHasDiagnostic(
				TimelineOutcome,
				TEXT("AD.UMG.PROVIDER.INVALID_INVOCATION"))
			&& Fixture.CaptureDigest() == InitialDigest);

	FAssetDocumentPrimitiveInvocation InvalidRange =
		MakeInvocation(
			Provider.Get(),
			TEXT("umg.movie_scene.float_track.add.v1"),
			Context,
			MakeAssetDocumentUMGObject({
				{TEXT("possessable_guid"),
					MakeAssetDocumentUMGString(
						PossessableGuid.ToString())},
				{TEXT("property_name"),
					MakeAssetDocumentUMGString(TEXT("Opacity"))},
				{TEXT("property_path"),
					MakeAssetDocumentUMGString(TEXT("RenderOpacity"))},
				{TEXT("section_start_frame"),
					MakeAssetDocumentUMGInteger(10)},
				{TEXT("section_end_frame"),
					MakeAssetDocumentUMGInteger(10)},
				{TEXT("default_value"),
					MakeAssetDocumentUMGNull()},
				{TEXT("keys"),
					MakeAssetDocumentUMGArray({})}
			}),
			{MakeHandle(TEXT("widget_animation"), AnimationHandle)});
	TestEqual(
		TEXT("invalid range fails before mutation"),
		Provider->Invoke(
			InvalidRange, Handles, &Transaction).Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);

	const FAssetDocumentPrimitiveOutcome FloatOutcome =
		Provider->Invoke(
			MakeInvocation(
				Provider.Get(),
				TEXT("umg.movie_scene.float_track.add.v1"),
				Context,
				MakeFloatInput(
					PossessableGuid,
					TEXT("Opacity"),
					TEXT("RenderOpacity")),
				{MakeHandle(
					TEXT("widget_animation"),
					AnimationHandle)}),
			Handles,
			&Transaction);
	MovieSceneRecordDiagnostics(
		*this, TEXT("float track add"), FloatOutcome);
	TestEqual(
		TEXT("float track lifecycle succeeds"),
		FloatOutcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestTrue(
		TEXT("float output track handle"),
		FindResultHandle(
			FloatOutcome, TEXT("movie_scene_track")).Slot != 0);
	TestTrue(
		TEXT("float output section handle"),
		FindResultHandle(
			FloatOutcome, TEXT("movie_scene_section")).Slot != 0);
	TestTrue(
		TEXT("float output channel handle"),
		FindResultHandle(
			FloatOutcome, TEXT("movie_scene_channel")).Slot != 0);
	UMovieSceneFloatTrack* FloatTrack =
		Cast<UMovieSceneFloatTrack>(
			MovieScene->FindTrack(
				UMovieSceneFloatTrack::StaticClass(),
				PossessableGuid,
				TEXT("RenderOpacity")));
	UMovieSceneFloatSection* FloatSection =
		FloatTrack != nullptr
			&& FloatTrack->GetAllSections().Num() == 1
			? Cast<UMovieSceneFloatSection>(
				FloatTrack->GetAllSections()[0])
			: nullptr;
	if (!TestNotNull(TEXT("float track readback"), FloatTrack)
		|| !TestNotNull(TEXT("float section readback"), FloatSection))
	{
		return false;
	}
	const FString AfterFloat = Fixture.CaptureDigest();
	const FAssetDocumentPrimitiveOutcome DuplicateTrackOutcome =
		Provider->Invoke(
			MakeInvocation(
				Provider.Get(),
				TEXT("umg.movie_scene.float_track.add.v1"),
				Context,
				MakeFloatInput(
					PossessableGuid,
					TEXT("Opacity"),
					TEXT("RenderOpacity")),
				{MakeHandle(
					TEXT("widget_animation"),
					AnimationHandle)}),
			Handles,
			&Transaction);
	TestTrue(
		TEXT("duplicate exact track identity fails without mutation"),
		DuplicateTrackOutcome.Status
				== EAssetDocumentPrimitiveOutcomeStatus::Failed
			&& MovieSceneHasDiagnostic(
				DuplicateTrackOutcome,
				TEXT("AD.UMG.MOVIE_SCENE.TRACK_IDENTITY_COLLISION"))
			&& Fixture.CaptureDigest() == AfterFloat);

	const FAssetDocumentPrimitiveOutcome TransformOutcome =
		Provider->Invoke(
			MakeInvocation(
				Provider.Get(),
				TEXT("umg.movie_scene.transform_track.add.v1"),
				Context,
				MakeTransformInput(PossessableGuid),
				{MakeHandle(
					TEXT("widget_animation"),
					AnimationHandle)}),
			Handles,
			&Transaction);
	MovieSceneRecordDiagnostics(
		*this, TEXT("transform track add"), TransformOutcome);
	TestEqual(
		TEXT("seven-channel transform lifecycle succeeds"),
		TransformOutcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	for (int32 Index = 0; Index < 7; ++Index)
	{
		TestTrue(
			TEXT("transform channel handle exists"),
			FindResultHandle(
				TransformOutcome,
				TEXT("movie_scene_channel"),
				Index).Slot != 0);
	}

	const FAssetDocumentOpaqueHandle StaleTrackHandle =
		FindResultHandle(
			FloatOutcome, TEXT("movie_scene_track"));
	const FAssetDocumentOpaqueHandle StaleSectionHandle =
		FindResultHandle(
			FloatOutcome, TEXT("movie_scene_section"));
	const FAssetDocumentOpaqueHandle StaleChannelHandle =
		FindResultHandle(
			FloatOutcome, TEXT("movie_scene_channel"));

	UWidgetAnimation* OtherAnimation =
		Fixture.AddFixtureAnimation(TEXT("WrongOwner"));
	if (!TestNotNull(
		TEXT("wrong-owner animation fixture"),
		OtherAnimation))
	{
		return false;
	}
	const FAssetDocumentOpaqueHandle OtherAnimationHandle =
		Handles.AddObjectHandle(
			OtherAnimation,
			EAssetDocumentHandleKind::Subobject,
			EAssetDocumentHandleAccess::Mutable);
	const FString BeforeWrongOwner = Fixture.CaptureDigest();
	const FAssetDocumentPrimitiveOutcome WrongOwner =
		InvokeTrackRemove(
			Provider.Get(), Context, Handles, Transaction,
			OtherAnimationHandle, StaleTrackHandle,
			nullptr, TEXT("track"));
	TestTrue(
		TEXT("track from another WidgetAnimation owner fails closed"),
		WrongOwner.Status
				== EAssetDocumentPrimitiveOutcomeStatus::Failed
			&& MovieSceneHasDiagnostic(
				WrongOwner,
				TEXT("AD.UMG.MOVIE_SCENE.OWNERSHIP_MISMATCH"))
			&& Fixture.CaptureDigest() == BeforeWrongOwner);

	Handles.AdvanceProviderGeneration();
	AnimationHandle =
		Handles.AddObjectHandle(
			Animation,
			EAssetDocumentHandleKind::Subobject,
			EAssetDocumentHandleAccess::Mutable);
	const FAssetDocumentOpaqueHandle TrackHandle =
		Handles.AddObjectHandle(
			FloatTrack,
			EAssetDocumentHandleKind::Track,
			EAssetDocumentHandleAccess::Mutable);
	const FAssetDocumentOpaqueHandle SectionHandle =
		Handles.AddObjectHandle(
			FloatSection,
			EAssetDocumentHandleKind::Section,
			EAssetDocumentHandleAccess::Mutable);
	const FString BeforeStaleHandles = Fixture.CaptureDigest();
	const FAssetDocumentPrimitiveOutcome StaleTrack =
		InvokeTrackRemove(
			Provider.Get(), Context, Handles, Transaction,
			AnimationHandle, StaleTrackHandle,
			nullptr, TEXT("track"));
	TestTrue(
		TEXT("stale track handle fails closed"),
		StaleTrack.Status
				== EAssetDocumentPrimitiveOutcomeStatus::Failed
			&& Fixture.CaptureDigest() == BeforeStaleHandles);
	const FAssetDocumentPrimitiveOutcome StaleSection =
		InvokeTrackRemove(
			Provider.Get(), Context, Handles, Transaction,
			AnimationHandle, TrackHandle,
			&StaleSectionHandle, TEXT("section"));
	TestTrue(
		TEXT("stale section handle fails closed"),
		StaleSection.Status
				== EAssetDocumentPrimitiveOutcomeStatus::Failed
			&& Fixture.CaptureDigest() == BeforeStaleHandles);
	const FAssetDocumentVerifiedLeaseResult StaleChannelLease =
		Handles.AcquireVerifiedLease(
			StaleChannelHandle,
			MakeChannelValidationContext(
				Context,
				FloatDescriptor->AllowedPhases[0]));
	TestTrue(
		TEXT("stale channel handle cannot acquire a verified lease"),
		StaleChannelLease.Lease == nullptr
			&& !StaleChannelLease.Diagnostics.IsEmpty());

	const FAssetDocumentPrimitiveOutcome SectionRemove =
		InvokeTrackRemove(
			Provider.Get(), Context, Handles, Transaction,
			AnimationHandle, TrackHandle,
			&SectionHandle, TEXT("section"));
	MovieSceneRecordDiagnostics(
		*this, TEXT("section remove"), SectionRemove);
	TestEqual(
		TEXT("section removal succeeds"),
		SectionRemove.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	const FAssetDocumentPrimitiveOutcome TrackRemove =
		InvokeTrackRemove(
			Provider.Get(), Context, Handles, Transaction,
			AnimationHandle, TrackHandle,
			nullptr, TEXT("track"));
	MovieSceneRecordDiagnostics(
		*this, TEXT("track remove"), TrackRemove);
	TestEqual(
		TEXT("track removal succeeds"),
		TrackRemove.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUMGMovieSceneRollbackTest,
	"UEAssetDoc.UMGProvider.MovieSceneRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUMGMovieSceneRollbackTest::RunTest(
	const FString& Parameters)
{
	using namespace AssetDocumentUMGMovieSceneProviderTestsPrivate;
	(void)Parameters;
	FAssetDocumentUMGTestUndoScope UnrealUndo(true);
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentUMGPrimitiveProvider();

	struct FRollbackBoundary
	{
		int32 PoisonStateRead = 0;
		const TCHAR* Label = nullptr;
	};
	const FRollbackBoundary CreationBoundaries[] = {
		{2, TEXT("after AddTrack")},
		{4, TEXT("after CreateNewSection")},
		{6, TEXT("after translation_x write")},
		{7, TEXT("after translation_y write")},
		{8, TEXT("after angle write")},
		{9, TEXT("after scale_x write")},
		{10, TEXT("after scale_y write")},
		{11, TEXT("after shear_x write")},
		{12, TEXT("after shear_y write")}};
	for (const FRollbackBoundary& Boundary : CreationBoundaries)
	{
		FAssetDocumentUMGProviderTestFixture Fixture;
		UWidgetAnimation* Animation = nullptr;
		UMovieScene* MovieScene = nullptr;
		FGuid PossessableGuid;
		if (!BuildAnimationFixture(
			*this, Fixture, Animation,
			MovieScene, PossessableGuid))
		{
			return false;
		}
		FAssetDocumentUMGProviderTestContext Context;
		FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
		FAssetDocumentUMGProviderTestTransaction Transaction(Context);
		const FAssetDocumentOpaqueHandle AnimationHandle =
			Handles.AddObjectHandle(
				Animation,
				EAssetDocumentHandleKind::Subobject,
				EAssetDocumentHandleAccess::Mutable);
		const FString Before = Fixture.CaptureDigest();
		Transaction.SetRestoreCallback(
			[MovieScene, PossessableGuid]()
			{
				const FMovieSceneBinding* Binding =
					MovieScene->FindBinding(PossessableGuid);
				const TArray<UMovieSceneTrack*> Tracks =
					Binding != nullptr
						? Binding->GetTracks()
						: TArray<UMovieSceneTrack*>();
				for (UMovieSceneTrack* Track : Tracks)
				{
					if (Track != nullptr
						&& Track->IsA(
							UMovieScene2DTransformTrack::StaticClass()))
					{
						MovieScene->RemoveTrack(*Track);
					}
				}
			});
		Transaction.SetPoisonAfterStateRead(
			Boundary.PoisonStateRead);
		const FAssetDocumentPrimitiveOutcome Outcome =
			Provider->Invoke(
				MakeInvocation(
					Provider.Get(),
					TEXT("umg.movie_scene.transform_track.add.v1"),
					Context,
					MakeTransformInput(PossessableGuid),
					{MakeHandle(
						TEXT("widget_animation"),
						AnimationHandle)}),
				Handles,
				&Transaction);
		TestTrue(
			*FString::Printf(
				TEXT("%s fails and rolls back"),
				Boundary.Label),
			Outcome.Status
					== EAssetDocumentPrimitiveOutcomeStatus::Failed
				&& Transaction.GetRollbackCount() == 1);
		TestEqual(
			*FString::Printf(
				TEXT("%s restores full digest"),
				Boundary.Label),
			Fixture.CaptureDigest(), Before);
		TestTrue(
			TEXT("creation footprint includes blueprint, animation, scene, planned track, section, channels and package"),
			Transaction.GetEnlistedFootprint().Num() >= 13);
	}

	struct FModifyFailureCase
	{
		bool bTransform = false;
		const TCHAR* PrimitiveId = nullptr;
		const TCHAR* AuthorityCode = nullptr;
		const TCHAR* Label = nullptr;
	};
	const FModifyFailureCase ModifyFailureCases[] = {
		{false,
			TEXT("umg.movie_scene.float_track.add.v1"),
			TEXT("AD.UMG.AUTHORITY.FLOAT_TRACK_CHAIN_UNADMITTED"),
			TEXT("float section Modify failure")},
		{true,
			TEXT("umg.movie_scene.transform_track.add.v1"),
			TEXT("AD.UMG.AUTHORITY.TRANSFORM_TRACK_CHAIN_UNADMITTED"),
			TEXT("transform section Modify failure")}};
	for (const FModifyFailureCase& FailureCase : ModifyFailureCases)
	{
		FAssetDocumentUMGProviderTestFixture Fixture;
		UWidgetAnimation* Animation = nullptr;
		UMovieScene* MovieScene = nullptr;
		FGuid PossessableGuid;
		if (!BuildAnimationFixture(
			*this, Fixture, Animation,
			MovieScene, PossessableGuid))
		{
			return false;
		}
		FAssetDocumentUMGProviderTestContext Context;
		FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
		FAssetDocumentUMGProviderTestTransaction Transaction(Context);
		const FAssetDocumentOpaqueHandle AnimationHandle =
			Handles.AddObjectHandle(
				Animation,
				EAssetDocumentHandleKind::Subobject,
				EAssetDocumentHandleAccess::Mutable);
		const FString Before = Fixture.CaptureDigest();
		Transaction.SetRestoreCallback(
			[MovieScene, PossessableGuid,
				bTransform = FailureCase.bTransform]()
			{
				const FMovieSceneBinding* Binding =
					MovieScene->FindBinding(PossessableGuid);
				const TArray<UMovieSceneTrack*> Tracks =
					Binding != nullptr
						? Binding->GetTracks()
						: TArray<UMovieSceneTrack*>();
				for (UMovieSceneTrack* Track : Tracks)
				{
					if (Track != nullptr
						&& (bTransform
							? Track->IsA(
								UMovieScene2DTransformTrack::StaticClass())
							: Track->IsA(
								UMovieSceneFloatTrack::StaticClass())))
					{
						MovieScene->RemoveTrack(*Track);
					}
				}
			});
		FAssetDocumentPrimitiveOutcome Outcome;
		{
			FAssetDocumentUMGTestUndoScope NoUnrealUndo(false);
			Outcome = Provider->Invoke(
				MakeInvocation(
					Provider.Get(),
					FailureCase.PrimitiveId,
					Context,
					FailureCase.bTransform
						? MakeTransformInput(PossessableGuid)
						: MakeFloatInput(
							PossessableGuid,
							TEXT("Opacity"),
							TEXT("RenderOpacity")),
					{MakeHandle(
						TEXT("widget_animation"),
						AnimationHandle)}),
				Handles,
				&Transaction);
		}
		TestTrue(
			*FString::Printf(
				TEXT("%s fails closed with its authority diagnostic"),
				FailureCase.Label),
			Outcome.Status
					== EAssetDocumentPrimitiveOutcomeStatus::Failed
				&& MovieSceneHasDiagnostic(
					Outcome, FailureCase.AuthorityCode)
				&& Transaction.GetRollbackCount() == 1);
		TestEqual(
			*FString::Printf(
				TEXT("%s restores full digest"),
				FailureCase.Label),
			Fixture.CaptureDigest(), Before);
	}

	FAssetDocumentUMGProviderTestFixture RemovalFixture;
	UWidgetAnimation* RemovalAnimation = nullptr;
	UMovieScene* RemovalMovieScene = nullptr;
	FGuid RemovalPossessableGuid;
	if (!BuildAnimationFixture(
		*this, RemovalFixture, RemovalAnimation,
		RemovalMovieScene, RemovalPossessableGuid))
	{
		return false;
	}
	UMovieSceneFloatTrack* RemovalTrack =
		RemovalMovieScene->AddTrack<UMovieSceneFloatTrack>(
			RemovalPossessableGuid);
	UMovieSceneFloatSection* RemovalSection =
		RemovalTrack != nullptr
			? Cast<UMovieSceneFloatSection>(
				RemovalTrack->CreateNewSection())
			: nullptr;
	if (!TestNotNull(TEXT("removal rollback track"), RemovalTrack)
		|| !TestNotNull(
			TEXT("removal rollback section"), RemovalSection))
	{
		return false;
	}
	RemovalTrack->SetPropertyNameAndPath(
		TEXT("Opacity"), TEXT("RenderOpacity"));
	static_cast<UMovieSceneTrack*>(RemovalTrack)->AddSection(
		UMovieSceneTrack::FSectionParameter(*RemovalSection));
	FAssetDocumentUMGProviderTestContext RemovalContext;
	FAssetDocumentUMGProviderTestHandleResolver RemovalHandles(
		RemovalContext);
	const FAssetDocumentOpaqueHandle RemovalAnimationHandle =
		RemovalHandles.AddObjectHandle(
			RemovalAnimation,
			EAssetDocumentHandleKind::Subobject,
			EAssetDocumentHandleAccess::Mutable);
	const FAssetDocumentOpaqueHandle RemovalTrackHandle =
		RemovalHandles.AddObjectHandle(
			RemovalTrack,
			EAssetDocumentHandleKind::Track,
			EAssetDocumentHandleAccess::Mutable);
	const FAssetDocumentOpaqueHandle RemovalSectionHandle =
		RemovalHandles.AddObjectHandle(
			RemovalSection,
			EAssetDocumentHandleKind::Section,
			EAssetDocumentHandleAccess::Mutable);

	{
		FAssetDocumentUMGProviderTestTransaction Transaction(
			RemovalContext);
		const FString Before = RemovalFixture.CaptureDigest();
		Transaction.SetRestoreCallback(
			[RemovalTrack, RemovalSection]()
			{
				if (!RemovalTrack->HasSection(*RemovalSection))
				{
					static_cast<UMovieSceneTrack*>(
						RemovalTrack)->AddSection(
						UMovieSceneTrack::FSectionParameter(
							*RemovalSection));
				}
			});
		Transaction.SetPoisonAfterStateRead(3);
		const FAssetDocumentPrimitiveOutcome Outcome =
			InvokeTrackRemove(
				Provider.Get(), RemovalContext,
				RemovalHandles, Transaction,
				RemovalAnimationHandle,
				RemovalTrackHandle,
				&RemovalSectionHandle,
				TEXT("section"));
		TestTrue(
			TEXT("failure after section removal rolls back"),
			Outcome.Status
					== EAssetDocumentPrimitiveOutcomeStatus::Failed
				&& Transaction.GetRollbackCount() == 1);
		TestEqual(
			TEXT("section removal rollback restores full digest"),
			RemovalFixture.CaptureDigest(), Before);
	}

	{
		FAssetDocumentUMGProviderTestTransaction Transaction(
			RemovalContext);
		const FString Before = RemovalFixture.CaptureDigest();
		Transaction.SetRestoreCallback(
			[RemovalMovieScene, RemovalTrack,
				RemovalPossessableGuid]()
			{
				FGuid ExistingBinding;
				if (!RemovalMovieScene->FindTrackBinding(
					*RemovalTrack, ExistingBinding))
				{
					RemovalMovieScene->AddGivenTrack(
						RemovalTrack,
						RemovalPossessableGuid);
				}
			});
		Transaction.SetPoisonAfterStateRead(3);
		const FAssetDocumentPrimitiveOutcome Outcome =
			InvokeTrackRemove(
				Provider.Get(), RemovalContext,
				RemovalHandles, Transaction,
				RemovalAnimationHandle,
				RemovalTrackHandle,
				nullptr,
				TEXT("track"));
		TestTrue(
			TEXT("failure after track removal rolls back"),
			Outcome.Status
					== EAssetDocumentPrimitiveOutcomeStatus::Failed
				&& Transaction.GetRollbackCount() == 1);
		TestEqual(
			TEXT("track removal rollback restores full digest"),
			RemovalFixture.CaptureDigest(), Before);
	}
	return true;
}
#endif
