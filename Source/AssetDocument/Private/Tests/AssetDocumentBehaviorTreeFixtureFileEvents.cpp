// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "CoreMinimal.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "DirectoryWatcherModule.h"
#include "IDirectoryWatcher.h"
#include "CoreGlobals.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// Behavior Tree fixture seams shared by the test-only module and the integration tests, so they live in
// a module that both can link.
ASSETDOCUMENT_API void FlushBehaviorTreeFixtureFileEvents();
ASSETDOCUMENT_API void DeleteBehaviorTreeFixtureLoadedAsset(UObject* Asset);

void FlushBehaviorTreeFixtureFileEvents()
{
 // Save can enqueue filesystem events that outlive a synchronous test. Process
 // them while the fixture still exists, before stock deletion marks it empty.
 UPackage::WaitForAsyncFileWrites();
 if (auto* Module = FModuleManager::GetModulePtr<FDirectoryWatcherModule>(TEXT("DirectoryWatcher")))
  if (IDirectoryWatcher* Watcher = Module->Get()) Watcher->Tick(-1.f);
 if (IAssetRegistry* Registry = IAssetRegistry::Get())
 {
  Registry->WaitForCompletion();
  Registry->Tick(-1.f);
 }
}

void DeleteBehaviorTreeFixtureLoadedAsset(UObject* Asset)
{
 if (Asset)
 {
  // Caller has removed only its owned files. Bypass the path API's registry
  // lookup, which can no longer find the package after those removal events.
  TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);
  TArray<UObject*> ObjectsToDelete{Asset};
  ObjectTools::ForceDeleteObjects(ObjectsToDelete, false);
 }
}

#endif
