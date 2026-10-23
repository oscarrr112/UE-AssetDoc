// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/MovieScene/AssetDocumentUMGWidgetAnimationHandlers.h"

#include "Provider/AssetDocumentUMGNativePayloads.h"
#include "Provider/AssetDocumentUMGValueDto.h"

#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/WidgetAnimation.h"
#include "WidgetBlueprint.h"
#include "Channels/MovieSceneChannelEditorData.h"
#include "Channels/MovieSceneChannelHandle.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSequence.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "Sections/MovieSceneFloatSection.h"

namespace AssetDocumentUMGWidgetAnimationHandlersPrivate
{
FAssetDocumentPrimitiveOutcome InvalidWidgetAnimationInvocation(const FString& Detail)
{
	return MakeAssetDocumentUMGFailure(
		TEXT("AD.UMG.PROVIDER.INVALID_INVOCATION"), *Detail);
}

FAssetDocumentRuntimeValueDto DescribeFloatChannel(
	FName Name,
	const FMovieSceneFloatChannel& Channel)
{
	TArray<FAssetDocumentRuntimeValueDto> Keys;
	const TArrayView<const FFrameNumber> Times = Channel.GetTimes();
	const TArrayView<const FMovieSceneFloatValue> Values = Channel.GetValues();
	for (int32 Index = 0; Index < FMath::Min(Times.Num(), Values.Num()); ++Index)
	{
		Keys.Add(MakeAssetDocumentUMGObject({
			{TEXT("frame"), MakeAssetDocumentUMGInteger(Times[Index].Value)},
			{TEXT("value"), MakeAssetDocumentUMGNumber(Values[Index].Value)}}));
	}
	return MakeAssetDocumentUMGObject({
		{TEXT("name"), MakeAssetDocumentUMGString(Name.ToString())},
		{TEXT("keys"), MakeAssetDocumentUMGArray(MoveTemp(Keys))}});
}

bool AddChannel(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	UMovieSceneSection& Section,
	FName Name,
	FMovieSceneFloatChannel& Channel,
	TArray<FAssetDocumentRuntimeValueDto>& OutChannels,
	FAssetDocumentPrimitiveOutcome& InOutOutcome)
{
	OutChannels.Add(DescribeFloatChannel(Name, Channel));
	return IssueAssetDocumentUMGChannelResult(
		Invocation, Handles, TEXT("movie_scene_channel"),
		EAssetDocumentHandleAccess::ReadOnly, &Section, &Channel, Name,
		InOutOutcome);
}

FName WidgetAnimationDtoNameForMask(EMovieScene2DTransformChannel Mask)
{
	switch (Mask)
	{
	case EMovieScene2DTransformChannel::TranslationX:
		return TEXT("translation_x");
	case EMovieScene2DTransformChannel::TranslationY:
		return TEXT("translation_y");
	case EMovieScene2DTransformChannel::Rotation:
		return TEXT("angle");
	case EMovieScene2DTransformChannel::ScaleX:
		return TEXT("scale_x");
	case EMovieScene2DTransformChannel::ScaleY:
		return TEXT("scale_y");
	case EMovieScene2DTransformChannel::ShearX:
		return TEXT("shear_x");
	case EMovieScene2DTransformChannel::ShearY:
		return TEXT("shear_y");
	default:
		return NAME_None;
	}
}

bool IsSingleTransformMask(EMovieScene2DTransformChannel Mask)
{
	return !WidgetAnimationDtoNameForMask(Mask).IsNone();
}

bool AddSectionFacts(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	UMovieSceneSection& Section,
	TArray<FAssetDocumentRuntimeValueDto>& OutSections,
	FAssetDocumentPrimitiveOutcome& InOutOutcome)
{
	if (!IssueAssetDocumentUMGUObjectResult(
			Invocation, Handles, TEXT("movie_scene_section"),
			EAssetDocumentHandleKind::Section, EAssetDocumentHandleAccess::ReadOnly,
			&Section, InOutOutcome))
	{
		return false;
	}

	TArray<FAssetDocumentRuntimeValueDto> Channels;
	if (UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(&Section))
	{
		if (!AddChannel(Invocation, Handles, Section, TEXT("float"),
			FloatSection->GetChannel(), Channels, InOutOutcome))
		{
			return false;
		}
	}
	else if (UMovieScene2DTransformSection* TransformSection =
		Cast<UMovieScene2DTransformSection>(&Section))
	{
		const FMovieSceneChannelProxy& Proxy =
			TransformSection->GetChannelProxy();
		const TArrayView<const FMovieSceneChannelMetaData> Metadata =
			Proxy.GetMetaData<FMovieSceneFloatChannel>();
		TSet<FName> ObservedDtoNames;
		TSet<uint32> ObservedMasks;
		if (Metadata.Num() != 7)
		{
			return false;
		}
		for (const FMovieSceneChannelMetaData& Meta : Metadata)
		{
			const EMovieScene2DTransformChannel Mask =
				TransformSection->GetMaskByName(
					Meta.Name).GetChannels();
			const FName DtoName = WidgetAnimationDtoNameForMask(Mask);
			TMovieSceneChannelHandle<FMovieSceneFloatChannel> Handle =
				Proxy.GetChannelByName<FMovieSceneFloatChannel>(
					Meta.Name);
			FMovieSceneFloatChannel* Channel = Handle.Get();
			const uint32 MaskValue = static_cast<uint32>(Mask);
			if (!IsSingleTransformMask(Mask)
				|| ObservedDtoNames.Contains(DtoName)
				|| ObservedMasks.Contains(MaskValue)
				|| Channel == nullptr
				|| !AddChannel(
					Invocation, Handles, Section,
					DtoName, *Channel,
					Channels, InOutOutcome))
			{
				return false;
			}
			ObservedDtoNames.Add(DtoName);
			ObservedMasks.Add(MaskValue);
		}
		if (ObservedDtoNames.Num() != 7
			|| ObservedMasks.Num() != 7)
		{
			return false;
		}
	}
	else
	{
		InOutOutcome.Diagnostics.Add({
			TEXT("/Animation/Section"),
			TEXT("AD.UMG.ANIMATION.EVIDENCE_UNAVAILABLE"),
			TEXT("The animation section has no admitted typed channel reader.")});
		return false;
	}

	OutSections.Add(MakeAssetDocumentUMGObject({
		{TEXT("name"), MakeAssetDocumentUMGString(Section.GetName())},
		{TEXT("class_path"), MakeAssetDocumentUMGString(Section.GetClass()->GetPathName())},
		{TEXT("channels"), MakeAssetDocumentUMGArray(MoveTemp(Channels))}}));
	return true;
}

bool AddAnimationFacts(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	UWidgetAnimation& Animation,
	TArray<FAssetDocumentRuntimeValueDto>& OutAnimationFacts,
	FAssetDocumentPrimitiveOutcome& InOutOutcome)
{
	UMovieSceneSequence& Sequence = Animation;
	UMovieScene* MovieScene = Sequence.GetMovieScene();
	if (!IsValid(MovieScene))
	{
		InOutOutcome.Diagnostics.Add({
			TEXT("/Animation"), TEXT("AD.UMG.ANIMATION.EVIDENCE_UNAVAILABLE"),
			TEXT("The WidgetAnimation has no live MovieScene.")});
		return false;
	}
	if (!IssueAssetDocumentUMGUObjectResult(
			Invocation, Handles, TEXT("widget_animation"),
			EAssetDocumentHandleKind::Subobject, EAssetDocumentHandleAccess::ReadOnly,
			&Animation, InOutOutcome))
	{
		return false;
	}

	TArray<FAssetDocumentRuntimeValueDto> Possessables;
	for (int32 Index = 0; Index < MovieScene->GetPossessableCount(); ++Index)
	{
		const FMovieScenePossessable& Possessable = MovieScene->GetPossessable(Index);
		const UClass* Class = Possessable.GetPossessedObjectClass();
		Possessables.Add(MakeAssetDocumentUMGObject({
			{TEXT("guid"), MakeAssetDocumentUMGString(Possessable.GetGuid().ToString())},
			{TEXT("name"), MakeAssetDocumentUMGString(Possessable.GetName())},
			{TEXT("class_path"), MakeAssetDocumentUMGString(
				Class != nullptr ? Class->GetPathName() : FString())}}));
	}

	TArray<FAssetDocumentRuntimeValueDto> BindingFacts;
	for (const FWidgetAnimationBinding& Binding : Animation.GetBindings())
	{
		BindingFacts.Add(MakeAssetDocumentUMGObject({
			{TEXT("possessable_guid"), MakeAssetDocumentUMGString(Binding.AnimationGuid.ToString())},
			{TEXT("widget_name"), MakeAssetDocumentUMGString(Binding.WidgetName.ToString())},
			{TEXT("slot_widget_name"), MakeAssetDocumentUMGString(Binding.SlotWidgetName.ToString())},
			{TEXT("is_root_widget"), MakeAssetDocumentUMGBool(Binding.bIsRootWidget)}}));
	}

	TArray<FAssetDocumentRuntimeValueDto> Tracks;
	TSet<UMovieSceneTrack*> SeenTracks;
	auto AddTrackFacts = [&](
		UMovieSceneTrack* Track) -> bool
	{
		if (SeenTracks.Contains(Track))
		{
			return true;
		}
		if (!IsValid(Track)
			|| !IssueAssetDocumentUMGUObjectResult(
				Invocation, Handles, TEXT("movie_scene_track"),
				EAssetDocumentHandleKind::Track, EAssetDocumentHandleAccess::ReadOnly,
				Track, InOutOutcome))
		{
			return false;
		}
		SeenTracks.Add(Track);
		TArray<FAssetDocumentRuntimeValueDto> Sections;
		for (UMovieSceneSection* Section : Track->GetAllSections())
		{
			if (!IsValid(Section)
				|| !AddSectionFacts(Invocation, Handles, *Section, Sections, InOutOutcome))
			{
				return false;
			}
		}
		Tracks.Add(MakeAssetDocumentUMGObject({
			{TEXT("name"), MakeAssetDocumentUMGString(Track->GetName())},
			{TEXT("class_path"), MakeAssetDocumentUMGString(Track->GetClass()->GetPathName())},
			{TEXT("sections"), MakeAssetDocumentUMGArray(MoveTemp(Sections))}}));
		return true;
	};
	for (UMovieSceneTrack* Track : MovieScene->GetTracks())
	{
		if (!AddTrackFacts(Track))
		{
			return false;
		}
	}
	const UMovieScene& ReadOnlyMovieScene = *MovieScene;
	for (const FMovieSceneBinding& Binding :
		ReadOnlyMovieScene.GetBindings())
	{
		for (UMovieSceneTrack* Track : Binding.GetTracks())
		{
			if (!AddTrackFacts(Track))
			{
				return false;
			}
		}
	}

	const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
	const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
	OutAnimationFacts.Add(MakeAssetDocumentUMGObject({
		{TEXT("name"), MakeAssetDocumentUMGString(Animation.GetName())},
		{TEXT("start_time"), MakeAssetDocumentUMGNumber(Animation.GetStartTime())},
		{TEXT("end_time"), MakeAssetDocumentUMGNumber(Animation.GetEndTime())},
		{TEXT("display_rate_numerator"), MakeAssetDocumentUMGInteger(DisplayRate.Numerator)},
		{TEXT("display_rate_denominator"), MakeAssetDocumentUMGInteger(DisplayRate.Denominator)},
		{TEXT("playback_start_frame"), MakeAssetDocumentUMGInteger(PlaybackRange.GetLowerBoundValue().Value)},
		{TEXT("playback_end_frame"), MakeAssetDocumentUMGInteger(PlaybackRange.GetUpperBoundValue().Value)},
		{TEXT("possessables"), MakeAssetDocumentUMGArray(MoveTemp(Possessables))},
		{TEXT("bindings"), MakeAssetDocumentUMGArray(MoveTemp(BindingFacts))},
		{TEXT("tracks"), MakeAssetDocumentUMGArray(MoveTemp(Tracks))}}));
	return true;
}
} // namespace AssetDocumentUMGWidgetAnimationHandlersPrivate


