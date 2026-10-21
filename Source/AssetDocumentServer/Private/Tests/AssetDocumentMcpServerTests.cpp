// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// UEAssetDoc.HttpServer.Mcp.Server: a second HTTP server of this editor, over real HTTP requests.
//   Its port is taken, so after the bind retries it falls back to the next free port and says so. With Require Auth
//   Token on, every route (the /mcp endpoint and the /assetdoc routes) needs the bearer token; the security checks
//   refuse a foreign Origin or Host and a body not sent as JSON on every route; an MCP session works end to end (tool
//   calls reach the routes in process) and a tool switched off in the settings is neither listed nor run. The editor's
//   own server writes the MCP URL, the bound and configured ports and the process id to the service discovery file,
//   readable by the current user only, and the test server leaves that file alone.

#include "AssetDocumentHttpServer.h"
#include "AssetDocumentMcpJson.h"
#include "AssetDocumentServerModule.h"
#include "AssetDocumentSettings.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "IPAddress.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Tests/AutomationCommon.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#endif

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentMcpServerTestsLocal
{
// A loopback socket listening on a free port, as another editor that holds the configured port.
struct FHeldPort
{
	FUniqueSocket Socket;
	uint32 Port = 0;

	FHeldPort()
	{
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (Sockets == nullptr)
		{
			return;
		}
		Socket = Sockets->CreateUniqueSocket(NAME_Stream, TEXT("MCP server test port holder"));
		TSharedRef<FInternetAddr> Address = Sockets->CreateInternetAddr();
		Address->SetLoopbackAddress();
		Address->SetPort(0);
		if (Socket && Socket->Bind(*Address) && Socket->Listen(1))
		{
			Port = static_cast<uint32>(Socket->GetPortNo());
		}
	}
};

struct FServerAnswer
{
	bool bDone = false;
	int32 Code = -1;
	FString Body;
	FString SessionId;
	FString Authenticate;
};

TSharedRef<FServerAnswer> SendToServer(uint32 Port, const FString& Verb, const FString& Path, const FString& Body,
	const TMap<FString, FString>& Headers)
{
	const TSharedRef<FServerAnswer> Answer = MakeShared<FServerAnswer>();
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(FString::Printf(TEXT("http://127.0.0.1:%u%s"), Port, *Path));
	Request->SetVerb(Verb);
	for (const TPair<FString, FString>& Header : Headers)
	{
		Request->SetHeader(Header.Key, Header.Value);
	}
	if (!Body.IsEmpty())
	{
		Request->SetContentAsString(Body);
	}
	Request->OnProcessRequestComplete().BindLambda([Answer](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
	{
		Answer->bDone = true;
		if (bConnected && Response.IsValid())
		{
			Answer->Code = Response->GetResponseCode();
			Answer->Body = Response->GetContentAsString();
			Answer->SessionId = Response->GetHeader(TEXT("Mcp-Session-Id"));
			Answer->Authenticate = Response->GetHeader(TEXT("WWW-Authenticate"));
		}
	});
	if (!Request->ProcessRequest())
	{
		Answer->bDone = true;
	}
	return Answer;
}

TMap<FString, FString> McpHeaders(const FString& Token, const FString& SessionId = FString())
{
	TMap<FString, FString> Headers = {{TEXT("Content-Type"), TEXT("application/json")}, {TEXT("Accept"), TEXT("application/json, text/event-stream")}};
	if (!Token.IsEmpty())
	{
		Headers.Add(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Token));
	}
	if (!SessionId.IsEmpty())
	{
		Headers.Add(TEXT("Mcp-Session-Id"), SessionId);
	}
	return Headers;
}

const TCHAR* const InitializeMessage = TEXT("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\","
	"\"capabilities\":{},\"clientInfo\":{\"name\":\"server-test\",\"version\":\"1\"}}}");

// The test's steps run one per latent update: start, the first requests, the session's requests, the end.
struct FServerTestState
{
	TSharedPtr<FAssetDocumentHttpServer> Server;
	TSharedPtr<FHeldPort> Held;
	bool bSavedExecutePython = true;
	int32 Step = 0;
	double Deadline = 0.0;
	TMap<FString, TSharedRef<FServerAnswer>> Answers;

	bool AllDone() const
	{
		for (const TPair<FString, TSharedRef<FServerAnswer>>& Pair : Answers)
		{
			if (!Pair.Value->bDone)
			{
				return false;
			}
		}
		return true;
	}

	const FServerAnswer& Get(const TCHAR* Name) const
	{
		return *Answers.FindChecked(Name);
	}
};
} // namespace AssetDocumentMcpServerTestsLocal

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMcpServerTest,
	"UEAssetDoc.HttpServer.Mcp.Server",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMcpServerTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMcpServerTestsLocal;
	UAssetDocumentSettings* Settings = UAssetDocumentSettings::Get();
	if (!TestNotNull(TEXT("the settings exist"), Settings))
	{
		return false;
	}

	// The editor's own server: the discovery file names the MCP endpoint and is readable by its user only.
	FAssetDocumentServerModule* Module = FModuleManager::GetModulePtr<FAssetDocumentServerModule>(TEXT("AssetDocumentServer"));
	const FAssetDocumentHttpServer* EditorServer = Module != nullptr ? Module->GetHttpServer() : nullptr;
	uint32 EditorPort = 0;
	if (EditorServer != nullptr && EditorServer->IsRunning())
	{
		EditorPort = EditorServer->GetPort();
		FAssetDocumentHttpServer::FlushServiceDiscovery();
		const FString DiscoveryPath = FAssetDocumentHttpServer::GetServiceDiscoveryFilePath();
		FString Discovery;
		const TSharedPtr<FJsonObject> Info = FFileHelper::LoadFileToString(Discovery, *DiscoveryPath) ? AssetDocumentMcpJson::ParseObject(Discovery) : nullptr;
		if (TestTrue(TEXT("the service discovery file is readable"), Info.IsValid()))
		{
			TestEqual(TEXT("discovery: the MCP URL"), Info->GetStringField(TEXT("mcpUrl")), FString::Printf(TEXT("http://127.0.0.1:%u/mcp"), EditorPort));
			TestEqual(TEXT("discovery: the bound port"), static_cast<uint32>(Info->GetNumberField(TEXT("port"))), EditorPort);
			TestEqual(TEXT("discovery: the configured port"), static_cast<uint32>(Info->GetNumberField(TEXT("configuredPort"))), EditorServer->GetConfiguredPort());
			TestEqual(TEXT("discovery: the process"), static_cast<uint32>(Info->GetNumberField(TEXT("pid"))), FPlatformProcess::GetCurrentProcessId());
			TestEqual(TEXT("discovery: the token only when one is required"), Info->HasField(TEXT("authToken")), !EditorServer->GetAuthToken().IsEmpty());
		}
#if PLATFORM_MAC || PLATFORM_LINUX
		struct stat Status;
		if (TestEqual(TEXT("the discovery file can be examined"), stat(TCHAR_TO_UTF8(*DiscoveryPath), &Status), 0))
		{
			TestEqual(TEXT("the discovery file is readable by its user only"), static_cast<int32>(Status.st_mode & 077), 0);
		}
#endif
	}
	else
	{
		AddInfo(TEXT("The editor's UEAssetDoc HTTP server is not running: its discovery file is not checked."));
	}

	// A second server whose port is held: it retries, then binds a later port.
	const TSharedRef<FServerTestState> State = MakeShared<FServerTestState>();
	State->Held = MakeShared<FHeldPort>();
	if (!TestTrue(TEXT("a free loopback port is held"), State->Held->Port != 0))
	{
		return false;
	}
	// The engine's listener logs each bind failure; the fallback is a warning, and so is the execute route's refusal
	// while execute_python is switched off.
	AddExpectedError(TEXT("unable to bind"), EAutomationExpectedErrorFlags::Contains, -1);
	AddExpectedError(TEXT("is not listening/bound"), EAutomationExpectedErrorFlags::Contains, -1);
	AddExpectedMessage(TEXT("failed to start listening"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	AddExpectedMessage(TEXT("could not bind port"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	AddExpectedMessage(TEXT("is in use (another editor?)"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1, false);
	AddExpectedMessage(TEXT("HTTP Error 403"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	State->Server = MakeShared<FAssetDocumentHttpServer>();
	State->Server->SetWritesServiceDiscovery(false);
	const bool bSavedRequireToken = Settings->bRequireAuthToken;
	Settings->bRequireAuthToken = true;
	const bool bStartedAtOnce = State->Server->Start(State->Held->Port, 3);
	Settings->bRequireAuthToken = bSavedRequireToken;
	State->bSavedExecutePython = Settings->bEnableExecutePython;
	TestFalse(TEXT("the held port is not bound at once"), bStartedAtOnce);
	TestTrue(TEXT("the server retries"), State->Server->IsWaitingForPort());
	State->Deadline = FPlatformTime::Seconds() + 20.0;

	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State, Settings, EditorPort]()
	{
		FAssetDocumentHttpServer& Server = *State->Server;
		const bool bTimedOut = FPlatformTime::Seconds() > State->Deadline;
		const uint32 HeldPort = State->Held->Port;
		if (State->Step == 0)
		{
			if (!Server.IsRunning() && !bTimedOut)
			{
				return false;
			}
			if (!TestTrue(TEXT("the server falls back to a later port"), Server.IsRunning()))
			{
				Server.Stop();
				return true;
			}
			const uint32 Port = Server.GetPort();
			TestTrue(*FString::Printf(TEXT("bound port %u is one of the 3 after %u"), Port, HeldPort), Port > HeldPort && Port <= HeldPort + 3);
			TestEqual(TEXT("the configured port is kept"), Server.GetConfiguredPort(), HeldPort);
			const FString Token = Server.GetAuthToken();
			TestEqual(TEXT("a token is generated when required"), Token.Len(), 64);

			const TMap<FString, FString> Json = {{TEXT("Content-Type"), TEXT("application/json")}};
			State->Answers.Add(TEXT("mcp without token"), SendToServer(Port, TEXT("POST"), TEXT("/mcp"), InitializeMessage, McpHeaders(FString())));
			State->Answers.Add(TEXT("health without token"), SendToServer(Port, TEXT("GET"), TEXT("/assetdoc/health"), FString(), {}));
			State->Answers.Add(TEXT("health"), SendToServer(Port, TEXT("GET"), TEXT("/assetdoc/health"), FString(),
				{{TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Token)}}));
			State->Answers.Add(TEXT("wrong token"), SendToServer(Port, TEXT("POST"), TEXT("/mcp"), InitializeMessage, McpHeaders(TEXT("0123"))));
			TMap<FString, FString> Origin = McpHeaders(Token);
			Origin.Add(TEXT("Origin"), TEXT("http://evil.example"));
			State->Answers.Add(TEXT("foreign origin"), SendToServer(Port, TEXT("POST"), TEXT("/mcp"), InitializeMessage, Origin));
			TMap<FString, FString> Host = McpHeaders(Token);
			Host.Add(TEXT("Host"), FString::Printf(TEXT("evil.example:%u"), Port));
			State->Answers.Add(TEXT("foreign host"), SendToServer(Port, TEXT("POST"), TEXT("/mcp"), InitializeMessage, Host));
			TMap<FString, FString> Plain = McpHeaders(Token);
			Plain.Add(TEXT("Content-Type"), TEXT("text/plain"));
			State->Answers.Add(TEXT("text body"), SendToServer(Port, TEXT("POST"), TEXT("/mcp"), InitializeMessage, Plain));
			TMap<FString, FString> PlainRoute = {{TEXT("Content-Type"), TEXT("text/plain")}, {TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Token)}};
			State->Answers.Add(TEXT("text body to a route"), SendToServer(Port, TEXT("POST"), TEXT("/assetdoc/execute"), TEXT("{\"Code\":\"print(1)\"}"), PlainRoute));
			State->Answers.Add(TEXT("initialize"), SendToServer(Port, TEXT("POST"), TEXT("/mcp"), InitializeMessage, McpHeaders(Token)));
			State->Answers.Add(TEXT("get"), SendToServer(Port, TEXT("GET"), TEXT("/mcp"), FString(),
				{{TEXT("Accept"), TEXT("text/event-stream")}, {TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Token)}}));
			State->Step = 1;
			return false;
		}
		if (State->Step == 1)
		{
			if (!State->AllDone() && !bTimedOut)
			{
				return false;
			}
			const uint32 Port = Server.GetPort();
			const FString Token = Server.GetAuthToken();
			auto Expect = [this, State](const TCHAR* Name, int32 Code)
			{
				const FServerAnswer& Answer = State->Get(Name);
				TestEqual(*FString::Printf(TEXT("%s answers %d (%s)"), Name, Code, *Answer.Body.Left(200)), Answer.Code, Code);
			};
			Expect(TEXT("mcp without token"), 401);
			TestEqual(TEXT("401 asks for a bearer token"), State->Get(TEXT("mcp without token")).Authenticate, FString(TEXT("Bearer")));
			TestTrue(TEXT("401 says where the token is"), State->Get(TEXT("mcp without token")).Body.Contains(TEXT("service.json")));
			Expect(TEXT("health without token"), 401);
			Expect(TEXT("health"), 200);
			const TSharedPtr<FJsonObject> Health = AssetDocumentMcpJson::ParseObject(State->Get(TEXT("health")).Body);
			TestTrue(TEXT("health names the bound port"), Health.IsValid() && static_cast<uint32>(Health->GetNumberField(TEXT("port"))) == Port);
			Expect(TEXT("wrong token"), 401);
			Expect(TEXT("foreign origin"), 403);
			Expect(TEXT("foreign host"), 403);
			Expect(TEXT("text body"), 415);
			Expect(TEXT("text body to a route"), 415);
			Expect(TEXT("initialize"), 200);
			Expect(TEXT("get"), 405);
			const FString Session = State->Get(TEXT("initialize")).SessionId;
			TestFalse(TEXT("initialize opens a session"), Session.IsEmpty());

			// In the session: a tool call through the routes, and execute_python switched off.
			Settings->bEnableExecutePython = false;
			State->Answers.Reset();
			const TMap<FString, FString> InSession = McpHeaders(Token, Session);
			State->Answers.Add(TEXT("health_check"), SendToServer(Port, TEXT("POST"), TEXT("/mcp"),
				TEXT("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"health_check\",\"arguments\":{}}}"), InSession));
			State->Answers.Add(TEXT("tools/list"), SendToServer(Port, TEXT("POST"), TEXT("/mcp"),
				TEXT("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\"}"), InSession));
			State->Answers.Add(TEXT("execute_python"), SendToServer(Port, TEXT("POST"), TEXT("/mcp"),
				TEXT("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"execute_python\",\"arguments\":{\"code\":\"print(1)\"}}}"), InSession));
			State->Answers.Add(TEXT("execute route"), SendToServer(Port, TEXT("POST"), TEXT("/assetdoc/execute"), TEXT("{\"Code\":\"print(1)\"}"),
				{{TEXT("Content-Type"), TEXT("application/json")}, {TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Token)}}));
			State->Step = 2;
			return false;
		}
		if (State->Step == 2)
		{
			if (!State->AllDone() && !bTimedOut)
			{
				return false;
			}
			Settings->bEnableExecutePython = State->bSavedExecutePython;
			const uint32 Port = Server.GetPort();
			const FServerAnswer& Health = State->Get(TEXT("health_check"));
			TestTrue(*FString::Printf(TEXT("health_check runs through the route (%s)"), *Health.Body.Left(300)), Health.Code == 200
				&& Health.Body.Contains(FString::Printf(TEXT("\\\"port\\\":%u"), Port)) && !Health.Body.Contains(TEXT("\"isError\":true")));
			const FServerAnswer& Listed = State->Get(TEXT("tools/list"));
			TestTrue(TEXT("execute_python switched off is not listed"), Listed.Code == 200 && Listed.Body.Contains(TEXT("\"name\":\"health_check\""))
				&& !Listed.Body.Contains(TEXT("\"name\":\"execute_python\"")));
			const FServerAnswer& Python = State->Get(TEXT("execute_python"));
			TestTrue(*FString::Printf(TEXT("execute_python switched off says so (%s)"), *Python.Body.Left(300)), Python.Code == 200
				&& Python.Body.Contains(TEXT("\"isError\":true")) && Python.Body.Contains(TEXT("turned off")));
			TestEqual(TEXT("the execute route refuses while switched off"), State->Get(TEXT("execute route")).Code, 403);

			Server.Stop();
			TestFalse(TEXT("the test server stops"), Server.IsRunning());
			if (EditorPort != 0)
			{
				FString Discovery;
				FFileHelper::LoadFileToString(Discovery, *FAssetDocumentHttpServer::GetServiceDiscoveryFilePath());
				const TSharedPtr<FJsonObject> Info = AssetDocumentMcpJson::ParseObject(Discovery);
				TestTrue(TEXT("the test server left the editor's discovery file alone"),
					Info.IsValid() && static_cast<uint32>(Info->GetNumberField(TEXT("port"))) == EditorPort);
			}
			return true;
		}
		return true;
	}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
