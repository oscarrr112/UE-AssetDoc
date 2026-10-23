// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentHandleRegistryRuntime.h"
#include "AssetDocumentPrimitiveRegistryRuntime.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentRequestCoordinator.h"

class FAssetDocumentNativeRequestContextRuntime final
	: public IAssetDocumentNativeRequestContext
{
public:
	FAssetDocumentNativeRequestContextRuntime(
		const FAssetDocumentRequestContextMetadata& InMetadata,
		const FGuid& InTransactionId,
		const TSharedRef<FAssetDocumentPrimitiveRegistryRuntime>& InRegistry,
		const TSharedRef<FAssetDocumentHandleRegistryRuntime>& InHandles,
		IAssetDocumentAtomicTransaction* InActiveTransaction);
	~FAssetDocumentNativeRequestContextRuntime() override;

	const FAssetDocumentRequestContextMetadata& GetMetadata() const override;
	FAssetDocumentPrimitiveOutcome Invoke(
		const FAssetDocumentPrimitiveInvocation& Invocation) override;
	FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override;
	FAssetDocumentIssuedHandleResult Handoff(
		const FAssetDocumentOpaqueHandle& SourceHandle,
		FStringView TargetProviderId,
		EAssetDocumentHandleAccess RequestedAccess) override;

private:
	FAssetDocumentPrimitiveInvocation PinInvocation(
		const FAssetDocumentPrimitiveInvocation& Invocation) const;
	FAssetDocumentPrimitiveOutcome InvokeResolved(
		const FAssetDocumentPrimitiveInvocation& Invocation,
		const FAssetDocumentPrimitiveDescriptor& Descriptor,
		const TSharedRef<IAssetDocumentPrimitiveProvider>& Provider);
	FAssetDocumentContractValidationResult AcquireInputs(
		FAssetDocumentInvocationHandleSession& Session,
		const FAssetDocumentPrimitiveInvocation& Invocation,
		const FAssetDocumentPrimitiveDescriptor& Descriptor) const;

	FAssetDocumentRequestContextMetadata Metadata;
	FGuid TransactionId;
	TSharedRef<FAssetDocumentPrimitiveRegistryRuntime> Registry;
	TSharedRef<FAssetDocumentHandleRegistryRuntime> Handles;
	IAssetDocumentAtomicTransaction* ActiveTransaction = nullptr;
	bool bInvokeActive = false;
};
