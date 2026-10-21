// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// End-to-end standalone animation assets through the production Service and the Python Profiles
// (profile.skeleton.v1, profile.blend_space*.v1 / aim_offset*.v1, profile.pose_asset.v1,
// profile.anim_composite.v1). Each class runs the same stages (RunStages):
//  1. create (Skeleton: update of a saved duplicate) and populate in one apply; Diff is clean;
//  2. the extraction re-applies as a no-op (no save, package file untouched) and Inspect works;
//  3. invalid documents fail with a diagnostic at a document path and change nothing;
//  4. an edit (rename / remove / change) applies; Diff is clean;
//  5. a failure in the middle of Apply (after editor writes) rolls back to the edited asset;
//  6. a human edit in an editor transaction, saved, then an AI apply: Ctrl+Z afterwards leaves the
//     asset as applied;
//  7. unloaded and reloaded from disk, it extracts the same.
// AnimationAssets.MontageSlot: a Montage on a Skeleton without a usable slot:
// the refusal names the Skeleton and its slots, a Skeleton document adds the slot, the Montage is
// then created, and the Skeleton refuses to remove a slot the Montage uses.

#include "AssetDocumentModule.h"
#include "AssetDocumentAssetDelete.h"
#include "AssetDocumentService.h"
#include "Runtime/AssetDocumentRuntimeTestHooks.h"

#include "Animation/AimOffsetBlendSpace.h"
#include "Animation/AimOffsetBlendSpace1D.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendProfile.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/PoseAsset.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/SkeletalMeshSocket.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentAnimationAssetsTests
{
const TCHAR* FolderName = TEXT("AssetDocumentAnimationAssets");
const TCHAR* EngineSkeleton = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
const TCHAR* PreviewMesh = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP");
const TCHAR* IdleAnim = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle");
const TCHAR* WalkAnim = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Walk_Fwd.Tutorial_Walk_Fwd");

// Editor operations open transactions; clear the undo buffer before force-deleting fixtures.
void ReleaseUndoHistory()
{
	if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
		&& GEditor->Trans->GetQueueLength() > 0)
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentAnimationAssetsTests", "Cleanup", "AssetDocument animation asset test cleanup"));
	}
}

bool SavePackage(UObject* Asset)
{
	UPackage* Package = Asset->GetOutermost();
	const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	return UPackage::SavePackage(Package, Asset, *File, Args);
}

struct FTarget
{
	FString AssetName;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	explicit FTarget(const TCHAR* Prefix)
		: AssetName(FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
	{
		PackageName = FString(TEXT("/Game/")) + FolderName + TEXT("/") + AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), FolderName, AssetName + TEXT(".assetdoc.json"));
	}

	UObject* Get() const { return FSoftObjectPath(ObjectPath).ResolveObject(); }

	template <typename T>
	T* Get() const { return Cast<T>(Get()); }

	// Saved assets are unloaded and unsaved ones deleted, as the Content Browser does.
	void Cleanup() const
	{
		ReleaseUndoHistory();
		if (UObject* Asset = Get())
		{
			if (IFileManager::Get().FileExists(*PackageFile))
			{
				UPackageTools::UnloadPackages({Asset->GetOutermost()});
			}
			else
			{
				Asset->GetOutermost()->ClearDirtyFlag();
				ObjectTools::DeleteObjects({Asset}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
			}
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		if (IFileManager::Get().FileExists(*PackageFile))
		{
			IFileManager::Get().Delete(*PackageFile, false, true);
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
		}
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}
};

// A saved /Game copy of the engine tutorial Skeleton (the engine asset itself is never edited).
struct FSkeletonCopy : FTarget
{
	FSkeletonCopy() : FTarget(TEXT("SK"))
	{
		USkeleton* Source = LoadObject<USkeleton>(nullptr, EngineSkeleton);
		if (Source != nullptr)
		{
			USkeleton* Copy = DuplicateObject<USkeleton>(Source, CreatePackage(*PackageName), *AssetName);
			Copy->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
			FAssetRegistryModule::AssetCreated(Copy);
			SavePackage(Copy);
		}
	}

	~FSkeletonCopy() { Cleanup(); }
};

FString Serialize(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid())
	{
		const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Text;
}

TSharedPtr<FJsonObject> Parse(const FString& Json)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
	return Object;
}

void LogResult(FAutomationTestBase& Test, const TCHAR* Stage, const FAssetDocumentResult& Result)
{
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), Stage, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s"), Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
}

FAssetDocumentResult Apply(FAutomationTestBase& Test, const FTarget& Target, const FString& Json, const TCHAR* Stage)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	FFileHelper::SaveStringToFile(Json, *Target.SidecarPath);
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	LogResult(Test, Stage, Result);
	return Result;
}

// Validate of an inline document (no sidecar written).
FAssetDocumentResult Validate(FAutomationTestBase& Test, const FString& Json, const TCHAR* Stage)
{
	FAssetDocumentValidateRequest Request;
	Request.Document = Parse(Json);
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Validate(Request);
	LogResult(Test, Stage, Result);
	return Result;
}

TSharedPtr<FJsonObject> Extract(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* Stage)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Target.ObjectPath;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Extract(Request);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract succeeds"), Stage), Result.IsSuccess() && Result.Payload.IsValid()))
	{
		LogResult(Test, Stage, Result);
		return nullptr;
	}
	return Result.Payload;
}

void TestInspect(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* DetailsKey)
{
	FAssetDocumentInspectRequest Request;
	Request.ClassOrAsset = Target.ObjectPath;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Inspect(Request);
	if (!Test.TestTrue(TEXT("inspect succeeds"), Result.IsSuccess() && Result.Payload.IsValid()))
	{
		LogResult(Test, TEXT("inspect"), Result);
		return;
	}
	const FString Text = Serialize(Result.Payload);
	Test.AddInfo(FString::Printf(TEXT("inspect: %s"), *Text.Left(4000)));
	Test.TestTrue(FString::Printf(TEXT("inspect reports %s"), DetailsKey), Text.Contains(DetailsKey));
}

void TestNoDiff(FAutomationTestBase& Test, const FString& Json, const TCHAR* Stage)
{
	FAssetDocumentDiffRequest Request;
	Request.Document = Parse(Json);
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Diff(Request);
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	const bool bOk = Result.IsSuccess() && Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("changed"), Changed);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: diff succeeds"), Stage), bOk))
	{
		LogResult(Test, Stage, Result);
		return;
	}
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: live asset equals the document"), Stage), Changed->Num(), 0))
	{
		Test.AddInfo(Serialize(Result.Payload));
	}
}

bool ExtractEquals(FAutomationTestBase& Test, const FTarget& Target, const FString& Expected, const TCHAR* Stage)
{
	const FString Actual = Serialize(Extract(Test, Target, Stage));
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: extraction is unchanged"), Stage), Actual, Expected))
	{
		int32 First = 0;
		while (First < Expected.Len() && First < Actual.Len() && Expected[First] == Actual[First])
		{
			++First;
		}
		const int32 Start = FMath::Max(0, First - 200);
		Test.AddInfo(FString::Printf(TEXT("expected ...%s"), *Expected.Mid(Start, 600)));
		Test.AddInfo(FString::Printf(TEXT("actual   ...%s"), *Actual.Mid(Start, 600)));
		return false;
	}
	return true;
}

// The failure carries Code at Path and its message contains each of Texts.
bool TestFailsWith(FAutomationTestBase& Test, const FAssetDocumentResult& Result, const FString& Code, const FString& Path,
	const TCHAR* Stage, TConstArrayView<const TCHAR*> Texts = {})
{
	Test.TestFalse(*FString::Printf(TEXT("%s: fails"), Stage), Result.IsSuccess());
	const FAssetDocumentDiagnostic* Found = Result.Diagnostics.FindByPredicate([&Code, &Path](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == Code && Diagnostic.Path == Path;
	});
	if (!Test.TestNotNull(*FString::Printf(TEXT("%s: diagnostic %s at %s"), Stage, *Code, *Path), Found))
	{
		return false;
	}
	for (const TCHAR* Text : Texts)
	{
		Test.TestTrue(*FString::Printf(TEXT("%s: message names '%s'"), Stage, Text), Found->Message.Contains(Text));
	}
	return true;
}

// A failed create leaves no asset, package file or registry entry.
void TestNothingCreated(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* Stage)
{
	Test.TestNull(*FString::Printf(TEXT("%s: no asset remains"), Stage), Target.Get());
	Test.TestFalse(*FString::Printf(TEXT("%s: no package file"), Stage), IFileManager::Get().FileExists(*Target.PackageFile));
	Test.TestFalse(*FString::Printf(TEXT("%s: no registry entry"), Stage),
		FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Target.ObjectPath)).IsValid());
}

FString Document(const FTarget& Target, const TCHAR* Class, const TCHAR* Action, const FString& Body,
	const FString& Properties = TEXT("{}"))
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"%s\",\"Action\":\"%s\",\"Properties\":%s,\"Body\":{%s}}"),
		*Target.PackageName, Class, Action, *Properties, *Body);
}

TArray<FString> BodyNames(const TSharedPtr<FJsonObject>& Document, const TCHAR* Field)
{
	TArray<FString> Names;
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Document.IsValid() && Document->TryGetObjectField(TEXT("Body"), Body) && (*Body)->TryGetArrayField(Field, Values))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			Names.Add(Value->AsString());
		}
	}
	return Names;
}

