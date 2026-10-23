// Copyright 2026 Oscar Peng and Houyu. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentHttpServer.h"
#include "AssetDocumentAssetDelete.h"
#include "AssetDocumentHttpSecurity.h"
#include "AssetDocumentMcpEndpoint.h"
#include "AssetDocumentMcpJson.h"
#include "AssetDocumentMcpTools.h"
#include "AssetDocumentPendingNotices.h"
#include "AssetDocumentServerModule.h"
#include "AssetDocumentSettings.h"
#include "AssetDocumentUserOnlyFile.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Async/Async.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Editor.h"
// Editor state
#include "Subsystems/EditorActorSubsystem.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "EditorModeManager.h"
#include "EditorModeRegistry.h"
// Python execution
#include "AssetDocumentServerPythonExecution.h"
// Viewport screenshot
#include "AssetDocumentViewportCapture.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/Base64.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"


namespace AssetDocumentHttpServerLocal
{
// A bind failure that outlasts the retries: an Error in the log and an editor notification (it was only a log line,
// and AI tools then failed with unclear connection errors).
void ReportBindFailure(uint32 Port)
{
	const FString Text = FAssetDocumentHttpServer::DescribeBindFailure(Port);
	UE_LOG(LogAssetDocumentServer, Error, TEXT("%s"), *Text);
	if (FSlateApplication::IsInitialized())
	{
		FNotificationInfo Info(FText::FromString(Text));
		Info.bFireAndForget = true;
		Info.ExpireDuration = 30.0f;
		Info.bUseLargeFont = false;
		if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
		{
			Item->SetCompletionState(SNotificationItem::CS_Fail);
		}
	}
}

void ReportPortFallback(uint32 ConfiguredPort, uint32 BoundPort)
{
	const FString Text = FString::Printf(TEXT("Port %u is in use (another editor?): the UEAssetDoc HTTP server of this editor "
		"listens on port %u. MCP clients configured with a URL use http://127.0.0.1:%u/mcp; the ue-assetdoc bridge finds "
		"this editor through Saved/UEAssetDoc/service.json of the project."), ConfiguredPort, BoundPort, BoundPort);
	UE_LOG(LogAssetDocumentServer, Warning, TEXT("%s"), *Text);
	if (FSlateApplication::IsInitialized())
	{
		FNotificationInfo Info(FText::FromString(Text));
		Info.bFireAndForget = true;
		Info.ExpireDuration = 15.0f;
		Info.bUseLargeFont = false;
		FSlateNotificationManager::Get().AddNotification(Info);
	}
}

bool IsLoopbackBindAddress(const FString& Address)
{
	return Address.Equals(TEXT("localhost"), ESearchCase::IgnoreCase) || Address.StartsWith(TEXT("127.")) || Address == TEXT("::1");
}

// Starts the engine's HTTP listeners with the listener of Port bound to the loopback interface (or to every interface
// when remote connections are allowed), whatever the project's [HTTPServer.Listeners] configuration says. The listener
// reads that configuration when it starts, so an override for Port is in place only while it starts.
void StartListenersWithBindAddress(FHttpServerModule& HttpServerModule, uint32 Port, bool bAllowRemoteConnections)
{
	const TCHAR* const Section = TEXT("HTTPServer.Listeners");
	FString Effective = TEXT("localhost");
	TArray<FString> Overrides;
	bool bHadOverrides = false;
	if (GConfig != nullptr)
	{
		GConfig->GetString(Section, TEXT("DefaultBindAddress"), Effective, GEngineIni);
		bHadOverrides = GConfig->GetArray(Section, TEXT("ListenerOverrides"), Overrides, GEngineIni) > 0;
		for (const FString& Entry : Overrides)
		{
			const FString Stripped = Entry.Replace(TEXT("("), TEXT("")).Replace(TEXT(")"), TEXT(""));
			uint32 EntryPort = 0;
			if (FParse::Value(*Stripped, TEXT("Port="), EntryPort) && EntryPort == Port)
			{
				FParse::Value(*Stripped, TEXT("BindAddress="), Effective);
				break;
			}
		}
	}
	const bool bWantLoopback = !bAllowRemoteConnections;
	if (GConfig == nullptr || IsLoopbackBindAddress(Effective) == bWantLoopback)
	{
		HttpServerModule.StartAllListeners();
		return;
	}
	UE_LOG(LogAssetDocumentServer, Log, TEXT("UEAssetDoc HTTP server: port %u binds %s (the project's HTTPServer.Listeners says %s)"),
		Port, bWantLoopback ? TEXT("127.0.0.1") : TEXT("every interface"), *Effective);
	TArray<FString> Patched = Overrides;
	Patched.Insert(FString::Printf(TEXT("(Port=%u,BindAddress=%s)"), Port, bWantLoopback ? TEXT("127.0.0.1") : TEXT("any")), 0);
	GConfig->SetArray(Section, TEXT("ListenerOverrides"), Patched, GEngineIni);
	HttpServerModule.StartAllListeners();
	if (bHadOverrides)
	{
		GConfig->SetArray(Section, TEXT("ListenerOverrides"), Overrides, GEngineIni);
	}
	else
	{
		GConfig->RemoveKey(Section, TEXT("ListenerOverrides"), GEngineIni);
	}
}

} // namespace AssetDocumentHttpServerLocal

namespace
{
	int32 GServiceDiscoveryWrites = 0;

	struct FRouteBindingSpec
	{
		const TCHAR* Path = TEXT("");
		EHttpServerRequestVerbs Verb = EHttpServerRequestVerbs::VERB_NONE;
		FHttpRequestHandler Handler;
		const TCHAR* LogMethod = TEXT("");
		const TCHAR* Description = TEXT("");
		bool bJsonContentType = false;
	};

	bool TryNormalizeExternalRouteMethodText(const FString& InMethodText, FString& OutMethodText)
	{
		const FString MethodText = InMethodText.TrimStartAndEnd().ToUpper();
		if (MethodText.IsEmpty() || MethodText == TEXT("GET"))
		{
			OutMethodText = TEXT("GET");
			return true;
		}
		if (MethodText == TEXT("POST"))
		{
			OutMethodText = TEXT("POST");
			return true;
		}
		if (MethodText == TEXT("PUT"))
		{
			OutMethodText = TEXT("PUT");
			return true;
		}
		if (MethodText == TEXT("PATCH"))
		{
			OutMethodText = TEXT("PATCH");
			return true;
		}
		if (MethodText == TEXT("DELETE"))
		{
			OutMethodText = TEXT("DELETE");
			return true;
		}
		return false;
	}

	FString GetExternalRouteMethodText(const FAssetDocumentExternalRoute& Route)
	{
		FString MethodText;
		if (TryNormalizeExternalRouteMethodText(Route.MethodText, MethodText))
		{
			return MethodText;
		}

		return TEXT("GET");
	}

	EHttpServerRequestVerbs GetExternalRouteVerb(const FAssetDocumentExternalRoute& Route)
	{
		const FString MethodText = GetExternalRouteMethodText(Route);
		if (MethodText == TEXT("POST"))
		{
			return EHttpServerRequestVerbs::VERB_POST;
		}
		if (MethodText == TEXT("PUT"))
		{
			return EHttpServerRequestVerbs::VERB_PUT;
		}
		if (MethodText == TEXT("PATCH"))
		{
			return EHttpServerRequestVerbs::VERB_PATCH;
		}
		if (MethodText == TEXT("DELETE"))
		{
			return EHttpServerRequestVerbs::VERB_DELETE;
		}
		return EHttpServerRequestVerbs::VERB_GET;
	}

