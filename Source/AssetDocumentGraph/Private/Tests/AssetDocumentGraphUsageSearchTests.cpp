// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// find_assets `uses` (graph content search, AssetDocumentUsageSearch.h) through the production Service:
// - Graph.UsageSearch.IndexAndLoaded: a saved Blueprint is searched from its Find-in-Blueprints index; the node keys
//   read from the index are the keys its loaded graphs report; with unsaved changes it is searched loaded; aliases,
//   node classes (subclasses included), functions (bare, owner-qualified, a library), wildcards, AND / OR and a
//   referenced asset all find it; an unknown term is an invalid query.
// - Graph.UsageSearch.NotIndexed: a Blueprint saved without index data and not loaded is reported as not searched.
// - Graph.UsageSearch.Engine: a search ("Blueprints with promotable operators and container functions")
//   in one call over /MoverExamples, then over all engine and plugin content (timed).

#include "AssetDocumentGraphAdapters.h"
#include "AssetDocumentService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetMathLibrary.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentGraphUsageSearchTests
{
const TCHAR* const Folder = TEXT("/Game/AssetDocumentUsageSearch");
const TCHAR* const Promotable = TEXT("K2:PromotableOperator:Add");
const TCHAR* const ArrayLength = TEXT("K2:CallArrayFunction:/Script/Engine.KismetArrayLibrary:Array_Length");
const TCHAR* const RandomInteger = TEXT("K2:CallFunction:/Script/Engine.KismetMathLibrary:RandomInteger");
const TCHAR* const ForEach = TEXT("K2:MacroInstance:/Engine/EditorBlueprintResources/StandardMacros.StandardMacros:ForEachLoop");
const TCHAR* const Branch = TEXT("K2:IfThenElse");
const TCHAR* const Tick = TEXT("K2:Event:/Script/Engine.Actor:ReceiveTick");

FString PackageFile(const FString& PackageName)
{
	return FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
}

TArray<TSharedPtr<FJsonObject>> Rows(const FAssetDocumentResult& Result)
{
	TArray<TSharedPtr<FJsonObject>> Out;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("Assets"), Values))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (Value->TryGetObject(Object))
			{
				Out.Add(*Object);
			}
		}
	}
	return Out;
}

TSharedPtr<FJsonObject> FindRow(const FAssetDocumentResult& Result, const FString& ObjectPath)
{
	for (const TSharedPtr<FJsonObject>& Row : Rows(Result))
	{
		if (Row->GetStringField(TEXT("ObjectPath")) == ObjectPath)
		{
			return Row;
		}
	}
	return nullptr;
}

// NodeGuid -> Action of a result row.
TMap<FString, FString> RowNodes(const TSharedPtr<FJsonObject>& Row)
{
	TMap<FString, FString> Out;
	const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
	if (!Row.IsValid() || !Row->TryGetArrayField(TEXT("Graphs"), Graphs))
	{
		return Out;
	}
	for (const TSharedPtr<FJsonValue>& Graph : *Graphs)
	{
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (Graph->AsObject()->TryGetArrayField(TEXT("Nodes"), Nodes))
		{
			for (const TSharedPtr<FJsonValue>& Node : *Nodes)
			{
				FString Action;
				Node->AsObject()->TryGetStringField(TEXT("Action"), Action);
				Out.Add(Node->AsObject()->GetStringField(TEXT("NodeGuid")), Action);
			}
		}
	}
	return Out;
}

FString CodeOf(const FAssetDocumentResult& Result)
{
	return Result.Diagnostics.IsEmpty() ? FString() : Result.Diagnostics[0].Code;
}

FAssetDocumentResult Find(const FString& Path, const FString& Uses, const FString& Limit = TEXT("100"))
{
	TMap<FString, FString> Query = {{TEXT("uses"), Uses}, {TEXT("limit"), Limit}};
	if (!Path.IsEmpty())
	{
		Query.Add(TEXT("path"), Path);
	}
	return FAssetDocumentService().FindAssets(Query);
}

