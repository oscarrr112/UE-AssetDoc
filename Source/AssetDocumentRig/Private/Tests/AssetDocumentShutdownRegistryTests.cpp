// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// UEAssetDoc.Shutdown.ClassPathRegistries: the class-keyed registries modules fill at startup (trial
//   target makers, exact Blueprint classes, hosted Blueprints) are keyed by class path, so ShutdownModule unregisters
//   without touching a UObject: on a normal editor exit the UObject exit purge (StaticExit) runs before any
//   ShutdownModule, and an FObjectKey of a purged class asserted (Index >= 0, UObjectArray.h). Registering and
//   unregistering a class that does not exist works; lookups by a live class still find their entry (a hosted entry
//   also matches subclasses).

#include "AssetDocumentEditorTrialScope.h"
#include "AssetDocumentExactBlueprintClasses.h"
#include "AssetDocumentGraphProvider.h"
#include "ControlRigBlueprintLegacy.h"
#include "Curves/CurveFloat.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentShutdownRegistryTests
{
// A class that is not loaded (as a class whose UObject the exit purge destroyed).
const FTopLevelAssetPath GoneClassPath(TEXT("/Script/AssetDocumentGone"), TEXT("GoneClass"));

int32 GResolveCalls = 0;
UBlueprint* CountingResolver(UObject& Host)
{
	++GResolveCalls;
	return nullptr;
}
} // namespace AssetDocumentShutdownRegistryTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentShutdownClassPathRegistriesTest,
	"UEAssetDoc.Shutdown.ClassPathRegistries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentShutdownClassPathRegistriesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentShutdownRegistryTests;

	// The live registrations of the Rig module are found by class.
	UClass* ControlRigClass = UControlRigBlueprint::StaticClass();
	TestNotNull(TEXT("Control Rig trial target maker found by class"),
		FAssetDocumentEditorTrialScope::FindTrialTargetMaker(*ControlRigClass));
	TestTrue(TEXT("ControlRigBlueprint is an exact Blueprint class"), IsAssetDocumentExactBlueprintClass(ControlRigClass));
	TestFalse(TEXT("plain Blueprint is not an exact Blueprint class"), IsAssetDocumentExactBlueprintClass(UBlueprint::StaticClass()));

	// Register and unregister a class that does not exist: no UObject is needed on either side.
	FAssetDocumentEditorTrialScope::RegisterTrialTargetMaker(GoneClassPath,
		[](UClass&, UObject*, TConstArrayView<UObject*>, FString&) -> UObject* { return nullptr; });
	FAssetDocumentEditorTrialScope::UnregisterTrialTargetMaker(GoneClassPath);
	FAssetDocumentEditorTrialScope::UnregisterTrialTargetMaker(GoneClassPath);
	TestNotNull(TEXT("unregistering another class keeps the Control Rig maker"),
		FAssetDocumentEditorTrialScope::FindTrialTargetMaker(*ControlRigClass));

	TestTrue(TEXT("exact class registered by path"), RegisterAssetDocumentExactBlueprintClass(GoneClassPath));
	TestFalse(TEXT("duplicate exact class refused"), RegisterAssetDocumentExactBlueprintClass(GoneClassPath));
	TestTrue(TEXT("exact class unregistered by path"), UnregisterAssetDocumentExactBlueprintClass(GoneClassPath));
	TestFalse(TEXT("second unregister finds nothing"), UnregisterAssetDocumentExactBlueprintClass(GoneClassPath));
	TestTrue(TEXT("ControlRigBlueprint still exact"), IsAssetDocumentExactBlueprintClass(ControlRigClass));

	RegisterAssetDocumentHostedBlueprint(GoneClassPath, &CountingResolver);
	UnregisterAssetDocumentHostedBlueprint(GoneClassPath);

	// A hosted entry matches its class and subclasses by class path, and is gone after unregistering by path.
	const FTopLevelAssetPath CurveBasePath = UCurveBase::StaticClass()->GetClassPathName();
	UCurveFloat* Host = NewObject<UCurveFloat>(GetTransientPackage(), NAME_None, RF_Transient);
	GResolveCalls = 0;
	RegisterAssetDocumentHostedBlueprint(CurveBasePath, &CountingResolver);
	bool bHostClass = false;
	ResolveAssetDocumentHostedBlueprint(*Host, &bHostClass);
	TestTrue(TEXT("subclass of the registered host class is a host"), bHostClass);
	TestEqual(TEXT("resolver called once"), GResolveCalls, 1);
	UnregisterAssetDocumentHostedBlueprint(CurveBasePath);
	ResolveAssetDocumentHostedBlueprint(*Host, &bHostClass);
	TestFalse(TEXT("unregistered host class is no host"), bHostClass);
	TestEqual(TEXT("resolver not called after unregister"), GResolveCalls, 1);
	Host->MarkAsGarbage();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
