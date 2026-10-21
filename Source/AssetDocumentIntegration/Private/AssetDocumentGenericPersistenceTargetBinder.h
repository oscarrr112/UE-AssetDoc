// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "AssetDocumentCoreRuntimeComposition.h"

class FAssetDocumentGenericPersistenceTargetBinder final
{
public:
	static TOptional<FAssetDocumentCorePersistenceTargetBinding> Bind(
		const FAssetDocumentCorePersistenceTargetRequest& Request,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);
};
