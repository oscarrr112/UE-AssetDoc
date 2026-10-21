// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Graphs/K2NodeAdapters/SelfNodeAdapter.h"

#include "K2Node_Self.h"

FAssetDocumentCapabilityResult FAssetDocumentK2SelfNodeAdapter::ConfigureNodeForApply(
	const FAssetDocumentNodeApplyContext& Context,
	UEdGraphNode* Node,
	const FAssetDocumentNodeSpec& NodeSpec) const
{
	if (!Cast<UK2Node_Self>(Node))
	{
		return FAssetDocumentCapabilityResult::Failure(
			FString::Printf(TEXT("Graph node '%s' is not a K2 self node"), *NodeSpec.Id),
			Context.NodePath,
			TEXT("UnsupportedGraphNodeClass"));
	}
	return FAssetDocumentCapabilityResult::Success();
}

bool FAssetDocumentK2SelfNodeAdapter::DoesNodeMatchSpec(
	const UBlueprint*,
	const UEdGraphNode* Node,
	const FAssetDocumentNodeSpec& NodeSpec) const
{
	return Node && Node->IsA<UK2Node_Self>() && Node->GetClass()->GetPathName() == NodeSpec.Class;
}

bool FAssetDocumentK2SelfNodeAdapter::ExtractNode(const UBlueprint*, const UK2Node_Self* Node, FAssetDocumentNodeSpec& OutNode) const
{
	if (!Node)
	{
		return false;
	}

	OutNode.Capability = GetCapability();
	return true;
}
