// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

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

	/**
	 * These settings decide what AI clients may do (port, remote connections, auth token, execute_python,
	 * delete_assets), so no tool of the plugin reads or changes them: only the user does, in Project Settings.
	 * True for this class (or a subclass), its default object and any instance of it.
	 */
	static bool IsProtectedSettingsObject(const UObject* Object);

	/**
	 * Why no tool of the plugin writes Object, or empty. Object is a class, its default object or an instance (subclasses
	 * included) of a protected settings class: these settings, or one of a short list of engine settings that make the
	 * editor run code or open a channel that runs it (Python startup scripts and remote execution, the Remote Control
	 * web server, command lines that render jobs run). Reading the engine ones stays allowed.
	 */
	static FString WhyWriteProtected(const UObject* Object);

	/** True when Text names a protected settings class or its default object (a console command or a value that names it). */
	static bool NamesProtectedSettings(const FString& Text);

	/** The refusal for a Text that names a protected settings class (see NamesProtectedSettings), or empty. */
	static FString WhyNamesProtectedSettings(const FString& Text);

	/** Where the user changes these settings, for refusals: "... change it in Project Settings > Plugins > UE-AssetDoc." */
	static FString DescribeProtectedSettings();

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
