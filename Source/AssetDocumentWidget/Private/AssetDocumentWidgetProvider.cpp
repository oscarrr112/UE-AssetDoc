// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentWidgetProvider.h"

#include "AssetDocumentGraphProvider.h"
#include "AssetDocumentWidgetPrimitives.h"

TSharedRef<IAssetDocumentPrimitiveProvider> CreateAssetDocumentWidgetPrimitiveProvider()
{
	using namespace UE::AssetDocument::Graph;
	return CreateAssetDocumentGraphTablePrimitiveProvider(Widget::ProviderId, &Widget::GetDescriptors, &Widget::GetBindings);
}
