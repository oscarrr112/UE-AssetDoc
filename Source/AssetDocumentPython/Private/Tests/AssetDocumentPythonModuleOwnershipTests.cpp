// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentRuntimeModule.h"

#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPythonGenericOwnershipDormantTest,
	"UEAssetDoc.GenericRuntime.PythonOwnershipDormant",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPythonGenericOwnershipDormantTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	IAssetDocumentRuntimeModule* Module =
		FModuleManager::LoadModulePtr<IAssetDocumentRuntimeModule>(
			TEXT("AssetDocumentPython"));
	TestNotNull(TEXT("Python owner module loads"), Module);
	if (Module != nullptr)
	{
		TestFalse(
			TEXT("module load does not activate a Python runtime revision"),
			Module->HasActiveRuntimeRegistration());
	}
	return true;
}

#endif
