// Copyright 2026 Oscar Peng and Houyu. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

class FJsonObject;
class IHttpRouter;
class FAssetDocumentMcpEndpoint;
class FAssetDocumentMcpTools;
struct FHttpRouteHandleInternal;
struct FHttpServerRequest;
struct FHttpServerResponse;

enum class EHttpServerRequestVerbs : uint16;
using FHttpRouteHandle = TSharedPtr<const FHttpRouteHandleInternal>;
using FHttpResultCallback = TFunction<void(TUniquePtr<FHttpServerResponse>&& Response)>;
using FHttpRequestHandler = TDelegate<bool(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)>;
using FAssetDocumentExternalRouteHandler = TDelegate<TSharedPtr<FJsonObject>(const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)>;

struct ASSETDOCUMENTSERVER_API FAssetDocumentExternalRoute
{
	FString Path;
	FAssetDocumentExternalRouteHandler Handler;
	FString MethodText;
	FString Description;
	bool bRequiresBody = false;
};

/**
 * HTTP Server for UEAssetDoc
 * Serves the editor tools to external clients (the MCP server, AI agents).
 *
 * MCP endpoint (Streamable HTTP): /mcp serves the MCP tools of the plugin's tool manifest (MCP/tools.json) to MCP
 * clients directly; the ue-assetdoc Node package only bridges stdio clients to it.
 *
 * Every request passes the security checks of AssetDocumentHttpSecurity first (local peer and Host, local Origin,
 * JSON bodies, the bearer token when Require Auth Token is on); the listener is bound to the loopback interface unless
 * Allow Remote Connections is on.
 *
 * Built-in endpoints:
 *   GET  /assetdoc/health - Health check
 *   GET  /assetdoc/context - Get current editor state (selected actors, assets, level, etc.)
 *   GET  /assetdoc/screenshot - Capture Level Viewport as base64 JPEG
 *   POST /assetdoc/execute - Execute Python code in the editor
 *   POST /assetdoc/delete - Delete assets
 * The other modules register theirs as external routes (RegisterExternalRoute): /assetdoc/assetdocument/..., the
 * level, play, asset editor, settings and import tools.
 *
 * Service Discovery:
 *   Writes service info to {ProjectDir}/Saved/UEAssetDoc/service.json once the engine runs (the routes the modules
 *   register during startup are written together), and again on the next tick after routes change: the port actually
 *   bound, the MCP URL, the process id and, when required, the bearer token (the file is readable by the current user
 *   only). AI Agents and the stdio bridge read this file to find the editor.
 *
 * Example usage (health):
 *   curl http://localhost:8559/assetdoc/health
 */
class ASSETDOCUMENTSERVER_API FAssetDocumentHttpServer
{
public:
	FAssetDocumentHttpServer();
	~FAssetDocumentHttpServer();

	/**
	 * Start the HTTP server on the specified port. Returns false when the port cannot be bound (for example still held
	 * by an editor that crashed): the server then retries for a few seconds (BindRetryDelays), and if the port stays
	 * taken it logs an Error and shows an editor notification (DescribeBindFailure). Stop cancels the retries.
	 */
	bool Start(uint32 Port = 8558) { return Start(Port, 0); }

	/**
	 * As Start(Port); when the port stays taken after the retries (another editor uses it), the next free port of the
	 * FallbackPorts ports after it is bound instead, the editor shows which, and the discovery file records it.
	 */
	bool Start(uint32 Port, int32 FallbackPorts);

	/** True while Start's port could not be bound yet and a retry is scheduled. */
	bool IsWaitingForPort() const { return BindRetryTicker.IsValid(); }

	/** The Error and notification text when the port stays taken after the retries. */
	static FString DescribeBindFailure(uint32 Port);

	/** Seconds before each bind retry (about five seconds in all). */
	static TConstArrayView<float> BindRetryDelays();

	/** Stop the HTTP server */
	void Stop();

	/** Check if server is running */
	bool IsRunning() const { return bIsRunning; }

	/** Get the port the server is running on */
	uint32 GetPort() const { return CurrentPort; }

	/** The port Start was asked for (the bound one differs after a fallback) */
	uint32 GetConfiguredPort() const { return ConfiguredPort; }

	/** The bearer token every request needs, or empty when none is required */
	const FString& GetAuthToken() const { return AuthToken; }

	/** The MCP endpoint (tests) */
	FAssetDocumentMcpEndpoint* GetMcpEndpoint() const { return McpEndpoint.Get(); }

	/** A server that is not the editor's own (tests) leaves the service discovery file alone: call before Start. */
	void SetWritesServiceDiscovery(bool bInWrites) { bWritesServiceDiscovery = bInWrites; }

	/** Get the path to the service discovery file */
	static FString GetServiceDiscoveryFilePath();

