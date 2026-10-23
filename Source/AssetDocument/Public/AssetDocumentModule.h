// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogAssetDocument, Log, All);

class FAssetDocumentService;
class FAssetDocumentHttpRoutes;
class FAssetDocumentEditorSync;
class FAssetDocumentFileWatcher;
class FAssetDocumentIngressQueue;

class ASSETDOCUMENT_API FAssetDocumentModule : public IModuleInterface
{
public:
	FAssetDocumentModule();
	virtual ~FAssetDocumentModule() override;
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static FAssetDocumentModule& Get();
	static bool IsAvailable();

	FAssetDocumentService& GetService();

private:
	TSharedPtr<FAssetDocumentService> Service;
	TSharedPtr<FAssetDocumentIngressQueue> IngressQueue;
	TUniquePtr<FAssetDocumentHttpRoutes> HttpRoutes;
	TUniquePtr<FAssetDocumentEditorSync> EditorSync;
	TUniquePtr<FAssetDocumentFileWatcher> FileWatcher;
};
