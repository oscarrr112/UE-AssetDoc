// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/MovieScene/AssetDocumentUMGMovieSceneHandlers.h"

#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/WidgetAnimation.h"
#include "WidgetBlueprint.h"
#include "Channels/MovieSceneChannelEditorData.h"
#include "Channels/MovieSceneChannelHandle.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Curves/RichCurve.h"
#include "KeyParams.h"
#include "MovieScene.h"
#include "MovieSceneSequence.h"
#include "MovieSceneSignedObject.h"
#include "MovieSceneTrack.h"
#include "Provider/AssetDocumentUMGMutationContract.h"
#include "Provider/AssetDocumentUMGNativePayloads.h"
#include "Provider/AssetDocumentUMGValueDto.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"

namespace AssetDocumentUMGMovieSceneHandlersPrivate
{
struct FTrackContext
{
	UWidgetAnimation* Animation = nullptr;
	UMovieScene* MovieScene = nullptr;
	UWidgetBlueprint* Blueprint = nullptr;
};

struct FTransformChannelAuthority
{
	FName DtoName;
	FName MetadataName;
	EMovieScene2DTransformChannel Mask =
		EMovieScene2DTransformChannel::None;
	FMovieSceneFloatChannel* Channel = nullptr;
};

FAssetDocumentRuntimeDiagnostic MakeDiagnostic(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/MovieScene");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Diagnostic;
}

FAssetDocumentPrimitiveOutcome FailMovieScene(
	const TCHAR* Code,
	const TCHAR* Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics = {})
{
	return MakeAssetDocumentUMGFailure(Code, Message, Diagnostics);
}

FAssetDocumentPrimitiveOutcome FailMovieSceneAfterMutation(
	IAssetDocumentAtomicTransaction& Transaction,
	const TCHAR* Code,
	const TCHAR* Message,
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics = {})
{
	if (Diagnostics.IsEmpty())
	{
		Diagnostics.Add(MakeDiagnostic(Code, Message));
	}
	RollbackAssetDocumentUMGMutation(
		Transaction, Code, Diagnostics);
	return MakeAssetDocumentUMGFailure(
		Code, Message, Diagnostics);
}

bool IsTransactionStillMutating(
	IAssetDocumentAtomicTransaction& Transaction)
{
	return Transaction.GetState()
		== EAssetDocumentAtomicState::Mutating;
}

bool ResolveTrackContext(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FTrackContext& Out,
	TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
	Out.Animation = ResolveWidgetAnimation(
		Invocation, Handles, TEXT("widget_animation"),
		EAssetDocumentHandleAccess::Mutable, Diagnostics);
	if (Out.Animation == nullptr)
	{
		return false;
	}
	UMovieSceneSequence& Sequence = *Out.Animation;
	Out.MovieScene = Sequence.GetMovieScene();
	Out.Blueprint = Cast<UWidgetBlueprint>(
		Out.Animation->GetOuter());
	if (!IsValid(Out.MovieScene)
		|| !IsValid(Out.Blueprint)
		|| Out.MovieScene->GetOuter() != Out.Animation)
	{
		Diagnostics.Add(MakeDiagnostic(
			TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
			TEXT("The animation, MovieScene, and WidgetBlueprint ownership chain is invalid.")));
		return false;
	}
	return true;
}

bool AddPlannedFootprint(
	TArray<FAssetDocumentMutationFootprint>& Footprint,
	const FString& StableId)
{
	for (const FAssetDocumentMutationFootprint& Item : Footprint)
	{
		if (Item.StableId == StableId)
		{
			return false;
		}
	}
	Footprint.Add({
		EAssetDocumentMutationFootprintKind::UObjectState,
		StableId,
		TEXT("rollback.uobject")});
	return true;
}

FAssetDocumentContractValidationResult EnlistTrackCreation(
	IAssetDocumentAtomicTransaction& Transaction,
	const FTrackContext& Context,
	const FGuid& PossessableGuid,
	FName PropertyName,
	const FString& PropertyPath,
	bool bTransform,
	TConstArrayView<FName> ChannelNames)
{
	TArray<UObject*> Existing = {
		Context.Animation, Context.MovieScene};
	TArray<FAssetDocumentMutationFootprint> Footprint =
		BuildWidgetBlueprintMutationFootprint(
			*Context.Blueprint, Existing);
	const FString Prefix = FString::Printf(
		TEXT("%s#%s:%s:%s:%s"),
		*Context.MovieScene->GetPathName(),
		bTransform ? TEXT("TransformTrack") : TEXT("FloatTrack"),
		*PossessableGuid.ToString(EGuidFormats::DigitsWithHyphens),
		*PropertyName.ToString(),
		*PropertyPath);
	AddPlannedFootprint(Footprint, Prefix);
	AddPlannedFootprint(Footprint, Prefix + TEXT("#Section"));
	for (FName ChannelName : ChannelNames)
	{
		AddPlannedFootprint(
			Footprint,
			Prefix + TEXT("#Channel:") + ChannelName.ToString());
	}
	return Transaction.EnlistFootprint(Footprint);
}

bool IsExactTrackIdentity(
	UMovieScenePropertyTrack& Track,
	const FGuid& PossessableGuid,
	FName PropertyName,
	const FString& PropertyPath,
	UMovieScene& MovieScene)
{
	FGuid ObservedGuid;
	return Track.GetOuter() == &MovieScene
		&& MovieScene.FindTrackBinding(Track, ObservedGuid)
		&& ObservedGuid == PossessableGuid
		&& Track.GetPropertyName() == PropertyName
		&& Track.GetPropertyPath() == FName(*PropertyPath);
}

EMovieSceneKeyInterpolation DecodeInterpolation(
	const FString& Value)
{
	if (Value == TEXT("auto"))
	{
		return EMovieSceneKeyInterpolation::Auto;
	}
	if (Value == TEXT("user"))
	{
		return EMovieSceneKeyInterpolation::User;
	}
	if (Value == TEXT("break"))
	{
		return EMovieSceneKeyInterpolation::Break;
	}
	if (Value == TEXT("linear"))
	{
		return EMovieSceneKeyInterpolation::Linear;
	}
	if (Value == TEXT("constant"))
	{
		return EMovieSceneKeyInterpolation::Constant;
	}
	return EMovieSceneKeyInterpolation::SmartAuto;
}

bool DoesKeyModeMatch(
	const FMovieSceneFloatValue& Actual,
	EMovieSceneKeyInterpolation Expected)
{
	switch (Expected)
	{
	case EMovieSceneKeyInterpolation::Linear:
		return Actual.InterpMode == RCIM_Linear;
	case EMovieSceneKeyInterpolation::Constant:
		return Actual.InterpMode == RCIM_Constant;
	case EMovieSceneKeyInterpolation::Auto:
		return Actual.InterpMode == RCIM_Cubic
			&& Actual.TangentMode == RCTM_Auto;
	case EMovieSceneKeyInterpolation::User:
		return Actual.InterpMode == RCIM_Cubic
			&& Actual.TangentMode == RCTM_User;
	case EMovieSceneKeyInterpolation::Break:
		return Actual.InterpMode == RCIM_Cubic
			&& Actual.TangentMode == RCTM_Break;
	case EMovieSceneKeyInterpolation::SmartAuto:
		return Actual.InterpMode == RCIM_Cubic
			&& Actual.TangentMode == RCTM_SmartAuto;
	default:
		return false;
	}
}

void MutateChannel(
	FMovieSceneFloatChannel& Channel,
	const FAssetDocumentUMGFloatChannelInput& Input)
{
	if (Input.DefaultValue.IsSet())
	{
		Channel.SetDefault(Input.DefaultValue.GetValue());
	}
	for (const FAssetDocumentUMGFloatKeyInput& Key : Input.Keys)
	{
		AddKeyToChannel(
			&Channel,
			FFrameNumber(Key.Frame),
			Key.Value,
			DecodeInterpolation(Key.Interpolation));
	}
}

bool ValidateChannelReadback(
	const FMovieSceneFloatChannel& Channel,
	const FAssetDocumentUMGFloatChannelInput& Input)
{
	const TOptional<float> ObservedDefault = Channel.GetDefault();
	if (ObservedDefault.IsSet() != Input.DefaultValue.IsSet()
		|| (ObservedDefault.IsSet()
			&& !FMath::IsNearlyEqual(
				ObservedDefault.GetValue(),
				Input.DefaultValue.GetValue())))
	{
		return false;
	}
	const TArrayView<const FFrameNumber> Times = Channel.GetTimes();
	const TArrayView<const FMovieSceneFloatValue> Values =
		Channel.GetValues();
	if (Times.Num() != Input.Keys.Num()
		|| Values.Num() != Input.Keys.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < Input.Keys.Num(); ++Index)
	{
		const FAssetDocumentUMGFloatKeyInput& Expected =
			Input.Keys[Index];
		if (Times[Index] != FFrameNumber(Expected.Frame)
			|| !FMath::IsNearlyEqual(
				Values[Index].Value, Expected.Value)
			|| !DoesKeyModeMatch(
				Values[Index],
				DecodeInterpolation(Expected.Interpolation)))
		{
			return false;
		}
	}
	return true;
}

bool IsSingleSupportedMask(
	EMovieScene2DTransformChannel Mask)
{
	return Mask == EMovieScene2DTransformChannel::TranslationX
		|| Mask == EMovieScene2DTransformChannel::TranslationY
		|| Mask == EMovieScene2DTransformChannel::Rotation
		|| Mask == EMovieScene2DTransformChannel::ScaleX
		|| Mask == EMovieScene2DTransformChannel::ScaleY
		|| Mask == EMovieScene2DTransformChannel::ShearX
		|| Mask == EMovieScene2DTransformChannel::ShearY;
}

FName MovieSceneDtoNameForMask(EMovieScene2DTransformChannel Mask)
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

bool BuildTransformAuthority(
	UMovieScene2DTransformSection& Section,
	TArray<FTransformChannelAuthority>& Out)
{
	const FMovieSceneChannelProxy& Proxy =
		Section.GetChannelProxy();
	const TArrayView<const FMovieSceneChannelMetaData> Metadata =
		Proxy.GetMetaData<FMovieSceneFloatChannel>();
	if (Metadata.Num() != 7)
	{
		return false;
	}
	TSet<FName> DtoNames;
	TSet<uint32> Masks;
	for (const FMovieSceneChannelMetaData& Meta : Metadata)
	{
		EMovieScene2DTransformChannel Mask =
			Section.GetMaskByName(Meta.Name).GetChannels();
		if (!IsSingleSupportedMask(Mask) && !Meta.Group.IsEmpty())
		{
			Mask = Section.GetMaskByName(
				FName(*Meta.Group.ToString())).GetChannels();
		}
		const FName DtoName = MovieSceneDtoNameForMask(Mask);
		TMovieSceneChannelHandle<FMovieSceneFloatChannel> Handle =
			Proxy.GetChannelByName<FMovieSceneFloatChannel>(Meta.Name);
		FMovieSceneFloatChannel* Channel = Handle.Get();
		const uint32 MaskValue = static_cast<uint32>(Mask);
		if (!IsSingleSupportedMask(Mask)
			|| DtoName.IsNone()
			|| DtoNames.Contains(DtoName)
			|| Masks.Contains(MaskValue)
			|| Channel == nullptr)
		{
			return false;
		}
		DtoNames.Add(DtoName);
		Masks.Add(MaskValue);
		Out.Add({DtoName, Meta.Name, Mask, Channel});
	}
	return DtoNames.Num() == 7 && Masks.Num() == 7;
}

bool ValidateTrackReadback(
	UMovieScene& Scene,
	UMovieScenePropertyTrack& Track,
	UMovieSceneSection& Section,
	TSubclassOf<UMovieSceneTrack> TrackClass,
	const FGuid& PossessableGuid,
	FName PropertyName,
	const FString& PropertyPath,
	int32 StartFrame,
	int32 EndFrame)
{
	UMovieSceneTrack* Found = Scene.FindTrack(
		TrackClass,
		PossessableGuid,
		FName(*PropertyPath));
	return Found == &Track
		&& IsExactTrackIdentity(
			Track, PossessableGuid,
			PropertyName, PropertyPath, Scene)
		&& Track.HasSection(Section)
		&& Track.GetAllSections().Contains(&Section)
		&& Section.GetOuter() == &Track
		&& Section.GetRange()
			== TRange<FFrameNumber>(
				TRangeBound<FFrameNumber>::Inclusive(
					FFrameNumber(StartFrame)),
				TRangeBound<FFrameNumber>::Exclusive(
					FFrameNumber(EndFrame)))
		&& Track.GetSignature().IsValid()
		&& Section.GetSignature().IsValid();
}

FAssetDocumentRuntimeValueDto MakeTrackOutput(
	UMovieScenePropertyTrack& Track,
	int32 StartFrame,
	int32 EndFrame,
	int32 KeyCount)
{
	return MakeAssetDocumentUMGObject({
		{TEXT("track_signature"),
			MakeAssetDocumentUMGString(
				Track.GetSignature().ToString())},
		{TEXT("property_name"),
			MakeAssetDocumentUMGString(
				Track.GetPropertyName().ToString())},
		{TEXT("property_path"),
			MakeAssetDocumentUMGString(
				Track.GetPropertyPath().ToString())},
		{TEXT("section_range"),
			MakeAssetDocumentUMGArray({
				MakeAssetDocumentUMGInteger(StartFrame),
				MakeAssetDocumentUMGInteger(EndFrame)})},
		{TEXT("key_count"),
			MakeAssetDocumentUMGInteger(KeyCount)}
	});
}

bool IssueTrackSection(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	UMovieSceneTrack& Track,
	UMovieSceneSection& Section,
	FAssetDocumentPrimitiveOutcome& Outcome)
{
	return IssueAssetDocumentUMGUObjectResult(
			Invocation, Handles, TEXT("movie_scene_track"),
			EAssetDocumentHandleKind::Track,
			EAssetDocumentHandleAccess::Mutable,
			&Track, Outcome)
		&& IssueAssetDocumentUMGUObjectResult(
			Invocation, Handles, TEXT("movie_scene_section"),
			EAssetDocumentHandleKind::Section,
			EAssetDocumentHandleAccess::Mutable,
			&Section, Outcome);
}
} // namespace AssetDocumentUMGMovieSceneHandlersPrivate


