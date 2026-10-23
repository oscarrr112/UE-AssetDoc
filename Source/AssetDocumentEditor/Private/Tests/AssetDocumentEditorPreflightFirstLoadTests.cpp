// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// The first validate of a modular Control Rig made from the engine's module library
// failed with 8 x PreflightIsolation.PackageNotRestored for the module and function library packages
// the validate loaded for the first time ("reload it from disk or restart the editor"); the same
// validate a second later passed. A first load is not a change: the validate passes and says nothing
// about reloading. This test is its own group so that it runs first in its editor process.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "AssetDocumentTypes.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPreflightFirstLoadModularRigTest,
	"UEAssetDoc.PreflightFirstLoad.ModularRigLibraries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentPreflightFirstLoadModularRigTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TCHAR* const Modules[] = {TEXT("/ControlRigModules/Modules56/Spine"), TEXT("/ControlRigModules/Modules56/Arm"),
		TEXT("/ControlRigModules/Modules56/Leg"), TEXT("/ControlRigModules/Modules56/Neck"), TEXT("/ControlRig/Modules/Modules56/Root")};
	int32 AlreadyLoaded = 0;
	for (const TCHAR* Module : Modules)
	{
		AlreadyLoaded += FindPackage(nullptr, Module) != nullptr ? 1 : 0;
	}
	AddInfo(FString::Printf(TEXT("module packages already loaded before the validate: %d of %d"), AlreadyLoaded, UE_ARRAY_COUNT(Modules)));
	const FString Target = FString::Printf(TEXT("/Game/AssetDocumentPreflight/CR_FirstLoad_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString Text = FString::Printf(TEXT(R"J({"SchemaVersion": 1, "Action": "Create", "Class": "/Script/ControlRigDeveloper.ControlRigBlueprint",
		"Target": "%s", "FactoryContext": "/ControlRigModules/Modules/Meshes/SKM_Biped_Template.SKM_Biped_Template",
		"Body": {"RigType": "Modular", "Modules": [
			{"Name": "Root", "Class": "/ControlRig/Modules/Modules56/Root.Root_C"},
			{"Name": "Spine", "Class": "/ControlRigModules/Modules56/Spine.Spine_C", "Parent": "Root"},
			{"Name": "Neck", "Class": "/ControlRigModules/Modules56/Neck.Neck_C", "Parent": "Spine"},
			{"Name": "Arm_L", "Class": "/ControlRigModules/Modules56/Arm.Arm_C", "Parent": "Spine"},
			{"Name": "Arm_R", "Class": "/ControlRigModules/Modules56/Arm.Arm_C", "Parent": "Spine"},
			{"Name": "Leg_L", "Class": "/ControlRigModules/Modules56/Leg.Leg_C", "Parent": "Spine"},
			{"Name": "Leg_R", "Class": "/ControlRigModules/Modules56/Leg.Leg_C", "Parent": "Spine"}],
		"Connections": {"Root/Root": "Bone(root)"}}})J"), *Target);
	FAssetDocumentValidateRequest Request;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Request.Document);
	if (!TestTrue(TEXT("document parses"), Request.Document.IsValid()))
	{
		return false;
	}
	for (int32 Attempt = 0; Attempt < 2; ++Attempt)
	{
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Validate(Request);
		const FString Stage = Attempt == 0 ? TEXT("first validate") : TEXT("second validate");
		AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *Result.Message));
		for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
		{
			AddInfo(FString::Printf(TEXT("%s:   [%s] %s %s %s"), *Stage, *Diagnostic.Severity, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
			TestFalse(*FString::Printf(TEXT("%s: no %s"), *Stage, *Diagnostic.Code), Diagnostic.Code == TEXT("PreflightIsolation.PackageNotRestored"));
			TestFalse(*FString::Printf(TEXT("%s: no advice to reload or restart (%s)"), *Stage, *Diagnostic.Code),
				Diagnostic.Message.Contains(TEXT("restart the editor")) || Diagnostic.Message.Contains(TEXT("Asset Actions > Reload")));
		}
		TestTrue(*FString::Printf(TEXT("%s passes"), *Stage), Result.IsSuccess());
		TestNull(*FString::Printf(TEXT("%s makes no asset"), *Stage),
			FindObject<UObject>(nullptr, *(Target + TEXT(".") + FPackageName::GetShortName(Target))));
	}
	return true;
}

#endif