	/**
	 * Calls one of the server's routes in process, as an HTTP request to it would (MCP tool calls): the built-in routes
	 * and the external ones. Method "GET" or "POST"; Endpoint below /assetdoc (e.g. "/assetdocument/extract").
	 * A path no route handles answers 404 as the HTTP server does.
	 */
	static TSharedPtr<FJsonObject> CallRoute(const FString& Method, const FString& Endpoint, const FString& Body,
		const TMap<FString, FString>& QueryParams, uint32 Port, int32& OutStatusCode);

	/** Register an external route: bound now on running servers (and on every later start); their service discovery
	 *  files are rewritten once on the next tick, however many routes change in between */
	static FDelegateHandle RegisterExternalRoute(const FAssetDocumentExternalRoute& Route);

	/** Unregister a previously registered external route (unbinds only that route) */
	static void UnregisterExternalRoute(FDelegateHandle Handle);

	/** Write now the service discovery file of every running server with a pending route change (the core ticker
	 *  otherwise does it on the next tick). Returns the number of files written. */
	static int32 FlushServiceDiscovery();

	/** Number of service discovery files this process has written (tests) */
	static int32 GetServiceDiscoveryWriteCount();

private:
	static TMap<FDelegateHandle, FAssetDocumentExternalRoute>& GetExternalRoutes();
	static TSet<FAssetDocumentHttpServer*>& GetActiveServers();

	/** Write service discovery file for AI agents */
	void WriteServiceDiscoveryFile();

	/** Delete service discovery file */
	void DeleteServiceDiscoveryFile();

	/** The route bodies the HTTP handlers and CallRoute share */
	static TSharedPtr<FJsonObject> RunHealth(uint32 Port, int32& OutStatusCode);
	static TSharedPtr<FJsonObject> RunContext(int32& OutStatusCode);
	static TSharedPtr<FJsonObject> RunExecute(const FString& RequestBody, int32& OutStatusCode);
	static TSharedPtr<FJsonObject> RunDelete(const FString& RequestBody, int32& OutStatusCode);
	static TSharedPtr<FJsonObject> MakeErrorJson(int32 StatusCode, const FString& ErrorMessage);

	/** Handle GET /health request */
	bool HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	/** Handle POST /delete request */
	bool HandleDelete(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	/** Handle GET /context request - returns current editor state */
	bool HandleContext(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	/** Handle GET /screenshot request - capture Level Viewport as base64 JPEG */
	bool HandleGetViewportScreenshot(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	/** Handle POST /execute request - execute Python code in the editor */
	bool HandleExecute(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	/** Send JSON response */
	void SendJsonResponse(const FHttpResultCallback& OnComplete, int32 StatusCode, TSharedPtr<FJsonObject> JsonResponse);

	/** Send error response */
	void SendErrorResponse(const FHttpResultCallback& OnComplete, int32 StatusCode, const FString& ErrorMessage);

	void BindExternalRoutes();
	void UnbindExternalRoutes();
	void BindExternalRoute(FDelegateHandle Handle, const FAssetDocumentExternalRoute& Route);
	void UnbindExternalRoute(FDelegateHandle Handle);

	/** Schedule one service discovery write on the next tick of the running engine */
	void MarkServiceDiscoveryDirty();
	bool TickServiceDiscovery(float DeltaTime);

	/** One bind attempt: binds the routes and starts listening; on failure undoes them and returns false. */
	bool TryStart(uint32 Port);
	void ScheduleBindRetry(uint32 Port, int32 Attempt);
	/** The retries are over: bind the first free fallback port, or report the failure */
	void FallBackOrReport(uint32 Port);
	/** Undo the bindings of TryStart */
	void UnbindAll();
	void CancelBindRetry();

private:
	/** Route handles for cleanup */
	TArray<FHttpRouteHandle> RouteHandles;

	/** Bound external routes by their registration handle */
	TMap<FDelegateHandle, FHttpRouteHandle> ExternalRouteHandles;

	/** The service discovery file is out of date; ServiceDiscoveryTicker writes it */
	bool bServiceDiscoveryDirty = false;
	FTSTicker::FDelegateHandle ServiceDiscoveryTicker;

	/** Server state */
	bool bIsRunning = false;
	uint32 CurrentPort = 0;

	FTSTicker::FDelegateHandle BindRetryTicker;

	/** The bound port, shared with the MCP endpoint's in-process route calls */
	TSharedRef<uint32> BoundPort = MakeShared<uint32>(0);

	/** Start's port and how many ports after it may be used instead */
	uint32 ConfiguredPort = 0;
	int32 FallbackPortCount = 0;

	/** Security settings fixed for this run of the server */
	bool bAllowRemoteConnections = false;
	FString AuthToken;
	FDelegateHandle SecurityPreprocessor;

	/** Whether this server writes (and deletes) the service discovery file */
	bool bWritesServiceDiscovery = true;

	/** The MCP endpoint and its tools */
	TSharedPtr<FAssetDocumentMcpTools> McpTools;
	TSharedPtr<FAssetDocumentMcpEndpoint> McpEndpoint;

	/** HTTP Router */
	TSharedPtr<IHttpRouter> HttpRouter;
};
