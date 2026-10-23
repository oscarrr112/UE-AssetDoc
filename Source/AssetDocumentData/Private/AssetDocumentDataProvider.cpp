// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentDataProvider.h"

#include "AssetDocumentDataPrimitives.h"
#include "AssetDocumentGraphProvider.h"

TSharedRef<IAssetDocumentPrimitiveProvider> CreateAssetDocumentDataPrimitiveProvider()
{
	using namespace UE::AssetDocument::Graph;
	return CreateAssetDocumentAssetTablePrimitiveProvider(Data::ProviderId, &Data::GetDescriptors, &Data::GetBindings);
}