// An Actor Blueprint in /Game with the given nodes in its event graph, compiled and saved; NodeGuid -> key its
// loaded graph reports.
UBlueprint* CreateSavedBlueprint(FAutomationTestBase& Test, const FString& PackageName, TConstArrayView<const TCHAR*> Keys,
	TMap<FString, FString>& OutLiveKeys)
{
	UPackage* Package = CreatePackage(*PackageName);
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package,
		FName(*FPackageName::GetShortName(PackageName)), BPTYPE_Normal, UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(), TEXT("AssetDocumentUsageSearchTests"));
	if (!Test.TestNotNull(TEXT("Blueprint created"), Blueprint))
	{
		return nullptr;
	}
	FAssetRegistryModule::AssetCreated(Blueprint);
	const TSharedPtr<IAssetDocumentGraphSchemaAdapter> K2 = FindAssetDocumentGraphSchemaAdapter(GetDefault<UEdGraphSchema_K2>());
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!Test.TestTrue(TEXT("K2 adapter and event graph"), K2.IsValid() && EventGraph != nullptr))
	{
		return Blueprint;
	}
	for (const TCHAR* Key : Keys)
	{
		FAssetDocumentGraphSpawnRequest Request;
		Request.Asset = Blueprint;
		Request.Graph = EventGraph;
		Request.ActionKey = Key;
		FAssetDocumentGraphSpawnResult Spawned;
		FAssetDocumentGraphError Error;
		if (Test.TestTrue(FString::Printf(TEXT("spawn %s (%s)"), Key, *Error.Message), K2->ResolveAndSpawn(Request, Spawned, Error))
			&& Spawned.Node != nullptr)
		{
			FAssetDocumentGraphNodeDescription Description;
			K2->DescribeNode(*Spawned.Node, Description);
			OutLiveKeys.Add(FormatAssetDocumentGraphGuid(Spawned.Node->NodeGuid), Description.ActionKey);
		}
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	const FString File = PackageFile(PackageName);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
	Test.TestTrue(TEXT("Blueprint saved"), UPackage::SavePackage(Package, Blueprint, *File, Args));
	FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({File});
	return Blueprint;
}

void RemoveSaved(const FString& PackageName)
{
	if (UPackage* Package = FindPackage(nullptr, *PackageName))
	{
		Package->SetDirtyFlag(false);
		UPackageTools::UnloadPackages({Package});
	}
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	const FString File = PackageFile(PackageName);
	if (IFileManager::Get().FileExists(*File))
	{
		IFileManager::Get().Delete(*File, false, true);
		FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({File});
	}
}

