// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Graphs/AssetDocumentNodeAdapter.h"

class UBlueprint;
class UK2Node_CallFunction;

class FAssetDocumentK2CallFunctionNodeAdapter final : public IAssetDocumentNodeAdapter
{
public:
	virtual FString GetClassPath() const override { return TEXT("/Script/BlueprintGraph.K2Node_CallFunction"); }
	virtual FString GetCapability() const override { return TEXT("CallFunction"); }
	virtual FAssetDocumentCapabilityResult ConfigureNodeForApply(
		const FAssetDocumentNodeApplyContext& Context,
		UEdGraphNode* Node,
		const FAssetDocumentNodeSpec& NodeSpec) const override;
	virtual bool DoesNodeMatchSpec(
		const UBlueprint* Blueprint,
		const UEdGraphNode* Node,
		const FAssetDocumentNodeSpec& NodeSpec) const override;
	virtual FAssetDocumentCapabilityResult CanRepresentExistingNode(
		const FAssetDocumentNodeApplyContext& Context,
		const UEdGraphNode* Node) const override;

	bool ExtractNode(const UBlueprint* Blueprint, const UK2Node_CallFunction* Node, FAssetDocumentNodeSpec& OutNode) const;
};
