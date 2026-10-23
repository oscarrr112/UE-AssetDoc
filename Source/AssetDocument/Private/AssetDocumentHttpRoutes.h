// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FAssetDocumentService;

class FAssetDocumentHttpRoutes
{
public:
	explicit FAssetDocumentHttpRoutes(const TSharedRef<FAssetDocumentService>& InService);
	~FAssetDocumentHttpRoutes();

	void Register();
	void Unregister();

private:
	TWeakPtr<FAssetDocumentService> Service;
	TArray<FDelegateHandle> RouteHandles;
};
