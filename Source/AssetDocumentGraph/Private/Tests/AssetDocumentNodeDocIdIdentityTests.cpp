// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// An editor-made Blueprint with two Print String nodes, the second disabled (Details > Enabled State, which the
// document does not carry). Once an apply has recorded each node's document Id, a document written without
// NodeGuids still names each node by its Id: applying it changes nothing and an edit reaches the right node, which
// keeps its disabled state. Nodes that only their action could name are replaced, with a warning.

#include "AssetDocumentTolerantTestSupport.h"

#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintNodeBinder.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentNodeDocIdIdentityTests
{
// The node objects of every Ubergraph page.
TArray<TSharedPtr<FJsonObject>> NodeDocIdNodes(const TSharedPtr<FJsonObject>& Document)
{
	TArray<TSharedPtr<FJsonObject>> Out;
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Pages = nullptr;
	if (!Document.IsValid() || !Document->TryGetObjectField(TEXT("Body"), Body) || !(*Body)->TryGetArrayField(TEXT("UbergraphPages"), Pages))
	{
		return Out;
	}
	for (const TSharedPtr<FJsonValue>& Page : *Pages)
	{
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (Page->Type != EJson::Object || !Page->AsObject()->TryGetArrayField(TEXT("Nodes"), Nodes))
		{
			continue;
		}
		for (const TSharedPtr<FJsonValue>& Node : *Nodes)
		{
			if (Node->Type == EJson::Object)
			{
				Out.Add(Node->AsObject());
			}
		}
	}
	return Out;
}

// The Print String node whose InString override is Text, or null.
TSharedPtr<FJsonObject> NodeDocIdPrint(const TSharedPtr<FJsonObject>& Document, const FString& Text)
{
	for (const TSharedPtr<FJsonObject>& Node : NodeDocIdNodes(Document))
	{
		FString Action;
		const TArray<TSharedPtr<FJsonValue>>* Overrides = nullptr;
		if (!Node->TryGetStringField(TEXT("Action"), Action) || Action != TEXT("K2:PrintString")
			|| !Node->TryGetArrayField(TEXT("PinOverrides"), Overrides))
		{
			continue;
		}
		for (const TSharedPtr<FJsonValue>& Override : *Overrides)
		{
			FString Pin;
			FString Value;
			if (Override->Type == EJson::Object && Override->AsObject()->TryGetStringField(TEXT("Pin"), Pin) && Pin == TEXT("InString")
				&& Override->AsObject()->TryGetStringField(TEXT("DefaultValue"), Value) && Value == Text)
			{
				return Node;
			}
		}
	}
	return nullptr;
}

void NodeDocIdSetText(const TSharedPtr<FJsonObject>& Node, const TCHAR* Text)
{
	TArray<TSharedPtr<FJsonValue>> Overrides;
	const TSharedPtr<FJsonObject> Override = MakeShared<FJsonObject>();
	Override->SetStringField(TEXT("Pin"), TEXT("InString"));
	Override->SetStringField(TEXT("DefaultValue"), Text);
	Overrides.Add(MakeShared<FJsonValueObject>(Override));
	Node->SetArrayField(TEXT("PinOverrides"), Overrides);
}

// The extract as an Update with every NodeGuid left out, after Edit.
FString NodeDocIdWithoutGuids(const TSharedPtr<FJsonObject>& Extracted, TFunctionRef<void(const TSharedPtr<FJsonObject>&)> Edit)
{
	using namespace AssetDocumentTolerantTestSupport;
	const TSharedPtr<FJsonObject> Document = Parse(Serialize(Extracted));
	Document->SetStringField(TEXT("Action"), TEXT("Update"));
	for (const TSharedPtr<FJsonObject>& Node : NodeDocIdNodes(Document))
	{
		Node->RemoveField(TEXT("NodeGuid"));
	}
	Edit(Document);
	return Serialize(Document);
}

UEdGraphNode* NodeDocIdFind(const AssetDocumentTolerantTestSupport::FTarget& Target, const FGuid& Guid)
{
	UBlueprint* Blueprint = Target.Get<UBlueprint>();
	UEdGraph* Graph = Blueprint != nullptr ? FBlueprintEditorUtils::FindEventGraph(Blueprint) : nullptr;
	if (Graph == nullptr)
	{
		return nullptr;
	}
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node != nullptr && Node->NodeGuid == Guid)
		{
			return Node;
		}
	}
	return nullptr;
}