	FString GetRequestBodyString(const FHttpServerRequest& Request)
	{
		if (Request.Body.IsEmpty())
		{
			return FString();
		}

		FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
		return FString(Converter.Length(), Converter.Get());
	}

	void SendExternalJsonResponse(const FHttpResultCallback& OnComplete, int32 StatusCode, TSharedPtr<FJsonObject> JsonResponse)
	{
		FString ResponseBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseBody);
		FJsonSerializer::Serialize(JsonResponse.ToSharedRef(), Writer);

		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseBody, TEXT("application/json"));
		Response->Code = static_cast<EHttpServerResponseCodes>(StatusCode);
		OnComplete(MoveTemp(Response));
	}

	TSharedPtr<FJsonObject> RunExternalRoute(const FAssetDocumentExternalRoute& Route, const FString& Body, const TMap<FString, FString>& QueryParams,
		int32& OutStatusCode)
	{
		if (!Route.Handler.IsBound())
		{
			TSharedPtr<FJsonObject> ErrorJson = MakeShared<FJsonObject>();
			ErrorJson->SetBoolField(TEXT("success"), false);
			ErrorJson->SetStringField(TEXT("error"), TEXT("External route handler is not bound"));
			OutStatusCode = 500;
			return ErrorJson;
		}

		OutStatusCode = 200;
		TSharedPtr<FJsonObject> ResponseJson = Route.Handler.Execute(Body, QueryParams, OutStatusCode);
		if (!ResponseJson.IsValid())
		{
			ResponseJson = MakeShared<FJsonObject>();
			ResponseJson->SetBoolField(TEXT("success"), false);
			ResponseJson->SetStringField(TEXT("error"), TEXT("External route handler returned no response"));
			OutStatusCode = 500;
		}
		return ResponseJson;
	}

	bool HandleExternalRoute(const FAssetDocumentExternalRoute& Route, const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
	{
		int32 StatusCode = 200;
		const TSharedPtr<FJsonObject> ResponseJson = RunExternalRoute(Route, GetRequestBodyString(Request), Request.QueryParams, StatusCode);
		SendExternalJsonResponse(OnComplete, StatusCode, ResponseJson);
		return true;
	}
}

FAssetDocumentHttpServer::FAssetDocumentHttpServer()
{
}

FAssetDocumentHttpServer::~FAssetDocumentHttpServer()
{
	Stop();
}

TMap<FDelegateHandle, FAssetDocumentExternalRoute>& FAssetDocumentHttpServer::GetExternalRoutes()
{
	static TMap<FDelegateHandle, FAssetDocumentExternalRoute> ExternalRoutes;
	return ExternalRoutes;
}

TSet<FAssetDocumentHttpServer*>& FAssetDocumentHttpServer::GetActiveServers()
{
	static TSet<FAssetDocumentHttpServer*> ActiveServers;
	return ActiveServers;
}

FDelegateHandle FAssetDocumentHttpServer::RegisterExternalRoute(const FAssetDocumentExternalRoute& Route)
{
	if (Route.Path.IsEmpty() || !Route.Handler.IsBound())
	{
		UE_LOG(LogAssetDocumentServer, Warning, TEXT("Ignoring invalid external route registration for path '%s'"), *Route.Path);
		return FDelegateHandle();
	}

	FString NormalizedMethodText;
	if (!TryNormalizeExternalRouteMethodText(Route.MethodText, NormalizedMethodText))
	{
		UE_LOG(LogAssetDocumentServer, Warning, TEXT("Ignoring external route registration for path '%s' with invalid method '%s'"), *Route.Path, *Route.MethodText);
		return FDelegateHandle();
	}

	FAssetDocumentExternalRoute NormalizedRoute = Route;
	NormalizedRoute.MethodText = NormalizedMethodText;

	FDelegateHandle Handle(FDelegateHandle::GenerateNewHandle);
	GetExternalRoutes().Add(Handle, NormalizedRoute);
	for (FAssetDocumentHttpServer* Server : GetActiveServers())
	{
		if (Server && Server->HttpRouter.IsValid())
		{
			Server->BindExternalRoute(Handle, NormalizedRoute);
			Server->MarkServiceDiscoveryDirty();
		}
	}
	return Handle;
}

void FAssetDocumentHttpServer::UnregisterExternalRoute(FDelegateHandle Handle)
{
	if (!Handle.IsValid())
	{
		return;
	}

	if (GetExternalRoutes().Remove(Handle) == 0)
	{
		return;
	}
	for (FAssetDocumentHttpServer* Server : GetActiveServers())
	{
		if (Server && Server->HttpRouter.IsValid())
		{
			Server->UnbindExternalRoute(Handle);
			Server->MarkServiceDiscoveryDirty();
		}
	}
}

int32 FAssetDocumentHttpServer::FlushServiceDiscovery()
{
	int32 Written = 0;
	for (FAssetDocumentHttpServer* Server : GetActiveServers())
	{
		if (Server && Server->bIsRunning && Server->bServiceDiscoveryDirty)
		{
			Server->WriteServiceDiscoveryFile();
			++Written;
		}
	}
	return Written;
}

int32 FAssetDocumentHttpServer::GetServiceDiscoveryWriteCount()
{
	return GServiceDiscoveryWrites;
}

void FAssetDocumentHttpServer::MarkServiceDiscoveryDirty()
{
	if (!bWritesServiceDiscovery)
	{
		return;
	}
	bServiceDiscoveryDirty = true;
	if (!ServiceDiscoveryTicker.IsValid())
	{
		ServiceDiscoveryTicker = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FAssetDocumentHttpServer::TickServiceDiscovery));
	}
}

bool FAssetDocumentHttpServer::TickServiceDiscovery(float DeltaTime)
{
	// The modules register their routes while the engine initializes (AssetDocument at Default, the level tools at
	// PostEngineInit): wait for the main loop so the whole startup set is one write.
	if (!GIsRunning)
	{
		return true;
	}
	ServiceDiscoveryTicker.Reset();
	if (bIsRunning && bServiceDiscoveryDirty)
	{
		WriteServiceDiscoveryFile();
	}
	return false;
}

FString FAssetDocumentHttpServer::DescribeBindFailure(uint32 Port)
{
	return FString::Printf(TEXT("The UEAssetDoc HTTP server cannot listen on port %u: the port is in use (often by an "
		"editor that crashed or is still closing, or by another editor using the same port). AI tools (MCP) cannot reach "
		"this editor. End the process that holds the port and restart the editor, or set another HTTP Server Port in "
		"Project Settings > Plugins > UE-AssetDoc."), Port);
}

TConstArrayView<float> FAssetDocumentHttpServer::BindRetryDelays()
{
	static const float Delays[] = {0.5f, 1.0f, 1.5f, 2.0f};
	return Delays;
}

