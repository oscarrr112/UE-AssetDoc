// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
//
// Behaviour of the editor-equivalent raw edits used by
// animation.asset.authoring.edit.v1 (AnimationAssetAuthoringRawEdits.h): each
// must leave the sequence exactly as the sequence editor's notify panel or
// details panel would.

#include "Provider/Sequence/AnimationAssetAuthoringRawEdits.h"
#include "Tests/AssetDocumentAnimationSequenceOwnedFragmentLifecycleTestTypes.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimNotifies/AnimNotify_PlaySound.h"
#include "Animation/AnimNotifies/AnimNotifyState_TimedParticleEffect.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Misc/AutomationTest.h"
#include "ReferenceSkeleton.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentAnimationAssetAuthoringRawEditsTests
{
UAnimSequence* MakeSequence(USkeleton& Skeleton, const TCHAR* Name)
{
	UAnimSequence* Sequence = NewObject<UAnimSequence>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(),
		UAnimSequence::StaticClass(), Name), RF_Transient | RF_Transactional);
	Sequence->SetSkeleton(&Skeleton);
	IAnimationDataController& Controller = Sequence->GetController();
	Controller.InitializeModel();
	Controller.SetFrameRate(FFrameRate(30, 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(30), false);
	Controller.AddBoneCurve(TEXT("root"), false);
	TArray<FVector3f> Positions, Scales;
	TArray<FQuat4f> Rotations;
	Positions.Init(FVector3f::ZeroVector, 31); Scales.Init(FVector3f::OneVector, 31); Rotations.Init(FQuat4f::Identity, 31);
	Controller.SetBoneTrackKeys(TEXT("root"), Positions, Rotations, Scales, false);
	Controller.NotifyPopulated();
	Sequence->WaitOnExistingCompression();
	return Sequence;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationAssetAuthoringRawEditsTest,
	"UEAssetDoc.AnimationProvider.AssetAuthoring.RawEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimationAssetAuthoringRawEditsTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimationAssetAuthoringRawEditsTests;
	namespace Raw = AssetDocumentAnimationAuthoringRaw;
	TStrongObjectPtr<USkeleton> Skeleton(NewObject<USkeleton>(GetTransientPackage(), NAME_None, RF_Transient));
	{
		FReferenceSkeletonModifier Modifier(Skeleton.Get());
		Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
	}
	TStrongObjectPtr<UAnimSequence> Sequence(MakeSequence(*Skeleton, TEXT("RawEdits")));
	TStrongObjectPtr<UAnimSequence> Pose(MakeSequence(*Skeleton, TEXT("RawEditsPose")));
	if (!TestEqual(TEXT("one second fixture"), Sequence->GetPlayLength(), 1.f)) return false;

	// Notify panel naming: class name without the AnimNotify_/AnimNotifyState_ prefix.
	TestEqual(TEXT("notify name"), Raw::MakeNotifyName(*UAnimNotify_PlaySound::StaticClass()), FName(TEXT("PlaySound")));
	TestEqual(TEXT("notify state name"), Raw::MakeNotifyName(*UAnimNotifyState_TimedParticleEffect::StaticClass()), FName(TEXT("TimedParticleEffect")));

	// Track list: replaced as named, white tracks.
	Raw::SetNotifyTracks(*Sequence, {TEXT("Sound"), TEXT("FX")});
	TestTrue(TEXT("named tracks in order"), Sequence->AnimNotifyTracks.Num() == 2 && Sequence->AnimNotifyTracks[0].TrackName == TEXT("Sound")
		&& Sequence->AnimNotifyTracks[1].TrackName == TEXT("FX") && Sequence->AnimNotifyTracks[1].TrackColor == FLinearColor::White);

	// SAnimNotifyTrack::CreateNewNotify placement: link, trigger offset, track, guid.
	const FGuid Guid = FGuid::NewGuid();
	TArray<FAnimNotifyEvent> Events;
	for (const float Time : {0.f, 0.5f, 1.f})
	{
		FAnimNotifyEvent& Event = Events.AddDefaulted_GetRef();
		Raw::PlaceNotify(*Sequence, Event, Time, 1, Guid);
		TestEqual(TEXT("notify time"), Event.GetTime(), Time);
		TestEqual(TEXT("notify track"), Event.TrackIndex, 1);
		TestEqual(TEXT("notify guid"), Event.Guid, Guid);
		TestTrue(TEXT("notify linked to the sequence"), Event.GetLinkedSequence() == Sequence.Get());
		TestEqual(TEXT("editor trigger offset"), Event.TriggerTimeOffset,
			GetTriggerTimeOffsetForType(Sequence->CalculateOffsetForNotify(Time)));
	}
	TestTrue(TEXT("a notify at the start fires after it"), Events[0].TriggerTimeOffset > 0.f);
	TestTrue(TEXT("a notify at the end fires before it"), Events[2].TriggerTimeOffset < 0.f);
	TestEqual(TEXT("a notify mid-sequence has no offset"), Events[1].TriggerTimeOffset, 0.f);

	// Notify state end: duration, end link and end trigger offset.
	FAnimNotifyEvent& State = Events[1];
	State.NotifyStateClass = NewObject<UAnimNotifyState_TimedParticleEffect>(Sequence.Get());
	Raw::PlaceNotifyStateEnd(*Sequence, State, 0.5f);
	TestEqual(TEXT("state duration"), State.GetDuration(), 0.5f);
	TestEqual(TEXT("state end time"), State.EndLink.GetTime(), 1.f);
	TestTrue(TEXT("end linked to the sequence"), State.EndLink.GetLinkedSequence() == Sequence.Get());
	TestEqual(TEXT("editor end trigger offset"), State.EndTriggerTimeOffset,
		GetTriggerTimeOffsetForType(Sequence->CalculateOffsetForNotify(1.f)));
	Raw::SetNotifies(*Sequence, MoveTemp(Events));
	TestEqual(TEXT("notifies replaced"), Sequence->Notifies.Num(), 3);

	// Sync markers: sorted by time, guids kept or minted, unique names refreshed.
	const FGuid Kept = FGuid::NewGuid();
	Raw::SetAuthoredSyncMarkers(*Sequence, {{TEXT("Right"), 0.8f, 1, Kept}, {TEXT("Left"), 0.2f, 0, FGuid()}});
	TestTrue(TEXT("markers sorted by time"), Sequence->AuthoredSyncMarkers.Num() == 2
		&& Sequence->AuthoredSyncMarkers[0].MarkerName == TEXT("Left") && Sequence->AuthoredSyncMarkers[1].MarkerName == TEXT("Right"));
	TestTrue(TEXT("authored guid kept"), Sequence->AuthoredSyncMarkers.Num() == 2 && Sequence->AuthoredSyncMarkers[1].Guid == Kept);
	TestTrue(TEXT("missing guid minted"), Sequence->AuthoredSyncMarkers.Num() == 2 && Sequence->AuthoredSyncMarkers[0].Guid.IsValid());
	TestTrue(TEXT("marker track kept"), Sequence->AuthoredSyncMarkers.Num() == 2 && Sequence->AuthoredSyncMarkers[1].TrackIndex == 1);
	TestTrue(TEXT("unique marker names refreshed"), Sequence->UniqueMarkerNames.Contains(TEXT("Left")) && Sequence->UniqueMarkerNames.Contains(TEXT("Right")));

	// Details-panel object picker: PostEditChangeProperty runs, so UE's own
	// rules apply (RefPoseSeq is cleared while the additive settings hide it).
	FProperty* RefPose = FindFProperty<FProperty>(UAnimSequence::StaticClass(), GET_MEMBER_NAME_CHECKED(UAnimSequence, RefPoseSeq));
	if (!TestNotNull(TEXT("RefPoseSeq property"), RefPose)) return false;
	Sequence->AdditiveAnimType = AAT_None;
	Raw::SetObjectReference(*Sequence, *RefPose, Pose.Get());
	TestNull(TEXT("hidden RefPoseSeq is cleared by PostEditChangeProperty"), Sequence->RefPoseSeq.Get());
	Sequence->AdditiveAnimType = AAT_LocalSpaceBase;
	Sequence->RefPoseType = ABPT_AnimScaled;
	Raw::SetObjectReference(*Sequence, *RefPose, Pose.Get());
	TestTrue(TEXT("visible RefPoseSeq is set"), Sequence->RefPoseSeq.Get() == Pose.Get());
	Raw::SetObjectReference(*Sequence, *RefPose, nullptr);
	TestNull(TEXT("reference cleared"), Sequence->RefPoseSeq.Get());

	// Instanced "+" on an object array: outer is the asset, transactional.
	UObject* Meta = Raw::NewInstancedSubobject(*Sequence, *UAssetDocumentSequenceLifecycleMetadataTest::StaticClass());
	TestTrue(TEXT("instanced object outer and class"), Meta && Meta->GetOuter() == Sequence.Get() && Meta->GetClass() == UAssetDocumentSequenceLifecycleMetadataTest::StaticClass());
	TestTrue(TEXT("instanced object is transactional"), Meta && Meta->HasAnyFlags(RF_Transactional));
	Sequence->WaitOnExistingCompression();
	return true;
}

#endif
