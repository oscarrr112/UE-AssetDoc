// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentProfile.h"
#include "Graphs/AssetDocumentGraphTypes.h"

class UBlueprint;
class UClass;
class UEdGraphNode;
class UObject;

struct FAssetDocumentNodeAdapterContext
{
	UObject* Asset = nullptr;
	FString GraphPath;
};

struct FAssetDocumentNodeApplyContext
{
	UBlueprint* Blueprint = nullptr;
	FString GraphPath;
	FString NodePath;
};

struct FAssetDocumentUnsupportedNodeDiagnostic
{
	FString Code;
	FString Path;
	FString Class;
	FString Capability;
	TSharedPtr<FJsonObject> Member;
	FString Reason;
	FString SuggestedAction;

	TSharedRef<FJsonObject> ToJsonObject() const;
};

class IAssetDocumentNodeAdapter
{
public:
	virtual ~IAssetDocumentNodeAdapter() = default;

	virtual FString GetClassPath() const = 0;
	virtual FString GetCapability() const { return FString(); }

	virtual FAssetDocumentCapabilityResult ConfigureNodeForApply(
		const FAssetDocumentNodeApplyContext& Context,
		UEdGraphNode* Node,
		const FAssetDocumentNodeSpec& NodeSpec) const;

	virtual bool DoesNodeMatchSpec(
		const UBlueprint* Blueprint,
		const UEdGraphNode* Node,
		const FAssetDocumentNodeSpec& NodeSpec) const;

	virtual FAssetDocumentCapabilityResult CanRepresentExistingNode(
		const FAssetDocumentNodeApplyContext& Context,
		const UEdGraphNode* Node) const;
};

class FAssetDocumentNodeAdapterRegistry
{
public:
	void RegisterAdapter(const FString& ClassPath, TSharedRef<IAssetDocumentNodeAdapter> Adapter);
	void RegisterAdapter(UClass* Class, TSharedRef<IAssetDocumentNodeAdapter> Adapter);

	TSharedPtr<IAssetDocumentNodeAdapter> FindAdapter(const FString& ClassPath) const;
	TSharedPtr<IAssetDocumentNodeAdapter> FindAdapter(UClass* Class) const;

	void Reset();

private:
	TMap<FString, TSharedPtr<IAssetDocumentNodeAdapter>> AdaptersByClassPath;
};
