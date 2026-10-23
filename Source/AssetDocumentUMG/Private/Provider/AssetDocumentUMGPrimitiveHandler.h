// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

using FAssetDocumentUMGPrimitiveHandler =
	FAssetDocumentPrimitiveOutcome (*)(
		const FAssetDocumentPrimitiveInvocation&,
		IAssetDocumentHandleResolver&,
		IAssetDocumentAtomicTransaction*);

struct FAssetDocumentUMGPrimitiveBinding
{
	FStringView PrimitiveId;
	uint32 Version = 0;
	FAssetDocumentUMGPrimitiveHandler Handler = nullptr;
};

FAssetDocumentPrimitiveOutcome MakeAssetDocumentUMGFailure(
	const TCHAR* Code,
	const TCHAR* Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics = {});

FAssetDocumentPrimitiveOutcome MakeAssetDocumentUMGSuccess(
	FAssetDocumentRuntimeValueDto&& Value);

TConstArrayView<FAssetDocumentUMGPrimitiveBinding>
GetAssetDocumentUMGWidgetTreePrimitiveBindings();
TConstArrayView<FAssetDocumentUMGPrimitiveBinding>
GetAssetDocumentUMGDesignerBindingPrimitiveBindings();
TConstArrayView<FAssetDocumentUMGPrimitiveBinding>
GetAssetDocumentUMGWidgetAnimationPrimitiveBindings();
TConstArrayView<FAssetDocumentUMGPrimitiveBinding>
GetAssetDocumentUMGAnimationBindingPrimitiveBindings();
TConstArrayView<FAssetDocumentUMGPrimitiveBinding>
GetAssetDocumentUMGMovieScenePrimitiveBindings();

ASSETDOCUMENTUMG_API TArray<FAssetDocumentUMGPrimitiveBinding>
BuildAssetDocumentUMGHandlerTable();
TConstArrayView<FAssetDocumentUMGPrimitiveBinding>
GetAssetDocumentUMGPrimitiveBindings();
bool IsAssetDocumentUMGPrimitiveAssemblyValid();
