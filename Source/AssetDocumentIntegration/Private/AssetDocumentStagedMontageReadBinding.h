// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Runtime/AssetDocumentPrimitiveProvider.h"
#include "Runtime/AssetDocumentRequestCoordinator.h"

class FAssetDocumentEditorStagedMontageReadLease;

// Transparent Editor provider facade, except for one exact staged Verify
// target resolver. The exported Editor seam owns native payload issuance.
class FAssetDocumentStagedMontageReadBinding final : public IAssetDocumentPrimitiveProvider
{
public:
	explicit FAssetDocumentStagedMontageReadBinding(TSharedRef<IAssetDocumentPrimitiveProvider> InInner);
	~FAssetDocumentStagedMontageReadBinding();
	FString GetProviderId() const override;
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> DescribePrimitives() const override;
	FAssetDocumentProviderAvailability InspectAvailability() const override;
	FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation& Invocation,
		IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* Transaction) override;
	bool Bind(const FAssetDocumentRuntimeRequestDto& OriginalRequest, const FString& ObjectPath,
		const FString& PackageFilename, uint64 FreshRevision);
	void Close();
private:
	TSharedRef<IAssetDocumentPrimitiveProvider> Inner;
	TOptional<FAssetDocumentRuntimeRequestDto> Request;
	FString StagedObjectPath;
	FString StagedPackageFilename;
	uint64 Revision = 0;
	TUniquePtr<FAssetDocumentEditorStagedMontageReadLease> ReadLease;
};