FString NodeDocIdText(const UEdGraphNode* Node)
{
	const UEdGraphPin* Pin = Node != nullptr ? Node->FindPin(TEXT("InString")) : nullptr;
	return Pin != nullptr ? Pin->DefaultValue : FString();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNodeDocIdIdentityTest, "UEAssetDoc.Codec.Blueprint.NodeDocIdIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNodeDocIdIdentityTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentNodeDocIdIdentityTests;
	const FTarget Target(TEXT("BP_NodeDocId"));
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
	const UFunction* PrintFunction = UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString));
	if (!TestNotNull(TEXT("event graph"), EventGraph) || !TestNotNull(TEXT("Print String"), PrintFunction))
	{
		return false;
	}
	// Two Print String nodes placed from the palette; the second is disabled in its Details.
	const TStrongObjectPtr<UBlueprintFunctionNodeSpawner> Spawner(UBlueprintFunctionNodeSpawner::Create(PrintFunction));
	UEdGraphNode* First = Spawner->Invoke(EventGraph, IBlueprintNodeBinder::FBindingSet(), FVector2D(400.0, 0.0));
	UEdGraphNode* Second = Spawner->Invoke(EventGraph, IBlueprintNodeBinder::FBindingSet(), FVector2D(400.0, 300.0));
	if (!TestNotNull(TEXT("first Print String placed"), First) || !TestNotNull(TEXT("second Print String placed"), Second))
	{
		return false;
	}
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	if (UEdGraphPin* Pin = First->FindPin(TEXT("InString")))
	{
		Schema->TrySetDefaultValue(*Pin, TEXT("First"));
	}
	if (UEdGraphPin* Pin = Second->FindPin(TEXT("InString")))
	{
		Schema->TrySetDefaultValue(*Pin, TEXT("Second"));
	}
	Second->SetEnabledState(ENodeEnabledState::Disabled);
	const FGuid FirstGuid = First->NodeGuid;
	const FGuid SecondGuid = Second->NodeGuid;
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Two Print String nodes"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestTrue(TEXT("both Print String nodes are extracted"),
		NodeDocIdPrint(Extracted, TEXT("First")).IsValid() && NodeDocIdPrint(Extracted, TEXT("Second")).IsValid());

	// An apply that changes the graph records each node's document Id.
	const FAssetDocumentResult Recorded = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		if (const TSharedPtr<FJsonObject> Node = NodeDocIdPrint(Document, TEXT("First")))
		{
			NodeDocIdSetText(Node, TEXT("First edited"));
		}
	}, TEXT("Edit the first node"));
	if (!TestTrue(TEXT("the edit applies"), Recorded.IsSuccess()))
	{
		return false;
	}
	TestEqual(TEXT("the edit reached the first node"), NodeDocIdText(NodeDocIdFind(Target, FirstGuid)), FString(TEXT("First edited")));
	const FAssetDocumentResult Reread = ExtractResult(Target);
	if (!TestTrue(TEXT("extract after the edit"), Reread.IsSuccess() && Reread.Payload.IsValid()))
	{
		return false;
	}

	// Without NodeGuids the recorded Ids name both nodes: nothing changes and nothing is replaced.
	{
		const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Target.PackageFile);
		const FAssetDocumentResult Applied = ApplyText(*this, Target,
			NodeDocIdWithoutGuids(Reread.Payload, [](const TSharedPtr<FJsonObject>&) {}), TEXT("Without NodeGuids"));
		TestTrue(TEXT("without NodeGuids: applies"), Applied.IsSuccess());
		TestFalse(TEXT("without NodeGuids: saves nothing"), Applied.bSavedAsset);
		TestEqual(TEXT("without NodeGuids: package file untouched"), IFileManager::Get().GetTimeStamp(*Target.PackageFile), Stamp);
		TestFalse(TEXT("without NodeGuids: no node is replaced"), HasDiagnostic(Applied, TEXT("Graph.NodeRecreated"), FString()));
		const UEdGraphNode* Kept = NodeDocIdFind(Target, SecondGuid);
		TestTrue(TEXT("without NodeGuids: the disabled node is kept, still disabled"),
			Kept != nullptr && Kept->GetDesiredEnabledState() == ENodeEnabledState::Disabled);
	}

	// An edit of the second node without NodeGuids reaches that node, which stays disabled.
	{
		const FAssetDocumentResult Applied = ApplyText(*this, Target, NodeDocIdWithoutGuids(Reread.Payload, [](const TSharedPtr<FJsonObject>& Document)
		{
			if (const TSharedPtr<FJsonObject> Node = NodeDocIdPrint(Document, TEXT("Second")))
			{
				NodeDocIdSetText(Node, TEXT("Second edited"));
			}
		}), TEXT("Edit the disabled node without NodeGuids"));
		TestTrue(TEXT("edit without NodeGuids: applies"), Applied.IsSuccess());
		const UEdGraphNode* Edited = NodeDocIdFind(Target, SecondGuid);
		TestTrue(TEXT("edit without NodeGuids: the same node is edited"), Edited != nullptr);
		TestEqual(TEXT("edit without NodeGuids: its text"), NodeDocIdText(Edited), FString(TEXT("Second edited")));
		TestTrue(TEXT("edit without NodeGuids: still disabled"),
			Edited != nullptr && Edited->GetDesiredEnabledState() == ENodeEnabledState::Disabled);
		TestEqual(TEXT("edit without NodeGuids: the other node is untouched"), NodeDocIdText(NodeDocIdFind(Target, FirstGuid)),
			FString(TEXT("First edited")));
	}

	// Ids the nodes were never applied with, with new texts, leave only the action, which both share: they are
	// replaced, with a warning. (New Ids alone change nothing: the document then holds what the graph holds.)
	{
		const FAssetDocumentResult Reread2 = ExtractResult(Target);
		if (!TestTrue(TEXT("extract after the second edit"), Reread2.IsSuccess() && Reread2.Payload.IsValid()))
		{
			return false;
		}
		const FAssetDocumentResult Applied = ApplyText(*this, Target, NodeDocIdWithoutGuids(Reread2.Payload, [](const TSharedPtr<FJsonObject>& Document)
		{
			if (const TSharedPtr<FJsonObject> Node = NodeDocIdPrint(Document, TEXT("First edited")))
			{
				Node->SetStringField(TEXT("Id"), TEXT("RenamedFirst"));
				NodeDocIdSetText(Node, TEXT("First renamed"));
			}
			if (const TSharedPtr<FJsonObject> Node = NodeDocIdPrint(Document, TEXT("Second edited")))
			{
				Node->SetStringField(TEXT("Id"), TEXT("RenamedSecond"));
				NodeDocIdSetText(Node, TEXT("Second renamed"));
			}
		}), TEXT("Renamed Ids without NodeGuids"));
		TestTrue(TEXT("renamed Ids: applies"), Applied.IsSuccess());
		TestTrue(TEXT("renamed Ids: the replaced nodes are reported"), HasDiagnostic(Applied, TEXT("Graph.NodeRecreated"), FString(), TEXT("warning")));
	}
	return !HasAnyErrors();
}

#endif
