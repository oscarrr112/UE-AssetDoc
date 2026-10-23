// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Runtime/AssetDocumentPersistenceCoordinator.h"
#include "Runtime/AssetDocumentPrimitiveRegistry.h"
#include "Runtime/AssetDocumentSyncCoordinator.h"

struct ASSETDOCUMENT_API FAssetDocumentRequestContextMetadata
{
	FGuid RequestId;
	uint64 PinnedRuntimeRevision = 0;
	EAssetDocumentRuntimeRequestPhase Phase = EAssetDocumentRuntimeRequestPhase::Validate;
	bool bCancellationDeferredInCriticalSection = false;
};

class ASSETDOCUMENT_API IAssetDocumentNativeRequestContext
{
public:
	virtual ~IAssetDocumentNativeRequestContext() = default;
	virtual const FAssetDocumentRequestContextMetadata& GetMetadata() const = 0;
	virtual FAssetDocumentPrimitiveOutcome Invoke(
		const FAssetDocumentPrimitiveInvocation& Invocation) = 0;
	virtual FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const = 0;
	virtual FAssetDocumentIssuedHandleResult Handoff(
		const FAssetDocumentOpaqueHandle& SourceHandle,
		FStringView TargetProviderId,
		EAssetDocumentHandleAccess RequestedAccess)
	{
		FAssetDocumentIssuedHandleResult Result;
		FAssetDocumentRuntimeDiagnostic& Diagnostic =
			Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/HandleHandoff");
		Diagnostic.Code = TEXT("AD.RUNTIME.HANDOFF_UNAVAILABLE");
		Diagnostic.Message =
			TEXT("This request context does not support provider handle handoff.");
		return Result;
	}
};

class ASSETDOCUMENT_API IAssetDocumentPythonRuntimeHost
{
public:
	virtual ~IAssetDocumentPythonRuntimeHost() = default;
	virtual FAssetDocumentRuntimeResultDto Dispatch(
		const FAssetDocumentRuntimeRequestDto& Request,
		IAssetDocumentNativeRequestContext& Context) = 0;
};

class ASSETDOCUMENT_API IAssetDocumentRequestObserver
{
public:
	virtual ~IAssetDocumentRequestObserver() = default;
	virtual void OnPhaseChanged(
		FGuid RequestId,
		EAssetDocumentRuntimeRequestPhase Phase) = 0;
};

struct ASSETDOCUMENT_API FAssetDocumentRequestCoordinatorDependencies
{
	IAssetDocumentPythonRuntimeHost& Host;
	IAssetDocumentPrimitiveRegistry& PrimitiveRegistry;
	IAssetDocumentHandleResolver& Handles;
	IAssetDocumentAtomicTransactionFactory& TransactionFactory;
	IAssetDocumentPersistenceCoordinator& Persistence;
	IAssetDocumentSyncCoordinator& Sync;
	IAssetDocumentCancellationView& Cancellation;
	IAssetDocumentRequestObserver& Observer;
};

class ASSETDOCUMENT_API IAssetDocumentRequestCoordinator
{
public:
	virtual ~IAssetDocumentRequestCoordinator() = default;
	virtual FAssetDocumentRuntimeResultDto Execute(
		const FAssetDocumentRuntimeRequestDto& Request,
		FAssetDocumentRequestCoordinatorDependencies& Dependencies) = 0;
};

namespace UE::AssetDocument::RequestCoordinator::Private
{
inline bool IsValidRequestPhase(EAssetDocumentRuntimeRequestPhase Phase)
{
	return static_cast<uint8>(Phase)
		<= static_cast<uint8>(EAssetDocumentRuntimeRequestPhase::Failed);
}
} // namespace UE::AssetDocument::RequestCoordinator::Private

inline bool IsAssetDocumentRequestPhaseTransitionAllowed(
	EAssetDocumentRuntimeRequestPhase From,
	EAssetDocumentRuntimeRequestPhase To)
{
	using namespace UE::AssetDocument::RequestCoordinator::Private;
	if (!IsValidRequestPhase(From) || !IsValidRequestPhase(To))
	{
		return false;
	}
	static constexpr bool Allowed[8][8] = {
		{false, true, false, false, false, false, false, true},
		{false, false, true, false, false, false, false, true},
		{false, false, false, true, true, false, false, true},
		{false, false, false, false, true, false, false, true},
		{false, false, false, false, false, true, false, true},
		{false, false, false, false, false, false, true, true},
		{false, false, false, false, false, false, false, false},
		{false, false, false, false, false, false, false, false},
	};
	return Allowed[static_cast<uint8>(From)][static_cast<uint8>(To)];
}
