// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Editor-made Pose Assets and Skeletons whose content the documents carry: a Pose Asset created from an animation
// with curves reads each pose's curve values and bone offsets (read only) and its curve list (curves left out are
// deleted); a Pose Asset without a source animation adds poses from the reference pose. A Skeleton reads its curves'
// bone links, its asset user data other than the curve metadata object (kept by every apply), its preview mesh,
// notify names, preview attached assets and retarget sources.

#include "AssetDocumentTolerantTestSupport.h"

#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimCurveMetadata.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimSequence.h"
#include "Animation/PoseAsset.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "IEditableSkeleton.h"
#include "ISkeletonEditorModule.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentFidelityRigTestsLocal
{
using namespace AssetDocumentTolerantTestSupport;

const TCHAR* FidelityEngineSkeleton = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
const TCHAR* FidelityIdleAnim = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle");
const TCHAR* FidelityEngineMesh = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP");
const TCHAR* FidelityCube = TEXT("/Engine/BasicShapes/Cube.Cube");
const TCHAR* FidelityUserDataClass = TEXT("/Script/AssetDocumentTests.AssetDocumentSequenceLifecycleUserDataTest");

// The Content Browser's Duplicate of an engine asset into the target's package (the engine asset is never edited).
template <typename T>
T* FidelityCopyOf(const FTarget& Target, const TCHAR* SourcePath)
{
	T* Source = LoadObject<T>(nullptr, SourcePath);
	if (Source == nullptr)
	{
		return nullptr;
	}
	T* Copy = DuplicateObject<T>(Source, CreatePackage(*Target.PackageName), *Target.AssetName);
	Copy->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Copy);
	Copy->MarkPackageDirty();
	return Copy;
}

TSharedPtr<FJsonObject> FidelityBody(const TSharedPtr<FJsonObject>& Document)
{
	return Document->GetObjectField(TEXT("Body"));
}

TArray<FString> FidelityStrings(const TSharedPtr<FJsonObject>& Document, const FString& Pointer)
{
	TArray<FString> Names;
	const TSharedPtr<FJsonValue> Value = At(Document, Pointer);
	if (Value.IsValid() && Value->Type == EJson::Array)
	{
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			// A row is a name, or an object with Name.
			FString Name;
			const TSharedPtr<FJsonObject>* Row = nullptr;
			if (Item->TryGetObject(Row))
			{
				(*Row)->TryGetStringField(TEXT("Name"), Name);
			}
			else
			{
				Item->TryGetString(Name);
			}
			Names.Add(Name);
		}
	}
	return Names;
}

TArray<TSharedPtr<FJsonValue>> FidelityStringValues(const TArray<FString>& Names)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& Name : Names)
	{
		Values.Add(MakeShared<FJsonValueString>(Name));
	}
	return Values;
}

// The animation editor's curve track: a float curve with one key (the same value on every frame).
void FidelityAddCurve(UAnimSequence& Sequence, const TCHAR* Name, float Value)
{
	IAnimationDataController& Controller = Sequence.GetController();
	const FAnimationCurveIdentifier Identifier(FName(Name), ERawCurveTrackTypes::RCT_Float);
	Controller.AddCurve(Identifier, AACF_Editable, false);
	Controller.SetCurveKeys(Identifier, {FRichCurveKey(0.f, Value)}, false);
}