bool FAssetDocumentHttpServer::Start(uint32 Port, int32 FallbackPorts)
{
	if (bIsRunning)
	{
		UE_LOG(LogAssetDocumentServer, Warning, TEXT("HTTP Server is already running on port %d"), CurrentPort);
		return true;
	}
	CancelBindRetry();
	ConfiguredPort = Port;
	FallbackPortCount = FMath::Max(0, FallbackPorts);

	// Security for this run: remote connections always need the token.
	const UAssetDocumentSettings* Settings = UAssetDocumentSettings::Get();
	bAllowRemoteConnections = Settings != nullptr && Settings->bAllowRemoteConnections;
	const bool bRequireToken = bAllowRemoteConnections || (Settings != nullptr && Settings->bRequireAuthToken);
	AuthToken = bRequireToken ? AssetDocumentHttpSecurity::GenerateToken() : FString();

	if (!McpTools.IsValid())
	{
		McpTools = MakeShared<FAssetDocumentMcpTools>();
		FString ManifestError;
		if (!McpTools->Load(FAssetDocumentMcpTools::DefaultMcpDirectory(), ManifestError))
		{
			UE_LOG(LogAssetDocumentServer, Warning, TEXT("UEAssetDoc MCP endpoint has no tools: %s"), *ManifestError);
		}
		McpEndpoint = MakeShared<FAssetDocumentMcpEndpoint>(McpTools.ToSharedRef(),
			[SharedPort = BoundPort](const FString& Method, const FString& Endpoint, const FString& Body, const TMap<FString, FString>& Query)
			{
				FAssetDocumentMcpRouteResult Result;
				Result.Json = CallRoute(Method, Endpoint, Body, Query, *SharedPort, Result.Status);
				return Result;
			},
			[]()
			{
				const UAssetDocumentSettings* Current = UAssetDocumentSettings::Get();
				FAssetDocumentMcpToolPolicy Policy;
				Policy.bExecutePython = Current == nullptr || Current->bEnableExecutePython;
				Policy.bDeleteAssets = Current == nullptr || Current->bEnableDeleteAssets;
				return Policy;
			});
	}
	if (TryStart(Port))
	{
		return true;
	}
	UE_LOG(LogAssetDocumentServer, Warning, TEXT("UEAssetDoc HTTP Server could not bind port %u; retrying for a few seconds"), Port);
	ScheduleBindRetry(Port, 0);
	return false;
}

void FAssetDocumentHttpServer::ScheduleBindRetry(uint32 Port, int32 Attempt)
{
	const TConstArrayView<float> Delays = BindRetryDelays();
	if (!Delays.IsValidIndex(Attempt))
	{
		FallBackOrReport(Port);
		return;
	}
	BindRetryTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[this, Port, Attempt](float)
		{
			BindRetryTicker.Reset();
			if (!bIsRunning)
			{
				if (TryStart(Port))
				{
					UE_LOG(LogAssetDocumentServer, Display, TEXT("UEAssetDoc HTTP Server bound port %u on retry %d"), Port, Attempt + 1);
				}
				else
				{
					ScheduleBindRetry(Port, Attempt + 1);
				}
			}
			return false;
		}), Delays[Attempt]);
}

void FAssetDocumentHttpServer::FallBackOrReport(uint32 Port)
{
	for (int32 Offset = 1; Offset <= FallbackPortCount; ++Offset)
	{
		const uint32 Candidate = Port + static_cast<uint32>(Offset);
		if (Candidate > 65535)
		{
			break;
		}
		if (TryStart(Candidate))
		{
			AssetDocumentHttpServerLocal::ReportPortFallback(Port, Candidate);
			return;
		}
	}
	AssetDocumentHttpServerLocal::ReportBindFailure(Port);
}

void FAssetDocumentHttpServer::CancelBindRetry()
{
	if (BindRetryTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(BindRetryTicker);
		BindRetryTicker.Reset();
	}
}

bool FAssetDocumentHttpServer::TryStart(uint32 Port)
{
	// Get HTTP router
	FHttpServerModule& HttpServerModule = FHttpServerModule::Get();
	HttpRouter = HttpServerModule.GetHttpRouter(Port);

	if (!HttpRouter.IsValid())
	{
		UE_LOG(LogAssetDocumentServer, Error, TEXT("Failed to get HTTP router for port %d"), Port);
		return false;
	}

	// Every request to this port passes the security checks before its route runs.
	const FAssetDocumentHttpSecurityPolicy SecurityPolicy{bAllowRemoteConnections, AuthToken};
	SecurityPreprocessor = HttpRouter->RegisterRequestPreprocessor(FHttpRequestHandler::CreateLambda(
		[SecurityPolicy](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			const TOptional<FAssetDocumentHttpRejection> Rejection = AssetDocumentHttpSecurity::Check(Request, SecurityPolicy);
			if (!Rejection.IsSet())
			{
				return false;
			}
			UE_LOG(LogAssetDocumentServer, Log, TEXT("Refused HTTP request to %s (%d): %s"), *Request.RelativePath.GetPath(),
				Rejection->Status, *Rejection->Message);
			TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(
				FString::Printf(TEXT("{\"success\":false,\"error\":%s}"), *AssetDocumentMcpJson::Quote(Rejection->Message)), TEXT("application/json"));
			Response->Code = static_cast<EHttpServerResponseCodes>(Rejection->Status);
			if (Rejection->Status == 401)
			{
				Response->Headers.Add(TEXT("WWW-Authenticate"), {TEXT("Bearer")});
			}
			OnComplete(MoveTemp(Response));
			return true;
		}));

	TArray<FRouteBindingSpec> RouteSpecs;
	RouteSpecs.Add({
		TEXT("/assetdoc/delete"),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateRaw(this, &FAssetDocumentHttpServer::HandleDelete),
		TEXT("POST"),
		TEXT("Delete assets"),
		true
	});
	RouteSpecs.Add({
		TEXT("/assetdoc/health"),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FAssetDocumentHttpServer::HandleHealth),
		TEXT("GET "),
		TEXT("Health check"),
		false
	});
	RouteSpecs.Add({
		TEXT("/assetdoc/context"),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FAssetDocumentHttpServer::HandleContext),
		TEXT("GET "),
		TEXT("Get current editor state (selected actors, assets, level, open editors)"),
		false
	});
	RouteSpecs.Add({
		TEXT("/assetdoc/screenshot"),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FAssetDocumentHttpServer::HandleGetViewportScreenshot),
		TEXT("GET "),
		TEXT("Capture Level Viewport as base64 JPEG"),
		false
	});
	RouteSpecs.Add({
		TEXT("/assetdoc/execute"),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateRaw(this, &FAssetDocumentHttpServer::HandleExecute),
		TEXT("POST"),
		TEXT("Execute Python code in the editor with undo support"),
		true
	});

	RouteHandles.Reset();
	for (const FRouteBindingSpec& Spec : RouteSpecs)
	{
		RouteHandles.Add(HttpRouter->BindRoute(FHttpPath(Spec.Path), Spec.Verb, Spec.Handler));
	}
	RouteHandles.Add(HttpRouter->BindRoute(FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_GET | EHttpServerRequestVerbs::VERB_POST | EHttpServerRequestVerbs::VERB_DELETE,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return McpEndpoint.IsValid() && McpEndpoint->HandleRequest(Request, OnComplete);
		})));
	BindExternalRoutes();

	// Start listeners
	AssetDocumentHttpServerLocal::StartListenersWithBindAddress(HttpServerModule, Port, bAllowRemoteConnections);

	// The router is returned (and the routes bound) even when its listener could not bind the port; only a router
	// asked for with bFailOnBindFailure says whether it listens (the same router when it does; a listener that does
	// not is dropped).
	if (HttpServerModule.GetHttpRouter(Port, /*bFailOnBindFailure*/ true) != HttpRouter)
	{
		UnbindAll();
		return false;
	}

	bIsRunning = true;
	CurrentPort = Port;
	*BoundPort = Port;
	GetActiveServers().Add(this);

	// Service discovery file for AI agents: written on the next tick of the running engine, with the routes the
	// modules loading after this one register in the meantime.
	MarkServiceDiscoveryDirty();

	UE_LOG(LogAssetDocumentServer, Display, TEXT("UEAssetDoc HTTP Server started on port %d"), Port);
	for (const FRouteBindingSpec& Spec : RouteSpecs)
	{
		UE_LOG(LogAssetDocumentServer, Display, TEXT("  %s http://localhost:%d%s"), Spec.LogMethod, Port, Spec.Path);
	}
	for (const TPair<FDelegateHandle, FAssetDocumentExternalRoute>& Pair : GetExternalRoutes())
	{
		UE_LOG(LogAssetDocumentServer, Display, TEXT("  %s http://localhost:%d%s"), *GetExternalRouteMethodText(Pair.Value), Port, *Pair.Value.Path);
	}
	UE_LOG(LogAssetDocumentServer, Display, TEXT("  MCP (Streamable HTTP): http://127.0.0.1:%d/mcp%s"), Port,
		AuthToken.IsEmpty() ? TEXT("") : TEXT(" (bearer token required: see the service discovery file)"));
	UE_LOG(LogAssetDocumentServer, Display, TEXT("  Service discovery: %s"), *GetServiceDiscoveryFilePath());

	return true;
}

