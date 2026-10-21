// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Graphs/AssetDocumentNodeAdapter.h"

class UBlueprint;
class UK2Node_Event;

class FAssetDocumentK2EventNodeAdapter final : public IAssetDocumentNodeAdapter
{
public:
	virtual FString GetClassPath() const override { return TEXT("/Script/BlueprintGraph.K2Node_Event"); }
	virtual FString GetCapability() const override { return TEXT("Event"); }
	virtual FAssetDocumentCapabilityResult ConfigureNodeForApply(
		const FAssetDocumentNodeApplyContext& Context,
		UEdGraphNode* Node,
		const FAssetDocumentNodeSpec& NodeSpec) const override;
	virtual bool DoesNodeMatchSpec(
		const UBlueprint* Blueprint,
		const UEdGraphNode* Node,
		const FAssetDocumentNodeSpec& NodeSpec) const override;

	bool ExtractNode(const UBlueprint* Blueprint, const UK2Node_Event* Node, FAssetDocumentNodeSpec& OutNode) const;
};
