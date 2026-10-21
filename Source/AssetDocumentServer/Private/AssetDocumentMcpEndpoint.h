// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentMcpTools.h"

struct FHttpServerRequest;
struct FHttpServerResponse;
using FHttpResultCallback = TFunction<void(TUniquePtr<FHttpServerResponse>&& Response)>;

/**
 * The editor's MCP endpoint (Streamable HTTP transport): POST carries one JSON-RPC message (initialize, ping,
 * tools/list, tools/call, notifications), answered as application/json, or as one text/event-stream event when the
 * client accepts only that; GET answers 405 (the server sends no messages of its own); DELETE ends a session.
 * initialize opens a session whose id the response carries in Mcp-Session-Id; every later message names it.
 * Tool calls run on the game thread through the editor's HTTP routes (FAssetDocumentMcpTools).
 */
class FAssetDocumentMcpEndpoint
{
public:
	FAssetDocumentMcpEndpoint(const TSharedRef<FAssetDocumentMcpTools>& InTools, FAssetDocumentMcpRouteCaller InRouteCaller,
		TFunction<FAssetDocumentMcpToolPolicy()> InPolicy);

	/** Handles a request to the endpoint path (any verb). */
	bool HandleRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	/** Protocol revisions the endpoint speaks, newest first. */
	static const TArray<FString>& SupportedProtocolVersions();

	int32 NumSessions() const { return Sessions.Num(); }

	/** Most sessions kept at once; the least recently used one is dropped beyond it. */
	static constexpr int32 MaxSessions = 64;

private:
	struct FSession
	{
		FString ProtocolVersion;
		double LastUsed = 0.0;
	};

	bool HandlePost(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleDelete(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	FString OpenSession(const FString& ProtocolVersion);

	TSharedRef<FAssetDocumentMcpTools> Tools;
	FAssetDocumentMcpRouteCaller RouteCaller;
	TFunction<FAssetDocumentMcpToolPolicy()> Policy;
	TMap<FString, FSession> Sessions;
};