// The index of the Body.Curves row named Name (INDEX_NONE: none).
int32 FidelityCurveIndex(const TSharedPtr<FJsonObject>& Document, const TCHAR* Name)
{
	const TArray<TSharedPtr<FJsonValue>>& Rows = FidelityBody(Document)->GetArrayField(TEXT("Curves"));
	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		if (Rows[Index]->AsObject()->GetStringField(TEXT("Name")) == Name)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

TArray<FName> FidelityLinkedBones(const USkeleton& Skeleton, FName Curve)
{
	TArray<FName> Names;
	if (const FCurveMetaData* MetaData = Skeleton.GetCurveMetaData(Curve))
	{
		for (const FBoneReference& Link : MetaData->LinkedBones)
		{
			Names.Add(Link.BoneName);
		}
	}
	return Names;
}

// The Anim Curves tab's bone links (SAnimCurveMetadataEditor::ApplyCurveBoneLinks).
void FidelitySetBoneLinks(USkeleton& Skeleton, FName Curve, const TArray<FName>& Bones, uint8 MaxLOD)
{
	TArray<FBoneReference> Links;
	for (const FName Bone : Bones)
	{
		Links.Add(FBoneReference(Bone));
	}
	static_cast<IInterface_AnimCurveMetaData*>(Skeleton.GetAssetUserData<UAnimCurveMetaData>())->SetCurveMetaDataBoneLinks(Curve, Links, MaxLOD, &Skeleton);
}

TSharedRef<IEditableSkeleton> FidelityEditable(USkeleton& Skeleton)
{
	return FModuleManager::LoadModuleChecked<ISkeletonEditorModule>(TEXT("SkeletonEditor")).CreateEditableSkeleton(&Skeleton);
}

int32 FidelityIntField(const UObject& Object, const TCHAR* Name)
{
	const FIntProperty* Property = FindFProperty<FIntProperty>(Object.GetClass(), Name);
	return Property != nullptr ? Property->GetPropertyValue_InContainer(&Object) : INDEX_NONE;
}

// The Create Pose Asset action on Sequence: one pose per frame.
UPoseAsset* FidelityPoseAssetFrom(const FTarget& Target, UAnimSequence& Sequence)
{
	UPoseAsset* PoseAsset = Target.Create<UPoseAsset>();
	PoseAsset->SetSkeleton(Sequence.GetSkeleton());
	PoseAsset->SourceAnimation = &Sequence;
	PoseAsset->CreatePoseFromAnimation(&Sequence);
	return PoseAsset;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityPoseAssetPoseDataTest, "UEAssetDoc.Fidelity.PoseAsset.PoseData",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelityPoseAssetPoseDataTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentFidelityRigTestsLocal;
	(void)Parameters;
	const FTarget SequenceTarget(TEXT("PA_Fidelity_Source"));
	const FTarget Target(TEXT("PA_Fidelity_Data"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); SequenceTarget.Cleanup(); };

	UAnimSequence* Sequence = FidelityCopyOf<UAnimSequence>(SequenceTarget, FidelityIdleAnim);
	if (!TestNotNull(TEXT("sequence copy"), Sequence))
	{
		return false;
	}
	FidelityAddCurve(*Sequence, TEXT("Smile"), 0.75f);
	FidelityAddCurve(*Sequence, TEXT("Blink"), 0.f);
	FidelityAddCurve(*Sequence, TEXT("Frown"), 0.4f);
	UPoseAsset* PoseAsset = FidelityPoseAssetFrom(Target, *Sequence);
	if (!TestTrue(TEXT("sequence saved"), SequenceTarget.Save()) || !TestTrue(TEXT("pose asset saved"), Target.Save())
		|| !TestTrue(TEXT("poses made"), PoseAsset->GetNumPoses() > 0))
	{
		return false;
	}

	// The saved pose data reads per pose; the extract re-applies as a no-op.
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Pose data"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonValue> Smile = At(Extracted, TEXT("/Body/Poses/0/Curves/Smile"));
	TestTrue(TEXT("the pose's curve value"), Smile.IsValid() && FMath::IsNearlyEqual(Smile->AsNumber(), 0.75, 1e-4));
	TestFalse(TEXT("a curve at 0 is left out"), At(Extracted, TEXT("/Body/Poses/0/Curves/Blink")).IsValid());
	bool bAnyBones = false;
	for (const TSharedPtr<FJsonValue>& Pose : FidelityBody(Extracted)->GetArrayField(TEXT("Poses")))
	{
		const TSharedPtr<FJsonObject>* Row = nullptr;
		const TSharedPtr<FJsonObject>* Bones = nullptr;
		bAnyBones |= Pose->TryGetObject(Row) && (*Row)->TryGetObjectField(TEXT("Bones"), Bones) && (*Bones)->Values.Num() > 0;
	}
	TestTrue(TEXT("bone offsets from the reference pose"), bAnyBones);
	TArray<FString> Curves = FidelityStrings(Extracted, TEXT("/Body/Curves"));
	Curves.Sort();
	TestTrue(TEXT("the curve list"), Curves == TArray<FString>({TEXT("Blink"), TEXT("Frown"), TEXT("Smile")}));

	// Pose data is read only.
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		At(Document, TEXT("/Body/Poses/0/Curves"))->AsObject()->SetNumberField(TEXT("Smile"), 0.25);
	}, TEXT("Changed curve value")), TEXT("PoseAsset.PoseDataReadOnly"), TEXT("/Body/Poses/0/Curves"), TEXT("Changed curve value"));
	// Curves come from the source animation.
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		TArray<TSharedPtr<FJsonValue>> Values = FidelityBody(Document)->GetArrayField(TEXT("Curves"));
		Values.Add(MakeShared<FJsonValueString>(TEXT("Nope")));
		FidelityBody(Document)->SetArrayField(TEXT("Curves"), Values);
	}, TEXT("New curve")), TEXT("PoseAsset.CurveAddUnsupported"), TEXT("/Body/Curves/3"), TEXT("New curve"));

	// Delete Curves: the list without Frown (the poses' rows still carry it, as extracted).
	const FAssetDocumentResult Deleted = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		TArray<FString> Kept = FidelityStrings(Document, TEXT("/Body/Curves"));
		Kept.Remove(TEXT("Frown"));
		FidelityBody(Document)->SetArrayField(TEXT("Curves"), FidelityStringValues(Kept));
	}, TEXT("Delete curve"));
	if (TestTrue(TEXT("Delete curve applied"), Deleted.IsSuccess()))
	{
		PoseAsset = Target.Get<UPoseAsset>();
		if (TestNotNull(TEXT("pose asset"), PoseAsset))
		{
			const TArray<FName> Names = PoseAsset->GetCurveFNames();
			TestFalse(TEXT("Frown deleted"), Names.Contains(FName(TEXT("Frown"))));
			const int32 SmileIndex = Names.IndexOfByKey(FName(TEXT("Smile")));
			float Value = 0.f;
			TestTrue(TEXT("Smile kept with its value"), SmileIndex != INDEX_NONE && PoseAsset->GetCurveValue(0, SmileIndex, Value)
				&& FMath::IsNearlyEqual(Value, 0.75f, 1e-4f));
		}
		const FAssetDocumentResult After = ExtractResult(Target);
		TestFalse(TEXT("Frown no longer read"), After.Payload.IsValid() && At(After.Payload, TEXT("/Body/Poses/0/Curves/Frown")).IsValid());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityPoseAssetAddPoseTest, "UEAssetDoc.Fidelity.PoseAsset.AddPose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelityPoseAssetAddPoseTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentFidelityRigTestsLocal;
	(void)Parameters;
	const FTarget Target(TEXT("PA_Fidelity_NoSource"));
	const FTarget Sourced(TEXT("PA_Fidelity_Sourced"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); Sourced.Cleanup(); };

	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, FidelityEngineSkeleton);
	UAnimSequence* Idle = LoadObject<UAnimSequence>(nullptr, FidelityIdleAnim);
	if (!TestNotNull(TEXT("skeleton"), Skeleton) || !TestNotNull(TEXT("idle"), Idle))
	{
		return false;
	}
	// A Pose Asset without a source animation: Add Pose from Reference.
	UPoseAsset* PoseAsset = Target.Create<UPoseAsset>();
	PoseAsset->SetSkeleton(Skeleton);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}
	const FAssetDocumentResult Added = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		FidelityBody(Document)->SetArrayField(TEXT("Poses"), FidelityStringValues({TEXT("Open"), TEXT("Closed")}));
	}, TEXT("Add poses"));
	if (TestTrue(TEXT("Add poses applied"), Added.IsSuccess()))
	{
		PoseAsset = Target.Get<UPoseAsset>();
		if (TestNotNull(TEXT("pose asset"), PoseAsset))
		{
			TestEqual(TEXT("two poses"), PoseAsset->GetNumPoses(), 2);
			TestEqual(TEXT("first pose"), PoseAsset->GetPoseNameByIndex(0), FName(TEXT("Open")));
			TestEqual(TEXT("second pose"), PoseAsset->GetPoseNameByIndex(1), FName(TEXT("Closed")));
		}
		TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("After add"));
	}
	// A new pose goes after the existing ones.
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		FidelityBody(Document)->SetArrayField(TEXT("Poses"), FidelityStringValues({TEXT("First"), TEXT("Open"), TEXT("Closed")}));
	}, TEXT("Add before")), TEXT("PoseAsset.PoseOrderUnsupported"), TEXT("/Body/Poses"), TEXT("Add before"));

	// A Pose Asset with a source animation: clearing the Source Animation row keeps the poses and allows Add Pose.
	UPoseAsset* SourcedAsset = FidelityPoseAssetFrom(Sourced, *Idle);
	if (!TestTrue(TEXT("sourced saved"), Sourced.Save()))
	{
		return false;
	}
	const int32 SourcePoses = SourcedAsset->GetNumPoses();
	TestRefused(*this, ApplyEdited(*this, Sourced, [](const TSharedPtr<FJsonObject>& Document)
	{
		TArray<FString> Names = FidelityStrings(Document, TEXT("/Body/Poses"));
		Names.Add(TEXT("Extra"));
		FidelityBody(Document)->SetArrayField(TEXT("Poses"), FidelityStringValues(Names));
	}, TEXT("Add with source")), TEXT("PoseAsset.PoseAddUnsupported"), FString::Printf(TEXT("/Body/Poses/%d"), SourcePoses), TEXT("Add with source"));
	const FAssetDocumentResult Cleared = ApplyEdited(*this, Sourced, [](const TSharedPtr<FJsonObject>& Document)
	{
		TArray<TSharedPtr<FJsonValue>> Rows = FidelityBody(Document)->GetArrayField(TEXT("Poses"));
		Rows.Add(MakeShared<FJsonValueString>(TEXT("Extra")));
		FidelityBody(Document)->SetArrayField(TEXT("Poses"), Rows);
		FidelityBody(Document)->SetStringField(TEXT("SourceAnimation"), FString());
	}, TEXT("Clear source and add"));
	if (TestTrue(TEXT("Clear source and add applied"), Cleared.IsSuccess()))
	{
		SourcedAsset = Sourced.Get<UPoseAsset>();
		if (TestNotNull(TEXT("sourced asset"), SourcedAsset))
		{
			TestNull(TEXT("no source animation"), SourcedAsset->SourceAnimation.Get());
			TestEqual(TEXT("one pose added"), SourcedAsset->GetNumPoses(), SourcePoses + 1);
			TestEqual(TEXT("added at the end"), SourcedAsset->GetPoseNameByIndex(SourcePoses), FName(TEXT("Extra")));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelitySkeletonCurveBoneLinksTest, "UEAssetDoc.Fidelity.Skeleton.CurveBoneLinks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelitySkeletonCurveBoneLinksTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentFidelityRigTestsLocal;
	(void)Parameters;
	const FTarget Target(TEXT("SK_Fidelity_Links"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	USkeleton* Skeleton = FidelityCopyOf<USkeleton>(Target, FidelityEngineSkeleton);
	if (!TestNotNull(TEXT("skeleton copy"), Skeleton))
	{
		return false;
	}
	Skeleton->AddCurveMetaData(TEXT("Blink"), false);
	FidelitySetBoneLinks(*Skeleton, TEXT("Blink"), {TEXT("head")}, 1);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Bone links"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	const int32 Blink = FidelityCurveIndex(Extracted, TEXT("Blink"));
	if (!TestTrue(TEXT("the curve is read"), Blink != INDEX_NONE))
	{
		return false;
	}
	TestTrue(TEXT("LinkedBones read"), FidelityStrings(Extracted, FString::Printf(TEXT("/Body/Curves/%d/LinkedBones"), Blink))
		== TArray<FString>({TEXT("head")}));
	const TSharedPtr<FJsonValue> MaxLOD = At(Extracted, FString::Printf(TEXT("/Body/Curves/%d/MaxLOD"), Blink));
	TestTrue(TEXT("MaxLOD read"), MaxLOD.IsValid() && FMath::IsNearlyEqual(MaxLOD->AsNumber(), 1.0));

	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [Blink](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Row = At(Document, FString::Printf(TEXT("/Body/Curves/%d"), Blink))->AsObject();
		Row->SetArrayField(TEXT("LinkedBones"), FidelityStringValues({TEXT("neck_01"), TEXT("head")}));
		Row->SetNumberField(TEXT("MaxLOD"), 255);
		const TSharedPtr<FJsonObject> Added = MakeShared<FJsonObject>();
		Added->SetStringField(TEXT("Name"), TEXT("Smile"));
		Added->SetArrayField(TEXT("LinkedBones"), FidelityStringValues({TEXT("head")}));
		TArray<TSharedPtr<FJsonValue>> Rows = FidelityBody(Document)->GetArrayField(TEXT("Curves"));
		Rows.Add(MakeShared<FJsonValueObject>(Added));
		FidelityBody(Document)->SetArrayField(TEXT("Curves"), Rows);
	}, TEXT("Edit bone links"));
	if (TestTrue(TEXT("Edit bone links applied"), Edited.IsSuccess()))
	{
		Skeleton = Target.Get<USkeleton>();
		if (TestNotNull(TEXT("skeleton"), Skeleton))
		{
			TestTrue(TEXT("new links"), FidelityLinkedBones(*Skeleton, TEXT("Blink")) == TArray<FName>({TEXT("neck_01"), TEXT("head")}));
			const FCurveMetaData* MetaData = Skeleton->GetCurveMetaData(TEXT("Blink"));
			TestTrue(TEXT("every LOD"), MetaData != nullptr && MetaData->MaxLOD == 0xFF);
			TestTrue(TEXT("new curve with links"), FidelityLinkedBones(*Skeleton, TEXT("Smile")) == TArray<FName>({TEXT("head")}));
		}
	}
	TestRefused(*this, ApplyEdited(*this, Target, [Blink](const TSharedPtr<FJsonObject>& Document)
	{
		At(Document, FString::Printf(TEXT("/Body/Curves/%d"), Blink))->AsObject()->SetArrayField(TEXT("LinkedBones"),
			FidelityStringValues({TEXT("no_such_bone")}));
	}, TEXT("Unknown bone")), TEXT("Skeleton.BoneNotFound"), FString::Printf(TEXT("/Body/Curves/%d/LinkedBones/0"), Blink), TEXT("Unknown bone"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelitySkeletonAssetUserDataTest, "UEAssetDoc.Fidelity.Skeleton.AssetUserData",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelitySkeletonAssetUserDataTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentFidelityRigTestsLocal;
	(void)Parameters;
	const FTarget Target(TEXT("SK_Fidelity_UserData"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	USkeleton* Skeleton = FidelityCopyOf<USkeleton>(Target, FidelityEngineSkeleton);
	UClass* UserDataClass = FindObject<UClass>(nullptr, FidelityUserDataClass);
	if (!TestNotNull(TEXT("skeleton copy"), Skeleton) || !TestNotNull(TEXT("user data class"), UserDataClass))
	{
		return false;
	}
	// The Details panel's Asset User Data array: a project element first, then the curve metadata object (made by the
	// first curve added afterwards).
	Skeleton->Modify();
	Skeleton->RemoveUserDataOfClass(UAnimCurveMetaData::StaticClass());
	UAssetUserData* UserData = NewObject<UAssetUserData>(Skeleton, UserDataClass, NAME_None, RF_Transactional);
	FindFProperty<FIntProperty>(UserDataClass, TEXT("First"))->SetPropertyValue_InContainer(UserData, 42);
	Skeleton->AddAssetUserData(UserData);
	Skeleton->AddCurveMetaData(TEXT("Blink"), false);
	const TWeakObjectPtr<UAnimCurveMetaData> CurveMetaData = Skeleton->GetAssetUserData<UAnimCurveMetaData>();
	if (!TestTrue(TEXT("curve metadata second"), CurveMetaData.IsValid() && Skeleton->GetAssetUserDataArray()->Num() == 2
		&& (*Skeleton->GetAssetUserDataArray())[1] == CurveMetaData.Get()) || !TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Asset user data"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonValue> Rows = At(Extracted, TEXT("/Properties/AssetUserData"));
	TestTrue(TEXT("only the project element is read"), Rows.IsValid() && Rows->Type == EJson::Array && Rows->AsArray().Num() == 1
		&& StringAt(Extracted, TEXT("/Properties/AssetUserData/0/Class")) == UserDataClass->GetPathName());

	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Element = At(Document, TEXT("/Properties/AssetUserData/0"))->AsObject();
		const TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetNumberField(TEXT("First"), 5);
		Element->SetObjectField(TEXT("Properties"), Properties);
	}, TEXT("Edit user data"));
	if (TestTrue(TEXT("Edit user data applied"), Edited.IsSuccess()))
	{
		Skeleton = Target.Get<USkeleton>();
		const UAssetUserData* Edit = Skeleton != nullptr ? Skeleton->GetAssetUserDataOfClass(UserDataClass) : nullptr;
		TestTrue(TEXT("user data written"), Edit != nullptr && FidelityIntField(*Edit, TEXT("First")) == 5);
		TestTrue(TEXT("curve metadata object kept"), Skeleton != nullptr && CurveMetaData.IsValid()
			&& Skeleton->GetAssetUserData<UAnimCurveMetaData>() == CurveMetaData.Get() && Skeleton->GetCurveMetaData(TEXT("Blink")) != nullptr);
		// The same user data again: the curve metadata object is not moved aside, so the package stays clean.
		const TSharedPtr<FJsonObject> Again = ExtractResult(Target).Payload;
		if (TestTrue(TEXT("extract after the edit"), Again.IsValid()))
		{
			TestAppliesUnchanged(*this, Target, Again, TEXT("Unchanged user data"));
			UObject* Asset = Target.Get();
			TestTrue(TEXT("unchanged user data leaves the package clean"), Asset != nullptr && !Asset->GetOutermost()->IsDirty());
		}
	}
	const FAssetDocumentResult Cleared = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		Document->GetObjectField(TEXT("Properties"))->SetArrayField(TEXT("AssetUserData"), TArray<TSharedPtr<FJsonValue>>());
	}, TEXT("Remove user data"));
	if (TestTrue(TEXT("Remove user data applied"), Cleared.IsSuccess()))
	{
		Skeleton = Target.Get<USkeleton>();
		if (TestNotNull(TEXT("skeleton"), Skeleton))
		{
			TestNull(TEXT("project element removed"), Skeleton->GetAssetUserDataOfClass(UserDataClass));
			TestTrue(TEXT("curve metadata kept"), Skeleton->GetCurveMetaData(TEXT("Blink")) != nullptr);
		}
	}
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Element = MakeShared<FJsonObject>();
		Element->SetStringField(TEXT("Class"), TEXT("/Script/Engine.AnimCurveMetaData"));
		TArray<TSharedPtr<FJsonValue>> Elements;
		Elements.Add(MakeShared<FJsonValueObject>(Element));
		Document->GetObjectField(TEXT("Properties"))->SetArrayField(TEXT("AssetUserData"), Elements);
	}, TEXT("Curve metadata element")), TEXT("Skeleton.BodyOwnedProperty"), TEXT("/Properties/AssetUserData/0"), TEXT("Curve metadata element"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelitySkeletonPreviewAndRegistriesTest, "UEAssetDoc.Fidelity.Skeleton.PreviewAndRegistries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelitySkeletonPreviewAndRegistriesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentFidelityRigTestsLocal;
	(void)Parameters;
	const FTarget Target(TEXT("SK_Fidelity_Preview"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	USkeleton* Skeleton = FidelityCopyOf<USkeleton>(Target, FidelityEngineSkeleton);
	USkeleton* EngineSkeletonAsset = LoadObject<USkeleton>(nullptr, FidelityEngineSkeleton);
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, FidelityEngineMesh);
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, FidelityCube);
	if (!TestNotNull(TEXT("skeleton copy"), Skeleton) || !TestNotNull(TEXT("mesh"), Mesh) || !TestNotNull(TEXT("cube"), Cube))
	{
		return false;
	}
	// The copy accepts the engine skeleton's meshes (Compatible Skeletons), a retarget source from the Retarget
	// Sources tab, a notify from the Animation Notifies tab and a preview asset attached to a bone in the viewport.
	Skeleton->Modify();
	Skeleton->AddCompatibleSkeleton(EngineSkeletonAsset);
	TSharedRef<IEditableSkeleton> Editable = FidelityEditable(*Skeleton);
	Editable->AddRetargetSource(TEXT("Tall"), Mesh);
	Editable->AddNotify(TEXT("Footstep"));
	Skeleton->PreviewAttachedAssetContainer.AddUniqueAttachedObject(Cube, TEXT("head"));
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Preview and registries"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("retarget source"), StringAt(Extracted, TEXT("/Body/RetargetSources/0/Name")), FString(TEXT("Tall")));
	TestEqual(TEXT("retarget source mesh"), StringAt(Extracted, TEXT("/Body/RetargetSources/0/Mesh")), FString(FidelityEngineMesh));
	TestTrue(TEXT("notify name"), FidelityStrings(Extracted, TEXT("/Body/AnimNotifyNames")).Contains(TEXT("Footstep")));
	TestEqual(TEXT("attached asset"), StringAt(Extracted, TEXT("/Body/PreviewAttachedAssets/0/Asset")), FString(FidelityCube));
	TestEqual(TEXT("attached to"), StringAt(Extracted, TEXT("/Body/PreviewAttachedAssets/0/AttachedTo")), FString(TEXT("head")));
	TestTrue(TEXT("no preview mesh"), At(Extracted, TEXT("/Body/PreviewMesh")).IsValid() && StringAt(Extracted, TEXT("/Body/PreviewMesh")).IsEmpty());

	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		FidelityBody(Document)->SetStringField(TEXT("PreviewMesh"), FidelityEngineMesh);
		TArray<FString> Names = FidelityStrings(Document, TEXT("/Body/AnimNotifyNames"));
		Names.Add(TEXT("Jump"));
		FidelityBody(Document)->SetArrayField(TEXT("AnimNotifyNames"), FidelityStringValues(Names));
	}, TEXT("Preview mesh and notify"));
	if (TestTrue(TEXT("Preview mesh and notify applied"), Edited.IsSuccess()))
	{
		Skeleton = Target.Get<USkeleton>();
		if (TestNotNull(TEXT("skeleton"), Skeleton))
		{
			TestEqual(TEXT("preview mesh"), static_cast<const IInterface_PreviewMeshProvider*>(Skeleton)->GetPreviewMesh(), Mesh);
			TestTrue(TEXT("new notify"), Skeleton->AnimationNotifies.Contains(FName(TEXT("Jump"))));
			TestTrue(TEXT("old notify kept"), Skeleton->AnimationNotifies.Contains(FName(TEXT("Footstep"))));
		}
	}
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		SetStringAt(Document, TEXT("/Body/RetargetSources/0/Name"), TEXT("Short"));
	}, TEXT("Retarget source")), TEXT("Skeleton.RetargetSourcesReadOnly"), TEXT("/Body/RetargetSources"), TEXT("Retarget source"));
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		FidelityBody(Document)->SetArrayField(TEXT("PreviewAttachedAssets"), TArray<TSharedPtr<FJsonValue>>());
	}, TEXT("Attached assets")), TEXT("Skeleton.PreviewAttachedAssetsReadOnly"), TEXT("/Body/PreviewAttachedAssets"), TEXT("Attached assets"));
	return !HasAnyErrors();
}

#endif
