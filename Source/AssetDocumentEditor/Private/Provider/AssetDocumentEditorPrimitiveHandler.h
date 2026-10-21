// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

using FAssetDocumentEditorPrimitiveHandler =
	FAssetDocumentPrimitiveOutcome (*)(
		const FAssetDocumentPrimitiveInvocation&,
		IAssetDocumentHandleResolver&,
		IAssetDocumentAtomicTransaction*);

struct FAssetDocumentEditorPrimitiveBinding
{
	FStringView PrimitiveId;
	uint32 Version = 0;
	FAssetDocumentEditorPrimitiveHandler Handler = nullptr;
};

TConstArrayView<FAssetDocumentEditorPrimitiveBinding>
GetAssetDocumentEditorClassReferencePrimitiveBindings();
TConstArrayView<FAssetDocumentEditorPrimitiveBinding>
GetAssetDocumentEditorPropertyPrimitiveBindings();
TConstArrayView<FAssetDocumentEditorPrimitiveBinding>
GetAssetDocumentEditorStructPrimitiveBindings();
TConstArrayView<FAssetDocumentEditorPrimitiveBinding>
GetAssetDocumentEditorEmbeddedPrimitiveBindings();
TConstArrayView<FAssetDocumentEditorPrimitiveBinding>
GetAssetDocumentEditorLifecyclePrimitiveBindings();

TConstArrayView<FAssetDocumentEditorPrimitiveBinding>
GetAssetDocumentEditorPrimitiveBindings();
bool IsAssetDocumentEditorPrimitiveAssemblyValid();