struct FInvalid
{
	FString Json;
	FString Code;
	FString Path;
};

// Documents are built from the extraction after stage 1 (A) where they depend on live names.
using FDocumentFn = TFunction<FString(const TSharedPtr<FJsonObject>& A)>;

struct FStages
{
	const TCHAR* Class = nullptr;
	const TCHAR* DetailsKey = nullptr;
	FString Create;
	const TCHAR* CreateAction = TEXT("Create");
	FDocumentFn Edit;
	FDocumentFn MidApplyFailure;
	FString MidApplyCode;
	FString MidApplyPath;
	FDocumentFn Invalid;            // one invalid document per call index (see InvalidCases)
	TArray<FInvalid> InvalidCases;  // static invalid documents
	TFunction<void(FAutomationTestBase&, UObject&, bool bA)> Check;
	TFunction<void(UObject&)> HumanEdit;
	FDocumentFn AfterHumanEdit;
};

bool RunStages(FAutomationTestBase& Test, const FTarget& Target, const FStages& Stages)
{
	// 1.
	if (!Test.TestTrue(TEXT("create / populate applies"), Apply(Test, Target, Stages.Create, TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UObject* Asset = Target.Get();
	if (!Test.TestNotNull(TEXT("asset exists"), Asset))
	{
		return false;
	}
	Test.TestTrue(TEXT("asset has the class"), Asset->GetClass()->GetPathName() == Stages.Class);
	Stages.Check(Test, *Asset, true);
	TestNoDiff(Test, Stages.Create, TEXT("after create"));

	// 2.
	const TSharedPtr<FJsonObject> Extracted = Extract(Test, Target, TEXT("extract A"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	const FString BaselineA = Serialize(Extracted);
	Extracted->SetStringField(TEXT("Action"), TEXT("Update"));
	const FString UpdateA = Serialize(Extracted);
	Test.AddInfo(FString::Printf(TEXT("extracted: %s"), *UpdateA.Left(6000)));
	Test.TestFalse(TEXT("package is saved clean"), Asset->GetOutermost()->IsDirty());
	const FDateTime SavedStamp = IFileManager::Get().GetTimeStamp(*Target.PackageFile);
	const FAssetDocumentResult Reapplied = Apply(Test, Target, UpdateA, TEXT("Reapply"));
	Test.TestTrue(TEXT("extraction re-applies"), Reapplied.IsSuccess());
	Test.TestFalse(TEXT("no-op re-apply reports no save"), Reapplied.bSavedAsset);
	Test.TestFalse(TEXT("no-op re-apply leaves the package clean"), Asset->GetOutermost()->IsDirty());
	Test.TestEqual(TEXT("no-op re-apply does not rewrite the package file"), IFileManager::Get().GetTimeStamp(*Target.PackageFile), SavedStamp);
	Test.TestTrue(TEXT("same asset object"), Target.Get() == Asset);
	ExtractEquals(Test, Target, BaselineA, TEXT("after no-op re-apply"));
	TestNoDiff(Test, UpdateA, TEXT("extraction"));
	TestInspect(Test, Target, Stages.DetailsKey);
	const TSharedPtr<FJsonObject> A = Parse(BaselineA);

	// 3.
	for (const FInvalid& Invalid : Stages.InvalidCases)
	{
		// Validate reports what Apply reports and changes nothing.
		TestFailsWith(Test, Validate(Test, Invalid.Json, TEXT("Invalid (validate)")), Invalid.Code, Invalid.Path,
			*FString::Printf(TEXT("%s (validate)"), *Invalid.Code));
		Test.TestFalse(TEXT("validate leaves the package clean"), Asset->GetOutermost()->IsDirty());
		const FAssetDocumentResult Refused = Apply(Test, Target, Invalid.Json, TEXT("Invalid"));
		TestFailsWith(Test, Refused, Invalid.Code, Invalid.Path, *Invalid.Code);
		Test.TestFalse(TEXT("refused document leaves the package clean"), Asset->GetOutermost()->IsDirty());
		ExtractEquals(Test, Target, BaselineA, *FString::Printf(TEXT("after %s"), *Invalid.Code));
	}

	// 4.
	const FString Edited = Stages.Edit(A);
	if (!Test.TestTrue(TEXT("edit applies"), Apply(Test, Target, Edited, TEXT("Edit")).IsSuccess()))
	{
		return false;
	}
	Asset = Target.Get();
	if (!Test.TestNotNull(TEXT("asset exists after edit"), Asset))
	{
		return false;
	}
	Stages.Check(Test, *Asset, false);
	Test.TestFalse(TEXT("edited package is saved"), Asset->GetOutermost()->IsDirty());
	TestNoDiff(Test, Edited, TEXT("after edit"));
	const FString BaselineB = Serialize(Extract(Test, Target, TEXT("extract B")));

	// 5.
	TestFailsWith(Test, Validate(Test, Stages.MidApplyFailure(A), TEXT("MidApplyFailure (validate)")), Stages.MidApplyCode,
		Stages.MidApplyPath, TEXT("mid-apply failure (validate)"));
	const FAssetDocumentResult MidFailure = Apply(Test, Target, Stages.MidApplyFailure(A), TEXT("MidApplyFailure"));
	TestFailsWith(Test, MidFailure, Stages.MidApplyCode, Stages.MidApplyPath, TEXT("mid-apply failure"));
	Asset = Target.Get();
	Test.TestTrue(TEXT("rolled-back package is clean"), Asset != nullptr && !Asset->GetOutermost()->IsDirty());
	ExtractEquals(Test, Target, BaselineB, TEXT("after mid-apply rollback"));

	// 6.
	FString Applied = BaselineB;
	if (Asset != nullptr && GEditor != nullptr && Stages.HumanEdit)
	{
		ReleaseUndoHistory();
		{
			const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentAnimationAssetsTests", "HumanEdit", "Edit"));
			Stages.HumanEdit(*Asset);
		}
		Asset->MarkPackageDirty();
		Test.TestTrue(TEXT("human edit is saved"), SavePackage(Asset));
		Test.TestTrue(TEXT("human edit is in the undo history"), GEditor->Trans->GetQueueLength() > 0);
		Test.TestTrue(TEXT("AI apply after the human edit succeeds"),
			Apply(Test, Target, Stages.AfterHumanEdit(A), TEXT("AfterHumanEdit")).IsSuccess());
		Applied = Serialize(Extract(Test, Target, TEXT("after AI apply")));
		GEditor->UndoTransaction();
		Asset = Target.Get();
		ExtractEquals(Test, Target, Applied, TEXT("after Ctrl+Z"));
		if (Asset != nullptr && Asset->GetOutermost()->IsDirty())
		{
			Test.TestTrue(TEXT("package saves after Ctrl+Z"), SavePackage(Asset));
		}
	}

	// 7.
	Asset = Target.Get();
	if (Asset != nullptr)
	{
		ReleaseUndoHistory();
		UPackageTools::UnloadPackages({Asset->GetOutermost()});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		Test.TestNull(TEXT("asset is unloaded"), Target.Get());
		ExtractEquals(Test, Target, Applied, TEXT("after reload from disk"));
	}
	return true;
}

USkeleton* EngineSkeletonAsset()
{
	return LoadObject<USkeleton>(nullptr, EngineSkeleton);
}

bool HasSample(const UBlendSpace& BlendSpace, const TCHAR* Animation, const FVector& Position, float RateScale)
{
	for (int32 Index = 0; Index < BlendSpace.GetNumberOfBlendSamples(); ++Index)
	{
		const FBlendSample& Sample = BlendSpace.GetBlendSample(Index);
		if (Sample.Animation != nullptr && Sample.Animation->GetPathName() == Animation
			&& Sample.SampleValue.Equals(Position, 1e-3) && FMath::IsNearlyEqual(Sample.RateScale, RateScale))
		{
			return true;
		}
	}
	return false;
}
} // namespace AssetDocumentAnimationAssetsTests


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationAssetsSkeletonTest, "UEAssetDoc.AnimationAssets.Skeleton",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationAssetsSkeletonTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimationAssetsTests;
	(void)Parameters;
	const TCHAR* Class = TEXT("/Script/Engine.Skeleton");
	{
		const FTarget Missing(TEXT("SK_New"));
		ON_SCOPE_EXIT { Missing.Cleanup(); };
		TestFailsWith(*this, Apply(*this, Missing, Document(Missing, Class, TEXT("Create"), TEXT("\"SlotGroups\":[]")), TEXT("Create")),
			TEXT("Skeleton.CreateUnsupported"), TEXT("/Target"), TEXT("skeleton create"), {TEXT("Skeletal Mesh")});
		TestNothingCreated(*this, Missing, TEXT("skeleton create"));
	}

	const FSkeletonCopy Target;
	if (!TestNotNull(TEXT("skeleton copy is saved"), Target.Get<USkeleton>()))
	{
		return false;
	}
	const FString BodyA = TEXT(
		"\"SlotGroups\":[{\"Name\":\"DefaultGroup\",\"Slots\":[\"DefaultSlot\",\"UpperBody\"]},{\"Name\":\"Face\",\"Slots\":[\"Jaw\"]}],"
		"\"Sockets\":[{\"Name\":\"hand_r_weapon\",\"Bone\":\"hand_r\",\"Location\":{\"X\":0,\"Y\":5,\"Z\":0},"
		"\"Rotation\":{\"Pitch\":0,\"Yaw\":90,\"Roll\":0}}],"
		"\"VirtualBones\":[{\"Name\":\"VB hand_ik\",\"Source\":\"pelvis\",\"Target\":\"hand_r\"}],"
		"\"BlendProfiles\":[{\"Name\":\"UpperBodyBlend\",\"Mode\":\"WeightFactor\",\"Bones\":{\"spine_01\":1.5}}],"
		"\"BoneRetargeting\":{\"pelvis\":\"AnimationScaled\"},"
		"\"Curves\":[{\"Name\":\"CTRL_Blink\",\"Material\":false,\"MorphTarget\":true}]");
	const FString BodyB = TEXT(
		"\"SlotGroups\":[{\"Name\":\"DefaultGroup\",\"Slots\":[\"DefaultSlot\",{\"Name\":\"Upper\",\"RenamedFrom\":\"UpperBody\"},\"Mouth\"]}],"
		"\"Sockets\":[{\"Name\":\"weapon_r\",\"RenamedFrom\":\"hand_r_weapon\",\"Bone\":\"hand_r\",\"Location\":{\"X\":0,\"Y\":7,\"Z\":0}}],"
		"\"VirtualBones\":[],"
		"\"BlendProfiles\":[{\"Name\":\"UpperBodyBlend\",\"Mode\":\"TimeFactor\",\"Bones\":{\"spine_02\":0.5}}],"
		"\"BoneRetargeting\":{},"
		"\"Curves\":[{\"Name\":\"CTRL_Wink\",\"RenamedFrom\":\"CTRL_Blink\",\"Material\":true,\"MorphTarget\":true}]");

	FStages Stages;
	Stages.Class = Class;
	Stages.DetailsKey = TEXT("SkeletonDetails");
	Stages.Create = Document(Target, Class, TEXT("Update"), BodyA);
	Stages.Edit = [&](const TSharedPtr<FJsonObject>&) { return Document(Target, Class, TEXT("Update"), BodyB); };
	// Slot ops run first; the new socket's bone does not exist.
	Stages.MidApplyFailure = [&](const TSharedPtr<FJsonObject>&)
	{
		return Document(Target, Class, TEXT("Update"), TEXT(
			"\"SlotGroups\":[{\"Name\":\"DefaultGroup\",\"Slots\":[\"DefaultSlot\",\"Upper\",\"Mouth\",\"Extra\"]}],"
			"\"Sockets\":[{\"Name\":\"weapon_r\",\"Bone\":\"hand_r\"},{\"Name\":\"bad\",\"Bone\":\"NoSuchBone\"}]"));
	};
	Stages.MidApplyCode = TEXT("Skeleton.BoneNotFound");
	Stages.MidApplyPath = TEXT("/Body/Sockets/1/Bone");
	Stages.InvalidCases = {
		{Document(Target, Class, TEXT("Update"), TEXT("\"VirtualBones\":[{\"Name\":\"hand_ik\",\"Source\":\"pelvis\",\"Target\":\"hand_r\"}]")),
			TEXT("Skeleton.InvalidVirtualBone"), TEXT("/Body/VirtualBones/0/Name")},
		{Document(Target, Class, TEXT("Update"), TEXT("\"SlotGroups\":[{\"Name\":\"Face\",\"Slots\":[\"Jaw\"]}]")),
			TEXT("Skeleton.DefaultGroupRequired"), TEXT("/Body/SlotGroups")},
		{Document(Target, Class, TEXT("Update"), TEXT("\"BoneRetargeting\":{\"pelvis\":\"Sideways\"}")),
			TEXT("Skeleton.InvalidRetargetingMode"), TEXT("/Body/BoneRetargeting/pelvis")}};
	Stages.Check = [](FAutomationTestBase& Test, UObject& Asset, bool bA)
	{
		USkeleton& Skeleton = *CastChecked<USkeleton>(&Asset);
		Test.TestEqual(TEXT("UpperBody slot"), Skeleton.ContainsSlotName(TEXT("UpperBody")), bA);
		Test.TestEqual(TEXT("renamed slot"), Skeleton.ContainsSlotName(TEXT("Upper")), !bA);
		Test.TestEqual(TEXT("Face group"), Skeleton.FindAnimSlotGroup(TEXT("Face")) != nullptr, bA);
		Test.TestEqual(TEXT("Mouth slot in DefaultGroup"),
			Skeleton.ContainsSlotName(TEXT("Mouth")) && Skeleton.GetSlotGroupName(TEXT("Mouth")) == TEXT("DefaultGroup"), !bA);
		const USkeletalMeshSocket* Socket = Skeleton.FindSocket(bA ? TEXT("hand_r_weapon") : TEXT("weapon_r"));
		if (Test.TestNotNull(TEXT("socket"), Socket))
		{
			Test.TestEqual(TEXT("socket bone"), Socket->BoneName, FName(TEXT("hand_r")));
			Test.TestEqual(TEXT("socket location"), Socket->RelativeLocation.Y, bA ? 5.0 : 7.0);
		}
		Test.TestEqual(TEXT("socket count"), Skeleton.Sockets.Num(), 1);
		Test.TestEqual(TEXT("virtual bones"), Skeleton.GetVirtualBones().Num(), bA ? 1 : 0);
		const UBlendProfile* Profile = Skeleton.GetBlendProfile(TEXT("UpperBodyBlend"));
		if (Test.TestNotNull(TEXT("blend profile"), Profile))
		{
			Test.TestEqual(TEXT("blend profile mode"), Profile->Mode, bA ? EBlendProfileMode::WeightFactor : EBlendProfileMode::TimeFactor);
			Test.TestEqual(TEXT("blend profile scale"), Profile->GetBoneBlendScale(FName(bA ? TEXT("spine_01") : TEXT("spine_02"))), bA ? 1.5f : 0.5f);
		}
		const int32 Pelvis = Skeleton.GetReferenceSkeleton().FindBoneIndex(TEXT("pelvis"));
		Test.TestEqual(TEXT("pelvis retargeting"), Skeleton.GetBoneTranslationRetargetingMode(Pelvis) == EBoneTranslationRetargetingMode::AnimationScaled, bA);
		const FCurveMetaData* Curve = Skeleton.GetCurveMetaData(bA ? TEXT("CTRL_Blink") : TEXT("CTRL_Wink"));
		if (Test.TestNotNull(TEXT("curve metadata"), Curve))
		{
			Test.TestTrue(TEXT("curve morph target"), Curve->Type.bMorphtarget);
			Test.TestEqual(TEXT("curve material"), static_cast<bool>(Curve->Type.bMaterial), !bA);
		}
	};
	Stages.HumanEdit = [](UObject& Asset)
	{
		USkeleton& Skeleton = *CastChecked<USkeleton>(&Asset);
		Skeleton.Modify();
		Skeleton.SetSlotGroupName(TEXT("HumanSlot"), TEXT("DefaultGroup"));
	};
	Stages.AfterHumanEdit = [&](const TSharedPtr<FJsonObject>&) { return Document(Target, Class, TEXT("Update"), BodyA); };
	RunStages(*this, Target, Stages);
	if (const USkeleton* Skeleton = Target.Get<USkeleton>())
	{
		TestFalse(TEXT("the AI apply removed the human slot"), Skeleton->ContainsSlotName(TEXT("HumanSlot")));
	}
	return true;
}

// Fab Paragon skeletons carry sockets on bone None. Extract and Inspect read the Skeleton with a
// warning at the socket instead of failing; an apply that leaves the socket alone keeps it as it is (the rest of the
// Skeleton is still edited); a document that puts a socket on no bone is refused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationAssetsSkeletonSocketWithoutBoneTest,
	"UEAssetDoc.AnimationAssets.SkeletonSocketWithoutBone", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationAssetsSkeletonSocketWithoutBoneTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimationAssetsTests;
	(void)Parameters;
	const FSkeletonCopy Target;
	USkeleton* Skeleton = Target.Get<USkeleton>();
	if (!TestNotNull(TEXT("skeleton copy is saved"), Skeleton))
	{
		return false;
	}
	USkeletalMeshSocket* Orphan = NewObject<USkeletalMeshSocket>(Skeleton);
	Orphan->SocketName = TEXT("fx_orphan");
	Orphan->BoneName = NAME_None;
	Skeleton->Sockets.Add(Orphan);
	USkeletalMeshSocket* Weapon = NewObject<USkeletalMeshSocket>(Skeleton);
	Weapon->SocketName = TEXT("hand_r_weapon");
	Weapon->BoneName = TEXT("hand_r");
	Skeleton->Sockets.Add(Weapon);
	TestTrue(TEXT("the skeleton with a socket on bone None is saved"), SavePackage(Skeleton));
	Skeleton = nullptr;
	Orphan = nullptr;
	Weapon = nullptr;

	const auto HasWarning = [](const FAssetDocumentResult& Result)
	{
		return Result.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == TEXT("Skeleton.SocketWithoutBone") && Diagnostic.Path.StartsWith(TEXT("/Body/Sockets/"))
				&& Diagnostic.Message.Contains(TEXT("fx_orphan"));
		});
	};
	FAssetDocumentExtractRequest ExtractRequest;
	ExtractRequest.AssetPath = Target.ObjectPath;
	const FAssetDocumentResult Extracted = FAssetDocumentModule::Get().GetService().Extract(ExtractRequest);
	LogResult(*this, TEXT("extract"), Extracted);
	if (!TestTrue(TEXT("extract succeeds"), Extracted.IsSuccess() && Extracted.Payload.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("extract warns about the socket on bone None"), HasWarning(Extracted));
	const FString Text = Serialize(Extracted.Payload);
	TestTrue(TEXT("extract lists the socket on bone None"), Text.Contains(TEXT("\"Bone\":\"None\"")) && Text.Contains(TEXT("fx_orphan")));
	TestTrue(TEXT("extract lists the rest of the Skeleton"), Text.Contains(TEXT("hand_r_weapon")) && Text.Contains(TEXT("SlotGroups")));

	FAssetDocumentInspectRequest InspectRequest;
	InspectRequest.ClassOrAsset = Target.ObjectPath;
	const FAssetDocumentResult Inspected = FAssetDocumentModule::Get().GetService().Inspect(InspectRequest);
	LogResult(*this, TEXT("inspect"), Inspected);
	TestTrue(TEXT("inspect succeeds"), Inspected.IsSuccess() && Serialize(Inspected.Payload).Contains(TEXT("SkeletonDetails")));
	TestTrue(TEXT("inspect warns about the socket on bone None"), HasWarning(Inspected));

	// The extraction re-applies as a no-op.
	const FAssetDocumentResult Reapplied = Apply(*this, Target, Text, TEXT("re-apply"));
	TestTrue(TEXT("the extraction re-applies"), Reapplied.IsSuccess());
	TestFalse(TEXT("re-applying it saves nothing"), Reapplied.bSavedAsset);

	// An edit of another socket applies and keeps the socket on bone None as it is.
	const FString Edited = Text.Replace(TEXT("\"Bone\":\"hand_r\""), TEXT("\"Bone\":\"hand_l\""));
	TestNotEqual(TEXT("the edit changes the document"), Edited, Text);
	TestTrue(TEXT("the edit applies"), Apply(*this, Target, Edited, TEXT("edit")).IsSuccess());
	if (const USkeleton* Applied = Target.Get<USkeleton>())
	{
		const USkeletalMeshSocket* Kept = Applied->FindSocket(TEXT("fx_orphan"));
		TestTrue(TEXT("the socket on bone None is kept as it is"), Kept != nullptr && Kept->BoneName.IsNone());
		const USkeletalMeshSocket* Moved = Applied->FindSocket(TEXT("hand_r_weapon"));
		TestTrue(TEXT("the other socket is edited"), Moved != nullptr && Moved->BoneName == FName(TEXT("hand_l")));
	}

	// A document that puts a socket on no bone is refused, and nothing changes.
	const TSharedPtr<FJsonObject> Added = Parse(Edited);
	const TSharedPtr<FJsonObject>* Body = nullptr;
	TArray<TSharedPtr<FJsonValue>> Sockets;
	if (TestTrue(TEXT("the document has a Body"), Added.IsValid() && Added->TryGetObjectField(TEXT("Body"), Body)))
	{
		Sockets = (*Body)->GetArrayField(TEXT("Sockets"));
		const TSharedPtr<FJsonObject> New = MakeShared<FJsonObject>();
		New->SetStringField(TEXT("Name"), TEXT("new_socket"));
		New->SetStringField(TEXT("Bone"), TEXT("None"));
		Sockets.Add(MakeShared<FJsonValueObject>(New));
		(*Body)->SetArrayField(TEXT("Sockets"), Sockets);
		TestFailsWith(*this, Apply(*this, Target, Serialize(Added), TEXT("new socket on bone None")), TEXT("Skeleton.InvalidSocket"),
			FString::Printf(TEXT("/Body/Sockets/%d/Bone"), Sockets.Num() - 1), TEXT("new socket on bone None"), {TEXT("new_socket")});
		const USkeleton* Unchanged = Target.Get<USkeleton>();
		TestTrue(TEXT("the refused socket is not added"), Unchanged != nullptr && Unchanged->FindSocket(TEXT("new_socket")) == nullptr);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationAssetsBlendSpaceTest, "UEAssetDoc.AnimationAssets.BlendSpace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationAssetsBlendSpaceTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimationAssetsTests;
	(void)Parameters;
	const TCHAR* Class = TEXT("/Script/Engine.BlendSpace");
	const FTarget Target(TEXT("BS"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	const FString Axes = TEXT("\"Axes\":{\"X\":{\"Name\":\"Direction\",\"Min\":-180,\"Max\":180,\"GridDivisions\":4,\"SnapToGrid\":true},")
		TEXT("\"Y\":{\"Name\":\"Speed\",\"Min\":0,\"Max\":600,\"GridDivisions\":4}}");
	const FString BodyA = FString::Printf(TEXT("\"Skeleton\":\"%s\",\"PreviewMesh\":\"%s\",%s,\"Samples\":[")
		TEXT("{\"Animation\":\"%s\",\"X\":0,\"Y\":0,\"SingleFrame\":3},{\"Animation\":\"%s\",\"X\":0,\"Y\":300,\"RateScale\":1.25}]"),
		EngineSkeleton, PreviewMesh, *Axes, IdleAnim, WalkAnim);
	// Sample (0, 300) removed, (90, 250) added, the idle sample's settings changed (SingleFrame null: blend all frames
	// again), Y renamed and narrowed.
	const FString BodyB = FString::Printf(TEXT("\"Axes\":{\"Y\":{\"Name\":\"Velocity\",\"Max\":500}},\"Samples\":[")
		TEXT("{\"Animation\":\"%s\",\"X\":0,\"Y\":0,\"RateScale\":0.5,\"IncludeInAnalyseAll\":false,\"SingleFrame\":null},")
		TEXT("{\"Animation\":\"%s\",\"X\":90,\"Y\":250}]"), IdleAnim, WalkAnim);

	FStages Stages;
	Stages.Class = Class;
	Stages.DetailsKey = TEXT("BlendSpaceDetails");
	Stages.Create = Document(Target, Class, TEXT("Create"), BodyA);
	Stages.Edit = [&](const TSharedPtr<FJsonObject>&)
	{
		return Document(Target, Class, TEXT("Update"), BodyB, TEXT("{\"TargetWeightInterpolationSpeedPerSec\":2.5}"));
	};
	// The axis edit runs first; the new sample's animation is a Skeleton.
	Stages.MidApplyFailure = [&](const TSharedPtr<FJsonObject>&)
	{
		return Document(Target, Class, TEXT("Update"), FString::Printf(TEXT("\"Axes\":{\"Y\":{\"Max\":550}},\"Samples\":[")
			TEXT("{\"Animation\":\"%s\",\"X\":0,\"Y\":0},{\"Animation\":\"%s\",\"X\":90,\"Y\":250},{\"Animation\":\"%s\",\"X\":-90,\"Y\":250}]"),
			IdleAnim, WalkAnim, EngineSkeleton));
	};
	Stages.MidApplyCode = TEXT("AnimationAsset.ReferenceNotFound");
	Stages.MidApplyPath = TEXT("/Body/Samples/2/Animation");
	Stages.InvalidCases = {
		{Document(Target, Class, TEXT("Update"), FString::Printf(TEXT("\"Samples\":[{\"Animation\":\"%s\",\"X\":500,\"Y\":0}]"), IdleAnim)),
			TEXT("BlendSpace.SampleOutOfRange"), TEXT("/Body/Samples/0/X")},
		{Document(Target, Class, TEXT("Update"), TEXT("\"Skeleton\":\"/Game/Other/SK_Other.SK_Other\"")),
			TEXT("BlendSpace.SkeletonImmutable"), TEXT("/Body/Skeleton")},
		{Document(Target, Class, TEXT("Update"), FString::Printf(TEXT("\"Samples\":[{\"Animation\":\"%s\",\"X\":0,\"Y\":0},")
			TEXT("{\"Animation\":\"%s\",\"X\":0,\"Y\":0}]"), IdleAnim, WalkAnim)),
			TEXT("BlendSpace.DuplicateSample"), TEXT("/Body/Samples/1")}};
	Stages.Check = [](FAutomationTestBase& Test, UObject& Asset, bool bA)
	{
		const UBlendSpace& BlendSpace = *CastChecked<UBlendSpace>(&Asset);
		Test.TestEqual(TEXT("skeleton"), BlendSpace.GetSkeleton(), EngineSkeletonAsset());
		const FBlendParameter& X = BlendSpace.GetBlendParameter(0);
		const FBlendParameter& Y = BlendSpace.GetBlendParameter(1);
		Test.TestEqual(TEXT("X name"), X.DisplayName, FString(TEXT("Direction")));
		Test.TestEqual(TEXT("X range"), X.Min == -180.f && X.Max == 180.f && X.GridNum == 4 && X.bSnapToGrid, true);
		Test.TestEqual(TEXT("Y name"), Y.DisplayName, FString(bA ? TEXT("Speed") : TEXT("Velocity")));
		Test.TestEqual(TEXT("Y max"), Y.Max, bA ? 600.f : 500.f);
		Test.TestEqual(TEXT("sample count"), BlendSpace.GetNumberOfBlendSamples(), 2);
		Test.TestTrue(TEXT("idle sample"), HasSample(BlendSpace, IdleAnim, FVector::ZeroVector, bA ? 1.f : 0.5f));
		Test.TestTrue(TEXT("walk sample"), bA ? HasSample(BlendSpace, WalkAnim, FVector(0, 300, 0), 1.25f)
			: HasSample(BlendSpace, WalkAnim, FVector(90, 250, 0), 1.f));
		Test.TestEqual(TEXT("details setting"), BlendSpace.TargetWeightInterpolationSpeedPerSec, bA ? 0.f : 2.5f);
		for (int32 Index = 0; Index < BlendSpace.GetNumberOfBlendSamples(); ++Index)
		{
			const FBlendSample& Sample = BlendSpace.GetBlendSample(Index);
			if (Sample.Animation != nullptr && Sample.Animation->GetPathName() == IdleAnim)
			{
				Test.TestEqual(TEXT("idle sample single frame (A: frame 3, B: null = all frames)"),
					Sample.bUseSingleFrameForBlending ? static_cast<int32>(Sample.FrameIndexToSample) : -1, bA ? 3 : -1);
			}
		}
	};
	Stages.HumanEdit = [](UObject& Asset)
	{
		UBlendSpace& BlendSpace = *CastChecked<UBlendSpace>(&Asset);
		BlendSpace.Modify();
		BlendSpace.TargetWeightInterpolationSpeedPerSec = 9.f;
	};
	Stages.AfterHumanEdit = [&](const TSharedPtr<FJsonObject>&)
	{
		return Document(Target, Class, TEXT("Update"), BodyB, TEXT("{\"TargetWeightInterpolationSpeedPerSec\":2.5}"));
	};
	RunStages(*this, Target, Stages);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationAssetsBlendSpaceClassesTest, "UEAssetDoc.AnimationAssets.BlendSpaceClasses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationAssetsBlendSpaceClassesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimationAssetsTests;
	(void)Parameters;
	// BlendSpace1D: one axis, populated, no-op re-apply, reload.
	{
		const TCHAR* Class = TEXT("/Script/Engine.BlendSpace1D");
		const FTarget Target(TEXT("BS1D"));
		ON_SCOPE_EXIT { Target.Cleanup(); };
		const FString Created = Document(Target, Class, TEXT("Create"), FString::Printf(TEXT("\"Skeleton\":\"%s\",")
			TEXT("\"Axes\":{\"X\":{\"Name\":\"Speed\",\"Min\":0,\"Max\":300,\"GridDivisions\":3}},")
			TEXT("\"Samples\":[{\"Animation\":\"%s\",\"X\":0},{\"Animation\":\"%s\",\"X\":300}]"), EngineSkeleton, IdleAnim, WalkAnim));
		if (TestTrue(TEXT("1D create applies"), Apply(*this, Target, Created, TEXT("1D")).IsSuccess()))
		{
			const UBlendSpace1D* BlendSpace = Target.Get<UBlendSpace1D>();
			TestTrue(TEXT("1D samples"), BlendSpace != nullptr && BlendSpace->GetNumberOfBlendSamples() == 2
				&& HasSample(*BlendSpace, WalkAnim, FVector(300, 0, 0), 1.f));
			TestNoDiff(*this, Created, TEXT("1D"));
			TestFailsWith(*this, Apply(*this, Target, Document(Target, Class, TEXT("Update"), TEXT("\"Axes\":{\"Y\":{\"Max\":1}}")), TEXT("1D Y")),
				TEXT("BlendSpace.InvalidAxis"), TEXT("/Body/Axes/Y"), TEXT("1D has no Y axis"));
			const TSharedPtr<FJsonObject> Extracted = Extract(*this, Target, TEXT("1D extract"));
			if (Extracted.IsValid())
			{
				const FString Baseline = Serialize(Extracted);
				Extracted->SetStringField(TEXT("Action"), TEXT("Update"));
				const FAssetDocumentResult Reapplied = Apply(*this, Target, Serialize(Extracted), TEXT("1D reapply"));
				TestTrue(TEXT("1D re-apply is a no-op"), Reapplied.IsSuccess() && !Reapplied.bSavedAsset);
				ReleaseUndoHistory();
				UPackageTools::UnloadPackages({Target.Get()->GetOutermost()});
				CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
				ExtractEquals(*this, Target, Baseline, TEXT("1D after reload"));
			}
		}
	}
	// Aim Offsets take additive rotation offsets only: a plain sequence is refused and the failed
	// create leaves nothing; without samples they are created with their axes.
	for (const TCHAR* Class : {TEXT("/Script/Engine.AimOffsetBlendSpace"), TEXT("/Script/Engine.AimOffsetBlendSpace1D")})
	{
		const bool b1D = FString(Class).EndsWith(TEXT("1D"));
		const FString AxesJson = b1D ? TEXT("\"Axes\":{\"X\":{\"Name\":\"Yaw\",\"Min\":-90,\"Max\":90}}")
			: TEXT("\"Axes\":{\"X\":{\"Name\":\"Yaw\",\"Min\":-90,\"Max\":90},\"Y\":{\"Name\":\"Pitch\",\"Min\":-45,\"Max\":45}}");
		{
			const FTarget Target(TEXT("AO_Bad"));
			ON_SCOPE_EXIT { Target.Cleanup(); };
			const FString Sample = FString::Printf(TEXT("{\"Animation\":\"%s\",\"X\":0%s}"), IdleAnim, b1D ? TEXT("") : TEXT(",\"Y\":0"));
			const FString BadCreate = Document(Target, Class, TEXT("Create"), FString::Printf(
				TEXT("\"Skeleton\":\"%s\",%s,\"Samples\":[%s]"), EngineSkeleton, *AxesJson, *Sample));
			TestFailsWith(*this, Validate(*this, BadCreate, Class), TEXT("BlendSpace.IncompatibleAnimation"), TEXT("/Body/Samples/0/Animation"),
				*FString::Printf(TEXT("%s (validate)"), Class));
			TestNothingCreated(*this, Target, *FString::Printf(TEXT("%s (validate)"), Class));
			TestFailsWith(*this, Apply(*this, Target, BadCreate, Class),
				TEXT("BlendSpace.IncompatibleAnimation"), TEXT("/Body/Samples/0/Animation"), Class);
			TestNothingCreated(*this, Target, Class);
		}
		const FTarget Target(TEXT("AO"));
		ON_SCOPE_EXIT { Target.Cleanup(); };
		const FString Created = Document(Target, Class, TEXT("Create"), FString::Printf(TEXT("\"Skeleton\":\"%s\",%s,\"Samples\":[]"),
			EngineSkeleton, *AxesJson));
		if (TestTrue(FString::Printf(TEXT("%s create applies"), Class), Apply(*this, Target, Created, Class).IsSuccess()))
		{
			const UBlendSpace* BlendSpace = Target.Get<UBlendSpace>();
			TestTrue(TEXT("aim offset class and axis"), BlendSpace != nullptr && BlendSpace->GetClass()->GetPathName() == Class
				&& BlendSpace->GetBlendParameter(0).DisplayName == TEXT("Yaw") && BlendSpace->GetBlendParameter(0).Min == -90.f);
			TestNoDiff(*this, Created, Class);
		}
	}
	// A blend space needs its Skeleton to be created.
	{
		const FTarget Target(TEXT("BS_NoSkeleton"));
		ON_SCOPE_EXIT { Target.Cleanup(); };
		TestFailsWith(*this, Apply(*this, Target, Document(Target, TEXT("/Script/Engine.BlendSpace"), TEXT("Create"), TEXT("\"Samples\":[]")),
			TEXT("no skeleton")), TEXT("BlendSpace.SkeletonRequired"), TEXT("/Body/Skeleton"), TEXT("no skeleton"));
		TestNothingCreated(*this, Target, TEXT("no skeleton"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationAssetsPoseAssetTest, "UEAssetDoc.AnimationAssets.PoseAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationAssetsPoseAssetTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimationAssetsTests;
	(void)Parameters;
	const TCHAR* Class = TEXT("/Script/Engine.PoseAsset");
	const FTarget Target(TEXT("PA"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	// Poses JSON of the A extraction: Pose_0 renamed to To, the last pose removed.
	const auto Poses = [](const TSharedPtr<FJsonObject>& A, const TCHAR* From, const TCHAR* To)
	{
		const TArray<FString> Names = BodyNames(A, TEXT("Poses"));
		TArray<FString> Rows;
		for (int32 Index = 0; Index + 1 < Names.Num(); ++Index)
		{
			Rows.Add(Index == 0 ? FString::Printf(TEXT("{\"Name\":\"%s\",\"RenamedFrom\":\"%s\"}"), To, From)
				: FString::Printf(TEXT("\"%s\""), *Names[Index]));
		}
		return FString::Printf(TEXT("\"Poses\":[%s]"), *FString::Join(Rows, TEXT(",")));
	};
	int32 CreatedPoses = 0;

	FStages Stages;
	Stages.Class = Class;
	Stages.DetailsKey = TEXT("PoseAssetDetails");
	Stages.Create = Document(Target, Class, TEXT("Create"), FString::Printf(TEXT("\"SourceAnimation\":\"%s\",\"PreviewMesh\":\"%s\""), WalkAnim, PreviewMesh));
	Stages.Edit = [&](const TSharedPtr<FJsonObject>& A)
	{
		return Document(Target, Class, TEXT("Update"), Poses(A, TEXT("Pose_0"), TEXT("Contact")) + TEXT(",\"Additive\":true,\"BasePose\":\"Contact\""));
	};
	// The rename runs first; the preview mesh is not a Skeletal Mesh.
	Stages.MidApplyFailure = [&](const TSharedPtr<FJsonObject>& A)
	{
		return Document(Target, Class, TEXT("Update"), Poses(A, TEXT("Contact"), TEXT("Plant"))
			+ FString::Printf(TEXT(",\"PreviewMesh\":\"%s\""), EngineSkeleton));
	};
	Stages.MidApplyCode = TEXT("AnimationAsset.ReferenceNotFound");
	Stages.MidApplyPath = TEXT("/Body/PreviewMesh");
	Stages.InvalidCases = {
		{Document(Target, Class, TEXT("Update"), TEXT("\"Poses\":[\"Brand_New\"]")), TEXT("PoseAsset.PoseAddUnsupported"), TEXT("/Body/Poses/0")},
		{Document(Target, Class, TEXT("Update"), TEXT("\"Skeleton\":\"/Game/Other/SK_Other.SK_Other\"")),
			TEXT("PoseAsset.SkeletonImmutable"), TEXT("/Body/Skeleton")},
		{Document(Target, Class, TEXT("Update"), TEXT("\"BasePose\":\"Nope\"")), TEXT("PoseAsset.PoseNotFound"), TEXT("/Body/BasePose")}};
	Stages.Check = [&CreatedPoses](FAutomationTestBase& Test, UObject& Asset, bool bA)
	{
		const UPoseAsset& PoseAsset = *CastChecked<UPoseAsset>(&Asset);
		Test.TestEqual(TEXT("skeleton from the source animation"), PoseAsset.GetSkeleton(), EngineSkeletonAsset());
		if (bA)
		{
			CreatedPoses = PoseAsset.GetNumPoses();
			Test.TestTrue(TEXT("one pose per source frame (at least 3)"), CreatedPoses >= 3);
			Test.TestEqual(TEXT("first pose"), PoseAsset.GetPoseNameByIndex(0), FName(TEXT("Pose_0")));
			Test.TestFalse(TEXT("not additive"), PoseAsset.IsValidAdditive());
			return;
		}
		Test.TestEqual(TEXT("last pose removed"), PoseAsset.GetNumPoses(), CreatedPoses - 1);
		Test.TestEqual(TEXT("renamed pose"), PoseAsset.GetPoseNameByIndex(0), FName(TEXT("Contact")));
		Test.TestTrue(TEXT("additive"), PoseAsset.IsValidAdditive());
		Test.TestEqual(TEXT("base pose"), PoseAsset.GetPoseNameByIndex(PoseAsset.GetBasePoseIndex()), FName(TEXT("Contact")));
	};
	Stages.HumanEdit = [](UObject& Asset)
	{
		UPoseAsset& PoseAsset = *CastChecked<UPoseAsset>(&Asset);
		PoseAsset.Modify();
		PoseAsset.ModifyPoseName(PoseAsset.GetPoseNameByIndex(1), TEXT("HumanPose"));
	};
	Stages.AfterHumanEdit = [&](const TSharedPtr<FJsonObject>&)
	{
		return Document(Target, Class, TEXT("Update"), TEXT("\"Additive\":false,\"BasePose\":\"\""));
	};
	RunStages(*this, Target, Stages);
	{
		const FTarget Missing(TEXT("PA_NoSource"));
		ON_SCOPE_EXIT { Missing.Cleanup(); };
		TestFailsWith(*this, Apply(*this, Missing, Document(Missing, Class, TEXT("Create"), TEXT("\"Additive\":false")), TEXT("no source")),
			TEXT("PoseAsset.SourceAnimationRequired"), TEXT("/Body/SourceAnimation"), TEXT("no source"));
		TestNothingCreated(*this, Missing, TEXT("no source"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationAssetsCompositeTest, "UEAssetDoc.AnimationAssets.AnimComposite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationAssetsCompositeTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimationAssetsTests;
	(void)Parameters;
	const TCHAR* Class = TEXT("/Script/Engine.AnimComposite");
	const FTarget Target(TEXT("AC"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	const FString SegmentsB = FString::Printf(TEXT("\"Segments\":[{\"Animation\":\"%s\",\"StartTime\":0.25,\"EndTime\":0.75},{\"Animation\":\"%s\"}]"),
		WalkAnim, IdleAnim);

	FStages Stages;
	Stages.Class = Class;
	Stages.DetailsKey = TEXT("CompositeDetails");
	Stages.Create = Document(Target, Class, TEXT("Create"), FString::Printf(TEXT("\"Skeleton\":\"%s\",\"Segments\":[")
		TEXT("{\"Animation\":\"%s\"},{\"Animation\":\"%s\",\"PlayRate\":1.5,\"LoopCount\":2}]"), EngineSkeleton, IdleAnim, WalkAnim));
	Stages.Edit = [&](const TSharedPtr<FJsonObject>&) { return Document(Target, Class, TEXT("Update"), SegmentsB); };
	// The segment track is replaced first; the preview mesh is not a Skeletal Mesh.
	Stages.MidApplyFailure = [&](const TSharedPtr<FJsonObject>&)
	{
		return Document(Target, Class, TEXT("Update"), FString::Printf(TEXT("\"Segments\":[{\"Animation\":\"%s\"}],\"PreviewMesh\":\"%s\""),
			IdleAnim, EngineSkeleton));
	};
	Stages.MidApplyCode = TEXT("AnimationAsset.ReferenceNotFound");
	Stages.MidApplyPath = TEXT("/Body/PreviewMesh");
	Stages.InvalidCases = {
		{Document(Target, Class, TEXT("Update"), FString::Printf(TEXT("\"Segments\":[{\"Animation\":\"%s\",\"PlayRate\":0}]"), IdleAnim)),
			TEXT("AnimComposite.InvalidSegment"), TEXT("/Body/Segments/0/PlayRate")},
		{Document(Target, Class, TEXT("Update"), FString::Printf(TEXT("\"Segments\":[{\"Animation\":\"%s\"}]"), *Target.ObjectPath)),
			TEXT("AnimComposite.IncompatibleAnimation"), TEXT("/Body/Segments/0/Animation")},
		{Document(Target, Class, TEXT("Update"), TEXT("\"Skeleton\":\"/Game/Other/SK_Other.SK_Other\"")),
			TEXT("AnimComposite.SkeletonImmutable"), TEXT("/Body/Skeleton")}};
	Stages.Check = [](FAutomationTestBase& Test, UObject& Asset, bool bA)
	{
		const UAnimComposite& Composite = *CastChecked<UAnimComposite>(&Asset);
		const TArray<FAnimSegment>& Segments = Composite.AnimationTrack.AnimSegments;
		Test.TestEqual(TEXT("skeleton"), Composite.GetSkeleton(), EngineSkeletonAsset());
		if (!Test.TestEqual(TEXT("segment count"), Segments.Num(), 2))
		{
			return;
		}
		const UAnimSequenceBase* First = Segments[0].GetAnimReference();
		Test.TestEqual(TEXT("first animation"), First != nullptr ? First->GetPathName() : FString(), FString(bA ? IdleAnim : WalkAnim));
		Test.TestEqual(TEXT("segments are back to back"), Segments[1].StartPos, Segments[0].StartPos + Segments[0].GetLength(), 1e-3f);
		if (bA)
		{
			Test.TestEqual(TEXT("play rate"), Segments[1].AnimPlayRate, 1.5f);
			Test.TestEqual(TEXT("loops"), Segments[1].LoopingCount, 2);
		}
		else
		{
			Test.TestEqual(TEXT("start time"), Segments[0].AnimStartTime, 0.25f);
			Test.TestEqual(TEXT("end time"), Segments[0].AnimEndTime, 0.75f);
		}
		Test.TestEqual(TEXT("composite length"), Composite.GetPlayLength(), Segments[1].StartPos + Segments[1].GetLength(), 1e-3f);
	};
	Stages.HumanEdit = [](UObject& Asset)
	{
		UAnimComposite& Composite = *CastChecked<UAnimComposite>(&Asset);
		Composite.Modify();
		Composite.AnimationTrack.AnimSegments[1].AnimPlayRate = 3.f;
	};
	// Omitted segment fields keep the live value; the AI sets the rate the human changed.
	Stages.AfterHumanEdit = [&](const TSharedPtr<FJsonObject>&)
	{
		return Document(Target, Class, TEXT("Update"), FString::Printf(TEXT("\"Segments\":[{\"Animation\":\"%s\",\"StartTime\":0.25,")
			TEXT("\"EndTime\":0.75},{\"Animation\":\"%s\",\"PlayRate\":2.0}]"), WalkAnim, IdleAnim));
	};
	RunStages(*this, Target, Stages);
	{
		const FTarget Missing(TEXT("AC_NoSkeleton"));
		ON_SCOPE_EXIT { Missing.Cleanup(); };
		TestFailsWith(*this, Apply(*this, Missing, Document(Missing, Class, TEXT("Create"), TEXT("\"Segments\":[]")), TEXT("no skeleton")),
			TEXT("AnimComposite.SkeletonRequired"), TEXT("/Body/Skeleton"), TEXT("no skeleton"));
		TestNothingCreated(*this, Missing, TEXT("no skeleton"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationAssetsMontageSlotTest, "UEAssetDoc.AnimationAssets.MontageSlot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationAssetsMontageSlotTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimationAssetsTests;
	(void)Parameters;
	// In a project: the project's Skeleton (a /Game copy of the tutorial one) has no
	// usable slot and the animation is the engine tutorial sequence.
	const FSkeletonCopy Skeleton;
	USkeleton* SkeletonAsset = Skeleton.Get<USkeleton>();
	if (!TestNotNull(TEXT("skeleton copy is saved"), SkeletonAsset))
	{
		return false;
	}
	TestFalse(TEXT("the copy has no DefaultSlot"), SkeletonAsset->ContainsSlotName(TEXT("DefaultSlot")));
	const TCHAR* Montage = TEXT("/Script/Engine.AnimMontage");
	const FTarget Target(TEXT("AM"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	const FString SkeletonOnly = FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"%s\",\"Action\":\"Create\",")
		TEXT("\"Definitions\":{},\"Properties\":{},\"Body\":{\"Skeleton\":\"%s\"}}"), *Target.PackageName, Montage, *Skeleton.ObjectPath);
	const FString WithSlot = FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"%s\",\"Action\":\"CreateOrUpdate\",")
		TEXT("\"Definitions\":{},\"Properties\":{},\"Body\":{\"Skeleton\":\"%s\",\"SlotAnimTracks\":[{\"SlotName\":\"UpperBody\",\"Segments\":[")
		TEXT("{\"AnimReference\":\"%s\",\"StartPos\":0.0,\"AnimStartTime\":0.0,\"AnimEndTime\":1.0,\"AnimPlayRate\":1.0,\"LoopingCount\":1}]}],")
		TEXT("\"CompositeSections\":[{\"Name\":\"Default\",\"NextSectionName\":null,\"LinkableTime\":0.0,\"SlotName\":\"UpperBody\",\"LinkMethod\":0}]}}"),
		*Target.PackageName, Montage, *Skeleton.ObjectPath, IdleAnim);

	// 1. The refusals name the Skeleton and say what it has (task 7: "InvalidSlot: DefaultSlot (at /Primitive)").
	TestFailsWith(*this, Apply(*this, Target, SkeletonOnly, TEXT("skeleton only")), TEXT("AnimMontage.UnknownSlot"),
		TEXT("/Body/SlotAnimTracks"), TEXT("skeleton only"), {*Skeleton.ObjectPath, TEXT("'DefaultSlot'"), TEXT("Body.SlotGroups")});
	TestNothingCreated(*this, Target, TEXT("skeleton only"));
	TestFailsWith(*this, Apply(*this, Target, WithSlot, TEXT("unknown slot")), TEXT("AnimMontage.UnknownSlot"),
		TEXT("/Body/SlotAnimTracks/0/SlotName"), TEXT("unknown slot"), {*Skeleton.ObjectPath, TEXT("'UpperBody'")});
	TestNothingCreated(*this, Target, TEXT("unknown slot"));

	// 2. A Skeleton document adds the slots (and, as the Details panel does, lists the tutorial
	// Skeleton as compatible so its animations can be used).
	const FString SkeletonEdit = Document(Skeleton, TEXT("/Script/Engine.Skeleton"), TEXT("Update"),
		TEXT("\"SlotGroups\":[{\"Name\":\"DefaultGroup\",\"Slots\":[\"DefaultSlot\",\"UpperBody\"]}]"),
		FString::Printf(TEXT("{\"CompatibleSkeletons\":[\"%s\"]}"), EngineSkeleton));
	if (!TestTrue(TEXT("skeleton slots are added"), Apply(*this, Skeleton, SkeletonEdit, TEXT("skeleton slots")).IsSuccess()))
	{
		return false;
	}
	SkeletonAsset = Skeleton.Get<USkeleton>();
	TestTrue(TEXT("UpperBody slot exists"), SkeletonAsset != nullptr && SkeletonAsset->ContainsSlotName(TEXT("UpperBody")));

	// 3. Task 7's skeleton-only document now creates its montage (on DefaultSlot); a montage with an
	// UpperBody track is created too.
	{
		const FTarget SkeletonOnlyTarget(TEXT("AM_SkeletonOnly"));
		ON_SCOPE_EXIT { SkeletonOnlyTarget.Cleanup(); };
		const FString TaskSeven = SkeletonOnly.Replace(*Target.PackageName, *SkeletonOnlyTarget.PackageName);
		TestTrue(TEXT("skeleton-only montage is created"), Apply(*this, SkeletonOnlyTarget, TaskSeven, TEXT("montage create")).IsSuccess());
		const UAnimMontage* Created = SkeletonOnlyTarget.Get<UAnimMontage>();
		TestTrue(TEXT("skeleton-only montage has the DefaultSlot track"), Created != nullptr && Created->SlotAnimTracks.Num() == 1
			&& Created->SlotAnimTracks[0].SlotName == TEXT("DefaultSlot"));
	}
	if (TestTrue(TEXT("UpperBody montage is created"), Apply(*this, Target, WithSlot, TEXT("montage slot")).IsSuccess()))
	{
		const UAnimMontage* Created = Target.Get<UAnimMontage>();
		TestTrue(TEXT("montage plays the idle on UpperBody"), Created != nullptr && Created->SlotAnimTracks.Num() == 1
			&& Created->SlotAnimTracks[0].SlotName == TEXT("UpperBody")
			&& Created->SlotAnimTracks[0].AnimTrack.AnimSegments.Num() == 1);
		TestNoDiff(*this, WithSlot, TEXT("montage"));

		// 4. As the Anim Slot Manager, a slot a montage uses cannot be removed.
		const FAssetDocumentResult InUse = Apply(*this, Skeleton, Document(Skeleton, TEXT("/Script/Engine.Skeleton"), TEXT("Update"),
			TEXT("\"SlotGroups\":[{\"Name\":\"DefaultGroup\",\"Slots\":[\"DefaultSlot\"]}]")), TEXT("slot in use"));
		TestFailsWith(*this, InUse, TEXT("Skeleton.SlotInUse"), TEXT("/Body/SlotGroups"), TEXT("slot in use"), {*Target.AssetName});
		TestTrue(TEXT("slot is kept"), Skeleton.Get<USkeleton>() != nullptr && Skeleton.Get<USkeleton>()->ContainsSlotName(TEXT("UpperBody")));
	}
	return true;
}
// A one-shot Create with Notifies / NotifyStates passed validate but failed
// apply (AnimationProvider.Montage.InvalidIntent: a new Montage has no notify track until it is loaded),
// and a NotifyState Duration of 0.35 failed the post-save check (read back as end - start, 0.34999996).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationAssetsMontageCreateNotifiesTest,
	"UEAssetDoc.AnimationAssets.MontageCreateWithNotifies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationAssetsMontageCreateNotifiesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimationAssetsTests;
	(void)Parameters;
	using namespace AssetDocumentAnimationAssetsTests;
	const FSkeletonCopy Skeleton;
	if (!TestNotNull(TEXT("skeleton copy is saved"), Skeleton.Get<USkeleton>()))
	{
		return false;
	}
	const FString SkeletonEdit = Document(Skeleton, TEXT("/Script/Engine.Skeleton"), TEXT("Update"),
		TEXT("\"SlotGroups\":[{\"Name\":\"DefaultGroup\",\"Slots\":[\"DefaultSlot\"]}]"),
		FString::Printf(TEXT("{\"CompatibleSkeletons\":[\"%s\"]}"), EngineSkeleton));
	if (!TestTrue(TEXT("the skeleton gets DefaultSlot"), Apply(*this, Skeleton, SkeletonEdit, TEXT("skeleton slots")).IsSuccess()))
	{
		return false;
	}
	const FTarget Target(TEXT("AM_Attack"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	auto Montage = [&](const TCHAR* Action, int32 StateTrack)
	{
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.AnimMontage\",\"Action\":\"%s\",")
			TEXT("\"Definitions\":{},\"Properties\":{},\"Body\":{\"Skeleton\":\"%s\",\"SlotAnimTracks\":[{\"SlotName\":\"DefaultSlot\",\"Segments\":[")
			TEXT("{\"AnimReference\":\"%s\",\"StartPos\":0.0,\"AnimStartTime\":0.0,\"AnimEndTime\":1.0,\"AnimPlayRate\":1.0,\"LoopingCount\":1}]}],")
			TEXT("\"CompositeSections\":[{\"Name\":\"Default\",\"NextSectionName\":null,\"LinkableTime\":0.0,\"SlotName\":\"DefaultSlot\",\"LinkMethod\":0}],")
			TEXT("\"Notifies\":[{\"Time\":0.55,\"TrackIndex\":0,\"Object\":{\"Class\":\"/Script/Engine.AnimNotify_PlaySound\",\"Properties\":{}}}],")
			TEXT("\"NotifyStates\":[{\"Time\":0.3,\"Duration\":0.35,\"TrackIndex\":%d,\"Object\":{\"Class\":\"/Script/Engine.AnimNotifyState_TimedParticleEffect\",\"Properties\":{}}}]}}"),
			*Target.PackageName, Action, *Skeleton.ObjectPath, IdleAnim, StateTrack);
	};
	// The sidecar file, as validate_asset_document / diff_asset_document read an AI's file_path.
	auto ValidateFile = [&](const FString& Json, const TCHAR* Stage)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
		FFileHelper::SaveStringToFile(Json, *Target.SidecarPath);
		FAssetDocumentValidateRequest Request;
		Request.FilePath = Target.SidecarPath;
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Validate(Request);
		LogResult(*this, Stage, Result);
		return Result;
	};
	// Validate and apply agree on a notify track the new Montage will not have.
	const FAssetDocumentResult BadTrack = ValidateFile(Montage(TEXT("Create"), 1), TEXT("validate track 1"));
	TestFalse(TEXT("validate rejects TrackIndex 1 on a new Montage"), BadTrack.IsSuccess());
	TestTrue(TEXT("validate names the missing track"), BadTrack.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
		{ return Diagnostic.Code == TEXT("AnimMontage.NotifyTrackMissing"); }));

	const FString Create = Montage(TEXT("Create"), 0);
	TestTrue(TEXT("validate accepts the one-shot Create"), ValidateFile(Create, TEXT("validate")).IsSuccess());
	// A failed content Create (after the notifies were staged and published) leaves the path reusable.
	{
		ON_SCOPE_EXIT { FAssetDocumentRuntimeTestHooks::Reset(); };
		FAssetDocumentRuntimeTestHooks::FailNextRequestAtPhase(EAssetDocumentRuntimeRequestPhase::Verify);
		const FAssetDocumentResult Failed = Apply(*this, Target, Create, TEXT("failed create"));
		TestFalse(TEXT("the injected Verify failure fails the Create"), Failed.IsSuccess());
		TestFalse(TEXT("failed create: no rollback residue"), Failed.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
			{ return Diagnostic.Code.StartsWith(TEXT("RollbackCreated.")) || Diagnostic.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
		TestNothingCreated(*this, Target, TEXT("failed create"));
	}
	if (!TestTrue(TEXT("the one-shot Create with notifies applies"), Apply(*this, Target, Create, TEXT("create")).IsSuccess()))
	{
		return true;
	}
	const UAnimMontage* Created = Target.Get<UAnimMontage>();
	TestTrue(TEXT("both notifies are on the Montage's first track"), Created != nullptr && Created->Notifies.Num() == 2
		&& Created->AnimNotifyTracks.Num() == 1 && Created->Notifies[0].TrackIndex == 0 && Created->Notifies[1].TrackIndex == 0);
	const TSharedPtr<FJsonObject> Extracted = Extract(*this, Target, TEXT("create"));
	const TArray<TSharedPtr<FJsonValue>>* States = nullptr;
	double Duration = 0.0;
	TestTrue(TEXT("the NotifyState Duration extracts as written (0.35)"), Extracted.IsValid()
		&& Extracted->GetObjectField(TEXT("Body"))->TryGetArrayField(TEXT("NotifyStates"), States) && States->Num() == 1
		&& (*States)[0]->AsObject()->TryGetNumberField(TEXT("Duration"), Duration) && Duration == 0.35);
	const FAssetDocumentResult Again = Apply(*this, Target, Montage(TEXT("CreateOrUpdate"), 0), TEXT("re-apply"));
	TestTrue(TEXT("re-applying the document succeeds"), Again.IsSuccess());
	TestTrue(TEXT("re-applying changes nothing"), Again.Message.Contains(TEXT("unchanged")));
	return true;
}

// delete_assets (a Montage could not be deleted, "may have references"): a Montage an apply just created and
// saved is deleted as the editor's Delete Assets dialog does; a deletion an in-memory user blocks names it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationAssetsDeleteAppliedMontageTest,
	"UEAssetDoc.AnimationAssets.DeleteAppliedMontage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationAssetsDeleteAppliedMontageTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimationAssetsTests;
	(void)Parameters;
	using namespace AssetDocumentAnimationAssetsTests;
	const FSkeletonCopy Skeleton;
	if (!TestNotNull(TEXT("skeleton copy is saved"), Skeleton.Get<USkeleton>()))
	{
		return false;
	}
	const FString SkeletonEdit = Document(Skeleton, TEXT("/Script/Engine.Skeleton"), TEXT("Update"),
		TEXT("\"SlotGroups\":[{\"Name\":\"DefaultGroup\",\"Slots\":[\"DefaultSlot\"]}]"),
		FString::Printf(TEXT("{\"CompatibleSkeletons\":[\"%s\"]}"), EngineSkeleton));
	if (!TestTrue(TEXT("the skeleton gets DefaultSlot"), Apply(*this, Skeleton, SkeletonEdit, TEXT("skeleton slots")).IsSuccess()))
	{
		return false;
	}
	const FTarget Target(TEXT("AM_TestA"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	const FString Create = FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.AnimMontage\",\"Action\":\"Create\",")
		TEXT("\"Definitions\":{},\"Properties\":{},\"Body\":{\"Skeleton\":\"%s\",\"SlotAnimTracks\":[{\"SlotName\":\"DefaultSlot\",\"Segments\":[")
		TEXT("{\"AnimReference\":\"%s\",\"StartPos\":0.0,\"AnimStartTime\":0.0,\"AnimEndTime\":1.0,\"AnimPlayRate\":1.0,\"LoopingCount\":1}]}],")
		TEXT("\"CompositeSections\":[{\"Name\":\"Default\",\"NextSectionName\":null,\"LinkableTime\":0.0,\"SlotName\":\"DefaultSlot\",\"LinkMethod\":0}],")
		TEXT("\"Notifies\":[{\"Time\":0.55,\"TrackIndex\":0,\"Object\":{\"Class\":\"/Script/Engine.AnimNotify_PlaySound\",\"Properties\":{}}}]}}"),
		*Target.PackageName, *Skeleton.ObjectPath, IdleAnim);
	if (!TestTrue(TEXT("the Montage is created and saved"), Apply(*this, Target, Create, TEXT("create")).IsSuccess()
		&& IFileManager::Get().FileExists(*Target.PackageFile)))
	{
		return true;
	}
	UObject* Montage = Target.Get();
	if (!TestNotNull(TEXT("the Montage is loaded"), Montage))
	{
		return true;
	}
	// What refused the plain deletion: the Montage's transient data controller (the apply edits through it) references
	// its data model.
	const FString Before = AssetDocumentAssetDelete::DescribeDeleteBlockers(AssetDocumentAssetDelete::FindDeleteBlockers(*Montage));
	AddInfo(FString::Printf(TEXT("before the delete: %s"), *Before));
	TestTrue(TEXT("the plain deletion is blocked by the data controller"), Before.Contains(TEXT("AnimDataController")));
	const FAssetDocumentDeleteOutcome Deleted = AssetDocumentAssetDelete::DeleteAssetAsEditor(*Montage);
	TestTrue(*FString::Printf(TEXT("the applied Montage is deleted (%s)"), *AssetDocumentAssetDelete::DescribeDeleteBlockers(Deleted.Blockers)),
		Deleted.bDeleted);
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestNull(TEXT("the Montage is gone"), Target.Get());
	TestFalse(TEXT("its file is gone"), IFileManager::Get().FileExists(*Target.PackageFile));
	TestFalse(TEXT("its registry entry is gone"), IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(Target.ObjectPath)).IsValid());

	// A user of the asset in memory (a material instance of a material) blocks the deletion, and is named.
	UPackage* ParentPackage = CreatePackage(*(FString(TEXT("/Game/")) + FolderName + TEXT("/M_DeleteParent_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UPackage* ChildPackage = CreatePackage(*(FString(TEXT("/Game/")) + FolderName + TEXT("/MI_DeleteChild_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UMaterial* Parent = NewObject<UMaterial>(ParentPackage, FName(*FPackageName::GetShortName(ParentPackage)), RF_Public | RF_Standalone);
	UMaterialInstanceConstant* Child = NewObject<UMaterialInstanceConstant>(ChildPackage, FName(*FPackageName::GetShortName(ChildPackage)),
		RF_Public | RF_Standalone);
	Child->Parent = Parent;
	TWeakObjectPtr<UMaterialInstanceConstant> WeakChild(Child);
	ON_SCOPE_EXIT
	{
		if (WeakChild.IsValid())
		{
			ObjectTools::DeleteObjects({WeakChild.Get()}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
		}
	};
	const FAssetDocumentDeleteOutcome Refused = AssetDocumentAssetDelete::DeleteAssetAsEditor(*Parent);
	TestFalse(TEXT("a referenced material is not deleted"), Refused.bDeleted);
	TestTrue(*FString::Printf(TEXT("the refusal names the material instance (%s)"), *AssetDocumentAssetDelete::DescribeDeleteBlockers(Refused.Blockers)),
		Refused.Blockers.ContainsByPredicate([&](const FAssetDocumentDeleteBlocker& Blocker)
		{
			return Blocker.Kind == TEXT("object") && Blocker.Referencer.Contains(Child->GetName());
		}));
	const FAssetDocumentDeleteOutcome Both = AssetDocumentAssetDelete::DeleteAssetAsEditor(*Child);
	TestTrue(TEXT("the material instance is deleted"), Both.bDeleted);
	TestTrue(TEXT("then the material is deleted"), AssetDocumentAssetDelete::DeleteAssetAsEditor(*Parent).bDeleted);
	return true;
}
#endif
