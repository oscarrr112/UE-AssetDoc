// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "AssetDocumentPythonRuntime.h"

class FAssetDocumentGenericReadOnlyDispatcher final
{
public:
	static FAssetDocumentRuntimeResultDto DispatchPhase(
		IAssetDocumentPythonRuntime& Runtime,
		const FAssetDocumentRuntimeRequestDto& Request,
		EAssetDocumentRuntimeRequestPhase Phase);

	static FAssetDocumentRuntimeResultDto DispatchValidate(
		IAssetDocumentPythonRuntime& Runtime,
		const FAssetDocumentRuntimeRequestDto& Request);
};