void FAssetDocumentHttpServer::UnbindAll()
{
	if (HttpRouter.IsValid())
	{
		UnbindExternalRoutes();
		for (const FHttpRouteHandle& Handle : RouteHandles)
		{
			if (Handle.IsValid())
			{
				HttpRouter->UnbindRoute(Handle);
			}
		}
		if (SecurityPreprocessor.IsValid())
		{
			HttpRouter->UnregisterRequestPreprocessor(SecurityPreprocessor);
		}
	}
	SecurityPreprocessor.Reset();
	RouteHandles.Reset();
	HttpRouter.Reset();
}

void FAssetDocumentHttpServer::Stop()
{
	CancelBindRetry();
	if (!bIsRunning)
	{
		return;
	}

	// Drop a pending discovery write, then delete the service discovery file
	if (ServiceDiscoveryTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ServiceDiscoveryTicker);
		ServiceDiscoveryTicker.Reset();
	}
	bServiceDiscoveryDirty = false;
	DeleteServiceDiscoveryFile();

	// Unbind routes
	UnbindAll();

	bIsRunning = false;
	CurrentPort = 0;
	*BoundPort = 0;
	GetActiveServers().Remove(this);

	UE_LOG(LogAssetDocumentServer, Display, TEXT("UEAssetDoc HTTP Server stopped"));
}

void FAssetDocumentHttpServer::BindExternalRoutes()
{
	if (!HttpRouter.IsValid())
	{
		return;
	}

	for (const TPair<FDelegateHandle, FAssetDocumentExternalRoute>& Pair : GetExternalRoutes())
	{
		BindExternalRoute(Pair.Key, Pair.Value);
	}
}

void FAssetDocumentHttpServer::BindExternalRoute(FDelegateHandle Handle, const FAssetDocumentExternalRoute& Route)
{
	if (!HttpRouter.IsValid() || ExternalRouteHandles.Contains(Handle))
	{
		return;
	}
	if (Route.Path.IsEmpty() || !Route.Handler.IsBound())
	{
		UE_LOG(LogAssetDocumentServer, Warning, TEXT("Skipping invalid external route for path '%s'"), *Route.Path);
		return;
	}

	const FHttpRouteHandle RouteHandle = HttpRouter->BindRoute(FHttpPath(Route.Path), GetExternalRouteVerb(Route),
		FHttpRequestHandler::CreateLambda([Route](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleExternalRoute(Route, Request, OnComplete);
		}));
	if (RouteHandle.IsValid())
	{
		ExternalRouteHandles.Add(Handle, RouteHandle);
	}
	else
	{
		UE_LOG(LogAssetDocumentServer, Warning, TEXT("Could not bind external route %s %s (already bound?)"), *GetExternalRouteMethodText(Route), *Route.Path);
	}
}

void FAssetDocumentHttpServer::UnbindExternalRoute(FDelegateHandle Handle)
{
	FHttpRouteHandle RouteHandle;
	if (ExternalRouteHandles.RemoveAndCopyValue(Handle, RouteHandle) && HttpRouter.IsValid())
	{
		HttpRouter->UnbindRoute(RouteHandle);
	}
}

void FAssetDocumentHttpServer::UnbindExternalRoutes()
{
	if (HttpRouter.IsValid())
	{
		for (const TPair<FDelegateHandle, FHttpRouteHandle>& Pair : ExternalRouteHandles)
		{
			HttpRouter->UnbindRoute(Pair.Value);
		}
	}
	ExternalRouteHandles.Reset();
}

bool FAssetDocumentHttpServer::HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	int32 StatusCode = 200;
	const TSharedPtr<FJsonObject> ResponseJson = RunHealth(CurrentPort, StatusCode);
	SendJsonResponse(OnComplete, StatusCode, ResponseJson);
	return true;
}

TSharedPtr<FJsonObject> FAssetDocumentHttpServer::RunHealth(uint32 Port, int32& OutStatusCode)
{
	TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
	ResponseJson->SetStringField(TEXT("status"), TEXT("ok"));
	ResponseJson->SetStringField(TEXT("service"), TEXT("UEAssetDoc"));
	ResponseJson->SetNumberField(TEXT("port"), Port);

	// Where /Game maps on disk (AssetDocument sidecars live next to it: /Game/X/Y -> <contentDir>/X/Y.assetdoc.json).
	ResponseJson->SetStringField(TEXT("projectDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	ResponseJson->SetStringField(TEXT("contentDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()));

	OutStatusCode = 200;
	return ResponseJson;
}

bool FAssetDocumentHttpServer::HandleContext(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	int32 StatusCode = 200;
	const TSharedPtr<FJsonObject> ResponseJson = RunContext(StatusCode);
	SendJsonResponse(OnComplete, StatusCode, ResponseJson);
	return true;
}

