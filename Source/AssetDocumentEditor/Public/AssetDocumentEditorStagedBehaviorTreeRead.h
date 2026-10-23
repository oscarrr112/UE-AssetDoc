// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

class IAssetDocumentHandleResolver;
struct FAssetDocumentHandleValidationContext;
struct FAssetDocumentEditorStagedBehaviorTreeReadState;

// One exact staged Verify scope. Closing invalidates its real Editor payload,
// including already acquired leases and readonly aliases, without unloading.
class ASSETDOCUMENTEDITOR_API FAssetDocumentEditorStagedBehaviorTreeReadLease final
{
public:
 ~FAssetDocumentEditorStagedBehaviorTreeReadLease();
 const FAssetDocumentOpaqueHandle& GetAssetReadHandle() const;
 const FAssetDocumentRuntimeValueDto& GetResolveValue() const;
 bool IsCurrent() const;
 void Close();
 FAssetDocumentEditorStagedBehaviorTreeReadLease(const FAssetDocumentEditorStagedBehaviorTreeReadLease&) = delete;
 FAssetDocumentEditorStagedBehaviorTreeReadLease& operator=(const FAssetDocumentEditorStagedBehaviorTreeReadLease&) = delete;
private:
 FAssetDocumentEditorStagedBehaviorTreeReadLease(TSharedRef<FAssetDocumentEditorStagedBehaviorTreeReadState>,
  FAssetDocumentOpaqueHandle, FAssetDocumentRuntimeValueDto);
 TSharedPtr<FAssetDocumentEditorStagedBehaviorTreeReadState> State;
 FAssetDocumentOpaqueHandle Handle;
 FAssetDocumentRuntimeValueDto Value;
 friend ASSETDOCUMENTEDITOR_API TUniquePtr<FAssetDocumentEditorStagedBehaviorTreeReadLease> ResolveAssetDocumentEditorStagedBehaviorTreeRead(
  const FAssetDocumentRuntimeRequestDto&, const FAssetDocumentHandleValidationContext&, const FString&, const FString&, uint64,
  IAssetDocumentHandleResolver&, TArray<FAssetDocumentRuntimeDiagnostic>&);
};

ASSETDOCUMENTEDITOR_API TUniquePtr<FAssetDocumentEditorStagedBehaviorTreeReadLease>
ResolveAssetDocumentEditorStagedBehaviorTreeRead(
 const FAssetDocumentRuntimeRequestDto& OriginalRequest, const FAssetDocumentHandleValidationContext& VerifyContext,
 const FString& StagedObjectPath, const FString& StagedPackageFilename, uint64 FreshRevision,
 IAssetDocumentHandleResolver& Handles, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics);