FAssetDocumentPrimitiveOutcome InvokeWidgetAnimationQuery(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AssetDocumentUMGWidgetAnimationHandlersPrivate;
	FAssetDocumentUMGAnimationQueryInput Input;
	FString DecodeDiagnostic;
	if (!DecodeAnimationQueryInput(Invocation.ValueArguments, Input, DecodeDiagnostic))
	{
		return InvalidWidgetAnimationInvocation(DecodeDiagnostic);
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	UWidgetBlueprint* Blueprint = ResolveWidgetBlueprint(
		Invocation, Handles, TEXT("widget_blueprint"),
		EAssetDocumentHandleAccess::ReadOnly, Diagnostics);
	if (Blueprint == nullptr)
	{
		return MakeAssetDocumentUMGFailure(
			TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
			TEXT("The WidgetBlueprint handle is invalid."), Diagnostics);
	}

	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	TArray<FAssetDocumentRuntimeValueDto> Animations;
	for (UWidgetAnimation* Animation : Blueprint->Animations)
	{
		if (!IsValid(Animation)
			|| (Input.AnimationName.IsSet()
				&& Animation->GetFName() != Input.AnimationName.GetValue()))
		{
			continue;
		}
		if (!AddAnimationFacts(Invocation, Handles, *Animation, Animations, Outcome))
		{
			return MakeAssetDocumentUMGFailure(
				TEXT("AD.UMG.ANIMATION.EVIDENCE_UNAVAILABLE"),
				TEXT("The WidgetAnimation could not be read through admitted public APIs."),
				Outcome.Diagnostics);
		}
	}
	if (Input.AnimationName.IsSet() && Animations.IsEmpty())
	{
		return MakeAssetDocumentUMGFailure(
			TEXT("AD.UMG.ANIMATION.NOT_FOUND"),
			TEXT("The requested WidgetAnimation is not present on the WidgetBlueprint."));
	}
	Outcome.Value = MakeAssetDocumentUMGObject({
		{TEXT("animations"), MakeAssetDocumentUMGArray(MoveTemp(Animations))}});
	return Outcome;
}

TConstArrayView<FAssetDocumentUMGPrimitiveBinding>
GetAssetDocumentUMGWidgetAnimationPrimitiveBindings()
{
	static const FAssetDocumentUMGPrimitiveBinding Bindings[] = {
		{TEXT("umg.animation.query.v1"), 1, &InvokeWidgetAnimationQuery}};
	return Bindings;
}