TSharedPtr<FJsonObject> FAssetDocumentHttpServer::RunContext(int32& OutStatusCode)
{
	TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();

	auto DoCollect = [&]()
	{
		// 1. selectedActors
		TArray<TSharedPtr<FJsonValue>> ActorsArray;
		if (UEditorActorSubsystem* ActorSubsystem = GEditor->GetEditorSubsystem<UEditorActorSubsystem>())
		{
			TArray<AActor*> SelectedActors = ActorSubsystem->GetSelectedLevelActors();
			for (AActor* Actor : SelectedActors)
			{
				if (!Actor) continue;
				TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
				ActorObj->SetStringField(TEXT("name"), Actor->GetActorLabel());
				ActorObj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
				ActorObj->SetStringField(TEXT("path"), Actor->GetPathName());

				FVector Loc = Actor->GetActorLocation();
				TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
				LocObj->SetNumberField(TEXT("x"), Loc.X);
				LocObj->SetNumberField(TEXT("y"), Loc.Y);
				LocObj->SetNumberField(TEXT("z"), Loc.Z);
				ActorObj->SetObjectField(TEXT("location"), LocObj);

				FRotator Rot = Actor->GetActorRotation();
				TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
				RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch);
				RotObj->SetNumberField(TEXT("yaw"), Rot.Yaw);
				RotObj->SetNumberField(TEXT("roll"), Rot.Roll);
				ActorObj->SetObjectField(TEXT("rotation"), RotObj);

				FVector Scale = Actor->GetActorScale3D();
				TSharedPtr<FJsonObject> ScaleObj = MakeShared<FJsonObject>();
				ScaleObj->SetNumberField(TEXT("x"), Scale.X);
				ScaleObj->SetNumberField(TEXT("y"), Scale.Y);
				ScaleObj->SetNumberField(TEXT("z"), Scale.Z);
				ActorObj->SetObjectField(TEXT("scale"), ScaleObj);

				ActorsArray.Add(MakeShared<FJsonValueObject>(ActorObj));
			}
		}
		ResponseJson->SetArrayField(TEXT("selectedActors"), ActorsArray);

		// 2. selectedAssets from Content Browser
		TArray<TSharedPtr<FJsonValue>> AssetsArray;
		{
			FContentBrowserModule& CBModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
			TArray<FAssetData> SelectedAssets;
			CBModule.Get().GetSelectedAssets(SelectedAssets);
			for (const FAssetData& AssetData : SelectedAssets)
			{
				TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
				AssetObj->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
				AssetObj->SetStringField(TEXT("class"), AssetData.AssetClassPath.GetAssetName().ToString());
				AssetObj->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
				AssetsArray.Add(MakeShared<FJsonValueObject>(AssetObj));
			}
		}
		ResponseJson->SetArrayField(TEXT("selectedAssets"), AssetsArray);

		// 3. currentLevel
		{
			TSharedPtr<FJsonObject> LevelObj = MakeShared<FJsonObject>();
			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (World)
			{
				LevelObj->SetStringField(TEXT("levelName"), World->GetCurrentLevel()->GetOutermost()->GetName());
				LevelObj->SetStringField(TEXT("worldName"), World->GetName());
				LevelObj->SetStringField(TEXT("worldPath"), World->GetPathName());
			}
			ResponseJson->SetObjectField(TEXT("currentLevel"), LevelObj);
		}

		// 4. openEditors
		TArray<TSharedPtr<FJsonValue>> EditorsArray;
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			TArray<UObject*> EditedAssets = AssetEditorSubsystem->GetAllEditedAssets();
			for (UObject* Asset : EditedAssets)
			{
				if (!Asset) continue;
				TSharedPtr<FJsonObject> EditorObj = MakeShared<FJsonObject>();
				EditorObj->SetStringField(TEXT("name"), Asset->GetName());
				EditorObj->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
				EditorObj->SetStringField(TEXT("path"), Asset->GetPathName());

				IAssetEditorInstance* EditorInstance = AssetEditorSubsystem->FindEditorForAsset(Asset, false);
				if (EditorInstance)
				{
					EditorObj->SetStringField(TEXT("editorName"), EditorInstance->GetEditorName().ToString());
				}

				EditorsArray.Add(MakeShared<FJsonValueObject>(EditorObj));
			}
		}
		ResponseJson->SetArrayField(TEXT("openEditors"), EditorsArray);

		// 5. isPlayInEditor
		ResponseJson->SetBoolField(TEXT("isPlayInEditor"), GEditor->IsPlaySessionInProgress());

		// 6. activeModes — iterate all registered modes and check which are active
		TArray<TSharedPtr<FJsonValue>> ModesArray;
		{
			FEditorModeTools& ModeTools = GLevelEditorModeTools();
			const auto& FactoryMap = FEditorModeRegistry::Get().GetFactoryMap();
			for (const auto& Pair : FactoryMap)
			{
				if (ModeTools.IsModeActive(Pair.Key))
				{
					ModesArray.Add(MakeShared<FJsonValueString>(Pair.Key.ToString()));
				}
			}
		}
		ResponseJson->SetArrayField(TEXT("activeModes"), ModesArray);

		// 7. pendingNotices: open problems from work the editor did on its own (AssetDocumentPendingNotices.h)
		ResponseJson->SetArrayField(TEXT("pendingNotices"), FAssetDocumentPendingNotices::OpenToJson());
	};

	if (IsInGameThread())
	{
		DoCollect();
	}
	else
	{
		FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [&]()
		{
			DoCollect();
			CompletionEvent->Trigger();
		});
		CompletionEvent->Wait();
		FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
	}

	ResponseJson->SetBoolField(TEXT("success"), true);
	OutStatusCode = 200;
	return ResponseJson;
}

TSharedPtr<FJsonObject> FAssetDocumentHttpServer::MakeErrorJson(int32 StatusCode, const FString& ErrorMessage)
{
	TSharedPtr<FJsonObject> ErrorJson = MakeShared<FJsonObject>();
	ErrorJson->SetBoolField(TEXT("success"), false);
	ErrorJson->SetStringField(TEXT("error"), ErrorMessage);
	UE_LOG(LogAssetDocumentServer, Warning, TEXT("HTTP Error %d: %s"), StatusCode, *ErrorMessage);
	return ErrorJson;
}

bool FAssetDocumentHttpServer::HandleExecute(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	int32 StatusCode = 200;
	const TSharedPtr<FJsonObject> ResponseJson = RunExecute(GetRequestBodyString(Request), StatusCode);
	SendJsonResponse(OnComplete, StatusCode, ResponseJson);
	return true;
}

