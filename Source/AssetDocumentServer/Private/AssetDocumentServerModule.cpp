// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentServerModule.h"
#include "AssetDocumentHttpServer.h"
#include "AssetDocumentSettings.h"
#include "AssetDocumentTestOnlyModules.h"
#include "Misc/CoreDelegates.h"

DEFINE_LOG_CATEGORY(LogAssetDocumentServer);

void FAssetDocumentServerModule::StartupModule()
{
	UE_LOG(LogAssetDocumentServer, Log, TEXT("AssetDocumentServer module starting up"));

	// Start HTTP server if enabled
	StartHttpServer();

#if WITH_DEV_AUTOMATION_TESTS
	FCoreDelegates::OnFEngineLoopInitComplete.AddStatic(&UE::AssetDocumentServer::RecordTestOnlyModulesAtStartup);
	FCoreDelegates::OnFEngineLoopInitComplete.AddStatic(&UE::AssetDocumentServer::AllowFirstAutomationListingToLoadTestOnlyModules);
#endif
}

void FAssetDocumentServerModule::ShutdownModule()
{
	UE_LOG(LogAssetDocumentServer, Log, TEXT("AssetDocumentServer module shutting down"));

	// Stop HTTP server
	StopHttpServer();
}

void FAssetDocumentServerModule::StartHttpServer()
{
	const UAssetDocumentSettings* Settings = UAssetDocumentSettings::Get();
	if (!Settings || !Settings->bEnableHttpServer)
	{
		UE_LOG(LogAssetDocumentServer, Log, TEXT("HTTP Server is disabled in settings"));
		return;
	}

	if (!HttpServer)
	{
		HttpServer = MakeUnique<FAssetDocumentHttpServer>();
	}

	if (!HttpServer->IsRunning() && !HttpServer->IsWaitingForPort())
	{
		// A second editor (the port taken) listens on one of the next ports and records it in the discovery file.
		constexpr int32 FallbackPorts = 10;
		HttpServer->Start(Settings->HttpServerPort, FallbackPorts);
	}
}

void FAssetDocumentServerModule::StopHttpServer()
{
	if (HttpServer && (HttpServer->IsRunning() || HttpServer->IsWaitingForPort()))
	{
		HttpServer->Stop();
	}
	HttpServer.Reset();
}

IMPLEMENT_MODULE(FAssetDocumentServerModule, AssetDocumentServer)
