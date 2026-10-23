// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Batch retarget export and extra bones:
// UEAssetDoc.IKRig.BatchRetarget: retarget_animations (Batch::RetargetAnimations) duplicates and
//   retargets a list or a folder of animations through an IK Retargeter as its Export window does, into a target
//   folder with a suffix / prefix, saved, and returns the created assets with their sources (otherwise only
//   execute_python with IKRetargetBatchOperation, whose return value lists the sources, plus a rename). Existing
//   outputs are refused unless existing is overwrite (replaced in place, no stray copy) or unique; bad arguments fail
//   at the argument and create nothing.
// UEAssetDoc.IKRig.RetargeterPinBones: the retargeter's own way to drive a bone the chains do not map
//   (ik_hand_gun): a Pin Bones op written in the IK Retargeter document (BonesToPin hand_r -> ik_hand_gun on the
//   target) applies, reads back, and the exported animation's ik_hand_gun follows hand_r; without the op it does not.

#include "AssetDocumentIKRetargetBatch.h"
#include "AssetDocumentIKRigTestSupport.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "AnimPose.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentIKRetargetBatchTests
{
using namespace AssetDocumentIKRigTests;

const TCHAR* const WalkAnim = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Walk_Fwd.Tutorial_Walk_Fwd");
const TCHAR* const IdleAnim = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle");

// A /Game folder of test content, removed at the end (every package under it, loaded or on disk).
// The starter animations copied into the scratch folder log engine warnings when they are duplicated and
// retargeted (import data loaded after its owner; the Tutorial clips carry no curves). Expected, not test failures.
void ExpectStarterAnimationWarnings(FAutomationTestBase& Test)
{
	Test.AddExpectedMessagePlain(TEXT("EnsureDependenciesAreLoaded"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	Test.AddExpectedMessagePlain(TEXT("No animation curves found for retargeting"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
}

struct FScratchFolder
{
	FString Path = FString::Printf(TEXT("/Game/%s/Batch_%s"), FolderName, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12));

	TArray<FString> Packages() const
	{
		TArray<FString> Names;
		TArray<FAssetData> Found;
		FAssetRegistryModule::GetRegistry().GetAssetsByPath(FName(*Path), Found, true);
		for (const FAssetData& Data : Found)
		{
			Names.AddUnique(Data.PackageName.ToString());
		}
		return Names;
	}

	~FScratchFolder()
	{
		ReleaseUndoHistory();
		TArray<UPackage*> Loaded;
		ForEachObjectOfClass(UPackage::StaticClass(), [this, &Loaded](UObject* Object)
		{
			if (Object->GetName().StartsWith(Path + TEXT("/")))
			{
				Loaded.Add(CastChecked<UPackage>(Object));
			}
		});
		for (UPackage* Package : Loaded)
		{
			Package->ClearDirtyFlag();
		}
		if (!Loaded.IsEmpty())
		{
			UPackageTools::UnloadPackages(Loaded);
		}
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		const FString Directory = FPackageName::LongPackageNameToFilename(Path + TEXT("/"));
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
		FAssetRegistryModule::GetRegistry().ScanPathsSynchronous({Directory}, true);
	}
};

UAnimSequence* CopyAnimation(FAutomationTestBase& Test, const TCHAR* Source, const FString& Folder, const TCHAR* Name)
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UAnimSequence* Copy = Cast<UAnimSequence>(AssetTools.DuplicateAsset(Name, Folder, LoadObject<UAnimSequence>(nullptr, Source)));
	if (Test.TestNotNull(*FString::Printf(TEXT("%s copied"), Name), Copy))
	{
		Test.TestTrue(*FString::Printf(TEXT("%s saved"), Name), Save(*Copy));
	}
	return Copy;
}

FAssetDocumentResult Retarget(const FString& Json)
{
	return UE::AssetDocument::Graph::Rig::Batch::RetargetAnimations(Parse(Json));
}

TArray<TSharedPtr<FJsonObject>> Rows(const FAssetDocumentResult& Result, const TCHAR* Field)
{
	TArray<TSharedPtr<FJsonObject>> Out;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Result.Payload.IsValid() && Result.Payload->TryGetArrayField(Field, Values))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			Out.Add(Value->AsObject());
		}
	}
	return Out;
}

