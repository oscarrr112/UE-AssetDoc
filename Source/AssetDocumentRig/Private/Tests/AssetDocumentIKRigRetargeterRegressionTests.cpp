// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Retargeter and IK Rig regressions:
// UEAssetDoc.IKRig.RetargeterRootMotionBones: a retargeter created with only its IK Rigs (the
//   factory's default op stack, whose Root Motion op was added before any preview mesh) takes the Root Motion op's
//   SourceRoot / TargetRoot / TargetPelvis; the values equal what a new op would fill in, so the sparse read omits
//   them, and verification must still find them equal (it failed with GenericRuntime.VerificationMismatch). A
//   setting written at its default value verifies too.
// UEAssetDoc.IKRig.RetargeterRootMotionPicks: the Root Motion op's SourceRoot / TargetRoot / TargetPelvis picked in
//   the op Details (bones other than the ones a new op fills in) read in the extract, which re-applies as a no-op;
//   the same settings written by a document into another retargeter are stored, survive save and reload, and verify.
// UEAssetDoc.IKRig.RetargeterProfileActive: a retarget profile stored in the asset and made current (an override of the
//   Root Motion op's settings, Force All IK Off) makes Extract warn IKRetargeter.ProfileActive at /Body; Inspect lists
//   the profile with its overrides; applying the extract keeps the profile and the current profile.
// UEAssetDoc.IKRig.ValidateLegacyMeshFirstLoad: the first validate that loads a never-loaded legacy
//   skeletal mesh (the UE4 tutorial mannequin, a copy under /Game so it is not in memory yet) passes: the mesh's
//   load-time upgrade (AssetImportData, mesh descriptions, the deferred post-load build) is not a change the preflight
//   made (it failed with PreflightIsolation.PackageNotRestored).

#include "AssetDocumentIKRigTestSupport.h"
#include "AssetDocumentTolerantTestSupport.h"