FAssetDocumentPrimitiveOutcome InvokeAssetDocumentUMGFloatTrackAdd(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AssetDocumentUMGMovieSceneHandlersPrivate;
	FAssetDocumentUMGFloatTrackAddInput Input;
	FString DecodeDiagnostic;
	if (!DecodeFloatTrackAddInput(
		Invocation.ValueArguments, Input, DecodeDiagnostic))
	{
		return FailMovieScene(
			TEXT("AD.UMG.PROVIDER.INVALID_INVOCATION"),
			*DecodeDiagnostic);
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	FTrackContext Context;
	if (!ResolveTrackContext(
		Invocation, Handles, Context, Diagnostics))
	{
		return FailMovieScene(
			TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
			TEXT("The MovieScene input handles are invalid."),
			Diagnostics);
	}
	if (Context.MovieScene->FindPossessable(Input.PossessableGuid)
		== nullptr)
	{
		return FailMovieScene(
			TEXT("AD.UMG.MOVIE_SCENE.POSSESSABLE_NOT_FOUND"),
			TEXT("The requested possessable does not exist."));
	}
	if (Context.MovieScene->FindTrack(
		UMovieSceneFloatTrack::StaticClass(),
		Input.PossessableGuid,
		FName(*Input.PropertyPath)) != nullptr)
	{
		return FailMovieScene(
			TEXT("AD.UMG.MOVIE_SCENE.TRACK_IDENTITY_COLLISION"),
			TEXT("The exact float track identity already exists."));
	}
	if (ActiveTransaction == nullptr)
	{
		return FailMovieScene(
			TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("The float track mutation requires a transaction."));
	}
	const FName ChannelName(TEXT("value"));
	const FAssetDocumentContractValidationResult Enlist =
		EnlistTrackCreation(
			*ActiveTransaction, Context,
			Input.PossessableGuid,
			Input.PropertyName,
			Input.PropertyPath,
			false,
			MakeArrayView(&ChannelName, 1));
	if (!Enlist.bValid)
	{
		return FailMovieScene(
			TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("The complete float track footprint was rejected."),
			Enlist.Diagnostics);
	}
	UMovieSceneFloatTrack* Track = Cast<UMovieSceneFloatTrack>(
		Context.MovieScene->AddTrack(
			UMovieSceneFloatTrack::StaticClass(),
			Input.PossessableGuid));
	if (!IsValid(Track)
		|| Track->GetOuter() != Context.MovieScene
		|| !IsTransactionStillMutating(*ActiveTransaction))
	{
		return FailMovieSceneAfterMutation(
			*ActiveTransaction,
			TEXT("AD.UMG.AUTHORITY.FLOAT_TRACK_CHAIN_UNADMITTED"),
			TEXT("The public AddTrack operation failed."));
	}
	Track->SetPropertyNameAndPath(
		Input.PropertyName, Input.PropertyPath);
	if (!IsTransactionStillMutating(*ActiveTransaction))
	{
		return FailMovieSceneAfterMutation(
			*ActiveTransaction,
			TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("The transaction stopped mutating after the float track property edit."));
	}
	UMovieSceneFloatSection* Section =
		Cast<UMovieSceneFloatSection>(
			Track->CreateNewSection());
	if (!IsValid(Section) || Section->GetOuter() != Track
		|| !IsTransactionStillMutating(*ActiveTransaction))
	{
		return FailMovieSceneAfterMutation(
			*ActiveTransaction,
			TEXT("AD.UMG.AUTHORITY.FLOAT_TRACK_CHAIN_UNADMITTED"),
			TEXT("The public CreateNewSection operation failed."));
	}
	static_cast<UMovieSceneTrack*>(Track)->AddSection(
		UMovieSceneTrack::FSectionParameter(*Section));
	if (!IsTransactionStillMutating(*ActiveTransaction))
	{
		return FailMovieSceneAfterMutation(
			*ActiveTransaction,
			TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("The transaction stopped mutating after the float section attach."));
	}
	if (!Section->Modify())
	{
		return FailMovieSceneAfterMutation(
			*ActiveTransaction,
			TEXT("AD.UMG.AUTHORITY.FLOAT_TRACK_CHAIN_UNADMITTED"),
			TEXT("The public float section Modify operation failed."));
	}
	Section->SetRange(TRange<FFrameNumber>(
		TRangeBound<FFrameNumber>::Inclusive(
			FFrameNumber(Input.SectionStartFrame)),
		TRangeBound<FFrameNumber>::Exclusive(
			FFrameNumber(Input.SectionEndFrame))));
	FMovieSceneFloatChannel& Channel = Section->GetChannel();
	MutateChannel(Channel, Input.Channel);
	if (!IsTransactionStillMutating(*ActiveTransaction))
	{
		return FailMovieSceneAfterMutation(
			*ActiveTransaction,
			TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("The transaction stopped mutating after the float channel edit."));
	}
	const bool bReadback =
		ValidateTrackReadback(
			*Context.MovieScene, *Track, *Section,
			UMovieSceneFloatTrack::StaticClass(),
			Input.PossessableGuid, Input.PropertyName,
			Input.PropertyPath,
			Input.SectionStartFrame,
			Input.SectionEndFrame)
		&& ValidateChannelReadback(Channel, Input.Channel);
	if (!ValidateWidgetBlueprintMutationCompletion(
		bReadback,
		TEXT("The public float track readback did not match the request."),
		Diagnostics))
	{
		return FailMovieSceneAfterMutation(
			*ActiveTransaction,
			TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"),
			TEXT("The float track postcondition failed."),
			MoveTemp(Diagnostics));
	}
	FAssetDocumentPrimitiveOutcome Outcome =
		MakeAssetDocumentUMGSuccess(MakeTrackOutput(
			*Track,
			Input.SectionStartFrame,
			Input.SectionEndFrame,
			Input.Channel.Keys.Num()));
	if (!IssueTrackSection(
			Invocation, Handles, *Track, *Section, Outcome)
		|| !IssueAssetDocumentUMGChannelResult(
			Invocation, Handles,
			TEXT("movie_scene_channel"),
			EAssetDocumentHandleAccess::Mutable,
			Section, &Channel, ChannelName, Outcome))
	{
		return FailMovieSceneAfterMutation(
			*ActiveTransaction,
			TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
			TEXT("The float track output handles could not be issued."),
			MoveTemp(Outcome.Diagnostics));
	}
	Outcome.ChangedSet = {
		MakeAssetDocumentUMGChangedRecord(
			*Context.Animation,
			TEXT("/animation/movie_scene"),
			TEXT("modify")),
		MakeAssetDocumentUMGChangedRecord(
			*Track, TEXT("/track"), TEXT("add")),
		MakeAssetDocumentUMGChangedRecord(
			*Section, TEXT("/section"), TEXT("add"))
	};
	return Outcome;
}

FAssetDocumentPrimitiveOutcome InvokeAssetDocumentUMGTransformTrackAdd(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AssetDocumentUMGMovieSceneHandlersPrivate;
	FAssetDocumentUMGTransformTrackAddInput Input;
	FString DecodeDiagnostic;
	if (!DecodeTransformTrackAddInput(
		Invocation.ValueArguments, Input, DecodeDiagnostic))
	{
		return FailMovieScene(
			TEXT("AD.UMG.PROVIDER.INVALID_INVOCATION"),
			*DecodeDiagnostic);
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	FTrackContext Context;
	if (!ResolveTrackContext(
		Invocation, Handles, Context, Diagnostics))
	{
		return FailMovieScene(
			TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
			TEXT("The MovieScene input handles are invalid."),
			Diagnostics);
	}
	if (Context.MovieScene->FindPossessable(Input.PossessableGuid)
		== nullptr)
	{
		return FailMovieScene(
			TEXT("AD.UMG.MOVIE_SCENE.POSSESSABLE_NOT_FOUND"),
			TEXT("The requested possessable does not exist."));
	}
	if (Context.MovieScene->FindTrack(
		UMovieScene2DTransformTrack::StaticClass(),
		Input.PossessableGuid,
		FName(*Input.PropertyPath)) != nullptr)
	{
		return FailMovieScene(
			TEXT("AD.UMG.MOVIE_SCENE.TRACK_IDENTITY_COLLISION"),
			TEXT("The exact transform track identity already exists."));
	}
	if (ActiveTransaction == nullptr)
	{
		return FailMovieScene(
			TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("The transform track mutation requires a transaction."));
	}
	TArray<FName> RequestedChannelNames;
	Input.Channels.GetKeys(RequestedChannelNames);
	const FAssetDocumentContractValidationResult Enlist =
		EnlistTrackCreation(
			*ActiveTransaction, Context,
			Input.PossessableGuid,
			Input.PropertyName,
			Input.PropertyPath,
			true,
			RequestedChannelNames);
	if (!Enlist.bValid)
	{
		return FailMovieScene(
			TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("The complete transform track footprint was rejected."),
			Enlist.Diagnostics);
	}
	UMovieScene2DTransformTrack* Track =
		Cast<UMovieScene2DTransformTrack>(
			Context.MovieScene->AddTrack(
				UMovieScene2DTransformTrack::StaticClass(),
				Input.PossessableGuid));
	if (!IsValid(Track)
		|| Track->GetOuter() != Context.MovieScene
		|| !IsTransactionStillMutating(*ActiveTransaction))
	{
		return FailMovieSceneAfterMutation(
			*ActiveTransaction,
			TEXT("AD.UMG.AUTHORITY.TRANSFORM_TRACK_CHAIN_UNADMITTED"),
			TEXT("The public transform AddTrack operation failed."));
	}
	Track->SetPropertyNameAndPath(
		Input.PropertyName, Input.PropertyPath);
	if (!IsTransactionStillMutating(*ActiveTransaction))
	{
		return FailMovieSceneAfterMutation(
			*ActiveTransaction,
			TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("The transaction stopped mutating after the transform track property edit."));
	}
	UMovieScene2DTransformSection* Section =
		Cast<UMovieScene2DTransformSection>(
			Track->CreateNewSection());
	if (!IsValid(Section) || Section->GetOuter() != Track
		|| !IsTransactionStillMutating(*ActiveTransaction))
	{
		return FailMovieSceneAfterMutation(
			*ActiveTransaction,
			TEXT("AD.UMG.AUTHORITY.TRANSFORM_TRACK_CHAIN_UNADMITTED"),
			TEXT("The public transform CreateNewSection operation failed."));
	}
	static_cast<UMovieSceneTrack*>(Track)->AddSection(
		UMovieSceneTrack::FSectionParameter(*Section));
	if (!IsTransactionStillMutating(*ActiveTransaction))
	{
		return FailMovieSceneAfterMutation(
			*ActiveTransaction,
			TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("The transaction stopped mutating after the transform section attach."));
	}
	if (!Section->Modify())
	{
		return FailMovieSceneAfterMutation(
			*ActiveTransaction,
			TEXT("AD.UMG.AUTHORITY.TRANSFORM_TRACK_CHAIN_UNADMITTED"),
			TEXT("The public transform section Modify operation failed."));
	}
	Section->SetRange(TRange<FFrameNumber>(
		TRangeBound<FFrameNumber>::Inclusive(
			FFrameNumber(Input.SectionStartFrame)),
		TRangeBound<FFrameNumber>::Exclusive(
			FFrameNumber(Input.SectionEndFrame))));
	TArray<FTransformChannelAuthority> Authority;
	if (!BuildTransformAuthority(*Section, Authority))
	{
		return FailMovieSceneAfterMutation(
			*ActiveTransaction,
			TEXT("AD.UMG.AUTHORITY.TRANSFORM_TRACK_CHAIN_UNADMITTED"),
			TEXT("The public transform channel metadata authority is not a seven-row bijection."));
	}
	EMovieScene2DTransformChannel Mask =
		EMovieScene2DTransformChannel::None;
	int32 KeyCount = 0;
	for (FTransformChannelAuthority& Entry : Authority)
	{
		const FAssetDocumentUMGFloatChannelInput* ChannelInput =
			Input.Channels.Find(Entry.DtoName);
		if (ChannelInput == nullptr)
		{
			continue;
		}
		Mask |= Entry.Mask;
		KeyCount += ChannelInput->Keys.Num();
		MutateChannel(*Entry.Channel, *ChannelInput);
		if (!IsTransactionStillMutating(*ActiveTransaction))
		{
			return FailMovieSceneAfterMutation(
				*ActiveTransaction,
				TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
				TEXT("The transaction stopped mutating after a transform channel edit."));
		}
	}
	Section->SetMask(FMovieScene2DTransformMask(Mask));
	if (!IsTransactionStillMutating(*ActiveTransaction))
	{
		return FailMovieSceneAfterMutation(
			*ActiveTransaction,
			TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("The transaction stopped mutating after the transform mask edit."));
	}
	bool bReadback =
		Section->GetMask().GetChannels() == Mask
		&& ValidateTrackReadback(
			*Context.MovieScene, *Track, *Section,
			UMovieScene2DTransformTrack::StaticClass(),
			Input.PossessableGuid, Input.PropertyName,
			Input.PropertyPath,
			Input.SectionStartFrame,
			Input.SectionEndFrame);
	for (const FTransformChannelAuthority& Entry : Authority)
	{
		if (const FAssetDocumentUMGFloatChannelInput* ChannelInput =
			Input.Channels.Find(Entry.DtoName))
		{
			bReadback = bReadback
				&& ValidateChannelReadback(
					*Entry.Channel, *ChannelInput);
		}
	}
	if (!ValidateWidgetBlueprintMutationCompletion(
		bReadback,
		TEXT("The public transform track readback did not match the request."),
		Diagnostics))
	{
		return FailMovieSceneAfterMutation(
			*ActiveTransaction,
			TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"),
			TEXT("The transform track postcondition failed."),
			MoveTemp(Diagnostics));
	}
	FAssetDocumentPrimitiveOutcome Outcome =
		MakeAssetDocumentUMGSuccess(MakeTrackOutput(
			*Track,
			Input.SectionStartFrame,
			Input.SectionEndFrame,
			KeyCount));
	if (!IssueTrackSection(
		Invocation, Handles, *Track, *Section, Outcome))
	{
		return FailMovieSceneAfterMutation(
			*ActiveTransaction,
			TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
			TEXT("The transform track output handles could not be issued."),
			MoveTemp(Outcome.Diagnostics));
	}
	for (const FTransformChannelAuthority& Entry : Authority)
	{
		if (Input.Channels.Contains(Entry.DtoName)
			&& !IssueAssetDocumentUMGChannelResult(
				Invocation, Handles,
				TEXT("movie_scene_channel"),
				EAssetDocumentHandleAccess::Mutable,
				Section, Entry.Channel,
				Entry.MetadataName, Outcome))
		{
			return FailMovieSceneAfterMutation(
				*ActiveTransaction,
				TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
				TEXT("A transform channel output handle could not be issued."),
				MoveTemp(Outcome.Diagnostics));
		}
	}
	Outcome.ChangedSet = {
		MakeAssetDocumentUMGChangedRecord(
			*Context.Animation,
			TEXT("/animation/movie_scene"),
			TEXT("modify")),
		MakeAssetDocumentUMGChangedRecord(
			*Track, TEXT("/track"), TEXT("add")),
		MakeAssetDocumentUMGChangedRecord(
			*Section, TEXT("/section"), TEXT("add"))
	};
	return Outcome;
}

FAssetDocumentPrimitiveOutcome InvokeAssetDocumentUMGTrackSectionRemove(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AssetDocumentUMGMovieSceneHandlersPrivate;
	FAssetDocumentUMGTrackSectionRemoveInput Input;
	FString DecodeDiagnostic;
	if (!DecodeTrackSectionRemoveInput(
		Invocation.ValueArguments, Input, DecodeDiagnostic))
	{
		return FailMovieScene(
			TEXT("AD.UMG.PROVIDER.INVALID_INVOCATION"),
			*DecodeDiagnostic);
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	FTrackContext Context;
	if (!ResolveTrackContext(
		Invocation, Handles, Context, Diagnostics))
	{
		return FailMovieScene(
			TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
			TEXT("The MovieScene input handles are invalid."),
			Diagnostics);
	}
	UMovieSceneTrack* Track = ResolveMovieSceneTrack(
		Invocation, Handles, TEXT("movie_scene_track"),
		EAssetDocumentHandleAccess::Mutable, Diagnostics);
	UMovieSceneSection* Section =
		Input.Target == EAssetDocumentUMGTrackSectionRemoveTarget::Section
			? ResolveMovieSceneSection(
				Invocation, Handles, TEXT("movie_scene_section"),
				EAssetDocumentHandleAccess::Mutable,
				Diagnostics, true)
			: nullptr;
	FGuid BindingGuid;
	if (!IsValid(Track)
		|| Track->GetOuter() != Context.MovieScene
		|| !Context.MovieScene->FindTrackBinding(
			*Track, BindingGuid)
		|| (Input.Target
				== EAssetDocumentUMGTrackSectionRemoveTarget::Section
			&& (!IsValid(Section)
				|| Section->GetOuter() != Track
				|| !Track->HasSection(*Section))))
	{
		return FailMovieScene(
			TEXT("AD.UMG.MOVIE_SCENE.OWNERSHIP_MISMATCH"),
			TEXT("The requested track or section is not owned by the animation MovieScene."),
			Diagnostics);
	}
	if (ActiveTransaction == nullptr)
	{
		return FailMovieScene(
			TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("The removal requires a transaction."));
	}
	TArray<UObject*> Touched = {
		Context.Animation,
		Context.MovieScene,
		Track};
	if (Section != nullptr)
	{
		Touched.Add(Section);
	}
	const FAssetDocumentContractValidationResult Enlist =
		EnlistWidgetBlueprintMutationFootprint(
			*ActiveTransaction, *Context.Blueprint, Touched);
	if (!Enlist.bValid)
	{
		return FailMovieScene(
			TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("The complete removal footprint was rejected."),
			Enlist.Diagnostics);
	}
	const FGuid RemovedSignature =
		Section != nullptr
			? Section->GetSignature()
			: Track->GetSignature();
	bool bRemoved = false;
	if (Input.Target
		== EAssetDocumentUMGTrackSectionRemoveTarget::Track)
	{
		bRemoved = Context.MovieScene->RemoveTrack(*Track)
			&& !Context.MovieScene->FindTrackBinding(
				*Track, BindingGuid);
	}
	else
	{
		Track->RemoveSection(
			UMovieSceneTrack::FSectionParameter(*Section));
		bRemoved = !Track->HasSection(*Section)
			&& !Track->GetAllSections().Contains(Section);
	}
	if (!IsTransactionStillMutating(*ActiveTransaction))
	{
		return FailMovieSceneAfterMutation(
			*ActiveTransaction,
			TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("The transaction stopped mutating after the MovieScene removal."));
	}
	if (!ValidateWidgetBlueprintMutationCompletion(
		bRemoved,
		TEXT("The public removal readback still contains the target."),
		Diagnostics))
	{
		return FailMovieSceneAfterMutation(
			*ActiveTransaction,
			TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"),
			TEXT("The MovieScene removal postcondition failed."),
			MoveTemp(Diagnostics));
	}
	FAssetDocumentPrimitiveOutcome Outcome =
		MakeAssetDocumentUMGSuccess(
			MakeAssetDocumentUMGObject({
				{TEXT("target"),
					MakeAssetDocumentUMGString(
						Input.Target
							== EAssetDocumentUMGTrackSectionRemoveTarget::Track
							? TEXT("track")
							: TEXT("section"))},
				{TEXT("removed_signature"),
					MakeAssetDocumentUMGString(
						RemovedSignature.ToString())}
			}));
	Outcome.ChangedSet = {
		MakeAssetDocumentUMGChangedRecord(
			*Context.Animation,
			TEXT("/animation/movie_scene"),
			TEXT("modify")),
		MakeAssetDocumentUMGChangedRecord(
			Section != nullptr
				? static_cast<UObject&>(*Section)
				: static_cast<UObject&>(*Track),
			Section != nullptr
				? TEXT("/section")
				: TEXT("/track"),
			TEXT("remove"))
	};
	return Outcome;
}

TConstArrayView<FAssetDocumentUMGPrimitiveBinding>
GetAssetDocumentUMGMovieScenePrimitiveBindings()
{
	static const FAssetDocumentUMGPrimitiveBinding Bindings[] = {
		{TEXT("umg.movie_scene.float_track.add.v1"), 1,
			&InvokeAssetDocumentUMGFloatTrackAdd},
		{TEXT("umg.movie_scene.transform_track.add.v1"), 1,
			&InvokeAssetDocumentUMGTransformTrackAdd},
		{TEXT("umg.movie_scene.track_section.remove.v1"), 1,
			&InvokeAssetDocumentUMGTrackSectionRemove}
	};
	return Bindings;
}