bool FailsWith(FAutomationTestBase& Test, const FAssetDocumentResult& Result, const TCHAR* Code, const TCHAR* Path, const TCHAR* Stage)
{
	Test.TestFalse(*FString::Printf(TEXT("%s: fails"), Stage), Result.IsSuccess());
	return Test.TestTrue(*FString::Printf(TEXT("%s: %s at %s (%s)"), Stage, Code, Path, *DiagnosticsText(Result)),
		Result.Diagnostics.ContainsByPredicate([Code, Path](const FAssetDocumentDiagnostic& Each) { return Each.Code == Code && Each.Path == Path; }));
}

bool IsSavedClean(const FString& ObjectPath)
{
	const UObject* Asset = FSoftObjectPath(ObjectPath).ResolveObject();
	const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
	return Asset != nullptr && !Asset->GetOutermost()->IsDirty()
		&& IFileManager::Get().FileExists(*FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()));
}

// Two meshes (the tutorial mannequin and its /Game copy), an auto-characterized IK Rig on each and a retargeter.
struct FRetargetSetup
{
	FFixtures Fixtures;
	const FTarget& Mesh;
	const FTarget& SourceRig;
	const FTarget& TargetRig;
	const FTarget& Retargeter;
	bool bReady = false;

	explicit FRetargetSetup(FAutomationTestBase& Test)
		: Mesh(Fixtures.Add(TEXT("SKM_BatchTarget")))
		, SourceRig(Fixtures.Add(TEXT("IK_BatchSource")))
		, TargetRig(Fixtures.Add(TEXT("IK_BatchTarget")))
		, Retargeter(Fixtures.Add(TEXT("RTG_Batch")))
	{
		UObject* Copy = Duplicate(Test, LoadObject<USkeletalMesh>(nullptr, TutorialMesh), Mesh);
		UIKRigDefinition* Source = Copy != nullptr ? MakeRig(Test, SourceRig, TutorialMesh, true) : nullptr;
		UIKRigDefinition* Target = Source != nullptr ? MakeRig(Test, TargetRig, *Mesh.ObjectPath, true) : nullptr;
		bReady = Target != nullptr && MakeRetargeter(Test, Retargeter, Source, Target) != nullptr;
	}
};

