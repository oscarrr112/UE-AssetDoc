// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "AssetDocumentSettings.generated.h"

/**
 * Settings for the UEAssetDoc plugin
 * Configure HTTP Server and other options
 *
 * Access via Project Settings -> Plugins -> UE-AssetDoc
 */
UCLASS(config = EditorPerProjectUserSettings, defaultconfig, meta = (DisplayName = "UE-AssetDoc"))
class ASSETDOCUMENTSERVER_API UAssetDocumentSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UAssetDocumentSettings();

	/** Get the singleton settings instance */
	static UAssetDocumentSettings* Get();

	//~ UDeveloperSettings interface
	virtual FName GetContainerName() const override { return FName("Project"); }
	virtual FName GetCategoryName() const override { return FName("Plugins"); }
	virtual FName GetSectionName() const override { return FName("UE-AssetDoc"); }

	//~ HTTP Server Settings

	/** Enable HTTP Server for external tool integration (AI Agents, MCP, etc.) */
	UPROPERTY(config, EditAnywhere, Category = "HTTP Server")
	bool bEnableHttpServer = true;

	/** Port for HTTP Server (default: 8559, note: 8558 is used by UE Zen Server) */
	UPROPERTY(config, EditAnywhere, Category = "HTTP Server", meta = (EditCondition = "bEnableHttpServer", ClampMin = "1024", ClampMax = "65535"))
	int32 HttpServerPort = 8559;

	/**
	 * Allow connections from other machines (default: false). Off: the server listens on the loopback interface only and
	 * refuses requests whose Host is not localhost, 127.0.0.1 or [::1]. On: it listens on every interface and always
	 * requires the bearer token (Require Auth Token). Takes effect when the editor starts.
	 */
	UPROPERTY(config, EditAnywhere, Category = "HTTP Server", meta = (EditCondition = "bEnableHttpServer"))
	bool bAllowRemoteConnections = false;

	//~ Security Settings

	/**
	 * Require "Authorization: Bearer <token>" on every request (default: false). The token is generated each time the
	 * editor starts and written to Saved/UEAssetDoc/service.json (readable by the current user only), where the
	 * ue-assetdoc stdio bridge reads it. Takes effect when the editor starts.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Security", meta = (EditCondition = "bEnableHttpServer"))
	bool bRequireAuthToken = false;

	/** Offer execute_python (run Python in the editor) to AI clients (default: true). Off: the tool is not listed and the route refuses. */
	UPROPERTY(config, EditAnywhere, Category = "Security", meta = (EditCondition = "bEnableHttpServer"))
	bool bEnableExecutePython = true;

	/** Offer delete_assets to AI clients (default: true). Off: the tool is not listed and the route refuses. */
	UPROPERTY(config, EditAnywhere, Category = "Security", meta = (EditCondition = "bEnableHttpServer"))
	bool bEnableDeleteAssets = true;

	//~ Logging Settings

	/** Log HTTP requests for debugging */
	UPROPERTY(config, EditAnywhere, Category = "Logging")
	bool bLogHttpRequests = false;
};