void Unload(const FString& PackageName)
{
	if (UPackage* Package = FindPackage(nullptr, *PackageName))
	{
		Package->SetDirtyFlag(false);
		UPackageTools::UnloadPackages({Package});
	}
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphUsageSearchIndexAndLoadedTest,
	"UEAssetDoc.Graph.UsageSearch.IndexAndLoaded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentGraphUsageSearchIndexAndLoadedTest::RunTest(const FString&)
{
	using namespace AssetDocumentGraphUsageSearchTests;
	const FString PackageName = FString::Printf(TEXT("%s/BP_UsageIndexed_%s"), Folder, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
	ON_SCOPE_EXIT { RemoveSaved(PackageName); };
	TMap<FString, FString> LiveKeys;
	const TCHAR* const Keys[] = {Promotable, ArrayLength, RandomInteger, ForEach, Branch, Tick};
	TWeakObjectPtr<UBlueprint> Blueprint = CreateSavedBlueprint(*this, PackageName, Keys, LiveKeys);
	if (!Blueprint.IsValid() || LiveKeys.Num() != UE_ARRAY_COUNT(Keys))
	{
		return false;
	}

	// Saved and unchanged: read from the index; its keys are the loaded graph's keys.
	const FAssetDocumentResult Indexed = Find(Folder, TEXT("K2:PromotableOperator,K2:CallArrayFunction"));
	const TSharedPtr<FJsonObject> IndexedRow = FindRow(Indexed, ObjectPath);
	if (!TestTrue(TEXT("the saved Blueprint is found"), IndexedRow.IsValid()))
	{
		AddError(FString::Printf(TEXT("result: %s"), *Indexed.Message));
		return false;
	}
	TestEqual(TEXT("searched from the index"), IndexedRow->GetStringField(TEXT("Source")), FString(TEXT("Index")));
	TestEqual(TEXT("two nodes match"), static_cast<int32>(IndexedRow->GetNumberField(TEXT("Matches"))), 2);
	const TMap<FString, FString> AllIndexed = RowNodes(FindRow(Find(Folder, TEXT("K2:*")), ObjectPath));
	for (const TPair<FString, FString>& Live : LiveKeys)
	{
		const FString* Index = AllIndexed.Find(Live.Key);
		TestTrue(FString::Printf(TEXT("index key of %s is the loaded key %s (index: %s)"), *Live.Key, *Live.Value, Index ? **Index : TEXT("missing")),
			Index != nullptr && (*Index == Live.Value || (Live.Value == ForEach && *Index == TEXT("K2:MacroInstance::ForEachLoop"))));
	}

	// Terms: aliases, classes, functions, wildcards, AND / OR, a referenced asset.
	struct FCase
	{
		const TCHAR* Uses;
		bool bFound;
	};
	const FCase Cases[] = {
		{TEXT("K2:Add"), true},
		{TEXT("K2:Add,K2:ForEachLoop,K2:Branch,K2:Tick"), true},
		{TEXT("K2:PromotableOperator:Multiply"), false},
		{TEXT("K2:Add,K2:Delay"), false},
		{TEXT("K2:Delay|K2:Add"), true},
		{TEXT("K2Node_CallArrayFunction"), true},
		{TEXT("/Script/BlueprintGraph.K2Node_PromotableOperator"), true},
		{TEXT("/Script/BlueprintGraph.K2Node_DynamicCast"), false},
		{TEXT("Array_Length"), true},
		{TEXT("array_length"), true},
		{TEXT("/Script/Engine.KismetMathLibrary:RandomInteger"), true},
		{TEXT("/Script/Engine.KismetMathLibrary.RandomInteger"), true},
		{TEXT("/Script/Engine.KismetArrayLibrary"), true},
		{TEXT("/Script/Engine.KismetStringLibrary"), false},
		{TEXT("K2:CallFunction:*:Random*"), true},
		{TEXT("K2:CallFunction:*:Add_*"), false},
		{TEXT("/Engine/EditorBlueprintResources/StandardMacros"), true},
	};
	for (const FCase& Case : Cases)
	{
		const FAssetDocumentResult Result = Find(Folder, Case.Uses);
		TestTrue(FString::Printf(TEXT("uses=%s succeeds"), Case.Uses), Result.IsSuccess());
		TestEqual(FString::Printf(TEXT("uses=%s finds the Blueprint"), Case.Uses), FindRow(Result, ObjectPath).IsValid(), Case.bFound);
	}
	const FAssetDocumentResult Unknown = Find(Folder, TEXT("/Script/Engine.NoSuchUsageSearchClass"));
	TestEqual(TEXT("an unknown path is an invalid query"), CodeOf(Unknown), FString(TEXT("Discovery.InvalidQuery")));
	TestEqual(TEXT("an empty term list is an invalid query"), CodeOf(Find(Folder, TEXT(" , |"))), FString(TEXT("Discovery.InvalidQuery")));

	// Unsaved changes: searched loaded, with the same keys.
	Blueprint->GetPackage()->SetDirtyFlag(true);
	const TSharedPtr<FJsonObject> LoadedRow = FindRow(Find(Folder, TEXT("K2:*")), ObjectPath);
	if (TestTrue(TEXT("the dirty Blueprint is found"), LoadedRow.IsValid()))
	{
		TestEqual(TEXT("searched loaded"), LoadedRow->GetStringField(TEXT("Source")), FString(TEXT("Loaded")));
		const TMap<FString, FString> LoadedNodes = RowNodes(LoadedRow);
		for (const TPair<FString, FString>& Live : LiveKeys)
		{
			TestEqual(FString::Printf(TEXT("loaded key of %s"), *Live.Key), LoadedNodes.FindRef(Live.Key), Live.Value);
		}
	}

	// Not loaded: the index alone.
	Unload(PackageName);
	TestFalse(TEXT("the Blueprint is unloaded"), Blueprint.IsValid());
	const TSharedPtr<FJsonObject> UnloadedRow = FindRow(Find(Folder, TEXT("K2:Add,/Script/Engine.KismetArrayLibrary:Array_Length")), ObjectPath);
	if (TestTrue(TEXT("the unloaded Blueprint is found"), UnloadedRow.IsValid()))
	{
		TestEqual(TEXT("unloaded: searched from the index"), UnloadedRow->GetStringField(TEXT("Source")), FString(TEXT("Index")));
	}
	TestNull(TEXT("searching did not load it"), FindObject<UBlueprint>(nullptr, *ObjectPath));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphUsageSearchNotIndexedTest,
	"UEAssetDoc.Graph.UsageSearch.NotIndexed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentGraphUsageSearchNotIndexedTest::RunTest(const FString&)
{
	using namespace AssetDocumentGraphUsageSearchTests;
	const FString PackageName = FString::Printf(TEXT("%s/BP_UsageNotIndexed_%s"), Folder, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
	ON_SCOPE_EXIT { RemoveSaved(PackageName); };
	IConsoleVariable* DisableIndex = IConsoleManager::Get().FindConsoleVariable(TEXT("bp.DisableSearchDataUpdateOnSave"));
	if (!TestNotNull(TEXT("bp.DisableSearchDataUpdateOnSave exists"), DisableIndex))
	{
		return false;
	}
	TMap<FString, FString> LiveKeys;
	{
		// The engine's own way to save a Blueprint without index data (as an older engine or a commandlet does).
		DisableIndex->Set(true, ECVF_SetByCode);
		ON_SCOPE_EXIT { DisableIndex->Set(false, ECVF_SetByCode); };
		const TCHAR* const Keys[] = {Branch};
		CreateSavedBlueprint(*this, PackageName, Keys, LiveKeys);
	}

	// Loaded: searched from its graphs.
	const TSharedPtr<FJsonObject> LoadedRow = FindRow(Find(Folder, TEXT("K2:Branch")), ObjectPath);
	if (TestTrue(TEXT("the loaded unindexed Blueprint is found"), LoadedRow.IsValid()))
	{
		AddInfo(FString::Printf(TEXT("loaded unindexed Blueprint searched from: %s"), *LoadedRow->GetStringField(TEXT("Source"))));
	}

	// Not loaded: reported, not guessed.
	Unload(PackageName);
	const FAssetDocumentResult Result = Find(Folder, TEXT("K2:Branch"));
	TestTrue(TEXT("search succeeds"), Result.IsSuccess());
	TestFalse(TEXT("the unindexed Blueprint is not a match"), FindRow(Result, ObjectPath).IsValid());
	const TSharedPtr<FJsonObject>* NotSearched = nullptr;
	if (TestTrue(TEXT("NotSearched is reported"), Result.Payload.IsValid() && Result.Payload->TryGetObjectField(TEXT("NotSearched"), NotSearched)))
	{
		bool bListed = false;
		for (const TSharedPtr<FJsonValue>& Value : (*NotSearched)->GetArrayField(TEXT("Assets")))
		{
			const TSharedPtr<FJsonObject> Row = Value->AsObject();
			if (Row->GetStringField(TEXT("ObjectPath")) == ObjectPath)
			{
				bListed = true;
				TestEqual(TEXT("reason"), Row->GetStringField(TEXT("Reason")), FString(TEXT("NotIndexed")));
			}
		}
		TestTrue(TEXT("the unindexed Blueprint is listed"), bListed);
		const TArray<TSharedPtr<FJsonValue>> Reasons = (*NotSearched)->GetArrayField(TEXT("Reasons"));
		TestTrue(TEXT("the reason is explained"), Reasons.Num() > 0 && !Reasons[0]->AsObject()->GetStringField(TEXT("Detail")).IsEmpty());
	}
	TestNull(TEXT("searching did not load it"), FindObject<UBlueprint>(nullptr, *ObjectPath));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphUsageSearchUnresolvedCallTest,
	"UEAssetDoc.Graph.UsageSearch.UnresolvedCall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentGraphUsageSearchUnresolvedCallTest::RunTest(const FString&)
{
	using namespace AssetDocumentGraphUsageSearchTests;
	const FString PackageName = FString::Printf(TEXT("%s/BP_UsageUnresolved_%s"), Folder, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
	ON_SCOPE_EXIT { RemoveSaved(PackageName); };
	TMap<FString, FString> LiveKeys;
	const TCHAR* const Keys[] = {RandomInteger};
	TWeakObjectPtr<UBlueprint> Blueprint = CreateSavedBlueprint(*this, PackageName, Keys, LiveKeys);
	if (!Blueprint.IsValid() || LiveKeys.Num() != 1)
	{
		return false;
	}
	// The call's function goes missing (as when its class drops it): the saved index records the node without its
	// function or owner.
	UK2Node_CallFunction* Call = nullptr;
	for (UEdGraphNode* Node : FBlueprintEditorUtils::FindEventGraph(Blueprint.Get())->Nodes)
	{
		Call = Call != nullptr ? Call : Cast<UK2Node_CallFunction>(Node);
	}
	if (!TestNotNull(TEXT("the call node"), Call))
	{
		return false;
	}
	Call->FunctionReference.SetExternalMember(TEXT("NoSuchUsageSearchFunction"), UKismetMathLibrary::StaticClass());
	TestNull(TEXT("its function does not resolve"), Call->GetTargetFunction());
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	const FString File = PackageFile(PackageName);
	TestTrue(TEXT("Blueprint saved again"), UPackage::SavePackage(Blueprint->GetPackage(), Blueprint.Get(), *File, Args));
	FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({File});
	Unload(PackageName);
	TestFalse(TEXT("the Blueprint is unloaded"), Blueprint.IsValid());

	const TSharedPtr<FJsonObject> Row = FindRow(Find(Folder, TEXT("K2:CallFunction")), ObjectPath);
	if (TestTrue(TEXT("the node is still a call node"), Row.IsValid()))
	{
		TestEqual(TEXT("searched from the index"), Row->GetStringField(TEXT("Source")), FString(TEXT("Index")));
		TArray<FString> IndexedKeys;
		RowNodes(Row).GenerateValueArray(IndexedKeys);
		AddInfo(FString::Printf(TEXT("indexed keys: %s"), *FString::Join(IndexedKeys, TEXT(", "))));
	}
	// A key whose function the index does not know matches no named function, owner or wildcard over them.
	for (const TCHAR* Uses : {RandomInteger, TEXT("K2:CallFunction:/Script/Engine.KismetSystemLibrary:PrintString"),
		TEXT("K2:CallFunction:/Script/Engine.KismetMathLibrary"), TEXT("K2:CallFunction:*:Random*"), TEXT("K2:CallFunction:*")})
	{
		const FAssetDocumentResult Result = Find(Folder, Uses);
		TestTrue(FString::Printf(TEXT("uses=%s succeeds"), Uses), Result.IsSuccess());
		TestFalse(FString::Printf(TEXT("uses=%s does not match the unresolved call"), Uses), FindRow(Result, ObjectPath).IsValid());
	}
	TestNull(TEXT("searching did not load it"), FindObject<UBlueprint>(nullptr, *ObjectPath));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphUsageSearchEngineTest,
	"UEAssetDoc.Graph.UsageSearch.Engine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentGraphUsageSearchEngineTest::RunTest(const FString&)
{
	using namespace AssetDocumentGraphUsageSearchTests;
	// A Blueprint using promotable operators and container (array / set / map) functions.
	const FString Uses = TEXT("K2:PromotableOperator,K2:CallArrayFunction|/Script/Engine.BlueprintSetLibrary|/Script/Engine.BlueprintMapLibrary");
	const double Start = FPlatformTime::Seconds();
	const FAssetDocumentResult Mover = Find(TEXT("/MoverExamples"), Uses);
	const double MoverSeconds = FPlatformTime::Seconds() - Start;
	TestTrue(TEXT("search succeeds"), Mover.IsSuccess());
	const TSharedPtr<FJsonObject> Pawn = FindRow(Mover, TEXT("/MoverExamples/Pawns/PathFollowingMannyPawn.PathFollowingMannyPawn"));
	if (TestTrue(TEXT("PathFollowingMannyPawn is found in one call"), Pawn.IsValid()))
	{
		const TMap<FString, FString> Nodes = RowNodes(Pawn);
		bool bOperator = false;
		bool bArray = false;
		for (const TPair<FString, FString>& Node : Nodes)
		{
			bOperator |= Node.Value.StartsWith(TEXT("K2:PromotableOperator:"));
			bArray |= Node.Value.StartsWith(TEXT("K2:CallArrayFunction:/Script/Engine.KismetArrayLibrary:"));
		}
		TestTrue(TEXT("its promotable operators and array functions are listed with their keys"), bOperator && bArray);
	}
	AddInfo(FString::Printf(TEXT("/MoverExamples: %d matches, %d searched, %.3f s"), Rows(Mover).Num(),
		Mover.Payload.IsValid() ? static_cast<int32>(Mover.Payload->GetNumberField(TEXT("Searched"))) : -1, MoverSeconds));

	// All engine and plugin content.
	const double AllStart = FPlatformTime::Seconds();
	const FAssetDocumentResult All = Find(FString(), Uses, TEXT("500"));
	const double AllSeconds = FPlatformTime::Seconds() - AllStart;
	TestTrue(TEXT("whole-content search succeeds"), All.IsSuccess());
	TestTrue(TEXT("whole-content search finds the trial Blueprint"),
		FindRow(All, TEXT("/MoverExamples/Pawns/PathFollowingMannyPawn.PathFollowingMannyPawn")).IsValid()
		|| (All.Payload.IsValid() && All.Payload->GetNumberField(TEXT("Total")) > 500));
	int32 NotSearched = 0;
	const TSharedPtr<FJsonObject>* NotSearchedObject = nullptr;
	if (All.Payload.IsValid() && All.Payload->TryGetObjectField(TEXT("NotSearched"), NotSearchedObject))
	{
		NotSearched = static_cast<int32>((*NotSearchedObject)->GetNumberField(TEXT("Total")));
	}
	AddInfo(FString::Printf(TEXT("all content: %d matches, %d searched, %d not searched, %.3f s"),
		All.Payload.IsValid() ? static_cast<int32>(All.Payload->GetNumberField(TEXT("Total"))) : -1,
		All.Payload.IsValid() ? static_cast<int32>(All.Payload->GetNumberField(TEXT("Searched"))) : -1, NotSearched, AllSeconds));
	// A term without an index prefilter (every node of every Blueprint is read).
	const double FullStart = FPlatformTime::Seconds();
	const FAssetDocumentResult Full = Find(FString(), TEXT("K2:*:Array_Length"), TEXT("500"));
	AddInfo(FString::Printf(TEXT("all content, unfiltered wildcard: %d matches, %.3f s"),
		Full.Payload.IsValid() ? static_cast<int32>(Full.Payload->GetNumberField(TEXT("Total"))) : -1, FPlatformTime::Seconds() - FullStart));

	// Blueprint families: each class searched from the index (counts per family, for the report).
	struct FFamily
	{
		const TCHAR* Class;
		const TCHAR* Uses;
		bool bRequired;
	};
	const FFamily Families[] = {
		{TEXT("/Script/Engine.AnimBlueprint"), TEXT("Anim:*"), true},
		{TEXT("/Script/UMGEditor.WidgetBlueprint"), TEXT("K2:*"), false},
		{TEXT("/Script/Blutility.EditorUtilityBlueprint"), TEXT("K2:*"), false},
		{TEXT("/Script/Blutility.EditorUtilityWidgetBlueprint"), TEXT("K2:*"), false},
		{TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint"), TEXT("ControlRigGraphNode"), false},
		{TEXT("/Script/Engine.World"), TEXT("K2:*"), false},
	};
	for (const FFamily& Family : Families)
	{
		if (FindObject<UClass>(nullptr, Family.Class) == nullptr)
		{
			AddInfo(FString::Printf(TEXT("%s: class not loaded"), Family.Class));
			continue;
		}
		const double FamilyStart = FPlatformTime::Seconds();
		const FAssetDocumentResult Result = FAssetDocumentService().FindAssets({{TEXT("class"), Family.Class}, {TEXT("uses"), Family.Uses}, {TEXT("limit"), TEXT("1")}});
		const int32 Total = Result.Payload.IsValid() ? static_cast<int32>(Result.Payload->GetNumberField(TEXT("Total"))) : -1;
		const TArray<TSharedPtr<FJsonObject>> First = Rows(Result);
		AddInfo(FString::Printf(TEXT("%s uses=%s: %d matches of %d searched (first %s from %s), %.3f s"), Family.Class, Family.Uses, Total,
			Result.Payload.IsValid() ? static_cast<int32>(Result.Payload->GetNumberField(TEXT("Searched"))) : -1,
			First.IsEmpty() ? TEXT("-") : *First[0]->GetStringField(TEXT("ObjectPath")), First.IsEmpty() ? TEXT("-") : *First[0]->GetStringField(TEXT("Source")),
			FPlatformTime::Seconds() - FamilyStart));
		if (Family.bRequired)
		{
			TestTrue(FString::Printf(TEXT("%s graphs are searched"), Family.Class), Total > 0);
		}
	}
	return true;
}
#endif
