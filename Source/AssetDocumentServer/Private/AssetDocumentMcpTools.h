// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

/** What one of the editor's HTTP routes answered: its status code and JSON body. */
struct FAssetDocumentMcpRouteResult
{
	int32 Status = 200;
	TSharedPtr<FJsonObject> Json;
};

/**
 * Calls one of the editor's HTTP routes in process. Method is "GET" or "POST"; Endpoint is the route below
 * /assetdoc (e.g. "/assetdocument/extract"); Body is the JSON request body (empty for GET); Query holds the
 * decoded query parameters.
 */
using FAssetDocumentMcpRouteCaller = TFunction<FAssetDocumentMcpRouteResult(const FString& Method, const FString& Endpoint,
	const FString& Body, const TMap<FString, FString>& Query)>;

/** Tools the editor's settings switch off: they are not listed, and a call answers a tool error saying so. */
struct FAssetDocumentMcpToolPolicy
{
	bool bExecutePython = true;
	bool bDeleteAssets = true;
};

/**
 * The MCP tools of the editor, read from the plugin's tool manifest (MCP/tools.json): the name, description and input
 * schema of each tool, and how a call maps onto the editor's HTTP routes. A call validates its arguments, calls the
 * route in process and turns the route's answer into the MCP result: the JSON as compact text, or a tool error whose
 * text lists the route's message and diagnostics and the full JSON. The guides (MCP/schemas) are read at each call.
 */
class FAssetDocumentMcpTools
{
public:
	/** The plugin's MCP directory (holds tools.json and schemas/). */
	static FString DefaultMcpDirectory();

	/** Reads <McpDirectory>/tools.json. */
	bool Load(const FString& InMcpDirectory, FString& OutError);

	bool IsLoaded() const { return Tools.Num() > 0; }

	/** {name, version} of the server, as the manifest names it. */
	TSharedPtr<FJsonObject> GetServerInfo() const;

	/** The tools/list entries {name, description, inputSchema}, in manifest order, without the tools the policy switches off. */
	TArray<TSharedPtr<FJsonValue>> ListTools(const FAssetDocumentMcpToolPolicy& Policy) const;

	/**
	 * Runs one tools/call. OnResult receives the MCP CallToolResult ({content[], isError?}); it is called before this
	 * returns, except for a play operation that is still pending, which is polled on the core ticker until it finishes.
	 */
	void CallTool(const FString& Name, const TSharedPtr<FJsonObject>& Arguments, const FAssetDocumentMcpToolPolicy& Policy,
		const FAssetDocumentMcpRouteCaller& RouteCaller, TFunction<void(TSharedPtr<FJsonObject>)> OnResult) const;

	/** A CallToolResult with one text block. */
	static TSharedPtr<FJsonObject> MakeTextResult(const FString& Text, bool bIsError);

	/** The text a disabled tool answers. */
	static FString DescribeDisabledTool(const FString& Name);

private:
	FString McpDirectory;
	TSharedPtr<FJsonObject> ServerInfo;
	TArray<TSharedPtr<FJsonObject>> Tools;
};
