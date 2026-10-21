// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Animation data reads:
// AnimationAssets.BoneSampling: sample_animation_bones (AnimData::SampleBones) returns bone and socket transforms of a
//   sequence with known keys at listed times and over start/end/step, in component, local and root space; root
//   "extracted" resets the root as the sequence's root motion settings say; RateScale gives PlayTime and the speed per
//   second of play; a montage is sampled through its segments; bad requests fail with an AI-readable code at the
//   argument; nothing is dirtied.
// AnimationAssets.BoneSamplingAdditive: an additive sequence (local space, reference pose base) is sampled as the full
//   pose it gives on its base: the root carries the additive travel and an unkeyed bone keeps its reference transform.
// AnimationAssets.MotionSummary: Inspect of an AnimSequence carries Motion (root displacement, pelvis at the first and
//   last frame), so in-place vs root motion is visible without sampling (Inspect used to have only RootMotion).
// AnimationAssets.MontageSegmentDurations: Inspect of a Montage lists each segment's real Duration, which divides by
//   the segment's AnimPlayRate and the sequence's RateScale (0-1.0 s at 1.15 on a 1.3 sequence lasts
//   0.669 s, not 0.870 s).

#include "AnimData/AnimationBoneSampling.h"
#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/SkeletalMeshSocket.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentAnimationDataTests
{
const TCHAR* const SkeletonPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");

FString ToText(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid())
	{
		FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
	}
	return Text;
}

TSharedPtr<FJsonObject> FromText(const FString& Text)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
	return Object;
}

TSet<FString> DirtyPackageNames()
{
	TArray<UPackage*> Packages;
	FEditorFileUtils::GetDirtyContentPackages(Packages);
	TSet<FString> Names;
	for (const UPackage* Package : Packages)
	{
		Names.Add(Package->GetName());
	}
	return Names;
}

// Test assets in one /Game folder, saved, removed (unloaded and deleted from disk) at the end.
struct FAnimDataScratch
{
	FString Path = FString::Printf(TEXT("/Game/AssetDocumentAnimData_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TArray<FString> Packages;

	FString PackageOf(const TCHAR* Name) { Packages.Add(Path / Name); return Path / Name; }

	bool Save(UObject* Asset) const
	{
		UPackage* Package = Asset->GetOutermost();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Package, Asset, *FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Args);
	}

	~FAnimDataScratch()
	{
		if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting)
		{
			GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentAnimationDataTests", "Cleanup", "Animation data test cleanup"));
		}
		for (int32 Index = Packages.Num() - 1; Index >= 0; --Index)
		{
			if (UPackage* Package = FindPackage(nullptr, *Packages[Index]))
			{
				Package->SetDirtyFlag(false);
				UPackageTools::UnloadPackages({Package});
			}
		}
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		for (const FString& Package : Packages)
		{
			const FString File = FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
			if (IFileManager::Get().FileExists(*File))
			{
				IFileManager::Get().Delete(*File, false, true);
				FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({File});
			}
		}
	}
};

// One second at 30 fps on the tutorial skeleton: the root moves 0 -> RootTravel cm along X (the root track keyed),
// everything else stays at the reference pose. Saved.
UAnimSequence* MakeSequence(FAutomationTestBase& Test, FAnimDataScratch& Folder, const TCHAR* Name, double RootTravel, float RateScale = 1.f)
{
	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, SkeletonPath);
	if (!Test.TestNotNull(TEXT("tutorial skeleton loads"), Skeleton))
	{
		return nullptr;
	}
	UPackage* Package = CreatePackage(*Folder.PackageOf(Name));
	UAnimSequence* Sequence = NewObject<UAnimSequence>(Package, Name, RF_Public | RF_Standalone | RF_Transactional);
	Sequence->SetSkeleton(Skeleton);
	IAnimationDataController& Controller = Sequence->GetController();
	Controller.InitializeModel();
	Controller.SetFrameRate(FFrameRate(30, 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(30), false);
	const FName Root = Skeleton->GetReferenceSkeleton().GetBoneName(0);
	Test.TestTrue(TEXT("root track added"), Controller.AddBoneCurve(Root, false));
	TArray<FVector3f> Positions, Scales;
	TArray<FQuat4f> Rotations;
	for (int32 Frame = 0; Frame <= 30; ++Frame)
	{
		Positions.Add(FVector3f(static_cast<float>(RootTravel * Frame / 30.0), 0.f, 0.f));
	}
	Scales.Init(FVector3f::OneVector, 31);
	Rotations.Init(FQuat4f::Identity, 31);
	Test.TestTrue(TEXT("root keys set"), Controller.SetBoneTrackKeys(Root, Positions, Rotations, Scales, false));
	Controller.NotifyPopulated();
	Sequence->RateScale = RateScale;
	FAssetRegistryModule::AssetCreated(Sequence);
	Test.TestTrue(TEXT("sequence saved"), Folder.Save(Sequence));
	return Sequence;
}

