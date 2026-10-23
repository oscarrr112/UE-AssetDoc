// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentAuthorityExceptionRegistry.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Runtime/AssetDocumentPrimitiveRegistry.h"
#include "Runtime/AssetDocumentRequestCoordinator.h"

struct ASSETDOCUMENTPYTHON_API FAssetDocumentPythonRuntimeConfiguration
{
	FString RepositoryRoot;
	FString PackageRootRelativePath;
};

struct ASSETDOCUMENTPYTHON_API FAssetDocumentPythonDerivedRegistryInput
{
	TArray<FAssetDocumentPrimitiveRequirement> RequiredPrimitives;
	TArray<TSharedRef<IAssetDocumentPrimitiveProvider>> Providers;
};

// Runs once per activation after the revision imports and before its registry freezes.
// Declared is the revision's dispatch.describe_required_primitives() wire;
// Registrations is its describe_runtime_registrations() projection. Unset
// result rejects the candidate and keeps the previous revision active.
using FAssetDocumentPythonDeriveRegistryInput = TFunction<TOptional<FAssetDocumentPythonDerivedRegistryInput>(
	TConstArrayView<FAssetDocumentPrimitiveRequirement> Declared,
	const FAssetDocumentRuntimeValueDto& Registrations,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)>;

struct ASSETDOCUMENTPYTHON_API FAssetDocumentPythonRevisionCandidate
{
	uint64 RuntimeRevision = 0;
	FString EntryModuleRelativePath;
	// Static registry input, used only when DeriveRegistryInput is unbound.
	TArray<FAssetDocumentPrimitiveRequirement> RequiredPrimitives;
	TArray<TSharedRef<IAssetDocumentPrimitiveProvider>> Providers;
	TArray<FAssetDocumentAuthorityCandidateRecord> AuthorityCatalog;
	FAssetDocumentPythonDeriveRegistryInput DeriveRegistryInput;
};

struct ASSETDOCUMENTPYTHON_API FAssetDocumentPythonRevisionActivationResult
{
	bool bActivated = false;
	uint64 ActiveRuntimeRevision = 0;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};

struct ASSETDOCUMENTPYTHON_API FAssetDocumentPythonRequestContextCreateResult
{
	TUniquePtr<IAssetDocumentNativeRequestContext> Context;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};

class ASSETDOCUMENTPYTHON_API IAssetDocumentPythonRuntime
{
public:
	virtual ~IAssetDocumentPythonRuntime() = default;
	virtual FAssetDocumentContractValidationResult InitializePythonBinding() = 0;
	virtual FAssetDocumentPythonRevisionActivationResult TryActivateRevision(
		const FAssetDocumentPythonRevisionCandidate& Candidate) = 0;
	virtual uint64 GetActiveRuntimeRevision() const = 0;
	virtual FAssetDocumentRuntimeValueDto GetActiveRuntimeRegistrationProjection() const
	{
		FAssetDocumentRuntimeValueDto Projection;
		Projection.Kind = EAssetDocumentRuntimeValueKind::Array;
		return Projection;
	}
	virtual bool HasActiveRequests() const = 0;
	virtual IAssetDocumentPythonRuntimeHost& GetHost() = 0;
	virtual const IAssetDocumentPrimitiveRegistry* GetActivePrimitiveRegistry() const = 0;
	virtual FAssetDocumentPythonRequestContextCreateResult CreateRequestContext(
		const FAssetDocumentRequestContextMetadata& Metadata,
		const FGuid& TransactionId,
		IAssetDocumentAtomicTransaction* ActiveTransaction) = 0;
	virtual FAssetDocumentHandleInvalidationResult InvalidateHandles(
		const FAssetDocumentHandleInvalidation& Event) = 0;
	virtual void ShutdownPythonBinding() = 0;
};

ASSETDOCUMENTPYTHON_API TUniquePtr<IAssetDocumentPythonRuntime> CreateAssetDocumentPythonRuntime(
	const FAssetDocumentPythonRuntimeConfiguration& Configuration);
