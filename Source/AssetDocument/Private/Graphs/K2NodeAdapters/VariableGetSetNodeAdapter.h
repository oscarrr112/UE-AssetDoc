// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Graphs/AssetDocumentNodeAdapter.h"

class UBlueprint;
class UK2Node_VariableGet;
class UK2Node_VariableSet;

class FAssetDocumentK2VariableGetNodeAdapter final : public IAssetDocumentNodeAdapter
{
public:
	virtual FString GetClassPath() const override { return TEXT("/Script/BlueprintGraph.K2Node_VariableGet"); }
	virtual FString GetCapability() const override { return TEXT("VariableGet"); }
	virtual FAssetDocumentCapabilityResult ConfigureNodeForApply(
		const FAssetDocumentNodeApplyContext& Context,
		UEdGraphNode* Node,
		const FAssetDocumentNodeSpec& NodeSpec) const override;
	virtual bool DoesNodeMatchSpec(
		const UBlueprint* Blueprint,
		const UEdGraphNode* Node,
		const FAssetDocumentNodeSpec& NodeSpec) const override;

	bool ExtractNode(const UBlueprint* Blueprint, const UK2Node_VariableGet* Node, FAssetDocumentNodeSpec& OutNode) const;
};

class FAssetDocumentK2VariableSetNodeAdapter final : public IAssetDocumentNodeAdapter
{
public:
	virtual FString GetClassPath() const override { return TEXT("/Script/BlueprintGraph.K2Node_VariableSet"); }
	virtual FString GetCapability() const override { return TEXT("VariableSet"); }
	virtual FAssetDocumentCapabilityResult ConfigureNodeForApply(
		const FAssetDocumentNodeApplyContext& Context,
		UEdGraphNode* Node,
		const FAssetDocumentNodeSpec& NodeSpec) const override;
	virtual bool DoesNodeMatchSpec(
		const UBlueprint* Blueprint,
		const UEdGraphNode* Node,
		const FAssetDocumentNodeSpec& NodeSpec) const override;

	bool ExtractNode(const UBlueprint* Blueprint, const UK2Node_VariableSet* Node, FAssetDocumentNodeSpec& OutNode) const;
};
