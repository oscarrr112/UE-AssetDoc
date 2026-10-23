// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentUMGProviderTestTypes.h"

FAssetDocumentUMGProviderTestPayload::FAssetDocumentUMGProviderTestPayload(
	FName InTypeId)
	: TypeId(InTypeId)
{
}

FName FAssetDocumentUMGProviderTestPayload::GetPayloadTypeId() const
{
	return TypeId;
}

bool FAssetDocumentUMGProviderTestPayload::IsAlive() const
{
	return bAlive;
}

void FAssetDocumentUMGProviderTestPayload::Invalidate()
{
	bAlive = false;
}