// A Montage on DefaultSlot whose segments are (animation, start, end, play rate), laid end to end. Saved.
UAnimMontage* MakeMontage(FAutomationTestBase& Test, FAnimDataScratch& Folder, const TCHAR* Name, TConstArrayView<TTuple<UAnimSequence*, float, float, float>> Segments)
{
	UPackage* Package = CreatePackage(*Folder.PackageOf(Name));
	UAnimMontage* Montage = NewObject<UAnimMontage>(Package, Name, RF_Public | RF_Standalone | RF_Transactional);
	Montage->SetSkeleton(Segments[0].Get<0>()->GetSkeleton());
	if (Montage->SlotAnimTracks.IsEmpty())
	{
		Montage->SlotAnimTracks.AddDefaulted();
	}
	Montage->SlotAnimTracks.SetNum(1);
	Montage->SlotAnimTracks[0].SlotName = FAnimSlotGroup::DefaultSlotName;
	float Position = 0.f;
	for (const TTuple<UAnimSequence*, float, float, float>& Row : Segments)
	{
		FAnimSegment& Segment = Montage->SlotAnimTracks[0].AnimTrack.AnimSegments.AddDefaulted_GetRef();
		Segment.SetAnimReference(Row.Get<0>());
		Segment.StartPos = Position;
		Segment.AnimStartTime = Row.Get<1>();
		Segment.AnimEndTime = Row.Get<2>();
		Segment.AnimPlayRate = Row.Get<3>();
		Segment.LoopingCount = 1;
		Position = Segment.GetEndPos();
	}
	IAnimationDataController& Controller = Montage->GetController();
	Controller.InitializeModel();
	Controller.SetFrameRate(FFrameRate(30, 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(FMath::RoundToInt(Position * 30.f)), false);
	Controller.NotifyPopulated();
	Montage->CompositeSections.Reset();
	FCompositeSection& Section = Montage->CompositeSections.AddDefaulted_GetRef();
	Section.SectionName = TEXT("Default");
	Section.Link(Montage, 0.f, 0);
	FAssetRegistryModule::AssetCreated(Montage);
	Test.TestTrue(TEXT("montage saved"), Folder.Save(Montage));
	return Montage;
}

FAssetDocumentResult Sample(const FString& Json)
{
	return UE::AssetDocument::AnimData::SampleBones(FromText(Json));
}

const TArray<TSharedPtr<FJsonValue>>& Samples(const FAssetDocumentResult& Result)
{
	static const TArray<TSharedPtr<FJsonValue>> Empty;
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	return Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("Samples"), Rows) ? *Rows : Empty;
}

// Location of Bone in sample Index.
FVector LocationAt(const FAssetDocumentResult& Result, int32 Index, const FString& Bone)
{
	const TArray<TSharedPtr<FJsonValue>>& Rows = Samples(Result);
	if (!Rows.IsValidIndex(Index))
	{
		return FVector(TNumericLimits<double>::Max());
	}
	const TSharedPtr<FJsonObject>* Value = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Location = nullptr;
	if (!Rows[Index]->AsObject()->TryGetObjectField(Bone, Value) || !(*Value)->TryGetArrayField(TEXT("Location"), Location) || Location->Num() != 3)
	{
		return FVector(TNumericLimits<double>::Max());
	}
	return FVector((*Location)[0]->AsNumber(), (*Location)[1]->AsNumber(), (*Location)[2]->AsNumber());
}

