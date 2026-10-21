// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "TypedPreimage/AssetDocumentAITypedPreimageInternal.h"

class IPropertyRowGenerator;
class IPropertyHandle;
class UBehaviorTreeGraph;
class UEdGraphNode;

namespace UE::AssetDocument::AI::TypedPreimage::Selector
{
using namespace UE::AssetDocument::AI::TypedPreimage;
inline constexpr const TCHAR* SelectorPath = TEXT("/AITypedPreimage/BehaviorTreeSelector");
// Resolves one canonical graph locator through stock typed graph containers.
UObject* ResolveExactSelectorOwner(UBehaviorTreeGraph& Graph,
 FStringView StableNodeId, UEdGraphNode** OutNode = nullptr);
// Capture only: neither arms a state nor changes the transaction.
FAssetDocumentEditorTypedPreimageResult CaptureSelectorState(
 const FAssetDocumentEditorAssetIdentityView& Identity,
 UObject& ExactPropertyOwner,
 TSharedRef<IPropertyRowGenerator> PropertyRows,
 TSharedRef<IPropertyHandle> SelectorPropertyHandle,
 TSharedPtr<FAliasRollbackState>& OutState,
 TFunction<TSharedPtr<IPropertyHandle>()> NestedRebind = {},
 TFunction<bool(IPropertyHandle&)> NestedBinding = {});
}

struct FAssetDocumentAISelectorBatchPreimageEntry
{
 UObject* ExactOwner;
 TSharedRef<IPropertyRowGenerator> PropertyRows;
 TSharedRef<IPropertyHandle> SelectorPropertyHandle;
 FString StableNodeId;
 FString PropertyPath;
 TFunction<TSharedPtr<IPropertyHandle>()> NestedRebind;
 TFunction<bool(IPropertyHandle&)> NestedBinding;
};

FAssetDocumentEditorTypedPreimageResult
CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorBatchPreimage(
 const IAssetDocumentVerifiedHandleLease& AssetLease,
 TConstArrayView<FAssetDocumentAISelectorBatchPreimageEntry> Entries,
 IAssetDocumentAtomicTransaction& Transaction);
