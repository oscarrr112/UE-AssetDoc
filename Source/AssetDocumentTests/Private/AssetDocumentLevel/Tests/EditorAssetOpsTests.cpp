// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Editor asset operations an AI client used execute_python for.
//
//   DeleteBatchInternalReferences: delete_assets deletes a batch whose referencers are all in the batch
//     (a reference cycle, a chain); an asset with a referencer outside the batch is refused, and so is
//     every batch asset it references.
//   DuplicateLevel: duplicate_asset copies a map (open or not, with external actors) as the Content
//     Browser's Duplicate does.
//   ExtractManyGraphs: extract_asset_document of a Blueprint with more graphs than one runtime value holds.
//   EditLevelSaveProperties: edit_level "save" writes a property change of a placed instance to disk.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "AssetDocumentAssetDelete.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetOpsTestSupport.h"
#include "EditorAssetOpsTestTypes.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/PointLight.h"
#include "EngineUtils.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "LevelEditTestSupport.h"
#include "LevelEditTestTypes.h"
#include "Misc/AutomationTest.h"
#include "PackageTools.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/UObjectHash.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentServerEditorAssetOpsTests
{
using namespace AssetDocumentLevelEditTests;
using namespace AssetDocumentAssetOpsTestSupport;

// The request delete_assets sends, run as its HTTP route does.
TArray<FAssetDocumentDeleteOutcome> DeleteBatch(const TArray<UObject*>& Assets)
{
	return AssetDocumentAssetDelete::DeleteAssetsAsEditor(Assets);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAssetOpsDeleteBatchInternalReferencesTest,
	"UEAssetDoc.AssetOps.DeleteBatchInternalReferences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAssetOpsDeleteBatchInternalReferencesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentServerEditorAssetOpsTests;
	FScopedTestAssets Assets;
	// A <-> B (a cycle), C -> A (a chain into the cycle): every referencer is in the batch.
	UAssetDocumentAssetOpsTestRefAsset* A = Assets.NewRefAsset(TEXT("BP_CharacterBase"));
	UAssetDocumentAssetOpsTestRefAsset* B = Assets.NewRefAsset(TEXT("BPC_CombatCore"));
	UAssetDocumentAssetOpsTestRefAsset* C = Assets.NewRefAsset(TEXT("S_Move"));
	A->Refs = {B};
	B->Refs = {A};
	C->Refs = {A};
	if (!TestTrue(TEXT("fixture saved"), FScopedTestAssets::Save({A, B, C})))
	{
		return false;
	}
	const TArray<FAssetDocumentDeleteOutcome> Outcomes = DeleteBatch({A, B, C});
	TestEqual(TEXT("one outcome per asset"), Outcomes.Num(), 3);
	for (int32 Index = 0; Index < Outcomes.Num(); ++Index)
	{
		TestTrue(*FString::Printf(TEXT("asset %d deleted"), Index), Outcomes[Index].bDeleted);
		if (!Outcomes[Index].bDeleted)
		{
			AddInfo(FString::Printf(TEXT("asset %d: %s"), Index, *AssetDocumentAssetDelete::DescribeDeleteBlockers(Outcomes[Index].Blockers)));
		}
	}
	for (const TCHAR* Name : {TEXT("BP_CharacterBase"), TEXT("BPC_CombatCore"), TEXT("S_Move")})
	{
		TestFalse(*FString::Printf(TEXT("%s is gone"), Name), AssetExists(Assets.ObjectPath(Name)));
		TestFalse(*FString::Printf(TEXT("%s's file is gone"), Name), IFileManager::Get().FileExists(*Assets.File(Name)));
	}

	// External referencers are still refused: Keep (not in the batch) -> Used <-> Pair; Free is independent.
	// Used is refused (Keep), Pair is refused because Used stays and references it, Free is deleted.
	UAssetDocumentAssetOpsTestRefAsset* Keep = Assets.NewRefAsset(TEXT("BP_Keep"));
	UAssetDocumentAssetOpsTestRefAsset* Used = Assets.NewRefAsset(TEXT("BP_Used"));
	UAssetDocumentAssetOpsTestRefAsset* Pair = Assets.NewRefAsset(TEXT("BP_Pair"));
	UAssetDocumentAssetOpsTestRefAsset* Free = Assets.NewRefAsset(TEXT("BP_Free"));
	Keep->Refs = {Used};
	Used->Refs = {Pair};
	Pair->Refs = {Used};
	if (!TestTrue(TEXT("second fixture saved"), FScopedTestAssets::Save({Keep, Used, Pair, Free})))
	{
		return false;
	}
	const TArray<FAssetDocumentDeleteOutcome> Mixed = DeleteBatch({Used, Pair, Free});
	if (!TestEqual(TEXT("one outcome per asset"), Mixed.Num(), 3))
	{
		return false;
	}
	TestFalse(TEXT("an asset referenced from outside the batch is refused"), Mixed[0].bDeleted);
	TestTrue(TEXT("its blocker names the outside referencer"), Mixed[0].Blockers.ContainsByPredicate([](const FAssetDocumentDeleteBlocker& Blocker)
	{
		return Blocker.Referencer.Contains(TEXT("BP_Keep")) && !Blocker.bRefusedInBatch;
	}));
	TestFalse(TEXT("a batch asset the refused asset references is refused too"), Mixed[1].bDeleted);
	TestTrue(TEXT("its blocker is the refused batch asset, marked so"), Mixed[1].Blockers.ContainsByPredicate([](const FAssetDocumentDeleteBlocker& Blocker)
	{
		return Blocker.Referencer.Contains(TEXT("BP_Used")) && Blocker.bRefusedInBatch;
	}));
	TestTrue(TEXT("an unrelated asset of the batch is deleted"), Mixed[2].bDeleted);
	TestTrue(TEXT("the refused assets still exist"), AssetExists(Assets.ObjectPath(TEXT("BP_Used"))) && AssetExists(Assets.ObjectPath(TEXT("BP_Pair"))));
	TestTrue(TEXT("and still reference each other"), IsValid(Used) && Used->Refs.Num() == 1 && Used->Refs[0] == Pair);
	TestFalse(TEXT("the deleted asset is gone"), AssetExists(Assets.ObjectPath(TEXT("BP_Free"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAssetOpsDuplicateLevelTest,
	"UEAssetDoc.AssetOps.DuplicateLevel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAssetOpsDuplicateLevelTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentServerEditorAssetOpsTests;
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Arena"), false);
	if (!TestNotNull(TEXT("map created"), World))
	{
		return false;
	}
	SpawnActor(*World->PersistentLevel, APointLight::StaticClass(), FVector(0, 0, 300), TEXT("ArenaLamp"));
	if (!TestTrue(TEXT("map saved"), FScopedTestMaps::SaveWorld(*World)))
	{
		return false;
	}

	FAssetDocumentDuplicateRequest Request;
	Request.Source = Maps.PackageName(TEXT("Arena"));
	Request.Target = Maps.PackageName(TEXT("ArenaCopy"));
	const FAssetDocumentResult Open = FAssetDocumentModule::Get().GetService().Duplicate(Request);
	AddInfo(Open.Message);
	TestTrue(TEXT("the open map duplicates"), Open.IsSuccess());
	TestEqual(TEXT("the editor still edits the source"), GetEditorWorld()->GetPackage()->GetName(), Maps.PackageName(TEXT("Arena")));
	TestTrue(TEXT("the copy is a map file"), FPackageName::DoesPackageExist(Maps.PackageName(TEXT("ArenaCopy"))));

	UWorld* Copy = Maps.LoadMap(TEXT("ArenaCopy"));
	if (TestNotNull(TEXT("the copy opens"), Copy))
	{
		bool bLamp = false;
		for (TActorIterator<APointLight> It(Copy); It; ++It)
		{
			bLamp |= It->GetActorLabel() == TEXT("ArenaLamp");
		}
		TestTrue(TEXT("the copy holds the source's actor"), bLamp);
	}

	// A map that is not open (Arena, now that its copy is open).
	Request.Target = Maps.PackageName(TEXT("ArenaClosedCopy"));
	const FAssetDocumentResult Closed = FAssetDocumentModule::Get().GetService().Duplicate(Request);
	AddInfo(Closed.Message);
	TestTrue(TEXT("a map that is not open duplicates"), Closed.IsSuccess());
	if (UWorld* ClosedCopy = Maps.LoadMap(TEXT("ArenaClosedCopy")))
	{
		bool bLamp = false;
		for (TActorIterator<APointLight> It(ClosedCopy); It; ++It)
		{
			bLamp |= It->GetActorLabel() == TEXT("ArenaLamp");
		}
		TestTrue(TEXT("its copy holds the actor"), bLamp);
	}
	else
	{
		AddError(TEXT("the copy of the closed map does not open"));
	}

	// World Partition (one file per actor): refused while open, as the Content Browser does; copied with
	// every actor file when not open.
	UWorld* Partitioned = Maps.NewMap(TEXT("Open"), true);
	if (!TestNotNull(TEXT("partitioned map created"), Partitioned))
	{
		return false;
	}
	SpawnActor(*Partitioned->PersistentLevel, APointLight::StaticClass(), FVector(0, 0, 300), TEXT("OpenLampA"));
	SpawnActor(*Partitioned->PersistentLevel, APointLight::StaticClass(), FVector(500, 0, 300), TEXT("OpenLampB"));
	if (!TestTrue(TEXT("partitioned map saved"), FScopedTestMaps::SaveWorld(*Partitioned)))
	{
		return false;
	}
	const auto ActorFiles = [](const FString& MapPackage)
	{
		FString Directory;
		FPackageName::TryConvertLongPackageNameToFilename(ULevel::GetExternalActorsPath(MapPackage), Directory);
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.uasset"), true, false);
		return Files.Num();
	};
	const int32 SourceFiles = ActorFiles(Maps.PackageName(TEXT("Open")));
	TestTrue(TEXT("the partitioned map has actor files"), SourceFiles >= 2);
	Request.Source = Maps.PackageName(TEXT("Open"));
	Request.Target = Maps.PackageName(TEXT("OpenCopy"));
	const FAssetDocumentResult InUse = FAssetDocumentModule::Get().GetService().Duplicate(Request);
	TestTrue(TEXT("the open partitioned map is refused"), !InUse.IsSuccess() && !InUse.Diagnostics.IsEmpty()
		&& InUse.Diagnostics[0].Code == TEXT("Duplicate.UnsupportedSource"));
	TestFalse(TEXT("nothing is left at the target"), FPackageName::DoesPackageExist(Maps.PackageName(TEXT("OpenCopy"))));

	TestNotNull(TEXT("another map opened"), Maps.LoadMap(TEXT("Arena")));
	const FAssetDocumentResult Copied = FAssetDocumentModule::Get().GetService().Duplicate(Request);
	AddInfo(Copied.Message);
	TestTrue(TEXT("the partitioned map duplicates when it is not open"), Copied.IsSuccess());
	TestEqual(TEXT("the copy has every actor file"), ActorFiles(Maps.PackageName(TEXT("OpenCopy"))), SourceFiles);
	TestEqual(TEXT("the source keeps its actor files"), ActorFiles(Maps.PackageName(TEXT("Open"))), SourceFiles);
	UWorld* PartitionedCopy = Maps.LoadMap(TEXT("OpenCopy"));
	TestTrue(TEXT("the copy opens as a partitioned map"), PartitionedCopy != nullptr && PartitionedCopy->IsPartitionedWorld());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAssetOpsExtractManyGraphsTest,
	"UEAssetDoc.AssetOps.ExtractManyGraphs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAssetOpsExtractManyGraphsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentServerEditorAssetOpsTests;
	FScopedTestAssets Assets;
	UPackage* Package = CreatePackage(*Assets.PackageName(TEXT("BP_ManyGraphs")));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, TEXT("BP_ManyGraphs"), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(Blueprint);
	// More graphs than one runtime value can list (a large AnimBlueprint has hundreds of state and transition graphs).
	constexpr int32 NumFunctions = 400;
	for (int32 Index = 0; Index < NumFunctions; ++Index)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*FString::Printf(TEXT("Fn%03d"), Index)),
			UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, /*bIsUserCreated*/ true, nullptr);
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("Blueprint saved"), FScopedTestAssets::Save({Blueprint})))
	{
		return false;
	}

	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Assets.ObjectPath(TEXT("BP_ManyGraphs"));
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Extract(Request);
	AddInfo(Result.Message);
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		AddInfo(FString::Printf(TEXT("[%s] %s %s"), *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
	if (!TestTrue(TEXT("the Blueprint extracts"), Result.IsSuccess()) || !TestTrue(TEXT("with a document"), Result.Payload.IsValid()))
	{
		return false;
	}
	FString Text;
	FJsonSerializer::Serialize(Result.Payload.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
	TestTrue(TEXT("the first function is in the document"), Text.Contains(TEXT("\"Fn000\"")));
	TestTrue(TEXT("the last function is in the document"), Text.Contains(FString::Printf(TEXT("\"Fn%03d\""), NumFunctions - 1)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAssetOpsEditLevelSavePropertiesTest,
	"UEAssetDoc.AssetOps.EditLevelSaveProperties",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAssetOpsEditLevelSavePropertiesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentServerEditorAssetOpsTests;
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Arena"), false);
	if (!TestNotNull(TEXT("map created"), World))
	{
		return false;
	}
	AActor* Dummy = SpawnActor(*World->PersistentLevel, AAssetDocumentLevelTestActor::StaticClass(), FVector::ZeroVector, TEXT("Dummy"));
	if (!TestNotNull(TEXT("placed instance"), Dummy) || !TestTrue(TEXT("map saved"), FScopedTestMaps::SaveWorld(*World)))
	{
		return false;
	}
	// Set changes a placed instance's properties; save: true writes the map.
	const FLevelToolResult Saved = Edit(*this, FString::Printf(TEXT(R"({"save": true, "ops": [
		{"op": "set", "actor": "%s", "properties": {"Score": 42.5}}]})"), *GetActorPath(*Dummy)), TEXT("set and save"));
	TestTrue(TEXT("the batch succeeds"), Saved.bSuccess);
	TestEqual(TEXT("saved_packages is the map"), PayloadStrings(Saved, TEXT("saved_packages")), TArray<FString>({Maps.PackageName(TEXT("Arena"))}));
	TestFalse(TEXT("the map is not dirty"), World->GetPackage()->IsDirty());

	GEditor->NewMap(false);
	UWorld* Reloaded = Maps.LoadMap(TEXT("Arena"));
	if (TestNotNull(TEXT("the map opens from disk"), Reloaded))
	{
		bool bFound = false;
		for (TActorIterator<AAssetDocumentLevelTestActor> It(Reloaded); It; ++It)
		{
			if (It->GetActorLabel() == TEXT("Dummy"))
			{
				bFound = true;
				TestEqual(TEXT("the saved value is on disk"), It->Score, 42.5f);
			}
		}
		TestTrue(TEXT("the instance is on disk"), bFound);
	}
	return true;
}
#endif