double NumberAt(const FAssetDocumentResult& Result, int32 Index, const FString& Field, const FString& Bone = FString())
{
	const TArray<TSharedPtr<FJsonValue>>& Rows = Samples(Result);
	double Value = -1.0;
	if (Rows.IsValidIndex(Index))
	{
		TSharedPtr<FJsonObject> Row = Rows[Index]->AsObject();
		const TSharedPtr<FJsonObject>* BoneObject = nullptr;
		if (!Bone.IsEmpty() && Row->TryGetObjectField(Bone, BoneObject))
		{
			Row = *BoneObject;
		}
		Row->TryGetNumberField(Field, Value);
	}
	return Value;
}

bool FailsWith(FAutomationTestBase& Test, const FAssetDocumentResult& Result, const TCHAR* Code, const TCHAR* Path, const TCHAR* Stage, const TCHAR* Text = nullptr)
{
	Test.TestFalse(*FString::Printf(TEXT("%s: fails"), Stage), Result.IsSuccess());
	const FAssetDocumentDiagnostic* Found = Result.Diagnostics.FindByPredicate([Code, Path](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == Code && Diagnostic.Path == Path;
	});
	if (!Test.TestNotNull(*FString::Printf(TEXT("%s: %s at %s (%s)"), Stage, Code, Path, *Result.Message), Found))
	{
		return false;
	}
	if (Text != nullptr)
	{
		Test.TestTrue(*FString::Printf(TEXT("%s: message names '%s' (%s)"), Stage, Text, *Found->Message), Found->Message.Contains(Text));
	}
	return true;
}

TSharedPtr<FJsonObject> InspectFacts(FAutomationTestBase& Test, const FString& ObjectPath, const TCHAR* Key)
{
	FAssetDocumentInspectRequest Request;
	Request.ClassOrAsset = ObjectPath;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Inspect(Request);
	if (!Test.TestTrue(*FString::Printf(TEXT("inspect %s (%s)"), *ObjectPath, *Result.Message), Result.IsSuccess() && Result.Payload.IsValid()))
	{
		return nullptr;
	}
	const TSharedPtr<FJsonObject>* Facts = nullptr;
	if (!Test.TestTrue(*FString::Printf(TEXT("inspect has %s: %s"), Key, *ToText(Result.Payload).Left(3000)), Result.Payload->TryGetObjectField(Key, Facts)))
	{
		return nullptr;
	}
	return *Facts;
}

