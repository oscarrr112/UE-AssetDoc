// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogAssetDocumentServer, Log, All);

class FAssetDocumentHttpServer;

/**
 * AssetDocumentServer module
 * Runs the HTTP server the editor tools are served on (AI agents, MCP) and holds the shared editor helpers
 * (Python execution, asset deletion, viewport capture, property utilities).
 */
class FAssetDocumentServerModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Get this module */
	static FAssetDocumentServerModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FAssetDocumentServerModule>("AssetDocumentServer");
	}

	/** Check if module is loaded */
	static bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("AssetDocumentServer");
	}

	/** Get HTTP Server instance */
	FAssetDocumentHttpServer* GetHttpServer() const { return HttpServer.Get(); }

private:
	/** Start HTTP server if enabled in settings */
	void StartHttpServer();

	/** Stop HTTP server */
	void StopHttpServer();

	/** HTTP Server for external tool integration */
	TUniquePtr<FAssetDocumentHttpServer> HttpServer;
};
