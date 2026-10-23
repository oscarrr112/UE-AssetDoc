// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Runtime/AssetDocumentPrimitiveProvider.h"

using FAssetDocumentAIPrimitiveHandler = FAssetDocumentPrimitiveOutcome (*)(
	const FAssetDocumentPrimitiveInvocation&,
	IAssetDocumentHandleResolver&,
	IAssetDocumentAtomicTransaction*);

struct FAssetDocumentAIPrimitiveBinding
{
	FString PrimitiveId;
	uint32 Version = 0;
	FAssetDocumentAIPrimitiveHandler Handler = nullptr;
};

const FAssetDocumentAIPrimitiveBinding* FindAssetDocumentAIPrimitiveBinding(
	FStringView PrimitiveId,
	uint32 Version);
ASSETDOCUMENTAI_API bool IsAssetDocumentAIPrimitiveAssemblyValid();
bool EnsureAssetDocumentAIProviderReady();