FVector VectorField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || Values->Num() != 3)
	{
		return FVector(TNumericLimits<double>::Max());
	}
	return FVector((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
}
} // namespace AssetDocumentAnimationDataTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationBoneSamplingTest, "UEAssetDoc.AnimationAssets.BoneSampling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationBoneSamplingTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimationDataTests;
	FAnimDataScratch Folder;
	UAnimSequence* Sequence = MakeSequence(*this, Folder, TEXT("AS_Travel"), 100.0);
	if (Sequence == nullptr)
	{
		return false;
	}
	const FReferenceSkeleton& RefSkeleton = Sequence->GetSkeleton()->GetReferenceSkeleton();
	const FString Root = RefSkeleton.GetBoneName(0).ToString();
	const FString Asset = Sequence->GetOutermost()->GetName();
	const TSet<FString> DirtyBefore = DirtyPackageNames();

	// Listed times, component space: the root follows its keys, the pelvis rides on it.
	const FAssetDocumentResult Listed = Sample(FString::Printf(TEXT(R"({"asset": "%s", "bones": ["%s", "pelvis"], "times": [0, 0.5, 1.0]})"), *Asset, *Root));
	if (!TestTrue(*FString::Printf(TEXT("listed times sample (%s)"), *Listed.Message), Listed.IsSuccess()))
	{
		return false;
	}
	AddInfo(ToText(Listed.Payload).Left(2000));
	TestEqual(TEXT("three samples"), Samples(Listed).Num(), 3);
	TestTrue(TEXT("root at 0.5 s is 50 cm along X"), LocationAt(Listed, 1, Root).Equals(FVector(50, 0, 0), 0.05));
	TestTrue(TEXT("root at 1.0 s is 100 cm along X"), LocationAt(Listed, 2, Root).Equals(FVector(100, 0, 0), 0.05));
	TestTrue(TEXT("the pelvis moves with the root (component space)"),
		(LocationAt(Listed, 2, TEXT("pelvis")) - LocationAt(Listed, 0, TEXT("pelvis"))).Equals(FVector(100, 0, 0), 0.05));
	TestEqual(TEXT("no PlayTime at RateScale 1"), NumberAt(Listed, 1, TEXT("PlayTime")), -1.0);

	// Root space removes the root's travel; local space is the parent-relative transform.
	const FAssetDocumentResult RootSpace = Sample(FString::Printf(TEXT(R"({"asset": "%s", "bones": ["pelvis"], "times": [0, 1], "space": "root"})"), *Asset));
	TestTrue(TEXT("root space: the pelvis does not travel"), RootSpace.IsSuccess() && LocationAt(RootSpace, 0, TEXT("pelvis")).Equals(LocationAt(RootSpace, 1, TEXT("pelvis")), 0.05));
	const FAssetDocumentResult Local = Sample(FString::Printf(TEXT(R"({"asset": "%s", "bones": ["pelvis"], "times": [0.5], "space": "local"})"), *Asset));
	const int32 PelvisIndex = RefSkeleton.FindBoneIndex(TEXT("pelvis"));
	TestTrue(TEXT("local space: the pelvis keeps its reference local translation"), Local.IsSuccess() && PelvisIndex != INDEX_NONE
		&& LocationAt(Local, 0, TEXT("pelvis")).Equals(RefSkeleton.GetRefBonePose()[PelvisIndex].GetLocation(), 0.05));

	// start/end/step (end included), and the default step of one frame.
	const FAssetDocumentResult Range = Sample(FString::Printf(TEXT(R"({"asset": "%s", "bones": ["%s"], "start": 0.2, "end": 0.6, "step": 0.2})"), *Asset, *Root));
	TestEqual(TEXT("range 0.2..0.6 by 0.2 has 3 samples"), Samples(Range).Num(), 3);
	TestTrue(TEXT("range ends at 0.6"), FMath::IsNearlyEqual(NumberAt(Range, 2, TEXT("Time")), 0.6, 1e-4));
	const FAssetDocumentResult Frames = Sample(FString::Printf(TEXT(R"({"asset": "%s", "bones": ["%s"], "start": 0})"), *Asset, *Root));
	TestEqual(TEXT("default step is one frame (31 samples over 1 s at 30 fps)"), Samples(Frames).Num(), 31);

	// Root "extracted": with root motion enabled the root is reset (RefPose lock), and the result says so.
	Sequence->bEnableRootMotion = true;
	Sequence->RootMotionRootLock = ERootMotionRootLock::RefPose;
	const FAssetDocumentResult Extracted = Sample(FString::Printf(TEXT(R"({"asset": "%s", "bones": ["%s"], "times": [0.5], "root": "extracted"})"), *Asset, *Root));
	TestTrue(TEXT("extracted: the root stays at its reference pose"), Extracted.IsSuccess()
		&& LocationAt(Extracted, 0, Root).Equals(RefSkeleton.GetRefBonePose()[0].GetLocation(), 0.05));
	const TSharedPtr<FJsonObject>* RootInfo = nullptr;
	TestTrue(TEXT("extracted: Root.RootLocked is reported"), Extracted.Payload.IsValid() && Extracted.Payload->TryGetObjectField(TEXT("Root"), RootInfo)
		&& (*RootInfo)->GetBoolField(TEXT("RootLocked")) && (*RootInfo)->GetBoolField(TEXT("EnableRootMotion")));
	const FAssetDocumentResult PoseMode = Sample(FString::Printf(TEXT(R"({"asset": "%s", "bones": ["%s"], "times": [0.5]})"), *Asset, *Root));
	TestTrue(TEXT("pose (default): the root moves as authored even with root motion enabled"), LocationAt(PoseMode, 0, Root).Equals(FVector(50, 0, 0), 0.05));
	Sequence->bEnableRootMotion = false;

	// RateScale 2: PlayTime is half the timeline; speed is per second of play.
	Sequence->RateScale = 2.f;
	const FAssetDocumentResult Scaled = Sample(FString::Printf(TEXT(R"({"asset": "%s", "bones": ["%s"], "times": [0, 0.5], "speed": true})"), *Asset, *Root));
	TestTrue(TEXT("RateScale 2: PlayTime of 0.5 is 0.25"), FMath::IsNearlyEqual(NumberAt(Scaled, 1, TEXT("PlayTime")), 0.25, 1e-4));
	TestTrue(*FString::Printf(TEXT("RateScale 2: root speed is 200 cm/s of play (%g)"), NumberAt(Scaled, 1, TEXT("Speed"), Root)),
		FMath::IsNearlyEqual(NumberAt(Scaled, 1, TEXT("Speed"), Root), 200.0, 0.5));
	TestTrue(TEXT("RateScale is stated"), Scaled.Payload.IsValid() && ToText(Scaled.Payload).Contains(TEXT("RateScale 2")));
	Sequence->RateScale = 1.f;

	// A skeleton socket is its bone's transform times the socket's.
	const TArray<TObjectPtr<USkeletalMeshSocket>>& Sockets = Sequence->GetSkeleton()->Sockets;
	if (!Sockets.IsEmpty() && Sockets[0] != nullptr)
	{
		const FString Socket = Sockets[0]->SocketName.ToString();
		const FString Bone = Sockets[0]->BoneName.ToString();
		const FAssetDocumentResult Both = Sample(FString::Printf(TEXT(R"({"asset": "%s", "bones": ["%s", "%s"], "times": [0.5]})"), *Asset, *Socket, *Bone));
		const FVector Expected = (Sockets[0]->GetSocketLocalTransform() * FTransform(LocationAt(Both, 0, Bone))).GetLocation();
		TestTrue(*FString::Printf(TEXT("socket %s sampled on %s"), *Socket, *Bone), Both.IsSuccess() && ToText(Both.Payload).Contains(TEXT("\"Sockets\"")));
		AddInfo(FString::Printf(TEXT("socket %s at %s, bone at %s"), *Socket, *LocationAt(Both, 0, Socket).ToString(), *Expected.ToString()));
	}

	// Montage: segment 0 plays 0..1 at rate 1, segment 1 plays 0..1 at rate 2 (0.5 s): 1.25 s is 0.5 s into the sequence.
	UAnimMontage* Montage = MakeMontage(*this, Folder, TEXT("AM_Travel"), {MakeTuple(Sequence, 0.f, 1.f, 1.f), MakeTuple(Sequence, 0.f, 1.f, 2.f)});
	if (Montage != nullptr)
	{
		const FAssetDocumentResult InMontage = Sample(FString::Printf(TEXT(R"({"asset": "%s", "bones": ["%s"], "times": [0.5, 1.25]})"), *Montage->GetOutermost()->GetName(), *Root));
		AddInfo(ToText(InMontage.Payload).Left(1500));
		TestTrue(*FString::Printf(TEXT("montage samples (%s)"), *InMontage.Message), InMontage.IsSuccess());
		TestEqual(TEXT("1.25 s is in segment 1"), NumberAt(InMontage, 1, TEXT("Segment")), 1.0);
		TestTrue(TEXT("segment 1 at 1.25 s is 0.5 s into the sequence"), FMath::IsNearlyEqual(NumberAt(InMontage, 1, TEXT("AnimTime")), 0.5, 1e-3));
		TestTrue(TEXT("and the root is where the sequence has it then"), LocationAt(InMontage, 1, Root).Equals(FVector(50, 0, 0), 0.1));
		FailsWith(*this, Sample(FString::Printf(TEXT(R"({"asset": "%s", "bones": ["%s"], "times": [0.5], "slot": "Nope"})"), *Montage->GetOutermost()->GetName(), *Root)),
			TEXT("AnimationSample.SlotNotFound"), TEXT("/slot"), TEXT("unknown slot"), TEXT("DefaultSlot"));
	}

	// Refusals at the argument.
	FailsWith(*this, Sample(FString::Printf(TEXT(R"({"asset": "%s", "bones": ["hand_rr"], "times": [0]})"), *Asset)),
		TEXT("AnimationSample.BoneNotFound"), TEXT("/bones/0"), TEXT("unknown bone"), TEXT("hand_r"));
	FailsWith(*this, Sample(FString::Printf(TEXT(R"({"asset": "%s", "bones": ["%s"], "times": [0, 2]})"), *Asset, *Root)),
		TEXT("AnimationSample.TimeOutOfRange"), TEXT("/times/1"), TEXT("time beyond the end"), TEXT("1.0000"));
	FailsWith(*this, Sample(FString::Printf(TEXT(R"({"asset": "%s", "bones": ["%s"], "start": 0, "end": 1, "step": 0.0001})"), *Asset, *Root)),
		TEXT("AnimationSample.TooManySamples"), TEXT("/step"), TEXT("too many times"));
	TArray<FString> ManyBones;
	for (int32 Index = 0; Index < FMath::Min(60, RefSkeleton.GetNum()); ++Index)
	{
		ManyBones.Add(FString::Printf(TEXT("\"%s\""), *RefSkeleton.GetBoneName(Index).ToString()));
	}
	FailsWith(*this, Sample(FString::Printf(TEXT(R"({"asset": "%s", "bones": [%s], "start": 0, "end": 1, "step": 0.02})"), *Asset, *FString::Join(ManyBones, TEXT(",")))),
		TEXT("AnimationSample.TooManySamples"), TEXT("/bones"), TEXT("too many transforms"), TEXT("at most 2000"));
	FailsWith(*this, Sample(FString::Printf(TEXT(R"({"asset": "%s", "bones": ["%s"]})"), *Asset, *Root)),
		TEXT("AnimationSample.InvalidArgument"), TEXT("/times"), TEXT("no times"));
	FailsWith(*this, Sample(FString::Printf(TEXT(R"({"asset": "%s", "bones": ["%s"], "times": [0]})"), SkeletonPath, *Root)),
		TEXT("AnimationSample.NotAnimation"), TEXT("/asset"), TEXT("not an animation"));

	// Reading dirtied nothing.
	for (const FString& Name : DirtyPackageNames())
	{
		TestTrue(*FString::Printf(TEXT("sampling did not dirty %s"), *Name), DirtyBefore.Contains(Name) || Name == Asset);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationBoneSamplingAdditiveTest, "UEAssetDoc.AnimationAssets.BoneSamplingAdditive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationBoneSamplingAdditiveTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimationDataTests;
	FAnimDataScratch Folder;
	UAnimSequence* Additive = MakeSequence(*this, Folder, TEXT("AS_AdditiveTravel"), 30.0);
	if (Additive == nullptr)
	{
		return false;
	}
	Additive->AdditiveAnimType = AAT_LocalSpaceBase;
	Additive->RefPoseType = ABPT_RefPose;
	if (!TestTrue(TEXT("the sequence is a valid additive"), Additive->IsValidAdditive()))
	{
		return false;
	}
	const FReferenceSkeleton& RefSkeleton = Additive->GetSkeleton()->GetReferenceSkeleton();
	const FString Root = RefSkeleton.GetBoneName(0).ToString();
	const int32 PelvisIndex = RefSkeleton.FindBoneIndex(TEXT("pelvis"));
	if (!TestTrue(TEXT("the skeleton has a pelvis"), PelvisIndex != INDEX_NONE))
	{
		return false;
	}
	const FString Asset = Additive->GetOutermost()->GetName();
	const FAssetDocumentResult Local = Sample(FString::Printf(TEXT(R"({"asset": "%s", "bones": ["%s", "pelvis"], "times": [0.5, 1.0], "space": "local"})"),
		*Asset, *Root));
	if (!TestTrue(*FString::Printf(TEXT("additive samples (%s)"), *Local.Message), Local.IsSuccess()))
	{
		return false;
	}
	AddInfo(ToText(Local.Payload).Left(2000));
	// Base (reference pose) plus the additive (keys minus reference pose) gives the keys back.
	TestTrue(TEXT("root at 0.5 s is 15 cm along X"), LocationAt(Local, 0, Root).Equals(FVector(15, 0, 0), 0.05));
	TestTrue(TEXT("root at 1.0 s is 30 cm along X"), LocationAt(Local, 1, Root).Equals(FVector(30, 0, 0), 0.05));
	const FVector PelvisRef = RefSkeleton.GetRefBonePose()[PelvisIndex].GetLocation();
	TestFalse(TEXT("the pelvis reference translation is not zero (the check below tells a full pose from a delta)"), PelvisRef.IsNearlyZero(0.05));
	TestTrue(TEXT("full pose: the unkeyed pelvis keeps its reference local translation"), LocationAt(Local, 0, TEXT("pelvis")).Equals(PelvisRef, 0.05));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationMotionSummaryTest, "UEAssetDoc.AnimationAssets.MotionSummary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationMotionSummaryTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimationDataTests;
	FAnimDataScratch Folder;
	UAnimSequence* Travel = MakeSequence(*this, Folder, TEXT("AS_Travel"), 120.0);
	UAnimSequence* InPlace = MakeSequence(*this, Folder, TEXT("AS_InPlace"), 0.0);
	if (Travel == nullptr || InPlace == nullptr)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Motion = InspectFacts(*this, Travel->GetPathName(), TEXT("Motion"));
	if (Motion.IsValid())
	{
		AddInfo(ToText(Motion));
		TestTrue(TEXT("root displacement is 120 cm along X"), VectorField(Motion, TEXT("RootDisplacement")).Equals(FVector(120, 0, 0), 0.05));
		TestTrue(TEXT("root distance 120"), FMath::IsNearlyEqual(Motion->GetNumberField(TEXT("RootDistance")), 120.0, 0.05));
		const TSharedPtr<FJsonObject>* Pelvis = nullptr;
		if (TestTrue(TEXT("pelvis facts"), Motion->TryGetObjectField(TEXT("Pelvis"), Pelvis)))
		{
			TestEqual(TEXT("the pelvis bone is found by name"), (*Pelvis)->GetStringField(TEXT("Bone")), FString(TEXT("pelvis")));
			TestTrue(TEXT("pelvis moves 120 cm"), VectorField(*Pelvis, TEXT("Displacement")).Equals(FVector(120, 0, 0), 0.05));
			TestTrue(TEXT("pelvis first and last frame"), (VectorField(*Pelvis, TEXT("Last")) - VectorField(*Pelvis, TEXT("First"))).Equals(FVector(120, 0, 0), 0.05));
		}
	}
	const TSharedPtr<FJsonObject> Still = InspectFacts(*this, InPlace->GetPathName(), TEXT("Motion"));
	TestTrue(TEXT("in place: no root displacement"), Still.IsValid() && VectorField(Still, TEXT("RootDisplacement")).Equals(FVector::ZeroVector, 0.01));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationMontageSegmentDurationsTest, "UEAssetDoc.AnimationAssets.MontageSegmentDurations",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationMontageSegmentDurationsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimationDataTests;
	FAnimDataScratch Folder;
	UAnimSequence* Dash = MakeSequence(*this, Folder, TEXT("AS_Dash"), 0.0, 1.3f);
	if (Dash == nullptr)
	{
		return false;
	}
	UAnimMontage* Montage = MakeMontage(*this, Folder, TEXT("AM_Dash"), {MakeTuple(Dash, 0.f, 1.f, 1.15f)});
	if (Montage == nullptr)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Sequence = InspectFacts(*this, Dash->GetPathName(), TEXT("Animation"));
	TestTrue(TEXT("the sequence states its RateScale and play duration"), Sequence.IsValid()
		&& FMath::IsNearlyEqual(Sequence->GetNumberField(TEXT("RateScale")), 1.3, 1e-4)
		&& FMath::IsNearlyEqual(Sequence->GetNumberField(TEXT("PlayDuration")), 1.0 / 1.3, 1e-3));
	const TSharedPtr<FJsonObject> Facts = InspectFacts(*this, Montage->GetPathName(), TEXT("Animation"));
	const TArray<TSharedPtr<FJsonValue>>* Segments = nullptr;
	if (!TestTrue(TEXT("montage facts list Segments"), Facts.IsValid() && Facts->TryGetArrayField(TEXT("Segments"), Segments) && Segments->Num() == 1))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Segment = (*Segments)[0]->AsObject();
	AddInfo(ToText(Segment));
	TestTrue(*FString::Printf(TEXT("segment Duration is 1.0 / (1.15 * 1.3) = 0.6689 (%g)"), Segment->GetNumberField(TEXT("Duration"))),
		FMath::IsNearlyEqual(Segment->GetNumberField(TEXT("Duration")), 1.0 / (1.15 * 1.3), 1e-3));
	TestTrue(TEXT("segment states the sequence RateScale"), FMath::IsNearlyEqual(Segment->GetNumberField(TEXT("AnimRateScale")), 1.3, 1e-4));
	TestTrue(TEXT("segment EndPos"), FMath::IsNearlyEqual(Segment->GetNumberField(TEXT("EndPos")), 1.0 / (1.15 * 1.3), 1e-3));
	TestEqual(TEXT("segment slot"), Segment->GetStringField(TEXT("Slot")), FString(TEXT("DefaultSlot")));
	return true;
}
#endif
