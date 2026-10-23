// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class ASSETDOCUMENT_API IAssetDocumentRuntimeModule : public IModuleInterface
{
public:
	virtual ~IAssetDocumentRuntimeModule();
	virtual FName GetRuntimeModuleName() const = 0;
	virtual bool HasActiveRuntimeRegistration() const;
};
