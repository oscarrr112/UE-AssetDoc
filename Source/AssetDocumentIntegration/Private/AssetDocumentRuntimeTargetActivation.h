// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once

#include "AssetDocumentEditorTargetLifecycleCatalog.h"
#include "Runtime/AssetDocumentRequestCoordinator.h"

enum class EAssetDocumentRuntimeTargetPolicy : uint8
{
	CreateOrUpdate = 0,
	UpdateOnly = 1,
};

struct FAssetDocumentRuntimeTargetActivation
{
	FString ExactAssetClassPath;
	FString LifecycleId;
	EAssetDocumentRuntimeTargetPolicy TargetPolicy;
	TArray<FAssetDocumentPrimitiveRequirement> RequiredPrimitives;
	bool bExistingOnly = false;
};

TOptional<FAssetDocumentRuntimeTargetActivation> ParseAndValidateRuntimeTargetActivation(
	const FAssetDocumentRuntimeValueDto& Projection,
	TConstArrayView<FAssetDocumentEditorTargetLifecycleCatalogEntry> Catalog,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

bool AreAssetDocumentRuntimeTargetActivationsEqual(
	const FAssetDocumentRuntimeTargetActivation& A,
	const FAssetDocumentRuntimeTargetActivation& B);

struct FAssetDocumentPreparedTargetDecision
{
	FAssetDocumentRuntimeTargetActivation Activation;
	FGuid RequestId;
	uint64 RuntimeRevision = 0;
	FString TargetPath;
	FString Action;
	bool bCreate = false;
};

// Pure matrix, shared by native preparation and the request-local Apply guard.
TOptional<bool> SelectAssetDocumentRuntimeTargetDecision(
	EAssetDocumentRuntimeTargetPolicy Policy, FStringView Action, bool bExists,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

TOptional<FAssetDocumentPreparedTargetDecision> PrepareAssetDocumentRuntimeTargetDecision(
	const FAssetDocumentRuntimeTargetActivation& Activation,
	const FAssetDocumentRuntimeRequestDto& Request,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

bool ValidateAssetDocumentPreparedTargetDecision(
	const FAssetDocumentPreparedTargetDecision& Prepared,
	const FAssetDocumentRuntimeRequestDto& Request,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

// Native-only decorator. Owns no registry and exposes exactly the existing context ABI.
class FAssetDocumentPreparedTargetContext final : public IAssetDocumentNativeRequestContext
{
public:
	FAssetDocumentPreparedTargetContext(IAssetDocumentNativeRequestContext& InInner,
		const FAssetDocumentPreparedTargetDecision& InPrepared,
		const FAssetDocumentRuntimeRequestDto& InRequest);
	const FAssetDocumentRequestContextMetadata& GetMetadata() const override;
	FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation& Invocation) override;
	FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override;
	FAssetDocumentIssuedHandleResult Handoff(const FAssetDocumentOpaqueHandle& Source,
		FStringView Provider, EAssetDocumentHandleAccess Access) override;
private:
	IAssetDocumentNativeRequestContext& Inner;
	const FAssetDocumentPreparedTargetDecision& Prepared;
	const FAssetDocumentRuntimeRequestDto& Request;
	bool bMutationStarted = false;
};
