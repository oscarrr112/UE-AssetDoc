// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// An editor-made Montage whose Preview Pose Asset (Asset Details) is set: Extract shows it in top-level Properties as
// the pose asset's path, the extract re-applies as a no-op, and Properties sets another pose asset and clears it.

#include "AssetDocumentTolerantTestSupport.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/PoseAsset.h"
#include "Animation/Skeleton.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
const TCHAR* const AnimMontagePreviewPoseSkeleton = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
const TCHAR* const AnimMontagePreviewPoseIdle = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle");

// The Content Browser's Duplicate of the engine skeleton into the target's package (the engine asset is never edited).
USkeleton* AnimMontagePreviewPoseSkeletonCopy(const AssetDocumentTolerantTestSupport::FTarget& Target)
{
	USkeleton* Source = LoadObject<USkeleton>(nullptr, AnimMontagePreviewPoseSkeleton);
	if (Source == nullptr)
	{
		return nullptr;
	}
	USkeleton* Copy = DuplicateObject<USkeleton>(Source, CreatePackage(*Target.PackageName), *Target.AssetName);
	Copy->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Copy);
	Copy->MarkPackageDirty();
	return Copy;
}

// A Pose Asset of Skeleton (the Content Browser's Create > Pose Asset), saved.
bool AnimMontagePreviewPoseMakePose(const AssetDocumentTolerantTestSupport::FTarget& Target, USkeleton& Skeleton)
{
	UPoseAsset* Pose = Target.Create<UPoseAsset>();
	Pose->SetSkeleton(&Skeleton);
	return Target.Save();
}

// The montage's PreviewPoseAsset value in the document's top-level Properties: its path, "null", or "<absent>".
FString AnimMontagePreviewPoseRead(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonValue> Value = AssetDocumentTolerantTestSupport::At(Document, TEXT("/Properties/PreviewPoseAsset"));
	if (!Value.IsValid())
	{
		return TEXT("<absent>");
	}
	FString Path;
	return Value->Type == EJson::Null ? FString(TEXT("null")) : Value->TryGetString(Path) ? Path : FString(TEXT("<not a path>"));
}