TSharedPtr<FJsonObject> FAssetDocumentHttpServer::RunExecute(const FString& RequestBody, int32& OutStatusCode)
{
	const UAssetDocumentSettings* Settings = UAssetDocumentSettings::Get();
	if (Settings != nullptr && !Settings->bEnableExecutePython)
	{
		OutStatusCode = 403;
		return MakeErrorJson(OutStatusCode, FAssetDocumentMcpTools::DescribeDisabledTool(TEXT("execute_python")));
	}

	if (RequestBody.IsEmpty())
	{
		OutStatusCode = 400;
		return MakeErrorJson(OutStatusCode, TEXT("Request body is empty"));
	}

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestBody);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		OutStatusCode = 400;
		return MakeErrorJson(OutStatusCode, FString::Printf(TEXT("Invalid JSON: %s"), *Reader->GetErrorMessage()));
	}

	FString Code;
	if (!JsonObject->TryGetStringField(TEXT("Code"), Code))
	{
		OutStatusCode = 400;
		return MakeErrorJson(OutStatusCode, TEXT("Missing required 'Code' field"));
	}

	FString Description;
	if (!JsonObject->TryGetStringField(TEXT("Description"), Description))
	{
		Description = TEXT("Python Execution via UEAssetDoc");
	}

	// Execute on game thread
	TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();

	auto DoExecute = [&]()
	{
		// One undo entry per successful script; a failing script is rolled back without
		// touching the user's previous undo entry (AssetDocumentServerEditorTransaction.h).
		const FAssetDocumentServerPythonExecutionResult Result = ExecuteAssetDocumentServerPython(Code, Description);
		ResponseJson->SetBoolField(TEXT("success"), Result.bSucceeded);
		if (!Result.Error.IsEmpty())
		{
			ResponseJson->SetStringField(TEXT("error"), Result.Error);
			return;
		}
		ResponseJson->SetStringField(TEXT("result"), Result.CommandResult);
		const auto Strings = [](const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Json;
			for (const FString& Value : Values)
			{
				Json.Add(MakeShared<FJsonValueString>(Value));
			}
			return Json;
		};
		if (Result.bSucceeded || Result.Rollback == EAssetDocumentTransactionRollback::Kept)
		{
			ResponseJson->SetBoolField(TEXT("undoable"), Result.bUndoEntryAdded);
			if (Result.bUndoEntryAdded)
			{
				// What undo_editor takes to undo exactly this script (expect_title / expect_id).
				ResponseJson->SetStringField(TEXT("undo_title"), Description);
				ResponseJson->SetStringField(TEXT("undo_id"), Result.UndoEntryId.ToString(EGuidFormats::Digits));
			}
		}
		if (!Result.NotUndoable.IsEmpty())
		{
			ResponseJson->SetArrayField(TEXT("not_undoable"), Strings(Result.NotUndoable));
		}
		if (!Result.bSucceeded)
		{
			ResponseJson->SetBoolField(TEXT("rolled_back"),
				Result.Rollback == EAssetDocumentTransactionRollback::Undone
				|| Result.Rollback == EAssetDocumentTransactionRollback::NothingRecorded);
			ResponseJson->SetStringField(TEXT("rollback"), LexToString(Result.Rollback));
			if (Result.Rollback == EAssetDocumentTransactionRollback::Undone)
			{
				// What the Undo restored (the script's recorded changes).
				ResponseJson->SetArrayField(TEXT("rolled_back_objects"), Strings(Result.RolledBackObjects));
				ResponseJson->SetNumberField(TEXT("rolled_back_object_count"), Result.RolledBackObjectCount);
			}
			else if (Result.Rollback == EAssetDocumentTransactionRollback::Kept)
			{
				ResponseJson->SetStringField(TEXT("rollback_note"), TEXT("Nothing was undone: the script also did work the Undo "
					"history cannot take back (not_undoable), and undoing only the rest would leave a half state. Everything the "
					"script did before it raised is kept; fix the state with another request (or undo_editor with undo_id for "
					"the undoable part)."));
			}
		}

		TArray<TSharedPtr<FJsonValue>> LogsArray;
		for (const FAssetDocumentServerPythonLogEntry& LogEntry : Result.Logs)
		{
			TSharedPtr<FJsonObject> LogEntryObj = MakeShared<FJsonObject>();
			LogEntryObj->SetStringField(TEXT("type"), LogEntry.Type);
			LogEntryObj->SetStringField(TEXT("message"), LogEntry.Message);
			LogsArray.Add(MakeShared<FJsonValueObject>(LogEntryObj));
		}
		ResponseJson->SetArrayField(TEXT("logs"), LogsArray);
	};

	if (IsInGameThread())
	{
		DoExecute();
	}
	else
	{
		FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [&]()
		{
			DoExecute();
			CompletionEvent->Trigger();
		});
		CompletionEvent->Wait();
		FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
	}

	OutStatusCode = ResponseJson->GetBoolField(TEXT("success")) ? 200 : 500;
	return ResponseJson;
}

bool FAssetDocumentHttpServer::HandleDelete(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	int32 StatusCode = 200;
	const TSharedPtr<FJsonObject> ResponseJson = RunDelete(GetRequestBodyString(Request), StatusCode);
	SendJsonResponse(OnComplete, StatusCode, ResponseJson);
	return true;
}

TSharedPtr<FJsonObject> FAssetDocumentHttpServer::RunDelete(const FString& RequestBody, int32& OutStatusCode)
{
	const UAssetDocumentSettings* Settings = UAssetDocumentSettings::Get();
	if (Settings != nullptr && !Settings->bEnableDeleteAssets)
	{
		OutStatusCode = 403;
		return MakeErrorJson(OutStatusCode, FAssetDocumentMcpTools::DescribeDisabledTool(TEXT("delete_assets")));
	}

	TSharedPtr<FJsonObject> JsonRequest;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestBody);
	if (!FJsonSerializer::Deserialize(Reader, JsonRequest) || !JsonRequest.IsValid())
	{
		OutStatusCode = 400;
		return MakeErrorJson(OutStatusCode, TEXT("Invalid JSON in request body"));
	}

	// Get asset paths to delete
	TArray<FString> AssetPaths;

	// Support both "Assets" array and single "Asset" string
	const TArray<TSharedPtr<FJsonValue>>* AssetsArray;
	if (JsonRequest->TryGetArrayField(TEXT("Assets"), AssetsArray))
	{
		for (const TSharedPtr<FJsonValue>& Value : *AssetsArray)
		{
			FString Path;
			if (Value->TryGetString(Path))
			{
				AssetPaths.Add(Path);
			}
		}
	}
	else
	{
		FString SingleAsset;
		if (JsonRequest->TryGetStringField(TEXT("Asset"), SingleAsset))
		{
			AssetPaths.Add(SingleAsset);
		}
	}

	if (AssetPaths.Num() == 0)
	{
		OutStatusCode = 400;
		return MakeErrorJson(OutStatusCode, TEXT("No assets specified. Use 'Assets' array or 'Asset' string."));
	}

	// Prepare response
	TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	int32 SuccessCount = 0;
	int32 FailedCount = 0;

	// Delete must run on game thread
	auto DoDelete = [&]()
	{
		// Resolve every asset first: the batch is deleted together, so references between its assets do not block
		// (as the editor's Delete Assets dialog with a multi-selection).
		TArray<UObject*> Resolved;
		for (const FString& AssetPath : AssetPaths)
		{
			// Build full asset path with object name suffix if needed
			const FString AssetName = FPaths::GetBaseFilename(AssetPath);
			const FString FullPathWithSuffix = FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
			UObject* Asset = StaticFindObject(UObject::StaticClass(), nullptr, *FullPathWithSuffix);
			if (!Asset)
			{
				Asset = LoadObject<UObject>(nullptr, *FullPathWithSuffix);
			}
			if (!Asset)
			{
				Asset = StaticFindObject(UObject::StaticClass(), nullptr, *AssetPath);
			}
			if (!Asset)
			{
				Asset = LoadObject<UObject>(nullptr, *AssetPath);
			}
			Resolved.Add(Asset);
		}
		TArray<UObject*> Found;
		for (UObject* Asset : Resolved)
		{
			if (Asset != nullptr)
			{
				Found.Add(Asset);
			}
		}
		// As the editor's Delete Assets dialog, without forcing: a refusal names what references the asset.
		const TArray<FAssetDocumentDeleteOutcome> FoundOutcomes = AssetDocumentAssetDelete::DeleteAssetsAsEditor(Found);
		int32 NextFound = 0;
		for (int32 Index = 0; Index < AssetPaths.Num(); ++Index)
		{
			const FString& AssetPath = AssetPaths[Index];
			TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
			ResultObj->SetStringField(TEXT("asset"), AssetPath);
			if (Resolved[Index] != nullptr)
			{
				const FAssetDocumentDeleteOutcome& Outcome = FoundOutcomes[NextFound++];
				if (Outcome.bUndoCleared)
				{
					ResultObj->SetBoolField(TEXT("undo_cleared"), true);
				}
				if (Outcome.bDeleted)
				{
					ResultObj->SetStringField(TEXT("status"), TEXT("Success"));
					ResultObj->SetStringField(TEXT("message"), Outcome.bUndoCleared
						? TEXT("Asset deleted; only the editor's Undo history referenced it, so it was cleared (as the Delete Assets dialog does)")
						: TEXT("Asset deleted successfully"));
					SuccessCount++;
					UE_LOG(LogAssetDocumentServer, Log, TEXT("Deleted asset: %s"), *AssetPath);
				}
				else
				{
					TArray<TSharedPtr<FJsonValue>> Referencers;
					for (const FAssetDocumentDeleteBlocker& Blocker : Outcome.Blockers)
					{
						TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("kind"), Blocker.Kind);
						if (!Blocker.Referencer.IsEmpty())
						{
							Entry->SetStringField(TEXT("referencer"), Blocker.Referencer);
						}
						if (!Blocker.Properties.IsEmpty())
						{
							TArray<TSharedPtr<FJsonValue>> Properties;
							for (const FString& Property : Blocker.Properties)
							{
								Properties.Add(MakeShared<FJsonValueString>(Property));
							}
							Entry->SetArrayField(TEXT("properties"), Properties);
						}
						if (Blocker.bRefusedInBatch)
						{
							Entry->SetBoolField(TEXT("refused_in_batch"), true);
						}
						Referencers.Add(MakeShared<FJsonValueObject>(Entry));
					}
					const FString Blockers = AssetDocumentAssetDelete::DescribeDeleteBlockers(Outcome.Blockers);
					ResultObj->SetStringField(TEXT("status"), TEXT("Failed"));
					ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("The editor's asset deletion refused it: %s. Remove these "
						"references first, or delete the referencing assets in the same call (references between the assets of one "
						"call do not block)."), *Blockers));
					ResultObj->SetArrayField(TEXT("referencers"), Referencers);
					FailedCount++;
					UE_LOG(LogAssetDocumentServer, Warning, TEXT("Failed to delete asset %s: %s"), *AssetPath, *Blockers);
				}
			}
			else
			{
				ResultObj->SetStringField(TEXT("status"), TEXT("Failed"));
				ResultObj->SetStringField(TEXT("error"), TEXT("Asset not found"));
				FailedCount++;
				UE_LOG(LogAssetDocumentServer, Warning, TEXT("Asset not found for deletion: %s"), *AssetPath);
			}

			ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
		}
	};

	if (IsInGameThread())
	{
		DoDelete();
	}
	else
	{
		FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [&]()
		{
			DoDelete();
			CompletionEvent->Trigger();
		});
		CompletionEvent->Wait();
		FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
	}

	ResponseJson->SetBoolField(TEXT("success"), FailedCount == 0);
	ResponseJson->SetNumberField(TEXT("total"), AssetPaths.Num());
	ResponseJson->SetNumberField(TEXT("succeeded"), SuccessCount);
	ResponseJson->SetNumberField(TEXT("failed"), FailedCount);
	ResponseJson->SetArrayField(TEXT("results"), ResultsArray);

	OutStatusCode = FailedCount > 0 ? 207 : 200;
	return ResponseJson;
}