#include "Retargeter/IKRetargetOps.h"
#include "Retargeter/IKRetargetProfile.h"
#include "Retargeter/RetargetOps/RootMotionGeneratorOp.h"
#include "StructUtils/InstancedStruct.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentIKRigRetargeterRegressionTests
{
using namespace AssetDocumentIKRigTests;

TSharedPtr<FJsonObject> MakeTrialDocument(const TCHAR* Class, const FTarget& Target, const TCHAR* Action, const FString& Body)
{
	return Parse(FString::Printf(TEXT(R"({"SchemaVersion": 1, "Class": "%s", "Action": "%s", "Target": "%s", "Body": %s})"),
		Class, Action, *Target.PackageName, *Body));
}

TSharedPtr<FJsonObject> OpOfType(const TSharedPtr<FJsonObject>& Body, const TCHAR* Type)
{
	const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
	if (Body.IsValid() && Body->TryGetArrayField(TEXT("Ops"), Ops))
	{
		for (const TSharedPtr<FJsonValue>& Op : *Ops)
		{
			const TSharedPtr<FJsonObject> Object = Op->AsObject();
			if (Object.IsValid() && Object->GetStringField(TEXT("Type")) == Type)
			{
				return Object;
			}
		}
	}
	return nullptr;
}

const FIKRetargetRootMotionOp* FindRootMotion(const UIKRetargeter& Retargeter)
{
	for (const FInstancedStruct& Op : Retargeter.GetRetargetOps())
	{
		if (const FIKRetargetRootMotionOp* RootMotion = Op.GetPtr<FIKRetargetRootMotionOp>())
		{
			return RootMotion;
		}
	}
	return nullptr;
}

int32 FindRootMotionIndex(const UIKRetargeter& Retargeter)
{
	const TArray<FInstancedStruct>& Ops = Retargeter.GetRetargetOps();
	for (int32 Index = 0; Index < Ops.Num(); ++Index)
	{
		if (Ops[Index].GetPtr<FIKRetargetRootMotionOp>() != nullptr)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

// The op Details' bone pickers (UIKRigStructViewer): Modify the retargeter, write the bone, re-initialize the op.
bool PickRootMotionBones(FAutomationTestBase& Test, UIKRetargeter& Retargeter, FName SourceRoot, FName TargetRoot, FName TargetPelvis)
{
	UIKRetargeterController* Controller = UIKRetargeterController::GetController(&Retargeter);
	const int32 Index = FindRootMotionIndex(Retargeter);
	if (!Test.TestTrue(TEXT("the retargeter has a Root Motion op"), Controller != nullptr && Index != INDEX_NONE))
	{
		return false;
	}
	FIKRetargetRootMotionOp* Op = Controller->GetRetargetOpStructAtIndex(Index)->GetMutablePtr<FIKRetargetRootMotionOp>();
	const TPair<FName, FName> Picks[] = {{TEXT("SourceRoot"), SourceRoot}, {TEXT("TargetRoot"), TargetRoot}, {TEXT("TargetPelvis"), TargetPelvis}};
	for (const TPair<FName, FName>& Pick : Picks)
	{
		FStructProperty* Property = CastField<FStructProperty>(FIKRetargetRootMotionOpSettings::StaticStruct()->FindPropertyByName(Pick.Key));
		if (!Test.TestNotNull(*FString::Printf(TEXT("settings field %s"), *Pick.Key.ToString()), Property))
		{
			return false;
		}
		Retargeter.Modify();
		Property->ContainerPtrToValuePtr<FBoneReference>(&Op->Settings)->BoneName = Pick.Value;
		const FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		Controller->OnOpPropertyChanged(Op->GetName(), Event);
	}
	return true;
}

// The Root Motion op's stored bones after a reload from disk: "SourceRoot,TargetRoot,TargetPelvis".
FString StoredRootMotionBones(const FTarget& Target)
{
	const UIKRetargeter* Retargeter = Cast<UIKRetargeter>(Reload(Target));
	const FIKRetargetRootMotionOp* Op = Retargeter != nullptr ? FindRootMotion(*Retargeter) : nullptr;
	if (Op == nullptr)
	{
		return FString();
	}
	return FString::Printf(TEXT("%s,%s,%s"), *Op->Settings.SourceRoot.BoneName.ToString(), *Op->Settings.TargetRoot.BoneName.ToString(),
		*Op->Settings.TargetPelvis.BoneName.ToString());
}

// The retargeter's stored profiles and current profile (no editor UI writes them in 5.7; reflection stands in).
TMap<FName, FRetargetProfile>* StoredProfiles(UIKRetargeter& Retargeter)
{
	const FMapProperty* Map = CastField<FMapProperty>(UIKRetargeter::StaticClass()->FindPropertyByName(TEXT("Profiles")));
	return Map != nullptr ? Map->ContainerPtrToValuePtr<TMap<FName, FRetargetProfile>>(&Retargeter) : nullptr;
}

FName* StoredCurrentProfile(UIKRetargeter& Retargeter)
{
	const FNameProperty* Current = CastField<FNameProperty>(UIKRetargeter::StaticClass()->FindPropertyByName(TEXT("CurrentProfile")));
	return Current != nullptr ? Current->ContainerPtrToValuePtr<FName>(&Retargeter) : nullptr;
}

AssetDocumentTolerantTestSupport::FTarget View(const FTarget& Target)
{
	AssetDocumentTolerantTestSupport::FTarget Out(TEXT("Unused"));
	Out.AssetName = Target.AssetName;
	Out.PackageName = Target.PackageName;
	Out.ObjectPath = Target.ObjectPath;
	Out.PackageFile = Target.PackageFile;
	Out.SidecarPath = Target.SidecarPath;
	return Out;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRetargeterRootMotionPicksTest, "UEAssetDoc.IKRig.RetargeterRootMotionPicks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRetargeterRootMotionPicksTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRigRetargeterRegressionTests;
	FFixtures Fixtures;
	const FTarget& SourceRig = Fixtures.Add(TEXT("IK_RootPickSource"));
	const FTarget& TargetRig = Fixtures.Add(TEXT("IK_RootPickTarget"));
	UIKRigDefinition* Source = MakeRig(*this, SourceRig, TutorialMesh, true);
	UIKRigDefinition* TargetDefinition = MakeRig(*this, TargetRig, TutorialMesh, true);
	const FTarget& Picked = Fixtures.Add(TEXT("RTG_RootPicked"));
	const FTarget& Copied = Fixtures.Add(TEXT("RTG_RootCopied"));
	UIKRetargeter* PickedAsset = Source != nullptr && TargetDefinition != nullptr ? MakeRetargeter(*this, Picked, Source, TargetDefinition) : nullptr;
	UIKRetargeter* CopiedAsset = PickedAsset != nullptr ? MakeRetargeter(*this, Copied, Source, TargetDefinition) : nullptr;
	if (CopiedAsset == nullptr)
	{
		return false;
	}
	// Bones other than the ones a new op fills in (the root and the target rig's retarget root).
	const FName Pelvis(TEXT("pelvis"));
	const FName Spine(TEXT("spine_01"));
	if (!PickRootMotionBones(*this, *PickedAsset, Pelvis, Pelvis, Spine) || !TestTrue(TEXT("picked retargeter saved"), Save(*PickedAsset)))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = AssetDocumentTolerantTestSupport::TestTolerantRoundTrip(*this, View(Picked),
		AssetDocumentTolerantTestSupport::FExpected(), TEXT("Picked bones"));
	const TSharedPtr<FJsonObject> PickedOp = Extracted.IsValid() ? OpOfType(BodyOf(Extracted), TEXT("RootMotion")) : nullptr;
	const TSharedPtr<FJsonObject>* PickedSettings = nullptr;
	if (!TestTrue(TEXT("the extract lists the picked bones"), PickedOp.IsValid() && PickedOp->TryGetObjectField(TEXT("Settings"), PickedSettings)))
	{
		return false;
	}
	TestEqual(TEXT("SourceRoot read"), Serialize((*PickedSettings)->GetObjectField(TEXT("SourceRoot"))), FString(TEXT(R"({"BoneName":"pelvis"})")));
	TestEqual(TEXT("TargetRoot read"), Serialize((*PickedSettings)->GetObjectField(TEXT("TargetRoot"))), FString(TEXT(R"({"BoneName":"pelvis"})")));
	TestEqual(TEXT("TargetPelvis read"), Serialize((*PickedSettings)->GetObjectField(TEXT("TargetPelvis"))), FString(TEXT(R"({"BoneName":"spine_01"})")));
	TestEqual(TEXT("the picks survive a reload"), StoredRootMotionBones(Picked), FString(TEXT("pelvis,pelvis,spine_01")));

	// The same settings written into the other retargeter by its document.
	const TSharedPtr<FJsonObject> Other = Extract(*this, Copied.ObjectPath, TEXT("copy target"));
	const TSharedPtr<FJsonObject> OtherOp = OpOfType(BodyOf(Other), TEXT("RootMotion"));
	if (!TestTrue(TEXT("the other retargeter has a Root Motion op"), OtherOp.IsValid()))
	{
		return false;
	}
	OtherOp->SetObjectField(TEXT("Settings"), Parse(Serialize(*PickedSettings)));
	const TSharedPtr<FJsonObject> Edited = MakeTrialDocument(RetargeterClass, Copied, TEXT("Update"), Serialize(BodyOf(Other)));
	const FAssetDocumentResult Applied = Apply(Copied, Edited);
	if (!TestTrue(*FString::Printf(TEXT("the picks apply (%s)"), *DiagnosticsText(Applied)), Applied.IsSuccess()))
	{
		return false;
	}
	TestTrue(TEXT("and are saved"), Applied.bSavedAsset);
	FString DiffText;
	if (!TestEqual(TEXT("no differences from the document"), DiffCount(Edited, DiffText), 0))
	{
		AddInfo(DiffText.Left(1500));
	}
	const FAssetDocumentResult Again = Apply(Copied, Edited);
	TestTrue(*FString::Printf(TEXT("re-apply succeeds (%s)"), *DiagnosticsText(Again)), Again.IsSuccess());
	TestFalse(TEXT("re-apply is a no-op"), Again.bSavedAsset);
	TestEqual(TEXT("the written bones survive a reload"), StoredRootMotionBones(Copied), FString(TEXT("pelvis,pelvis,spine_01")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRetargeterProfileActiveTest, "UEAssetDoc.IKRig.RetargeterProfileActive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRetargeterProfileActiveTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRigRetargeterRegressionTests;
	FFixtures Fixtures;
	const FTarget& SourceRig = Fixtures.Add(TEXT("IK_ProfileSource"));
	const FTarget& TargetRig = Fixtures.Add(TEXT("IK_ProfileTarget"));
	UIKRigDefinition* Source = MakeRig(*this, SourceRig, TutorialMesh, true);
	UIKRigDefinition* TargetDefinition = MakeRig(*this, TargetRig, TutorialMesh, true);
	const FTarget& Profiled = Fixtures.Add(TEXT("RTG_Profiled"));
	UIKRetargeter* Asset = Source != nullptr && TargetDefinition != nullptr ? MakeRetargeter(*this, Profiled, Source, TargetDefinition) : nullptr;
	const FIKRetargetRootMotionOp* RootMotion = Asset != nullptr ? FindRootMotion(*Asset) : nullptr;
	TMap<FName, FRetargetProfile>* Profiles = Asset != nullptr ? StoredProfiles(*Asset) : nullptr;
	FName* Current = Asset != nullptr ? StoredCurrentProfile(*Asset) : nullptr;
	if (!TestTrue(TEXT("a retargeter with a Root Motion op and reflected profile storage"), RootMotion != nullptr && Profiles != nullptr && Current != nullptr))
	{
		return false;
	}
	// A copy of the op's settings with Rotate With Pelvis flipped (made through reflection: the settings type is not exported).
	const bool bLiveRotate = RootMotion->Settings.bRotateWithPelvis;
	UScriptStruct* SettingsType = FIKRetargetRootMotionOpSettings::StaticStruct();
	const FBoolProperty* Rotate = CastField<FBoolProperty>(SettingsType->FindPropertyByName(TEXT("bRotateWithPelvis")));
	if (!TestNotNull(TEXT("settings field bRotateWithPelvis"), Rotate))
	{
		return false;
	}
	FRetargetOpProfile OpProfile;
	OpProfile.OpToApplySettingsTo = RootMotion->GetName();
	OpProfile.SettingsToApply.InitializeAs(SettingsType, reinterpret_cast<const uint8*>(&RootMotion->Settings));
	Rotate->SetPropertyValue_InContainer(OpProfile.SettingsToApply.GetMutableMemory(), !bLiveRotate);
	FRetargetProfile Profile;
	Profile.RetargetOpProfiles.Add(MoveTemp(OpProfile));
	Profile.bForceAllIKOff = true;
	const FName Run(TEXT("Run"));
	Asset->Modify();
	Profiles->Add(Run, MoveTemp(Profile));
	*Current = Run;
	if (!TestTrue(TEXT("profiled retargeter saved"), Save(*Asset)))
	{
		return false;
	}

	AssetDocumentTolerantTestSupport::FExpected Expected;
	Expected.Warnings.Add({TEXT("IKRetargeter.ProfileActive"), TEXT("/Body")});
	AssetDocumentTolerantTestSupport::TestTolerantRoundTrip(*this, View(Profiled), Expected, TEXT("Current profile"));

	const FAssetDocumentResult Inspected = Inspect(Profiled.ObjectPath);
	const TSharedPtr<FJsonObject>* Listed = nullptr;
	const TSharedPtr<FJsonObject>* Overrides = nullptr;
	const TSharedPtr<FJsonObject>* RunOverrides = nullptr;
	if (!TestTrue(TEXT("Inspect lists the profile's overrides"), Inspected.IsSuccess() && Inspected.Payload.IsValid()
		&& Inspected.Payload->TryGetObjectField(TEXT("Profiles"), Listed) && (*Listed)->TryGetObjectField(TEXT("Overrides"), Overrides)
		&& (*Overrides)->TryGetObjectField(TEXT("Run"), RunOverrides)))
	{
		AddInfo(DiagnosticsText(Inspected));
		return false;
	}
	AddInfo(FString::Printf(TEXT("Profiles: %s"), *Serialize(*Listed)));
	TestEqual(TEXT("Inspect names the current profile"), (*Listed)->GetStringField(TEXT("Current")), FString(TEXT("Run")));
	TestTrue(TEXT("Force All IK Off listed"), (*RunOverrides)->GetBoolField(TEXT("ForceAllIKOff")));
	const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
	if (TestTrue(TEXT("one op override"), (*RunOverrides)->TryGetArrayField(TEXT("Ops"), Ops) && Ops->Num() == 1))
	{
		const TSharedPtr<FJsonObject> Op = (*Ops)[0]->AsObject();
		TestEqual(TEXT("the override names the Root Motion op"), Op->GetStringField(TEXT("Op")), RootMotion->GetName().ToString());
		TestEqual(TEXT("and its settings type"), Op->GetStringField(TEXT("SettingsType")), SettingsType->GetName());
		const TSharedPtr<FJsonObject>* Settings = nullptr;
		TestTrue(TEXT("only the overridden setting is listed"), Op->TryGetObjectField(TEXT("Settings"), Settings) && (*Settings)->Values.Num() == 1
			&& (*Settings)->HasField(TEXT("bRotateWithPelvis")) && (*Settings)->GetBoolField(TEXT("bRotateWithPelvis")) == !bLiveRotate);
	}

	UIKRetargeter* Reloaded = Cast<UIKRetargeter>(Reload(Profiled));
	TMap<FName, FRetargetProfile>* Kept = Reloaded != nullptr ? StoredProfiles(*Reloaded) : nullptr;
	FName* KeptCurrent = Reloaded != nullptr ? StoredCurrentProfile(*Reloaded) : nullptr;
	TestTrue(TEXT("the profile and the current profile are kept"), Kept != nullptr && Kept->Contains(Run) && KeptCurrent != nullptr && *KeptCurrent == Run);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRetargeterRootMotionBonesTest, "UEAssetDoc.IKRig.RetargeterRootMotionBones",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRetargeterRootMotionBonesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRigRetargeterRegressionTests;
	FFixtures Fixtures;
	const FTarget& SourceRig = Fixtures.Add(TEXT("IK_RootMotionSource"));
	const FTarget& TargetRig = Fixtures.Add(TEXT("IK_RootMotionTarget"));
	if (MakeRig(*this, SourceRig, TutorialMesh, true) == nullptr || MakeRig(*this, TargetRig, TutorialMesh, true) == nullptr)
	{
		return false;
	}

	// The same for an IK Rig: a solver setting written at a new solver's value (the sparse read omits it).
	{
		const TSharedPtr<FJsonObject> Rig = Extract(*this, SourceRig.ObjectPath, TEXT("source rig"));
		const TArray<TSharedPtr<FJsonValue>>* Solvers = nullptr;
		TSharedPtr<FJsonObject> FullBody;
		if (BodyOf(Rig)->TryGetArrayField(TEXT("Solvers"), Solvers))
		{
			for (const TSharedPtr<FJsonValue>& Solver : *Solvers)
			{
				if (Solver->AsObject()->GetStringField(TEXT("Type")) == TEXT("FullBodyIK"))
				{
					FullBody = Solver->AsObject();
					break;
				}
			}
		}
		if (TestTrue(TEXT("the auto-made rig has a Full Body IK solver"), FullBody.IsValid()))
		{
			TSharedPtr<FJsonObject> Settings = FullBody->HasField(TEXT("Settings")) ? FullBody->GetObjectField(TEXT("Settings")) : MakeShared<FJsonObject>();
			Settings->SetNumberField(TEXT("Iterations"), 20); // FIKRigFBIKSettings' default
			FullBody->SetObjectField(TEXT("Settings"), Settings);
			const TSharedPtr<FJsonObject> RigUpdate = MakeTrialDocument(IKRigClass, SourceRig, TEXT("Update"), Serialize(BodyOf(Rig)));
			const FAssetDocumentResult RigApplied = Apply(SourceRig, RigUpdate);
			TestTrue(*FString::Printf(TEXT("a solver setting at its default applies (%s)"), *DiagnosticsText(RigApplied)), RigApplied.IsSuccess());
			FString RigDiff;
			if (!TestEqual(TEXT("and the rig has no differences"), DiffCount(RigUpdate, RigDiff), 0))
			{
				AddInfo(RigDiff.Left(1500));
			}
		}
	}

	// Create with only the two rigs (the factory's default op stack).
	const FTarget& Target = Fixtures.Add(TEXT("RTG_RootMotion"));
	const FAssetDocumentResult Created = Apply(Target, MakeTrialDocument(RetargeterClass, Target, TEXT("Create"),
		FString::Printf(TEXT(R"({"Source": {"IKRig": "%s"}, "Target": {"IKRig": "%s"}})"), *SourceRig.ObjectPath, *TargetRig.ObjectPath)));
	if (!TestTrue(*FString::Printf(TEXT("retargeter Create (%s)"), *DiagnosticsText(Created)), Created.IsSuccess()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Extracted = Extract(*this, Target.ObjectPath, TEXT("after Create"));
	const TSharedPtr<FJsonObject> RootMotion = OpOfType(BodyOf(Extracted), TEXT("RootMotion"));
	if (!TestTrue(TEXT("the default stack has a Root Motion op"), RootMotion.IsValid()))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("Root Motion op as created: %s"), *Serialize(RootMotion)));

	// The edit: the root and pelvis bones (the values a new op would fill in for these rigs).
	RootMotion->SetObjectField(TEXT("Settings"), Parse(TEXT(R"({"SourceRoot": {"BoneName": "root"}, "TargetRoot": {"BoneName": "root"},
		"TargetPelvis": {"BoneName": "pelvis"}})")));
	const TSharedPtr<FJsonObject> Edited = MakeTrialDocument(RetargeterClass, Target, TEXT("Update"), Serialize(BodyOf(Extracted)));
	const FAssetDocumentResult Applied = Apply(Target, Edited);
	if (!TestTrue(*FString::Printf(TEXT("Root Motion bones apply (%s)"), *DiagnosticsText(Applied)), Applied.IsSuccess()))
	{
		return false;
	}
	const UIKRetargeter* Retargeter = Cast<UIKRetargeter>(Target.Get());
	const FIKRetargetRootMotionOp* Op = Retargeter != nullptr ? FindRootMotion(*Retargeter) : nullptr;
	if (TestNotNull(TEXT("the Root Motion op is there"), Op))
	{
		TestEqual(TEXT("SourceRoot stored"), Op->Settings.SourceRoot.BoneName, FName(TEXT("root")));
		TestEqual(TEXT("TargetRoot stored"), Op->Settings.TargetRoot.BoneName, FName(TEXT("root")));
		TestEqual(TEXT("TargetPelvis stored"), Op->Settings.TargetPelvis.BoneName, FName(TEXT("pelvis")));
	}
	FString DiffText;
	if (!TestEqual(TEXT("no differences from the document"), DiffCount(Edited, DiffText), 0))
	{
		AddInfo(DiffText.Left(1500));
	}
	const FAssetDocumentResult Again = Apply(Target, Edited);
	TestTrue(*FString::Printf(TEXT("re-apply succeeds (%s)"), *DiagnosticsText(Again)), Again.IsSuccess());
	TestFalse(TEXT("re-apply is a no-op"), Again.bSavedAsset);

	// A setting written at the value a new op has (sparse reads omit it) verifies as well.
	const TSharedPtr<FJsonObject> Defaults = Parse(Serialize(Edited));
	const TSharedPtr<FJsonObject> Rooted = OpOfType(BodyOf(Defaults), TEXT("RootMotion"));
	Rooted->GetObjectField(TEXT("Settings"))->SetBoolField(TEXT("bPropagateToNonRetargetedChildren"), true); // the setting's default
	const FAssetDocumentResult AtDefault = Apply(Target, Defaults);
	TestTrue(*FString::Printf(TEXT("a setting at its default value applies (%s)"), *DiagnosticsText(AtDefault)), AtDefault.IsSuccess());
	if (!TestEqual(TEXT("and has no differences"), DiffCount(Defaults, DiffText), 0))
	{
		AddInfo(DiffText.Left(1500));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRigValidateLegacyMeshFirstLoadTest, "UEAssetDoc.IKRig.ValidateLegacyMeshFirstLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRigValidateLegacyMeshFirstLoadTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRigRetargeterRegressionTests;
	// A copy of the tutorial mannequin's package under /Game: on disk, never loaded in this editor.
	const FString Folder = FString::Printf(TEXT("__AssetDocumentTests__/LegacyMesh_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12));
	const FString MeshPackage = FString(TEXT("/Game/")) + Folder + TEXT("/TutorialTPP");
	const FString MeshPath = MeshPackage + TEXT(".TutorialTPP");
	const FString Source = FPaths::EngineContentDir() / TEXT("Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.uasset");
	const FString Copy = FPackageName::LongPackageNameToFilename(MeshPackage, FPackageName::GetAssetPackageExtension());
	FTarget Rig(TEXT("IK_LegacyFirstLoad"));
	ON_SCOPE_EXIT
	{
		Rig.Cleanup();
		if (UPackage* Package = FindPackage(nullptr, *MeshPackage))
		{
			Package->ClearDirtyFlag();
			UPackageTools::UnloadPackages({Package});
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Copy), false, true);
		FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({Copy});
	};
	if (!TestEqual(TEXT("the legacy mesh is copied"), IFileManager::Get().Copy(*Copy, *Source), static_cast<uint32>(COPY_OK)))
	{
		return false;
	}
	FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({Copy});
	if (!TestNull(TEXT("the copy is not loaded yet"), FindPackage(nullptr, *MeshPackage)))
	{
		return false;
	}

	// The rig: a goal on foot_l, Full Body IK from the pelvis, a leg chain, the retarget root.
	FAssetDocumentValidateRequest Request;
	Request.Document = MakeTrialDocument(IKRigClass, Rig, TEXT("Create"), FString::Printf(TEXT(R"({"PreviewMesh": "%s",
		"Goals": {"LeftFootIK": {"Bone": "foot_l"}},
		"Solvers": [{"Type": "FullBodyIK", "StartBone": "pelvis", "Goals": {"LeftFootIK": {}}}],
		"RetargetRoot": "pelvis",
		"Chains": {"LeftLeg": {"Start": "thigh_l", "End": "foot_l", "Goal": "LeftFootIK"}}})"), *MeshPath));
	const FAssetDocumentResult Validated = FAssetDocumentModule::Get().GetService().Validate(Request);
	AddInfo(FString::Printf(TEXT("first validate: %s"), *DiagnosticsText(Validated)));
	TestTrue(TEXT("the first validate that loads the legacy mesh passes"), Validated.IsSuccess());
	TestFalse(TEXT("no PackageNotRestored"), HasCode(Validated, TEXT("PreflightIsolation.PackageNotRestored")));
	const UPackage* Loaded = FindPackage(nullptr, *MeshPackage);
	if (TestNotNull(TEXT("the validate loaded the mesh"), Loaded))
	{
		TestFalse(TEXT("the mesh package is not dirty"), Loaded->IsDirty());
	}
	const FAssetDocumentResult Second = FAssetDocumentModule::Get().GetService().Validate(Request);
	TestTrue(*FString::Printf(TEXT("a second validate passes (%s)"), *DiagnosticsText(Second)), Second.IsSuccess());
	return true;
}

#endif
