// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentGraphAdapters.h"

#include "AssetDocumentGraphPropertyCodec.h"
#include "Engine/Blueprint.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphSchema.h"

namespace UE::AssetDocument::Graph::Registry
{
TArray<TSharedRef<IAssetDocumentGraphSchemaAdapter>>& SchemaAdapters()
{
	static TArray<TSharedRef<IAssetDocumentGraphSchemaAdapter>> Adapters;
	return Adapters;
}

TArray<TSharedRef<IAssetDocumentGraphHostAdapter>>& HostAdapters()
{
	static TArray<TSharedRef<IAssetDocumentGraphHostAdapter>> Adapters;
	return Adapters;
}

TArray<TSharedRef<IAssetDocumentBlueprintSettingsAdapter>>& SettingsAdapters()
{
	static TArray<TSharedRef<IAssetDocumentBlueprintSettingsAdapter>> Adapters;
	return Adapters;
}

TArray<TSharedRef<IAssetDocumentBlueprintRebuildAdapter>>& RebuildAdapters()
{
	static TArray<TSharedRef<IAssetDocumentBlueprintRebuildAdapter>> Adapters;
	return Adapters;
}

TArray<TSharedRef<IAssetDocumentBoundEventSourceAdapter>>& BoundEventSourceAdapters()
{
	static TArray<TSharedRef<IAssetDocumentBoundEventSourceAdapter>> Adapters;
	return Adapters;
}

template <typename AdapterType>
TArray<TSharedRef<AdapterType>> FindAllMatching(
	const TArray<TSharedRef<AdapterType>>& Adapters, const UBlueprint* Blueprint)
{
	TArray<TSharedRef<AdapterType>> Matching;
	if (Blueprint == nullptr)
	{
		return Matching;
	}
	for (const TSharedRef<AdapterType>& Adapter : Adapters)
	{
		const UClass* Handled = Adapter->GetBlueprintClass();
		if (Handled != nullptr && Blueprint->IsA(Handled))
		{
			Matching.Add(Adapter);
		}
	}
	return Matching;
}

int32 InheritanceDepth(const UClass* Class)
{
	int32 Depth = 0;
	for (const UClass* It = Class; It != nullptr; It = It->GetSuperClass())
	{
		++Depth;
	}
	return Depth;
}

template <typename AdapterType>
TSharedPtr<AdapterType> FindDeepest(
	const TArray<TSharedRef<AdapterType>>& Adapters,
	const UClass* Class,
	UClass* (AdapterType::*GetClass)() const)
{
	TSharedPtr<AdapterType> Best;
	int32 BestDepth = -1;
	if (Class == nullptr)
	{
		return Best;
	}
	// Later registrations win ties so a module can override a built-in adapter.
	for (const TSharedRef<AdapterType>& Adapter : Adapters)
	{
		const UClass* Handled = ((*Adapter).*GetClass)();
		if (Handled == nullptr || !Class->IsChildOf(Handled))
		{
			continue;
		}
		const int32 Depth = InheritanceDepth(Handled);
		if (Depth >= BestDepth)
		{
			Best = Adapter;
			BestDepth = Depth;
		}
	}
	return Best;
}
} // namespace UE::AssetDocument::Graph::Registry

void RegisterAssetDocumentGraphSchemaAdapter(TSharedRef<IAssetDocumentGraphSchemaAdapter> Adapter)
{
	check(IsInGameThread());
	UE::AssetDocument::Graph::Registry::SchemaAdapters().AddUnique(Adapter);
}

void UnregisterAssetDocumentGraphSchemaAdapter(const TSharedRef<IAssetDocumentGraphSchemaAdapter>& Adapter)
{
	UE::AssetDocument::Graph::Registry::SchemaAdapters().Remove(Adapter);
}

void RegisterAssetDocumentGraphHostAdapter(TSharedRef<IAssetDocumentGraphHostAdapter> Adapter)
{
	check(IsInGameThread());
	UE::AssetDocument::Graph::Registry::HostAdapters().AddUnique(Adapter);
}

