// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

struct FAssetDocumentCapabilityResult;

struct FAssetDocumentGraphParseOptions
{
	FString Path = TEXT("");
	bool bRejectUnknownGraphFields = true;
};

struct FAssetDocumentGraphDiagnostic
{
	FString Code;
	FString Path;
	FString Message;
};

struct FAssetDocumentGraphEndpoint
{
	FString Node;
	FString Pin;

	FString ToKey() const;
	TSharedRef<FJsonObject> ToJsonObject() const;
};

struct FAssetDocumentLinkSpec
{
	FAssetDocumentGraphEndpoint From;
	FAssetDocumentGraphEndpoint To;

	FString ToKey() const;
	TSharedRef<FJsonObject> ToJsonObject() const;
};

struct FAssetDocumentMemberRef
{
	FString Kind;
	FString OwnerClass;
	FString Name;
	FString Guid;
	bool bHasSelfContext = false;
	bool bSelfContext = false;

	TSharedRef<FJsonObject> ToJsonObject() const;
};

struct FAssetDocumentPinOverrideSpec
{
	FString Pin;
	FString Direction;
	FString PersistentGuid;
	FString SplitState;
	FString ParentPin;
	TArray<FString> SubPins;
	bool bHasSubPins = false;
	TSharedPtr<FJsonValue> Type;
	TSharedPtr<FJsonValue> DefaultValue;
	TSharedPtr<FJsonValue> DefaultObject;
	TSharedPtr<FJsonValue> DefaultTextValue;
	TOptional<bool> Hidden;
	TOptional<bool> AdvancedView;

	FString ToSemanticKey() const;
	TSharedRef<FJsonObject> ToJsonObject() const;
};

struct FAssetDocumentNodeSpec
{
	FString Id;
	FString NodeGuid;
	FString Class;
	FString Capability;
	FString Kind;
	TSharedPtr<FJsonObject> Spawner;
	TSharedPtr<FJsonObject> Fields;
	TSharedPtr<FJsonObject> Pins;
	TSharedPtr<FJsonObject> Member;
	TArray<FAssetDocumentPinOverrideSpec> PinOverrides;
	TSharedPtr<FJsonObject> Position;
	TSharedPtr<FJsonObject> SubgraphRefs;
	TSharedPtr<FJsonObject> Evidence;
	FString Comment;
	bool bHasComment = false;
	TOptional<bool> CommentBubblePinned;
	TOptional<bool> CommentBubbleVisible;
	FString AdvancedPinDisplay;
	FString EnabledState;

	TSharedRef<FJsonObject> ToJsonObject() const;
};

struct FAssetDocumentGraphSpec
{
	FString Id;
	FString Kind;
	TSharedPtr<FJsonObject> Owner;
	FString OwnerNodeId;
	FString OwnerPin;
	FString Name;
	FString Schema;
	FString GraphGuid;
	FString Category;
	FString Description;
	TSharedPtr<FJsonObject> Signature;
	TSharedPtr<FJsonValue> EntryPins;
	TSharedPtr<FJsonValue> ResultPins;
	TSharedPtr<FJsonObject> Position;
	TSharedPtr<FJsonValue> Metadata;
	TSharedPtr<FJsonObject> Evidence;
	TArray<FAssetDocumentNodeSpec> Nodes;
	TArray<FAssetDocumentLinkSpec> Links;
	TArray<FAssetDocumentGraphSpec> Subgraphs;

	TSharedRef<FJsonObject> ToJsonObject() const;
};

struct FAssetDocumentGraphParseResult
{
	TArray<FAssetDocumentGraphSpec> Graphs;
	TArray<FAssetDocumentGraphDiagnostic> Diagnostics;

	bool IsValid() const { return Diagnostics.IsEmpty(); }
	void AddDiagnostic(const FString& Code, const FString& Path, const FString& Message);
	FAssetDocumentCapabilityResult ToCapabilityResult() const;
};

namespace AssetDocumentGraphJson
{
	bool AreJsonValuesEqual(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right);
	bool AreJsonObjectsEqual(const TSharedPtr<FJsonObject>& Left, const TSharedPtr<FJsonObject>& Right);
}