TSharedPtr<FJsonObject> FAssetDocumentHttpServer::CallRoute(const FString& Method, const FString& Endpoint, const FString& Body,
	const TMap<FString, FString>& QueryParams, uint32 Port, int32& OutStatusCode)
{
	const FString Path = TEXT("/assetdoc") + Endpoint;
	if (Method == TEXT("GET") && Path == TEXT("/assetdoc/health"))
	{
		return RunHealth(Port, OutStatusCode);
	}
	if (Method == TEXT("GET") && Path == TEXT("/assetdoc/context"))
	{
		return RunContext(OutStatusCode);
	}
	if (Method == TEXT("POST") && Path == TEXT("/assetdoc/execute"))
	{
		return RunExecute(Body, OutStatusCode);
	}
	if (Method == TEXT("POST") && Path == TEXT("/assetdoc/delete"))
	{
		return RunDelete(Body, OutStatusCode);
	}
	for (const auto& Pair : GetExternalRoutes())
	{
		if (Pair.Value.Path == Path && GetExternalRouteMethodText(Pair.Value) == Method)
		{
			// The route may unregister routes while it runs; it runs on a copy.
			const FAssetDocumentExternalRoute Route = Pair.Value;
			return RunExternalRoute(Route, Body, QueryParams, OutStatusCode);
		}
	}
	// As the HTTP server answers a path no route handles.
	OutStatusCode = 404;
	TSharedPtr<FJsonObject> NotFound = MakeShared<FJsonObject>();
	NotFound->SetStringField(TEXT("errorCode"), TEXT("route_handler_not_found"));
	NotFound->SetStringField(TEXT("errorMessage"), TEXT(""));
	return NotFound;
}

void FAssetDocumentHttpServer::SendJsonResponse(const FHttpResultCallback& OnComplete, int32 StatusCode, TSharedPtr<FJsonObject> JsonResponse)
{
	// Serialize JSON to string
	FString ResponseBody;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseBody);
	FJsonSerializer::Serialize(JsonResponse.ToSharedRef(), Writer);

	// Create response
	TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseBody, TEXT("application/json"));
	Response->Code = static_cast<EHttpServerResponseCodes>(StatusCode);

	OnComplete(MoveTemp(Response));
}

void FAssetDocumentHttpServer::SendErrorResponse(const FHttpResultCallback& OnComplete, int32 StatusCode, const FString& ErrorMessage)
{
	SendJsonResponse(OnComplete, StatusCode, MakeErrorJson(StatusCode, ErrorMessage));
}

FString FAssetDocumentHttpServer::GetServiceDiscoveryFilePath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEAssetDoc"), TEXT("service.json"));
}

