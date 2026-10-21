// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Runtime/AssetDocumentPrimitiveProvider.h"
#include "Runtime/AssetDocumentRequestCoordinator.h"

class FAssetDocumentEditorStagedBehaviorTreeReadLease;

// Exact staged Verify resolver facade. Other Editor primitives retain their
// original provider; AI's readonly path translation consumes the query below.
class FAssetDocumentStagedBehaviorTreeReadBinding final : public IAssetDocumentPrimitiveProvider
{
public:
 explicit FAssetDocumentStagedBehaviorTreeReadBinding(TSharedRef<IAssetDocumentPrimitiveProvider> InInner);
 ~FAssetDocumentStagedBehaviorTreeReadBinding();
 FString GetProviderId() const override;
 TConstArrayView<FAssetDocumentPrimitiveDescriptor> DescribePrimitives() const override;
 FAssetDocumentProviderAvailability InspectAvailability() const override;
 FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation&, IAssetDocumentHandleResolver&,
  IAssetDocumentAtomicTransaction*) override;
 bool Bind(const FAssetDocumentRuntimeRequestDto& OriginalRequest, const FString& StagedObjectPath,
  const FString& StagedPackageFilename, uint64 FreshRevision);
 void Close();
 bool IsBound() const;
 FString GetStagedObjectPath() const;
 bool TryGetStagedObjectPath(const FGuid& RequestId, uint64 FreshRevision,
  const FString& OriginalTarget, FString& OutPath) const;
private:
 TSharedRef<IAssetDocumentPrimitiveProvider> Inner;
 TOptional<FAssetDocumentRuntimeRequestDto> Request;
 FGuid ClosedRequestId;
 FString ClosedTarget;
 FString StagedObjectPath, StagedPackageFilename;
 uint64 Revision = 0, ClosedRevision = 0;
 TUniquePtr<FAssetDocumentEditorStagedBehaviorTreeReadLease> ReadLease;
};

TSharedRef<IAssetDocumentPrimitiveProvider> WrapAssetDocumentBehaviorTreeStagedAIReadProvider(
 TSharedRef<IAssetDocumentPrimitiveProvider> Inner, TSharedRef<FAssetDocumentStagedBehaviorTreeReadBinding> Binding);
