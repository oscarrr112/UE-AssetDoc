// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentTestOnlyModules.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"

namespace UE::AssetDocumentServer
{
TArray<FName> GetTestOnlyModuleNames()
{
	TArray<FName> Names;
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UEAssetDoc")))
	{
		for (const FModuleDescriptor& Module : Plugin->GetDescriptor().Modules)
		{
			if (Module.LoadingPhase == ELoadingPhase::None)
			{
				Names.Add(Module.Name);
			}
		}
	}
	return Names;
}

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
bool GTestOnlyModulesStartupRecorded = false;
TArray<FName> GTestOnlyModulesLoadedAtStartup;
}

void RecordTestOnlyModulesAtStartup()
{
	GTestOnlyModulesLoadedAtStartup.Reset();
	for (const FName Name : GetTestOnlyModuleNames())
	{
		if (FModuleManager::Get().IsModuleLoaded(Name))
		{
			GTestOnlyModulesLoadedAtStartup.Add(Name);
		}
	}
	GTestOnlyModulesStartupRecorded = true;
}

void AllowFirstAutomationListingToLoadTestOnlyModules()
{
	FAutomationTestFramework::Get().SetRequestedTestFilter(
		EAutomationTestFlags_FilterMask & ~EAutomationTestFlags::SmokeFilter);
}

bool GetTestOnlyModulesLoadedAtStartup(TArray<FName>& OutLoaded)
{
	OutLoaded = GTestOnlyModulesLoadedAtStartup;
	return GTestOnlyModulesStartupRecorded;
}
#endif
}
