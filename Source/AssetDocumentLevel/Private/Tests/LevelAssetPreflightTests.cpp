// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Every validate of a Camera Animation Sequence Create failed with
// PreflightIsolation.PackageNotRestored naming the open map ("objects were created in it
// (CineCameraActor 'CineCameraActor_1', ...) ... reload it from disk ... or restart the editor"),
// although the map was not changed: the editor's factory makes the default Cine Camera by spawning
// a transient actor in the level editing viewport's world and destroying it at once. The validate
// passes, the map stays clean and unchanged, and nothing advises reloading.

#include "EngineUtils.h"
#include "LevelEditTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelPreflightCameraAnimationValidateTest,
	"UEAssetDoc.Level.Preflight.CameraAnimationCreateValidate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelPreflightCameraAnimationValidateTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditTests;
	FScopedTestAsset Shake(TEXT("CAS_SmallShake"));
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Cine"), false);
	if (!TestNotNull(TEXT("map created"), World)
		|| !TestTrue(TEXT("level built and saved"), Edit(*this, TEXT(R"({"description": "AI: build", "save": true, "ops": [
			{"op": "spawn", "class": "/Script/Engine.StaticMeshActor", "label": "Door"}]})"), TEXT("build")).bSuccess))
	{
		return false;
	}
	const UPackage* MapPackage = World->GetPackage();
	const TArray<FString> ActorsBefore = ListedActorPaths(*World->PersistentLevel);
	FScopedLevelEditingViewport Viewport;
	AddInfo(FString::Printf(TEXT("level editing viewport world: %s"), GCurrentLevelEditingViewportClient != nullptr
		&& GCurrentLevelEditingViewportClient->GetWorld() != nullptr ? *GCurrentLevelEditingViewportClient->GetWorld()->GetPathName() : TEXT("none")));
	for (int32 Attempt = 0; Attempt < 2; ++Attempt)
	{
		const TCHAR* Stage = Attempt == 0 ? TEXT("first validate") : TEXT("second validate");
		const FAssetDocumentResult Result = Shake.Validate(*this, CameraAnimationDocument(Shake.PackageName), Stage);
		TestTrue(*FString::Printf(TEXT("%s passes"), Stage), Result.IsSuccess());
		for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
		{
			TestFalse(*FString::Printf(TEXT("%s: no %s"), Stage, *Diagnostic.Code), Diagnostic.Code == TEXT("PreflightIsolation.PackageNotRestored"));
			TestFalse(*FString::Printf(TEXT("%s: no advice to reload or restart"), Stage),
				Diagnostic.Message.Contains(TEXT("restart the editor")) || Diagnostic.Message.Contains(TEXT("Asset Actions > Reload")));
		}
		TestFalse(*FString::Printf(TEXT("%s: the map stays clean"), Stage), MapPackage->IsDirty());
		TestEqual(*FString::Printf(TEXT("%s: the map's actors are unchanged"), Stage), ListedActorPaths(*World->PersistentLevel), ActorsBefore);
		TestNull(*FString::Printf(TEXT("%s: no asset is made"), Stage), FindObject<UObject>(nullptr, *Shake.ObjectPath()));
	}
	int32 CameraActors = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		CameraActors += It->GetClass()->GetName().Contains(TEXT("CineCamera")) ? 1 : 0;
	}
	TestEqual(TEXT("no Cine Camera is left in the map"), CameraActors, 0);
	return true;
}

#endif