void UnregisterAssetDocumentGraphHostAdapter(const TSharedRef<IAssetDocumentGraphHostAdapter>& Adapter)
{
	UE::AssetDocument::Graph::Registry::HostAdapters().Remove(Adapter);
}

void RegisterAssetDocumentBlueprintSettingsAdapter(TSharedRef<IAssetDocumentBlueprintSettingsAdapter> Adapter)
{
	check(IsInGameThread());
	UE::AssetDocument::Graph::Registry::SettingsAdapters().AddUnique(Adapter);
}

void UnregisterAssetDocumentBlueprintSettingsAdapter(const TSharedRef<IAssetDocumentBlueprintSettingsAdapter>& Adapter)
{
	UE::AssetDocument::Graph::Registry::SettingsAdapters().Remove(Adapter);
}

void RegisterAssetDocumentBlueprintRebuildAdapter(TSharedRef<IAssetDocumentBlueprintRebuildAdapter> Adapter)
{
	check(IsInGameThread());
	UE::AssetDocument::Graph::Registry::RebuildAdapters().AddUnique(Adapter);
}

void UnregisterAssetDocumentBlueprintRebuildAdapter(const TSharedRef<IAssetDocumentBlueprintRebuildAdapter>& Adapter)
{
	UE::AssetDocument::Graph::Registry::RebuildAdapters().Remove(Adapter);
}

void RegisterAssetDocumentBoundEventSourceAdapter(TSharedRef<IAssetDocumentBoundEventSourceAdapter> Adapter)
{
	check(IsInGameThread());
	UE::AssetDocument::Graph::Registry::BoundEventSourceAdapters().AddUnique(Adapter);
}

void UnregisterAssetDocumentBoundEventSourceAdapter(const TSharedRef<IAssetDocumentBoundEventSourceAdapter>& Adapter)
{
	UE::AssetDocument::Graph::Registry::BoundEventSourceAdapters().Remove(Adapter);
}

TArray<TSharedRef<IAssetDocumentBlueprintRebuildAdapter>> FindAssetDocumentBlueprintRebuildAdapters(const UBlueprint* Blueprint)
{
	return UE::AssetDocument::Graph::Registry::FindAllMatching(
		UE::AssetDocument::Graph::Registry::RebuildAdapters(), Blueprint);
}

TArray<TSharedRef<IAssetDocumentBlueprintRebuildAdapter>> GetAssetDocumentBlueprintRebuildAdapters()
{
	return UE::AssetDocument::Graph::Registry::RebuildAdapters();
}

TArray<TSharedRef<IAssetDocumentBoundEventSourceAdapter>> FindAssetDocumentBoundEventSourceAdapters(const UBlueprint* Blueprint)
{
	return UE::AssetDocument::Graph::Registry::FindAllMatching(
		UE::AssetDocument::Graph::Registry::BoundEventSourceAdapters(), Blueprint);
}

TSharedPtr<IAssetDocumentBlueprintSettingsAdapter> FindAssetDocumentBlueprintSettingsAdapter(const UBlueprint* Blueprint)
{
	return Blueprint == nullptr ? nullptr
		: UE::AssetDocument::Graph::Registry::FindDeepest(
			UE::AssetDocument::Graph::Registry::SettingsAdapters(), Blueprint->GetClass(),
			&IAssetDocumentBlueprintSettingsAdapter::GetBlueprintClass);
}

FAssetDocumentRuntimeValueDto EncodeAssetDocumentGraphPropertyValue(const FProperty& Property, const void* ValuePtr)
{
	return UE::AssetDocument::Graph::EncodePropertyValue(Property, ValuePtr);
}

bool DecodeAssetDocumentGraphPropertyValue(
	const FProperty& Property, void* ValuePtr, const FAssetDocumentRuntimeValueDto& Value, FString& OutError)
{
	return UE::AssetDocument::Graph::DecodePropertyValue(Property, ValuePtr, Value, OutError);
}

