// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// An editor-made Blueprint holding an async action node placed from its palette action (Async Load Game From
// Slot): it reads with the palette's key (factory class and function), its own extract re-applies as a no-op, the
// older key form without the function still names the node, and a second async node placed by its key applies.

#include "AssetDocumentTolerantTestSupport.h"

#include "BlueprintActionDatabase.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintNodeBinder.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "GameFramework/AsyncActionHandleSaveGame.h"
#include "K2Node_AsyncAction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentTolerantBlueprintGraphTests
{
const TCHAR* const LoadKey = TEXT("K2:AsyncAction:/Script/Engine.AsyncActionHandleSaveGame:AsyncLoadGameFromSlot");
const TCHAR* const SaveKey = TEXT("K2:AsyncAction:/Script/Engine.AsyncActionHandleSaveGame:AsyncSaveGameToSlot");
const TCHAR* const ClassOnlyKey = TEXT("K2:AsyncAction:/Script/Engine.AsyncActionHandleSaveGame");

// The palette action the editor's context menu offers for a factory function of an async action class.
const UBlueprintNodeSpawner* FindFactoryAction(const UClass* FactoryClass, FName FactoryName)
{
	const UFunction* Factory = FactoryClass->FindFunctionByName(FactoryName);
	const FBlueprintActionDatabase::FActionList* Actions = FBlueprintActionDatabase::Get().GetAllActions().Find(FObjectKey(FactoryClass));
	if (Factory == nullptr || Actions == nullptr)
	{
		return nullptr;
	}
	for (const UBlueprintNodeSpawner* Spawner : *Actions)
	{
		const UBlueprintFunctionNodeSpawner* FunctionSpawner = Cast<UBlueprintFunctionNodeSpawner>(Spawner);
		if (FunctionSpawner != nullptr && FunctionSpawner->GetFunction() == Factory
			&& FunctionSpawner->NodeClass == UK2Node_AsyncAction::StaticClass())
		{
			return FunctionSpawner;
		}
	}
	return nullptr;
}

// The index of the node whose Action contains Needle (INDEX_NONE if none), with its page's Nodes and the page.
int32 FindNode(const TSharedPtr<FJsonObject>& Document, const FString& Needle, TArray<TSharedPtr<FJsonValue>>& OutNodes,
	TSharedPtr<FJsonObject>* OutPage = nullptr)
{
	OutNodes.Reset();
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Pages = nullptr;
	if (!Document.IsValid() || !Document->TryGetObjectField(TEXT("Body"), Body) || !(*Body)->TryGetArrayField(TEXT("UbergraphPages"), Pages))
	{
		return INDEX_NONE;
	}
	for (const TSharedPtr<FJsonValue>& Page : *Pages)
	{
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (Page->Type != EJson::Object || !Page->AsObject()->TryGetArrayField(TEXT("Nodes"), Nodes))
		{
			continue;
		}
		for (int32 Index = 0; Index < Nodes->Num(); ++Index)
		{
			FString Action;
			if ((*Nodes)[Index]->Type == EJson::Object && (*Nodes)[Index]->AsObject()->TryGetStringField(TEXT("Action"), Action)
				&& Action.Contains(Needle))
			{
				OutNodes = *Nodes;
				if (OutPage != nullptr)
				{
					*OutPage = Page->AsObject();
				}
				return Index;
			}
		}
	}
	return INDEX_NONE;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantBlueprintAsyncNodeTest, "UEAssetDoc.Tolerant.BlueprintAsyncNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantBlueprintAsyncNodeTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantBlueprintGraphTests;
	const FTarget Target(TEXT("BP_AsyncNode"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*Target.PackageName),
		FName(*Target.AssetName), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("blueprint created"), Blueprint))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(Blueprint);
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	const UBlueprintNodeSpawner* Spawner = FindFactoryAction(UAsyncActionHandleSaveGame::StaticClass(),
		GET_FUNCTION_NAME_CHECKED(UAsyncActionHandleSaveGame, AsyncLoadGameFromSlot));
	if (!TestNotNull(TEXT("event graph"), EventGraph) || !TestNotNull(TEXT("palette action of Async Load Game From Slot"), Spawner))
	{
		return false;
	}
	// Placed from the context menu, with a slot name typed into its pin.
	UEdGraphNode* Node = Spawner->Invoke(EventGraph, IBlueprintNodeBinder::FBindingSet(), FVector2D(400.0, 0.0));
	if (!TestNotNull(TEXT("async node placed"), Node))
	{
		return false;
	}
	if (UEdGraphPin* SlotPin = Node->FindPin(TEXT("SlotName")))
	{
		GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*SlotPin, TEXT("Slot1"));
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Async node"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Nodes;
	const int32 LoadIndex = FindNode(Extracted, TEXT("AsyncLoadGameFromSlot"), Nodes);
	FString Action;
	if (TestTrue(TEXT("the async node is extracted"), LoadIndex != INDEX_NONE))
	{
		Nodes[LoadIndex]->AsObject()->TryGetStringField(TEXT("Action"), Action);
	}
	TestEqual(TEXT("the async node reads with the palette key"), Action, FString(LoadKey));

	// A document written with the older key form (the factory class alone) names the same node: no change.
	{
		const TSharedPtr<FJsonObject> Older = Parse(Serialize(Extracted));
		Older->SetStringField(TEXT("Action"), TEXT("Update"));
		TArray<TSharedPtr<FJsonValue>> OlderNodes;
		const int32 Index = FindNode(Older, TEXT("AsyncLoadGameFromSlot"), OlderNodes);
		if (TestTrue(TEXT("older form: node found"), Index != INDEX_NONE))
		{
			OlderNodes[Index]->AsObject()->SetStringField(TEXT("Action"), ClassOnlyKey);
			const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Target.PackageFile);
			const FAssetDocumentResult Applied = ApplyText(*this, Target, Serialize(Older), TEXT("Older key form"));
			TestTrue(TEXT("older form: applies"), Applied.IsSuccess());
			TestFalse(TEXT("older form: saves nothing"), Applied.bSavedAsset);
			TestEqual(TEXT("older form: package file untouched"), IFileManager::Get().GetTimeStamp(*Target.PackageFile), Stamp);
		}
	}

	// A second async node placed by its palette key applies and passes the post-apply check.
	const FAssetDocumentResult Added = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		TArray<TSharedPtr<FJsonValue>> PageNodes;
		TSharedPtr<FJsonObject> Page;
		if (FindNode(Document, TEXT("AsyncLoadGameFromSlot"), PageNodes, &Page) != INDEX_NONE)
		{
			const TSharedPtr<FJsonObject> Save = MakeShared<FJsonObject>();
			Save->SetStringField(TEXT("Id"), TEXT("SaveGame"));
			Save->SetStringField(TEXT("Action"), SaveKey);
			PageNodes.Add(MakeShared<FJsonValueObject>(Save));
			Page->SetArrayField(TEXT("Nodes"), PageNodes);
		}
	}, TEXT("Add async node"));
	if (TestTrue(TEXT("a second async node applies"), Added.IsSuccess()))
	{
		const FAssetDocumentResult Reread = ExtractResult(Target);
		TArray<TSharedPtr<FJsonValue>> Unused;
		TestTrue(TEXT("both async nodes read back"), Reread.IsSuccess()
			&& FindNode(Reread.Payload, TEXT("AsyncLoadGameFromSlot"), Unused) != INDEX_NONE
			&& FindNode(Reread.Payload, TEXT("AsyncSaveGameToSlot"), Unused) != INDEX_NONE);
	}
	return !HasAnyErrors();
}

#endif
