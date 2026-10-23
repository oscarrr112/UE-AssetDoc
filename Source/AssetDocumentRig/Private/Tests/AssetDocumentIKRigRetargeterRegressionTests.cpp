// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Retargeter and IK Rig regressions:
// UEAssetDoc.IKRig.RetargeterRootMotionBones: a retargeter created with only its IK Rigs (the
//   factory's default op stack, whose Root Motion op was added before any preview mesh) takes the Root Motion op's
//   SourceRoot / TargetRoot / TargetPelvis; the values equal what a new op would fill in, so the sparse read omits
//   them, and verification must still find them equal (it failed with GenericRuntime.VerificationMismatch). A
//   setting written at its default value verifies too.
// UEAssetDoc.IKRig.ValidateLegacyMeshFirstLoad: the first validate that loads a never-loaded legacy
//   skeletal mesh (the UE4 tutorial mannequin, a copy under /Game so it is not in memory yet) passes: the mesh's
//   load-time upgrade (AssetImportData, mesh descriptions, the deferred post-load build) is not a change the preflight
//   made (it failed with PreflightIsolation.PackageNotRestored).

#include "AssetDocumentIKRigTestSupport.h"

#include "Retargeter/IKRetargetOps.h"
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
