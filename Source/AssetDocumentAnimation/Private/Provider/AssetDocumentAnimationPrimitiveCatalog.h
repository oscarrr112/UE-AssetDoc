// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

enum class EAssetDocumentAnimationDecisionDisposition : uint8
{
	Admitted = 0,
	OrdinaryStableBlocker = 1,
	FrozenG1StableBlocker = 2,
};

struct FAssetDocumentAnimationDecision
{
	FString PrimitiveId;
	uint32 Version = 1;
	EAssetDocumentAnimationDecisionDisposition Disposition =
		EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker;
	FString ConsumerId;
	FString AuthorityGateId;
	FString StableBlockerDiagnostic;
};

using FAssetDocumentAnimationPrimitiveHandler = FAssetDocumentPrimitiveOutcome (*) (
	const FAssetDocumentPrimitiveInvocation&,
	IAssetDocumentHandleResolver&,
	IAssetDocumentAtomicTransaction*);

struct FAssetDocumentAnimationPrimitiveBinding
{
	FStringView PrimitiveId;
	uint32 Version = 1;
	FAssetDocumentAnimationPrimitiveHandler Handler = nullptr;
};

TConstArrayView<FAssetDocumentAnimationDecision>
GetAssetDocumentAnimationDecisions();

TConstArrayView<FAssetDocumentPrimitiveDescriptor>
GetAssetDocumentAnimationPrimitiveDescriptors();

TConstArrayView<FAssetDocumentAnimationPrimitiveBinding>
GetAssetDocumentAnimationPrimitiveBindings();