FVector ComponentLocation(const UAnimSequence& Sequence, FName Bone, double Time)
{
	FAnimPose Pose;
	FAnimPoseEvaluationOptions Options;
	Options.EvaluationType = EAnimDataEvalType::Raw;
	UAnimPoseExtensions::GetAnimPoseAtTime(&Sequence, Time, Options, Pose);
	return UAnimPoseExtensions::GetBonePose(Pose, Bone, EAnimPoseSpaces::World).GetLocation();
}
} // namespace AssetDocumentIKRetargetBatchTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRetargetBatchTest, "UEAssetDoc.IKRig.BatchRetarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRetargetBatchTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRetargetBatchTests;
	ExpectStarterAnimationWarnings(*this);
	FScratchFolder Scratch;
	FRetargetSetup Setup(*this);
	if (!Setup.bReady)
	{
		return false;
	}
	const FString Source = Scratch.Path / TEXT("Src");
	const FString Out = Scratch.Path / TEXT("Out");
	UAnimSequence* Walk = CopyAnimation(*this, WalkAnim, Source, TEXT("AS_Walk"));
	UAnimSequence* Idle = CopyAnimation(*this, IdleAnim, Source, TEXT("AS_Idle"));
	if (Walk == nullptr || Idle == nullptr)
	{
		return false;
	}
	const FString WalkPath = Walk->GetPathName();
	const FString Expected = Out / TEXT("AS_Walk_RT.AS_Walk_RT");

	// A list into a target folder with a suffix: created, saved, returned with its source.
	const FAssetDocumentResult First = Retarget(FString::Printf(TEXT(R"({"retargeter": "%s", "assets": ["%s"], "target_folder": "%s", "suffix": "_RT"})"),
		*Setup.Retargeter.PackageName, *Walk->GetOutermost()->GetName(), *Out));
	AddInfo(Serialize(First.Payload));
	if (!TestTrue(*FString::Printf(TEXT("retarget succeeds (%s)"), *DiagnosticsText(First)), First.IsSuccess()))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonObject>> Created = Rows(First, TEXT("Created"));
	if (TestEqual(TEXT("one asset created"), Created.Num(), 1))
	{
		TestEqual(TEXT("at target_folder/<name><suffix>"), Created[0]->GetStringField(TEXT("Asset")), Expected);
		TestEqual(TEXT("with its source"), Created[0]->GetStringField(TEXT("Source")), WalkPath);
		TestEqual(TEXT("an AnimSequence"), Created[0]->GetStringField(TEXT("Class")), FString(TEXT("AnimSequence")));
	}
	TestTrue(TEXT("the new asset is saved"), IsSavedClean(Expected));
	const UAnimSequence* Result = LoadObject<UAnimSequence>(nullptr, *Expected);
	TestTrue(TEXT("the new asset is on the target mesh's skeleton"), Result != nullptr
		&& Result->GetSkeleton() == LoadObject<USkeletalMesh>(nullptr, *Setup.Mesh.ObjectPath)->GetSkeleton());
	TestTrue(TEXT("the source is untouched"), !Walk->GetOutermost()->IsDirty());

	// The same again: the output exists and is refused; nothing new is made.
	const int32 Before = Scratch.Packages().Num();
	const FAssetDocumentResult Again = Retarget(FString::Printf(TEXT(R"({"retargeter": "%s", "assets": ["%s"], "target_folder": "%s", "suffix": "_RT"})"),
		*Setup.Retargeter.PackageName, *WalkPath, *Out));
	if (FailsWith(*this, Again, TEXT("Retarget.TargetExists"), TEXT("/existing"), TEXT("existing output")))
	{
		TestTrue(TEXT("the refusal names the output"), Again.Message.Contains(Out / TEXT("AS_Walk_RT")));
	}
	TestEqual(TEXT("nothing new after the refusal"), Scratch.Packages().Num(), Before);

	// overwrite: replaced in place (no numbered copy left), saved.
	const FAssetDocumentResult Overwrite = Retarget(FString::Printf(TEXT(R"({"retargeter": "%s", "assets": ["%s"], "target_folder": "%s", "suffix": "_RT", "existing": "overwrite"})"),
		*Setup.Retargeter.PackageName, *WalkPath, *Out));
	AddInfo(Serialize(Overwrite.Payload));
	TestTrue(*FString::Printf(TEXT("overwrite succeeds (%s)"), *DiagnosticsText(Overwrite)), Overwrite.IsSuccess());
	const TArray<TSharedPtr<FJsonObject>> Replaced = Rows(Overwrite, TEXT("Created"));
	TestTrue(TEXT("overwrite: the new asset has the old name"), Replaced.Num() == 1 && Replaced[0]->GetStringField(TEXT("Asset")) == Expected);
	TestTrue(TEXT("overwrite: saved"), IsSavedClean(Expected));
	TestEqual(TEXT("overwrite: no extra asset"), Scratch.Packages().Num(), Before);

	// unique: a numbered name next to the existing one.
	const FAssetDocumentResult Unique = Retarget(FString::Printf(TEXT(R"({"retargeter": "%s", "assets": ["%s"], "target_folder": "%s", "suffix": "_RT", "existing": "unique"})"),
		*Setup.Retargeter.PackageName, *WalkPath, *Out));
	const TArray<TSharedPtr<FJsonObject>> Numbered = Rows(Unique, TEXT("Created"));
	TestTrue(*FString::Printf(TEXT("unique: a new name (%s)"), *DiagnosticsText(Unique)), Unique.IsSuccess() && Numbered.Num() == 1
		&& Numbered[0]->GetStringField(TEXT("Asset")) != Expected && Numbered[0]->GetStringField(TEXT("Source")) == WalkPath);

	// A folder with a prefix: every animation of the folder on the source skeleton.
	const FString Out2 = Scratch.Path / TEXT("Out2");
	const FAssetDocumentResult Folder = Retarget(FString::Printf(TEXT(R"({"retargeter": "%s", "folder": "%s", "target_folder": "%s", "prefix": "RT_"})"),
		*Setup.Retargeter.PackageName, *Source, *Out2));
	AddInfo(Serialize(Folder.Payload));
	TestTrue(*FString::Printf(TEXT("folder retarget succeeds (%s)"), *DiagnosticsText(Folder)), Folder.IsSuccess());
	TestEqual(TEXT("both animations of the folder"), Rows(Folder, TEXT("Created")).Num(), 2);
	TestTrue(TEXT("RT_AS_Idle saved"), IsSavedClean(Out2 / TEXT("RT_AS_Idle.RT_AS_Idle")));
	TestTrue(TEXT("RT_AS_Walk saved"), IsSavedClean(Out2 / TEXT("RT_AS_Walk.RT_AS_Walk")));

	// Refusals at the argument; nothing made.
	const int32 BeforeRefusals = Scratch.Packages().Num();
	FailsWith(*this, Retarget(FString::Printf(TEXT(R"({"retargeter": "%s/Nope", "assets": ["%s"]})"), *Scratch.Path, *WalkPath)),
		TEXT("Retarget.RetargeterNotFound"), TEXT("/retargeter"), TEXT("unknown retargeter"));
	FailsWith(*this, Retarget(FString::Printf(TEXT(R"({"retargeter": "%s", "assets": ["%s"], "target_folder": "%s"})"), *Setup.Retargeter.PackageName, *Setup.Mesh.ObjectPath, *Out)),
		TEXT("Retarget.NotAnimation"), TEXT("/assets/0"), TEXT("a mesh is not an animation"));
	FailsWith(*this, Retarget(FString::Printf(TEXT(R"({"retargeter": "%s", "assets": ["%s"], "folder": "%s"})"), *Setup.Retargeter.PackageName, *WalkPath, *Source)),
		TEXT("Retarget.InvalidArgument"), TEXT("/assets"), TEXT("assets and folder"));
	FailsWith(*this, Retarget(FString::Printf(TEXT(R"({"retargeter": "%s", "assets": ["%s"], "target_folder": "/Engine/Nope"})"), *Setup.Retargeter.PackageName, *WalkPath)),
		TEXT("Retarget.InvalidFolder"), TEXT("/target_folder"), TEXT("engine folder"));
	FailsWith(*this, Retarget(FString::Printf(TEXT(R"({"retargeter": "%s", "assets": ["%s"], "source_mesh": "%s", "target_mesh": "%s"})"),
		*Setup.Retargeter.PackageName, *WalkPath, *Setup.Mesh.ObjectPath, *Setup.Mesh.ObjectPath)),
		TEXT("Retarget.SameMesh"), TEXT("/target_mesh"), TEXT("same mesh"));
	TestEqual(TEXT("refusals made nothing"), Scratch.Packages().Num(), BeforeRefusals);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRetargeterPinBonesTest, "UEAssetDoc.IKRig.RetargeterPinBones",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRetargeterPinBonesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRetargetBatchTests;
	ExpectStarterAnimationWarnings(*this);
	FScratchFolder Scratch;
	FRetargetSetup Setup(*this);
	if (!Setup.bReady)
	{
		return false;
	}
	// The source walk with ik_hand_gun held at its reference pose (as in packs whose gun bone is not animated).
	UAnimSequence* Walk = CopyAnimation(*this, WalkAnim, Scratch.Path / TEXT("Src"), TEXT("AS_Walk"));
	if (Walk == nullptr)
	{
		return false;
	}
	const FName Gun(TEXT("ik_hand_gun"));
	const FName Hand(TEXT("hand_r"));
	{
		const FReferenceSkeleton& RefSkeleton = Walk->GetSkeleton()->GetReferenceSkeleton();
		const int32 GunIndex = RefSkeleton.FindBoneIndex(Gun);
		if (!TestTrue(TEXT("the tutorial skeleton has ik_hand_gun"), GunIndex != INDEX_NONE))
		{
			return false;
		}
		IAnimationDataController& Controller = Walk->GetController();
		const int32 Keys = Walk->GetDataModel()->GetNumberOfKeys();
		const FTransform Ref = RefSkeleton.GetRefBonePose()[GunIndex];
		TArray<FVector3f> Positions, Scales;
		TArray<FQuat4f> Rotations;
		Positions.Init(FVector3f(Ref.GetLocation()), Keys);
		Rotations.Init(FQuat4f(Ref.GetRotation()), Keys);
		Scales.Init(FVector3f(Ref.GetScale3D()), Keys);
		Controller.OpenBracket(NSLOCTEXT("AssetDocumentIKRetargetBatchTests", "Hold", "Hold ik_hand_gun"), false);
		if (!Walk->GetDataModel()->IsValidBoneTrackName(Gun))
		{
			Controller.AddBoneCurve(Gun, false);
		}
		TestTrue(TEXT("ik_hand_gun held"), Controller.SetBoneTrackKeys(Gun, Positions, Rotations, Scales, false));
		Controller.CloseBracket(false);
		TestTrue(TEXT("source saved"), Save(*Walk));
	}
	const double Time = Walk->GetPlayLength() * 0.5;
	const double SourceGap = FVector::Dist(ComponentLocation(*Walk, Gun, Time), ComponentLocation(*Walk, Hand, Time));

	// Without a Pin Bones op the exported gun bone does not follow the hand.
	const FString Out = Scratch.Path / TEXT("Out");
	const FAssetDocumentResult Plain = Retarget(FString::Printf(TEXT(R"({"retargeter": "%s", "assets": ["%s"], "target_folder": "%s", "suffix": "_Plain"})"),
		*Setup.Retargeter.PackageName, *Walk->GetPathName(), *Out));
	const UAnimSequence* PlainResult = LoadObject<UAnimSequence>(nullptr, *(Out / TEXT("AS_Walk_Plain.AS_Walk_Plain")));
	if (!TestTrue(*FString::Printf(TEXT("plain export (%s)"), *DiagnosticsText(Plain)), Plain.IsSuccess() && PlainResult != nullptr))
	{
		return false;
	}
	const double PlainGap = FVector::Dist(ComponentLocation(*PlainResult, Gun, Time), ComponentLocation(*PlainResult, Hand, Time));
	AddInfo(FString::Printf(TEXT("ik_hand_gun to hand_r: source %.2f cm, plain export %.2f cm"), SourceGap, PlainGap));
	TestTrue(TEXT("without the op the gun bone stays away from the hand"), PlainGap > 1.0);

	// The retargeter's Pin Bones op in its document: copy hand_r to ik_hand_gun on the target.
	const TSharedPtr<FJsonObject> Document = Extract(*this, Setup.Retargeter.ObjectPath, TEXT("retargeter"));
	if (Document == nullptr)
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Ops = BodyOf(Document)->GetArrayField(TEXT("Ops"));
	Ops.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"({"Name": "Pin Gun", "Type": "PinBone", "Settings": {
		"BonesToPin": [{"BoneToCopyFrom": {"BoneName": "hand_r"}, "BoneToCopyTo": {"BoneName": "ik_hand_gun"}}],
		"SkeletonToCopyFrom": "Target"}})"))));
	BodyOf(Document)->SetArrayField(TEXT("Ops"), Ops);
	const FAssetDocumentResult Applied = Apply(Setup.Retargeter, Document);
	if (!TestTrue(*FString::Printf(TEXT("Pin Bones op applies (%s)"), *DiagnosticsText(Applied)), Applied.IsSuccess()))
	{
		return false;
	}
	FString DiffText;
	if (!TestEqual(TEXT("the op reads back as written"), DiffCount(Document, DiffText), 0))
	{
		AddInfo(DiffText.Left(2000));
	}
	const TSharedPtr<FJsonObject> Reread = Extract(*this, Setup.Retargeter.ObjectPath, TEXT("after the op"));
	AddInfo(Serialize(BodyOf(Reread)->GetArrayField(TEXT("Ops")).Last()->AsObject()));

	const FAssetDocumentResult Pinned = Retarget(FString::Printf(TEXT(R"({"retargeter": "%s", "assets": ["%s"], "target_folder": "%s", "suffix": "_Pinned"})"),
		*Setup.Retargeter.PackageName, *Walk->GetPathName(), *Out));
	const UAnimSequence* PinnedResult = LoadObject<UAnimSequence>(nullptr, *(Out / TEXT("AS_Walk_Pinned.AS_Walk_Pinned")));
	if (!TestTrue(*FString::Printf(TEXT("pinned export (%s)"), *DiagnosticsText(Pinned)), Pinned.IsSuccess() && PinnedResult != nullptr))
	{
		return false;
	}
	for (const double At : {0.0, Time, static_cast<double>(PinnedResult->GetPlayLength())})
	{
		const double Gap = FVector::Dist(ComponentLocation(*PinnedResult, Gun, At), ComponentLocation(*PinnedResult, Hand, At));
		TestTrue(*FString::Printf(TEXT("with the op ik_hand_gun follows hand_r at %.2f s (%.3f cm)"), At, Gap), Gap < 0.5);
	}
	return true;
}

#endif
