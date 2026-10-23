// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Runtime/AssetDocumentRequestCoordinator.h"

class FAssetDocumentPythonRevisionRuntime;

class FAssetDocumentPythonHost final : public IAssetDocumentPythonRuntimeHost
{
public:
	explicit FAssetDocumentPythonHost(FAssetDocumentPythonRevisionRuntime& InRevisions);
	FAssetDocumentRuntimeResultDto Dispatch(
		const FAssetDocumentRuntimeRequestDto& Request,
		IAssetDocumentNativeRequestContext& Context) override;

private:
	FAssetDocumentPythonRevisionRuntime& Revisions;
};