FString AnimMontagePreviewPoseOf(const AssetDocumentTolerantTestSupport::FTarget& Target)
{
	const UAnimMontage* Montage = Target.Get<UAnimMontage>();
	if (Montage == nullptr)
	{
		return TEXT("<no montage>");
	}
	return Montage->PreviewPoseAsset != nullptr ? Montage->PreviewPoseAsset->GetPathName() : FString(TEXT("null"));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentCodecAnimMontagePreviewPoseAssetTest, "UEAssetDoc.Codec.AnimMontage.PreviewPoseAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCodecAnimMontagePreviewPoseAssetTest::RunTest(const FString& Parameters)
{
	namespace Tolerant = AssetDocumentTolerantTestSupport;
	const Tolerant::FTarget SkeletonTarget(TEXT("SK_PreviewPose"));
	const Tolerant::FTarget GuardTarget(TEXT("PA_PreviewPoseGuard"));
	const Tolerant::FTarget AimTarget(TEXT("PA_PreviewPoseAim"));
	const Tolerant::FTarget Target(TEXT("AM_PreviewPose"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		AimTarget.Cleanup();
		GuardTarget.Cleanup();
		SkeletonTarget.Cleanup();
	};

	// A copy of the engine skeleton that plays the engine skeleton's animations and lists DefaultSlot.
	USkeleton* Skeleton = AnimMontagePreviewPoseSkeletonCopy(SkeletonTarget);
	USkeleton* EngineSkeleton = LoadObject<USkeleton>(nullptr, AnimMontagePreviewPoseSkeleton);
	UAnimSequence* Idle = LoadObject<UAnimSequence>(nullptr, AnimMontagePreviewPoseIdle);
	if (!TestNotNull(TEXT("skeleton copy"), Skeleton) || !TestNotNull(TEXT("engine skeleton"), EngineSkeleton)
		|| !TestNotNull(TEXT("idle"), Idle))
	{
		return false;
	}
	Skeleton->Modify();
	Skeleton->AddCompatibleSkeleton(EngineSkeleton);
	Skeleton->RegisterSlotNode(FAnimSlotGroup::DefaultSlotName);
	if (!TestTrue(TEXT("skeleton saved"), SkeletonTarget.Save())
		|| !TestTrue(TEXT("pose asset Guard saved"), AnimMontagePreviewPoseMakePose(GuardTarget, *Skeleton))
		|| !TestTrue(TEXT("pose asset Aim saved"), AnimMontagePreviewPoseMakePose(AimTarget, *Skeleton)))
	{
		return false;
	}

	// A Montage of the Idle animation on DefaultSlot, its Preview Pose Asset picked in the Asset Details panel.
	UAnimMontage* Montage = Target.Create<UAnimMontage>();
	Montage->SetSkeleton(Skeleton);
	Montage->SlotAnimTracks.Reset();
	Montage->CompositeSections.Reset();
	FSlotAnimationTrack& Track = Montage->SlotAnimTracks.AddDefaulted_GetRef();
	Track.SlotName = FAnimSlotGroup::DefaultSlotName;
	FAnimSegment& Segment = Track.AnimTrack.AnimSegments.AddDefaulted_GetRef();
	Segment.SetAnimReference(Idle, true);
	Segment.AnimStartTime = 0.f;
	Segment.AnimEndTime = 0.5f;
	Segment.AnimPlayRate = 1.f;
	Segment.LoopingCount = 1;
	Segment.StartPos = 0.f;
	IAnimationDataController& Controller = Montage->GetController();
	Controller.InitializeModel();
	Controller.SetFrameRate(FFrameRate(30, 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(15), false);
	Controller.NotifyPopulated();
	FCompositeSection& Section = Montage->CompositeSections.AddDefaulted_GetRef();
	Section.SectionName = TEXT("Default");
	Section.Link(Montage, 0.f, 0);
	Montage->PreviewPoseAsset = GuardTarget.Get<UPoseAsset>();
	if (!TestNotNull(TEXT("preview pose asset set"), Montage->PreviewPoseAsset.Get()) || !TestTrue(TEXT("montage saved"), Target.Save()))
	{
		return false;
	}

	// Extract shows it; the extract re-applies as a no-op.
	const TSharedPtr<FJsonObject> Extracted = Tolerant::TestTolerantRoundTrip(*this, Target, Tolerant::FExpected(), TEXT("Montage"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("Properties.PreviewPoseAsset is the pose asset"), AnimMontagePreviewPoseRead(Extracted), GuardTarget.ObjectPath);

	// Another pose asset.
	const FString Aim = AimTarget.ObjectPath;
	if (TestTrue(TEXT("another pose asset is set"), Tolerant::ApplyEdited(*this, Target, [&Aim](const TSharedPtr<FJsonObject>& Document)
	{
		Document->GetObjectField(TEXT("Properties"))->SetStringField(TEXT("PreviewPoseAsset"), Aim);
	}, TEXT("Set another")).IsSuccess()))
	{
		TestEqual(TEXT("the montage previews the other pose asset"), AnimMontagePreviewPoseOf(Target), Aim);
		const FAssetDocumentResult Reread = Tolerant::ExtractResult(Target);
		TestEqual(TEXT("it reads back"), Reread.IsSuccess() ? AnimMontagePreviewPoseRead(Reread.Payload) : FString(), Aim);
		Montage = Target.Get<UAnimMontage>();
		TestTrue(TEXT("the timeline kept"), Montage != nullptr && Montage->SlotAnimTracks.Num() == 1
			&& Montage->SlotAnimTracks[0].AnimTrack.AnimSegments.Num() == 1);
	}

	// Cleared.
	if (TestTrue(TEXT("the pose asset is cleared"), Tolerant::ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		Document->GetObjectField(TEXT("Properties"))->SetField(TEXT("PreviewPoseAsset"), MakeShared<FJsonValueNull>());
	}, TEXT("Clear")).IsSuccess()))
	{
		TestEqual(TEXT("the montage previews no pose asset"), AnimMontagePreviewPoseOf(Target), FString(TEXT("null")));
		const FAssetDocumentResult Reread = Tolerant::ExtractResult(Target);
		TestEqual(TEXT("null reads back"), Reread.IsSuccess() ? AnimMontagePreviewPoseRead(Reread.Payload) : FString(), FString(TEXT("null")));
	}
	return !HasAnyErrors();
}

#endif