TSharedPtr<IAssetDocumentGraphSchemaAdapter> FindAssetDocumentGraphSchemaAdapter(const UEdGraphSchema* Schema)
{
	return Schema == nullptr ? nullptr
		: UE::AssetDocument::Graph::Registry::FindDeepest(
			UE::AssetDocument::Graph::Registry::SchemaAdapters(), Schema->GetClass(),
			&IAssetDocumentGraphSchemaAdapter::GetSchemaClass);
}

TSharedPtr<IAssetDocumentGraphHostAdapter> FindAssetDocumentGraphHostAdapter(const UObject* Asset)
{
	return Asset == nullptr ? nullptr
		: UE::AssetDocument::Graph::Registry::FindDeepest(
			UE::AssetDocument::Graph::Registry::HostAdapters(), Asset->GetClass(),
			&IAssetDocumentGraphHostAdapter::GetAssetClass);
}

// ---- Default adapter behavior ------------------------------------------------

UObject* IAssetDocumentGraphSchemaAdapter::ResolvePropertyContainer(UEdGraphNode& Node, FStringView Container) const
{
	return Container == TEXTVIEW("node") ? &Node : nullptr;
}

FString IAssetDocumentGraphSchemaAdapter::ClassifySubgraph(const UEdGraph& Subgraph, const UEdGraphNode& OwnerNode) const
{
	return TEXT("other");
}

void IAssetDocumentGraphSchemaAdapter::GetOwnedSubgraphs(const UEdGraphNode& Node, TArray<UEdGraph*>& OutGraphs) const
{
	OutGraphs = Node.GetSubGraphs();
}

bool IAssetDocumentGraphSchemaAdapter::SetUserPins(
	UEdGraphNode& Node, TConstArrayView<FAssetDocumentGraphUserPin> Pins, FAssetDocumentGraphError& OutError) const
{
	OutError.Set(TEXT("Graph.UserPinsUnsupported"),
		FString::Printf(TEXT("Node class %s has no user-defined pins."), *Node.GetClass()->GetName()));
	return false;
}

bool IAssetDocumentGraphSchemaAdapter::CanAddDynamicPin(const UEdGraphNode& Node) const
{
	return false;
}

bool IAssetDocumentGraphSchemaAdapter::AddDynamicPin(UEdGraphNode& Node, FAssetDocumentGraphError& OutError) const
{
	OutError.Set(TEXT("Graph.DynamicPinUnsupported"),
		FString::Printf(TEXT("Node class %s does not support adding pins."), *Node.GetClass()->GetName()));
	return false;
}

bool IAssetDocumentGraphSchemaAdapter::RemoveDynamicPin(
	UEdGraphNode& Node, UEdGraphPin* Pin, FAssetDocumentGraphError& OutError) const
{
	OutError.Set(TEXT("Graph.DynamicPinUnsupported"),
		FString::Printf(TEXT("Node class %s does not support removing pins."), *Node.GetClass()->GetName()));
	return false;
}

bool IAssetDocumentGraphSchemaAdapter::SetPinType(
	UEdGraphNode& Node, UEdGraphPin& Pin, const FEdGraphPinType& Type, FAssetDocumentGraphError& OutError) const
{
	OutError.Set(TEXT("Graph.PinTypeNotEditable"), FString::Printf(TEXT("Pin '%s' of %s has no editable type."),
		*Pin.PinName.ToString(), *Node.GetClass()->GetName()), TEXT("/pin_name"));
	return false;
}

bool IAssetDocumentGraphSchemaAdapter::ConnectPins(UEdGraphPin& A, UEdGraphPin& B,
	const TMap<FName, FEdGraphPinType>* KeepTypesA, const TMap<FName, FEdGraphPinType>* KeepTypesB) const
{
	(void)KeepTypesA;
	(void)KeepTypesB;
	const UEdGraph* Graph = A.GetOwningNode()->GetGraph();
	return Graph != nullptr && Graph->GetSchema() != nullptr && Graph->GetSchema()->TryCreateConnection(&A, &B);
}