void FAssetDocumentHttpServer::WriteServiceDiscoveryFile()
{
	bServiceDiscoveryDirty = false;
	TArray<FRouteBindingSpec> RouteSpecs;
	RouteSpecs.Add({TEXT("/assetdoc/health"), EHttpServerRequestVerbs::VERB_GET, FHttpRequestHandler(), TEXT("GET"), TEXT("Health check"), false});
	RouteSpecs.Add({TEXT("/assetdoc/delete"), EHttpServerRequestVerbs::VERB_POST, FHttpRequestHandler(), TEXT("POST"), TEXT("Delete assets"), true});
	RouteSpecs.Add({TEXT("/assetdoc/context"), EHttpServerRequestVerbs::VERB_GET, FHttpRequestHandler(), TEXT("GET"), TEXT("Get current editor state (selected actors, assets, level, open editors)"), false});
	RouteSpecs.Add({TEXT("/assetdoc/execute"), EHttpServerRequestVerbs::VERB_POST, FHttpRequestHandler(), TEXT("POST"), TEXT("Execute Python code in the editor with undo support"), true});

	TSharedPtr<FJsonObject> ServiceInfo = MakeShared<FJsonObject>();

	// Basic info
	ServiceInfo->SetStringField(TEXT("service"), TEXT("UEAssetDoc"));
	ServiceInfo->SetNumberField(TEXT("port"), CurrentPort);
	ServiceInfo->SetNumberField(TEXT("configuredPort"), ConfiguredPort);
	ServiceInfo->SetStringField(TEXT("host"), TEXT("127.0.0.1"));
	ServiceInfo->SetStringField(TEXT("baseUrl"), FString::Printf(TEXT("http://127.0.0.1:%d"), CurrentPort));
	ServiceInfo->SetStringField(TEXT("mcpUrl"), FString::Printf(TEXT("http://127.0.0.1:%d/mcp"), CurrentPort));
	ServiceInfo->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
	ServiceInfo->SetStringField(TEXT("projectDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	ServiceInfo->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString(EVersionComponent::Patch));
	if (!AuthToken.IsEmpty())
	{
		ServiceInfo->SetStringField(TEXT("authToken"), AuthToken);
	}

	// Endpoints
	TArray<TSharedPtr<FJsonValue>> EndpointsArray;
	for (const FRouteBindingSpec& Spec : RouteSpecs)
	{
		TSharedPtr<FJsonObject> Endpoint = MakeShared<FJsonObject>();
		Endpoint->SetStringField(TEXT("method"), FString(Spec.LogMethod).TrimStartAndEnd());
		Endpoint->SetStringField(TEXT("path"), Spec.Path);
		Endpoint->SetStringField(TEXT("description"), Spec.Description);
		if (Spec.bJsonContentType)
		{
			Endpoint->SetStringField(TEXT("contentType"), TEXT("application/json"));
		}
		EndpointsArray.Add(MakeShared<FJsonValueObject>(Endpoint));
	}

	for (const TPair<FDelegateHandle, FAssetDocumentExternalRoute>& Pair : GetExternalRoutes())
	{
		const FAssetDocumentExternalRoute& Route = Pair.Value;
		if (Route.Path.IsEmpty())
		{
			continue;
		}

		TSharedPtr<FJsonObject> Endpoint = MakeShared<FJsonObject>();
		Endpoint->SetStringField(TEXT("method"), GetExternalRouteMethodText(Route).TrimStartAndEnd());
		Endpoint->SetStringField(TEXT("path"), Route.Path);
		Endpoint->SetStringField(TEXT("description"), Route.Description);
		if (Route.bRequiresBody)
		{
			Endpoint->SetStringField(TEXT("contentType"), TEXT("application/json"));
		}
		EndpointsArray.Add(MakeShared<FJsonValueObject>(Endpoint));
	}

	ServiceInfo->SetArrayField(TEXT("endpoints"), EndpointsArray);

	// Timestamp
	ServiceInfo->SetStringField(TEXT("startTime"), FDateTime::UtcNow().ToIso8601());

	// Serialize to string
	FString JsonString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonString);
	FJsonSerializer::Serialize(ServiceInfo.ToSharedRef(), Writer);

	// Ensure directory exists
	FString DirectoryPath = FPaths::GetPath(GetServiceDiscoveryFilePath());
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*DirectoryPath))
	{
		PlatformFile.CreateDirectoryTree(*DirectoryPath);
	}

	// Write file (readable by the current user only: it can hold the bearer token)
	FString SaveError;
	if (AssetDocumentUserOnlyFile::Save(JsonString, GetServiceDiscoveryFilePath(), SaveError))
	{
		++GServiceDiscoveryWrites;
		UE_LOG(LogAssetDocumentServer, Log, TEXT("Service discovery file written: %s"), *GetServiceDiscoveryFilePath());
	}
	else
	{
		UE_LOG(LogAssetDocumentServer, Warning, TEXT("Service discovery file not written (it could not be made readable by the current user "
			"only: %s): %s"), *SaveError, *GetServiceDiscoveryFilePath());
	}
}

void FAssetDocumentHttpServer::DeleteServiceDiscoveryFile()
{
	if (!bWritesServiceDiscovery)
	{
		return;
	}
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	FString FilePath = GetServiceDiscoveryFilePath();

	// Another editor of the same project may have written the file since (it listens on another port): keep its file.
	FString Existing;
	if (FFileHelper::LoadFileToString(Existing, *FilePath))
	{
		const TSharedPtr<FJsonObject> Info = AssetDocumentMcpJson::ParseObject(Existing);
		double Pid = 0.0;
		double Port = 0.0;
		if (Info.IsValid() && Info->TryGetNumberField(TEXT("pid"), Pid) && Info->TryGetNumberField(TEXT("port"), Port)
			&& (static_cast<uint32>(Pid) != FPlatformProcess::GetCurrentProcessId() || static_cast<uint32>(Port) != CurrentPort))
		{
			return;
		}
	}

	if (PlatformFile.FileExists(*FilePath))
	{
		if (PlatformFile.DeleteFile(*FilePath))
		{
			UE_LOG(LogAssetDocumentServer, Log, TEXT("Service discovery file deleted: %s"), *FilePath);
		}
		else
		{
			UE_LOG(LogAssetDocumentServer, Warning, TEXT("Failed to delete service discovery file: %s"), *FilePath);
		}
	}
}

bool FAssetDocumentHttpServer::HandleGetViewportScreenshot(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// The active Level Viewport, redrawn before its pixels are read (its last frame can predate the tools' edits).
	TArray<FColor> Bitmap;
	FIntPoint ViewportSize;
	int32 ErrorStatus = 500;
	FString Error;
	if (!FAssetDocumentViewportCapture::Capture(FAssetDocumentViewportCapture::FindViewportClient(), Bitmap, ViewportSize, ErrorStatus, Error))
	{
		SendErrorResponse(OnComplete, ErrorStatus, Error);
		return true;
	}

	// Nearest-neighbour downsample to max 1280px on longest side
	const int32 MaxDim = 1280;
	if (ViewportSize.X > MaxDim || ViewportSize.Y > MaxDim)
	{
		float Scale = FMath::Min((float)MaxDim / ViewportSize.X, (float)MaxDim / ViewportSize.Y);
		int32 NewWidth  = FMath::Max(1, FMath::RoundToInt(ViewportSize.X * Scale));
		int32 NewHeight = FMath::Max(1, FMath::RoundToInt(ViewportSize.Y * Scale));

		TArray<FColor> Scaled;
		Scaled.SetNumUninitialized(NewWidth * NewHeight);
		for (int32 Y = 0; Y < NewHeight; ++Y)
		{
			for (int32 X = 0; X < NewWidth; ++X)
			{
				int32 SrcX = FMath::Clamp(FMath::RoundToInt(X / Scale), 0, ViewportSize.X - 1);
				int32 SrcY = FMath::Clamp(FMath::RoundToInt(Y / Scale), 0, ViewportSize.Y - 1);
				Scaled[Y * NewWidth + X] = Bitmap[SrcY * ViewportSize.X + SrcX];
			}
		}
		Bitmap = MoveTemp(Scaled);
		ViewportSize.X = NewWidth;
		ViewportSize.Y = NewHeight;
	}

	// Compress to JPEG
	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>("ImageWrapper");
	TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::JPEG);
	if (!ImageWrapper.IsValid())
	{
		SendErrorResponse(OnComplete, 500, "Failed to create JPEG encoder");
		return true;
	}

	ImageWrapper->SetRaw(Bitmap.GetData(), Bitmap.Num() * sizeof(FColor),
		ViewportSize.X, ViewportSize.Y, ERGBFormat::BGRA, 8);
	TArray64<uint8> CompressedData = ImageWrapper->GetCompressed(85);

	if (CompressedData.Num() == 0)
	{
		SendErrorResponse(OnComplete, 500, "JPEG compression produced empty output");
		return true;
	}

	// Base64 encode
	FString Base64 = FBase64::Encode(CompressedData.GetData(), (int32)CompressedData.Num());

	TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
	ResponseJson->SetBoolField(TEXT("success"), true);
	ResponseJson->SetStringField(TEXT("data"), Base64);
	ResponseJson->SetStringField(TEXT("mimeType"), TEXT("image/jpeg"));
	ResponseJson->SetNumberField(TEXT("width"), ViewportSize.X);
	ResponseJson->SetNumberField(TEXT("height"), ViewportSize.Y);

	UE_LOG(LogAssetDocumentServer, Log, TEXT("Viewport screenshot captured: %dx%d, %lld bytes compressed, %d chars base64"),
		ViewportSize.X, ViewportSize.Y, CompressedData.Num(), Base64.Len());

	SendJsonResponse(OnComplete, 200, ResponseJson);
	return true;
}

