// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Behaviour tests of the raw-edit files of the standalone animation asset primitives:
//   AnimAssets/AssetDocumentSkeletonRawEdits: a socket removed as the skeleton tree's Delete does
//     (preview attachments on that socket go too) and a socket Details write (the socket's change
//     event fires; fields that are not editable are refused).
//   AnimAssets/AssetDocumentBlendSpaceEdits: axis and sample Details writes read back, the blend
//     space is re-validated, and a value that does not decode changes nothing.
// Both work on transient copies; no package is written.

#include "AnimAssets/AssetDocumentBlendSpaceEdits.h"
#include "AnimAssets/AssetDocumentSkeletonRawEdits.h"
#include "AnimBlueprint/AssetDocumentAnimBlueprintAdapters.h"

#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentAnimationAssetRawEditsTests
{
const TCHAR* Skeleton = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
const TCHAR* PreviewMesh = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP");
const TCHAR* IdleAnim = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle");
} // namespace AssetDocumentAnimationAssetRawEditsTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationAssetsSkeletonRawEditsTest, "UEAssetDoc.AnimationAssets.RawEdits.Skeleton",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationAssetsSkeletonRawEditsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentAnimationAssetRawEditsTests;
	namespace SkeletonEdits = UE::AssetDocument::Animation::SkeletonEdits;
	USkeleton* Source = LoadObject<USkeleton>(nullptr, Skeleton);
	UObject* Attachment = LoadObject<UObject>(nullptr, PreviewMesh);
	if (!TestNotNull(TEXT("tutorial skeleton"), Source) || !TestNotNull(TEXT("attachment asset"), Attachment))
	{
		return false;
	}
	USkeleton* Copy = DuplicateObject<USkeleton>(Source, GetTransientPackage());
	USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(Copy);
	Socket->SocketName = TEXT("RawEditSocket");
	Socket->BoneName = TEXT("hand_r");
	Copy->Sockets.Add(Socket);
	Copy->PreviewAttachedAssetContainer.AddAttachedObject(Attachment, Socket->SocketName);
	const int32 SocketCount = Copy->Sockets.Num();

	// Details write: the value is set and the socket's change event reports the property.
	FName Changed;
	Socket->OnPropertyChanged().AddLambda([&Changed](const USkeletalMeshSocket*, const FProperty* Property)
	{
		Changed = Property != nullptr ? Property->GetFName() : NAME_None;
	});
	TestTrue(TEXT("location write"), SkeletonEdits::SetSocketField(*Socket, GET_MEMBER_NAME_CHECKED(USkeletalMeshSocket, RelativeLocation),
		[](USkeletalMeshSocket& Target) { Target.RelativeLocation = FVector(1, 2, 3); }));
	TestEqual(TEXT("location reads back"), Socket->RelativeLocation, FVector(1, 2, 3));
	TestEqual(TEXT("change event names the property"), Changed, GET_MEMBER_NAME_CHECKED(USkeletalMeshSocket, RelativeLocation));
	TestTrue(TEXT("socket is transactional"), Socket->HasAnyFlags(RF_Transactional));
	TestFalse(TEXT("unknown field is refused"), SkeletonEdits::SetSocketField(*Socket, TEXT("NoSuchField"),
		[](USkeletalMeshSocket& Target) { Target.RelativeScale = FVector::ZeroVector; }));
	TestEqual(TEXT("refused write changes nothing"), Socket->RelativeScale, FVector::OneVector);

	// Delete: the socket and its preview attachment are gone, other sockets stay.
	SkeletonEdits::RemoveSocket(*Copy, *Socket);
	TestEqual(TEXT("socket removed"), Copy->Sockets.Num(), SocketCount - 1);
	TestNull(TEXT("socket not found"), Copy->FindSocket(TEXT("RawEditSocket")));
	TestNull(TEXT("preview attachment removed"), Copy->PreviewAttachedAssetContainer.GetAttachedObjectByAttachName(TEXT("RawEditSocket")));
	Copy->ClearFlags(RF_Standalone);
	Copy->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationAssetsBlendSpaceRawEditsTest, "UEAssetDoc.AnimationAssets.RawEdits.BlendSpace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationAssetsBlendSpaceRawEditsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentAnimationAssetRawEditsTests;
	using namespace UE::AssetDocument::AnimBlueprint;
	namespace Edits = UE::AssetDocument::Animation::BlendSpaceEdits;
	USkeleton* TargetSkeleton = LoadObject<USkeleton>(nullptr, Skeleton);
	UAnimSequence* Idle = LoadObject<UAnimSequence>(nullptr, IdleAnim);
	if (!TestNotNull(TEXT("tutorial skeleton"), TargetSkeleton) || !TestNotNull(TEXT("idle"), Idle))
	{
		return false;
	}
	UBlendSpace* BlendSpace = NewObject<UBlendSpace>(GetTransientPackage(), NAME_None, RF_Transactional);
	BlendSpace->SetSkeleton(TargetSkeleton);
	TestEqual(TEXT("2D blend space has two axes"), Edits::NumAxes(*BlendSpace), 2);
	FString Error;

	// Axis Details writes.
	TestTrue(TEXT("axis max write"), Edits::WriteAxisField(*BlendSpace, 0, TEXT("Max"), MakeNumberValue(200.0), Error));
	TestTrue(TEXT("axis name write"), Edits::WriteAxisField(*BlendSpace, 1, TEXT("DisplayName"), MakeStringValue(TEXT("Speed")), Error));
	TestEqual(TEXT("axis max reads back"), BlendSpace->GetBlendParameter(0).Max, 200.f);
	TestEqual(TEXT("axis name reads back"), BlendSpace->GetBlendParameter(1).DisplayName, FString(TEXT("Speed")));
	TestFalse(TEXT("an axis value that does not decode is refused"),
		Edits::WriteAxisField(*BlendSpace, 0, TEXT("GridNum"), MakeStringValue(TEXT("many")), Error));
	TestFalse(TEXT("the refusal says why"), Error.IsEmpty());
	TestFalse(TEXT("an axis that does not exist is refused"), Edits::WriteAxisField(*BlendSpace, 3, TEXT("Max"), MakeNumberValue(1.0), Error));

	// Sample Details writes, after the editor's add.
	BlendSpace->Modify();
	const int32 Added = BlendSpace->AddSample(Idle, FVector(100, 0, 0));
	Edits::NotifyPostChange(*BlendSpace);
	if (TestTrue(TEXT("sample added"), Added != INDEX_NONE && BlendSpace->GetNumberOfBlendSamples() == 1))
	{
		TestTrue(TEXT("rate scale write"), Edits::WriteSampleField(*BlendSpace, 0, TEXT("RateScale"), MakeNumberValue(2.0), Error));
		TestEqual(TEXT("rate scale reads back"), BlendSpace->GetBlendSample(0).RateScale, 2.f);
		TestFalse(TEXT("a sample value that does not decode is refused"),
			Edits::WriteSampleField(*BlendSpace, 0, TEXT("RateScale"), MakeStringValue(TEXT("fast")), Error));
		TestEqual(TEXT("refused write changes nothing"), BlendSpace->GetBlendSample(0).RateScale, 2.f);
		TestTrue(TEXT("the sample is valid after the editor's validation"), BlendSpace->GetBlendSample(0).bIsValid);
	}
	BlendSpace->MarkAsGarbage();
	return true;
}
#endif