bool IAssetDocumentGraphSchemaAdapter::SetFunctionFlags(
	UObject& Asset, UEdGraph& Graph, const FAssetDocumentRuntimeValueDto& Flags,
	FAssetDocumentRuntimeValueDto& OutFlags, FAssetDocumentGraphError& OutError) const
{
	OutError.Set(TEXT("Graph.GraphOperationUnsupported"),
		FString::Printf(TEXT("Graph %s has no function signature flags."), *Graph.GetName()));
	return false;
}

bool IAssetDocumentGraphSchemaAdapter::SetNodeSetting(
	UObject& Asset, UEdGraphNode& Node, FStringView Path, const FAssetDocumentRuntimeValueDto& Value,
	bool& bOutHandled, FAssetDocumentGraphError& OutError) const
{
	bOutHandled = false;
	return true;
}

bool IAssetDocumentGraphSchemaAdapter::MapNodeSetting(
	const UEdGraphNode& Node, FString& InOutPath, FAssetDocumentRuntimeValueDto& InOutValue,
	FAssetDocumentGraphError& OutError) const
{
	return true;
}

FString IAssetDocumentGraphSchemaAdapter::ValidatePinDefault(
	const UEdGraphPin& Pin, const FString& Value, UObject* Object, const FText& Text) const
{
	const UEdGraphSchema* Schema = Pin.GetSchema();
	return Schema != nullptr ? Schema->IsPinDefaultValid(&Pin, Value, Object, Text) : FString(TEXT("The pin has no schema."));
}

UEdGraphPin* IAssetDocumentGraphSchemaAdapter::ResolvePinAlias(UEdGraphNode& Node, FName Name, EEdGraphPinDirection Direction) const
{
	return nullptr;
}

bool IAssetDocumentGraphSchemaAdapter::DescribeConversion(
	const UEdGraphPin& Output, const UEdGraphPin& Input, FAssetDocumentGraphConversion& OutConversion) const
{
	return false;
}

bool IAssetDocumentGraphSchemaAdapter::CheckActionKey(
	UObject& Asset, UEdGraph& Graph, const FString& Key, FAssetDocumentGraphError& OutError) const
{
	return true;
}

void IAssetDocumentGraphSchemaAdapter::GetTemplateNodes(
	UObject& Asset, UEdGraph& Graph, TConstArrayView<FString> Keys, TMap<FString, UEdGraphNode*>& OutNodes) const
{
}

bool IAssetDocumentGraphSchemaAdapter::RenameNode(
	UObject& Asset, UEdGraphNode& Node, const FString& Name, bool& bOutHandled, FAssetDocumentGraphError& OutError) const
{
	bOutHandled = false;
	return true;
}

bool IAssetDocumentGraphHostAdapter::CreateGraph(
	UObject& Asset, FStringView Role, FStringView Name, UEdGraph* ParentGraph, UEdGraph*& OutGraph,
	FAssetDocumentGraphError& OutError) const
{
	OutError.Set(TEXT("Graph.GraphOperationUnsupported"),
		FString::Printf(TEXT("%s does not support creating graphs."), *Asset.GetClass()->GetName()));
	return false;
}

bool IAssetDocumentGraphHostAdapter::RemoveGraph(UObject& Asset, UEdGraph& Graph, FAssetDocumentGraphError& OutError) const
{
	OutError.Set(TEXT("Graph.GraphOperationUnsupported"),
		FString::Printf(TEXT("%s does not support removing graphs."), *Asset.GetClass()->GetName()));
	return false;
}

bool IAssetDocumentGraphHostAdapter::RenameGraph(
	UObject& Asset, UEdGraph& Graph, FStringView NewName, FAssetDocumentGraphError& OutError) const
{
	OutError.Set(TEXT("Graph.GraphOperationUnsupported"),
		FString::Printf(TEXT("%s does not support renaming graphs."), *Asset.GetClass()->GetName()));
	return false;
}

void IAssetDocumentGraphHostAdapter::NotifyGraphChanged(UObject& Asset, UEdGraph* Graph, bool bStructural) const
{
	if (Graph != nullptr)
	{
		Graph->NotifyGraphChanged();
	}
	Asset.MarkPackageDirty();
}
