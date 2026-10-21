// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Graphs/AssetDocumentNodeAdapter.h"

#include "EdGraph/EdGraphNode.h"
#include "UObject/Class.h"

namespace
{
FString GetClassPathName(UClass* Class)
{
	return Class ? Class->GetPathName() : FString();
}
}

FAssetDocumentCapabilityResult IAssetDocumentNodeAdapter::ConfigureNodeForApply(
	const FAssetDocumentNodeApplyContext& Context,
	UEdGraphNode*,
	const FAssetDocumentNodeSpec&) const
{
	return FAssetDocumentCapabilityResult::Failure(
		FString::Printf(TEXT("Graph node class '%s' is not supported for apply"), *GetClassPath()),
		Context.NodePath,
		TEXT("UnsupportedGraphNodeClass"));
}

bool IAssetDocumentNodeAdapter::DoesNodeMatchSpec(
	const UBlueprint*,
	const UEdGraphNode* Node,
	const FAssetDocumentNodeSpec& NodeSpec) const
{
	return Node && Node->GetClass()->GetPathName() == NodeSpec.Class;
}

FAssetDocumentCapabilityResult IAssetDocumentNodeAdapter::CanRepresentExistingNode(
	const FAssetDocumentNodeApplyContext& Context,
	const UEdGraphNode* Node) const
{
	if (Node && Node->GetClass()->GetPathName() == GetClassPath())
	{
		return FAssetDocumentCapabilityResult::Success();
	}
	return FAssetDocumentCapabilityResult::Failure(
		FString::Printf(TEXT("Existing graph node class '%s' is not represented by adapter '%s'"), *GetClassPathName(Node ? Node->GetClass() : nullptr), *GetClassPath()),
		Context.NodePath,
		TEXT("UnsupportedGraphNodeClass"));
}

TSharedRef<FJsonObject> FAssetDocumentUnsupportedNodeDiagnostic::ToJsonObject() const
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("Code"), Code);
	Object->SetStringField(TEXT("Path"), Path);
	Object->SetStringField(TEXT("Class"), Class);
	Object->SetStringField(TEXT("Capability"), Capability);
	if (Member.IsValid())
	{
		Object->SetObjectField(TEXT("Member"), Member);
	}
	else
	{
		Object->SetField(TEXT("Member"), MakeShared<FJsonValueNull>());
	}
	Object->SetStringField(TEXT("Reason"), Reason);
	Object->SetStringField(TEXT("SuggestedAction"), SuggestedAction);
	return Object;
}

void FAssetDocumentNodeAdapterRegistry::RegisterAdapter(const FString& ClassPath, TSharedRef<IAssetDocumentNodeAdapter> Adapter)
{
	if (!ClassPath.IsEmpty())
	{
		AdaptersByClassPath.Add(ClassPath, Adapter);
	}
}

void FAssetDocumentNodeAdapterRegistry::RegisterAdapter(UClass* Class, TSharedRef<IAssetDocumentNodeAdapter> Adapter)
{
	RegisterAdapter(GetClassPathName(Class), Adapter);
}

TSharedPtr<IAssetDocumentNodeAdapter> FAssetDocumentNodeAdapterRegistry::FindAdapter(const FString& ClassPath) const
{
	if (const TSharedPtr<IAssetDocumentNodeAdapter>* Adapter = AdaptersByClassPath.Find(ClassPath))
	{
		return *Adapter;
	}
	return nullptr;
}

TSharedPtr<IAssetDocumentNodeAdapter> FAssetDocumentNodeAdapterRegistry::FindAdapter(UClass* Class) const
{
	return FindAdapter(GetClassPathName(Class));
}

void FAssetDocumentNodeAdapterRegistry::Reset()
{
	AdaptersByClassPath.Reset();
}
