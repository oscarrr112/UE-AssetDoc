// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// UEAssetDoc.HttpServer.Mcp.Endpoint: the editor's MCP endpoint (Streamable HTTP) with a stub of the
//   editor's routes. initialize negotiates the protocol version ("2025-11-25" and "2025-06-18" among them) and opens a
//   session named in Mcp-Session-Id; every later message needs a known session (400 without, 404 for an unknown or
//   ended one); notifications answer 202; ping, tools/list (the manifest's tools, without those the settings switch
//   off, read at each request) and tools/call answer JSON, or one event-stream event to a client that accepts only that;
//   malformed messages, batches, unknown methods and bad params answer JSON-RPC errors; GET answers 405; DELETE ends
//   the session; the oldest sessions are dropped beyond the session limit.
// UEAssetDoc.HttpServer.Mcp.PendingNotices: a notice of work the editor did on its own (a failed file
//   watcher apply) is appended to the next tool result once, listed by get_editor_context while open, and gone once
//   resolved.

#include "AssetDocumentMcpEndpoint.h"
#include "AssetDocumentMcpJson.h"
#include "AssetDocumentMcpTools.h"
#include "AssetDocumentPendingNotices.h"
#include "AssetDocumentHttpServer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentMcpEndpointTestsLocal
{
struct FEndpointAnswer
{
	bool bAnswered = false;
	int32 Code = 0;
	TMap<FString, TArray<FString>> Headers;
	FString Body;

	FString Header(const FString& Name) const
	{
		for (const TPair<FString, TArray<FString>>& Pair : Headers)
		{
			if (Pair.Key.Equals(Name, ESearchCase::IgnoreCase) && Pair.Value.Num() > 0)
			{
				return Pair.Value[0];
			}
		}
		return FString();
	}

	TSharedPtr<FJsonObject> Json() const
	{
		return AssetDocumentMcpJson::ParseObject(Body);
	}

	int32 ErrorCode() const
	{
		const TSharedPtr<FJsonObject> Message = Json();
		const TSharedPtr<FJsonObject>* Error = nullptr;
		double ErrorNumber = 0.0;
		return Message.IsValid() && Message->TryGetObjectField(TEXT("error"), Error) && (*Error)->TryGetNumberField(TEXT("code"), ErrorNumber)
			? static_cast<int32>(ErrorNumber) : 0;
	}

	TSharedPtr<FJsonObject> Result() const
	{
		const TSharedPtr<FJsonObject> Message = Json();
		const TSharedPtr<FJsonObject>* Found = nullptr;
		return Message.IsValid() && Message->TryGetObjectField(TEXT("result"), Found) ? *Found : nullptr;
	}
};

FEndpointAnswer SendToEndpoint(FAssetDocumentMcpEndpoint& Endpoint, EHttpServerRequestVerbs Verb, const FString& Body,
	const TMap<FString, FString>& Headers)
{
	FHttpServerRequest Request;
	Request.Verb = Verb;
	for (const TPair<FString, FString>& Header : Headers)
	{
		// The server lower-cases header names and splits values at commas.
		TArray<FString> Values;
		Header.Value.ParseIntoArray(Values, TEXT(","), true);
		Request.Headers.Add(Header.Key.ToLower(), Values);
	}
	const FTCHARToUTF8 Utf8(*Body);
	Request.Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());

	FEndpointAnswer Answer;
	Endpoint.HandleRequest(Request, [&Answer](TUniquePtr<FHttpServerResponse>&& Response)
	{
		Answer.bAnswered = true;
		Answer.Code = static_cast<int32>(Response->Code);
		Answer.Headers = Response->Headers;
		const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Response->Body.GetData()), Response->Body.Num());
		Answer.Body = FString(Text.Length(), Text.Get());
	});
	return Answer;
}

const TCHAR* const AcceptBoth = TEXT("application/json, text/event-stream");

TMap<FString, FString> JsonHeaders(const FString& SessionId = FString(), const FString& ProtocolVersion = FString())
{
	TMap<FString, FString> Headers = {{TEXT("Content-Type"), TEXT("application/json")}, {TEXT("Accept"), AcceptBoth}};
	if (!SessionId.IsEmpty())
	{
		Headers.Add(TEXT("Mcp-Session-Id"), SessionId);
	}
	if (!ProtocolVersion.IsEmpty())
	{
		Headers.Add(TEXT("MCP-Protocol-Version"), ProtocolVersion);
	}
	return Headers;
}

FString InitializeBody(const FString& Version)
{
	return FString::Printf(TEXT("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"%s\","
		"\"capabilities\":{},\"clientInfo\":{\"name\":\"endpoint-test\",\"version\":\"1\"}}}"), *Version);
}

FString Request(int32 Id, const FString& Method, const FString& Params = FString())
{
	return Params.IsEmpty() ? FString::Printf(TEXT("{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\"}"), Id, *Method)
		: FString::Printf(TEXT("{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":%s}"), Id, *Method, *Params);
}

struct FEndpointStub
{
	TArray<FString> Calls;
	FAssetDocumentMcpToolPolicy Policy;
};
} // namespace AssetDocumentMcpEndpointTestsLocal

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMcpEndpointTest,
	"UEAssetDoc.HttpServer.Mcp.Endpoint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMcpEndpointTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMcpEndpointTestsLocal;
	const EHttpServerRequestVerbs Post = EHttpServerRequestVerbs::VERB_POST;

	const TSharedRef<FAssetDocumentMcpTools> Tools = MakeShared<FAssetDocumentMcpTools>();
	FString LoadError;
	if (!TestTrue(TEXT("the tool manifest loads"), Tools->Load(FAssetDocumentMcpTools::DefaultMcpDirectory(), LoadError)))
	{
		return false;
	}
	const TSharedRef<FEndpointStub> Stub = MakeShared<FEndpointStub>();
	FAssetDocumentMcpEndpoint Endpoint(Tools,
		[Stub](const FString& Method, const FString& Route, const FString& Body, const TMap<FString, FString>& Query)
		{
			Stub->Calls.Add(Method + TEXT(" ") + Route);
			FAssetDocumentMcpRouteResult Result;
			Result.Json = AssetDocumentMcpJson::ParseObject(TEXT("{\"status\":\"ok\",\"service\":\"UEAssetDoc\",\"port\":1}"));
			return Result;
		},
		[Stub]() { return Stub->Policy; });
	// Tool results here are compared whole: notices of work the editor did before this test are not appended to them.
	FAssetDocumentPendingNotices::TakeUnreported();

	// GET opens no stream.
	FEndpointAnswer Answer = SendToEndpoint(Endpoint, EHttpServerRequestVerbs::VERB_GET, FString(), {{TEXT("Accept"), TEXT("text/event-stream")}});
	TestEqual(TEXT("GET answers 405"), Answer.Code, 405);
	TestEqual(TEXT("405 names the allowed methods"), Answer.Header(TEXT("Allow")), FString(TEXT("POST, DELETE")));

	// initialize negotiates the version and opens a session.
	for (const TPair<FString, FString>& Version : TArray<TPair<FString, FString>>{
		{TEXT("2025-06-18"), TEXT("2025-06-18")}, {TEXT("2025-11-25"), TEXT("2025-11-25")}, {TEXT("1999-01-01"), TEXT("2025-11-25")}})
	{
		Answer = SendToEndpoint(Endpoint, Post, InitializeBody(Version.Key), JsonHeaders());
		const TSharedPtr<FJsonObject> Result = Answer.Result();
		TestEqual(*FString::Printf(TEXT("initialize %s answers 200"), *Version.Key), Answer.Code, 200);
		TestTrue(*FString::Printf(TEXT("initialize %s opens a session"), *Version.Key), !Answer.Header(TEXT("Mcp-Session-Id")).IsEmpty());
		TestTrue(*FString::Printf(TEXT("initialize %s negotiates %s"), *Version.Key, *Version.Value),
			Result.IsValid() && Result->GetStringField(TEXT("protocolVersion")) == Version.Value);
	}
	TestTrue(TEXT("both current protocol versions are supported"),
		FAssetDocumentMcpEndpoint::SupportedProtocolVersions().Contains(TEXT("2025-11-25")) && FAssetDocumentMcpEndpoint::SupportedProtocolVersions().Contains(TEXT("2025-06-18")));
	Answer = SendToEndpoint(Endpoint, Post, InitializeBody(TEXT("2025-06-18")), JsonHeaders());
	const FString Session = Answer.Header(TEXT("Mcp-Session-Id"));
	TestEqual(TEXT("the initialize result"), AssetDocumentMcpJson::StringifyObject(Answer.Json()),
		FString::Printf(TEXT("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{\"tools\":{}},\"serverInfo\":%s}}"),
			*AssetDocumentMcpJson::StringifyObject(Tools->GetServerInfo())));
	TestEqual(TEXT("four sessions are open"), Endpoint.NumSessions(), 4);
	const TMap<FString, FString> InSession = JsonHeaders(Session, TEXT("2025-06-18"));

	// Notifications and client responses need no answer.
	Answer = SendToEndpoint(Endpoint, Post, TEXT("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}"), InSession);
	TestTrue(TEXT("a notification answers 202 without a body"), Answer.Code == 202 && Answer.Body.IsEmpty());
	Answer = SendToEndpoint(Endpoint, Post, TEXT("{\"jsonrpc\":\"2.0\",\"id\":9,\"result\":{}}"), InSession);
	TestEqual(TEXT("a client response answers 202"), Answer.Code, 202);

	// Sessions.
	Answer = SendToEndpoint(Endpoint, Post, Request(2, TEXT("tools/list")), JsonHeaders());
	TestTrue(TEXT("a message without a session answers 400"), Answer.Code == 400 && Answer.Body.Contains(TEXT("Mcp-Session-Id")));
	Answer = SendToEndpoint(Endpoint, Post, Request(2, TEXT("tools/list")), JsonHeaders(TEXT("00000000-0000-0000-0000-000000000000")));
	TestTrue(TEXT("an unknown session answers 404 -32001"), Answer.Code == 404 && Answer.ErrorCode() == -32001);
	Answer = SendToEndpoint(Endpoint, Post, Request(2, TEXT("ping")), JsonHeaders(Session, TEXT("1999-01-01")));
	TestEqual(TEXT("an unsupported MCP-Protocol-Version answers 400"), Answer.Code, 400);

	// ping and tools/list.
	Answer = SendToEndpoint(Endpoint, Post, Request(3, TEXT("ping")), InSession);
	TestEqual(TEXT("ping"), Answer.Body, FString(TEXT("{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{}}")));
	TestEqual(TEXT("an answer names its session"), Answer.Header(TEXT("Mcp-Session-Id")), Session);
	Answer = SendToEndpoint(Endpoint, Post, Request(4, TEXT("tools/list")), InSession);
	const TSharedPtr<FJsonObject> Listed = Answer.Result();
	if (TestTrue(TEXT("tools/list answers tools"), Listed.IsValid() && Listed->HasTypedField<EJson::Array>(TEXT("tools"))))
	{
		const TSharedPtr<FJsonObject> Expected = MakeShared<FJsonObject>();
		Expected->SetArrayField(TEXT("tools"), Tools->ListTools(FAssetDocumentMcpToolPolicy()));
		TestEqual(TEXT("tools/list is the manifest"), AssetDocumentMcpJson::StringifyObject(Listed), AssetDocumentMcpJson::StringifyObject(Expected));
		TestEqual(TEXT("33 tools"), Listed->GetArrayField(TEXT("tools")).Num(), 33);
	}

	// tools/call.
	Answer = SendToEndpoint(Endpoint, Post, Request(5, TEXT("tools/call"), TEXT("{\"name\":\"health_check\",\"arguments\":{}}")), InSession);
	TestEqual(TEXT("tools/call answers the tool result"), Answer.Body, FString(TEXT(
		"{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"{\\\"status\\\":\\\"ok\\\",\\\"service\\\":\\\"UEAssetDoc\\\",\\\"port\\\":1}\"}]}}")));
	TestTrue(TEXT("the call reached the route"), Stub->Calls.Num() == 1 && Stub->Calls[0] == TEXT("GET /health"));
	Answer = SendToEndpoint(Endpoint, Post, Request(6, TEXT("tools/call"), TEXT("{\"name\":\"health_check\"}")), InSession);
	TestEqual(TEXT("arguments are optional"), Answer.Code, 200);
	Answer = SendToEndpoint(Endpoint, Post, Request(7, TEXT("tools/call"), TEXT("{\"name\":\"no_such_tool\",\"arguments\":{}}")), InSession);
	TestTrue(TEXT("an unknown tool is a tool error"), Answer.Body.Contains(TEXT("\"isError\":true")) && Answer.Body.Contains(TEXT("unknown tool: no_such_tool")));
	Answer = SendToEndpoint(Endpoint, Post, Request(8, TEXT("tools/call"), TEXT("{\"arguments\":{}}")), InSession);
	TestEqual(TEXT("tools/call without a name is -32602"), Answer.ErrorCode(), -32602);
	Answer = SendToEndpoint(Endpoint, Post, Request(9, TEXT("tools/call"), TEXT("{\"name\":\"health_check\",\"arguments\":[1]}")), InSession);
	TestEqual(TEXT("tools/call with array arguments is -32602"), Answer.ErrorCode(), -32602);
	Answer = SendToEndpoint(Endpoint, Post, Request(10, TEXT("resources/list")), InSession);
	TestEqual(TEXT("an unknown method is -32601"), Answer.ErrorCode(), -32601);

	// Settings read at each request.
	Stub->Policy.bExecutePython = false;
	Stub->Calls.Reset();
	Answer = SendToEndpoint(Endpoint, Post, Request(11, TEXT("tools/list")), InSession);
	TestFalse(TEXT("execute_python switched off is not listed"), Answer.Body.Contains(TEXT("\"name\":\"execute_python\"")));
	Answer = SendToEndpoint(Endpoint, Post, Request(12, TEXT("tools/call"), TEXT("{\"name\":\"execute_python\",\"arguments\":{\"code\":\"print(1)\"}}")), InSession);
	TestTrue(TEXT("execute_python switched off says so"), Answer.Body.Contains(TEXT("\"isError\":true")) && Answer.Body.Contains(TEXT("turned off")));
	TestEqual(TEXT("execute_python switched off runs nothing"), Stub->Calls.Num(), 0);
	Stub->Policy.bExecutePython = true;
	Answer = SendToEndpoint(Endpoint, Post, Request(13, TEXT("tools/list")), InSession);
	TestTrue(TEXT("execute_python switched on is listed again"), Answer.Body.Contains(TEXT("\"name\":\"execute_python\"")));

	// Malformed messages.
	Answer = SendToEndpoint(Endpoint, Post, TEXT("{"), InSession);
	TestTrue(TEXT("invalid JSON is -32700"), Answer.Code == 400 && Answer.ErrorCode() == -32700);
	Answer = SendToEndpoint(Endpoint, Post, FString::Printf(TEXT("[%s]"), *Request(14, TEXT("ping"))), InSession);
	TestTrue(TEXT("a batch is -32600"), Answer.Code == 400 && Answer.ErrorCode() == -32600);
	Answer = SendToEndpoint(Endpoint, Post, TEXT("{\"foo\":1}"), InSession);
	TestTrue(TEXT("a non-message is -32600"), Answer.Code == 400 && Answer.ErrorCode() == -32600);
	Answer = SendToEndpoint(Endpoint, Post, TEXT("{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"initialize\"}"), JsonHeaders());
	TestEqual(TEXT("initialize without params is -32602"), Answer.ErrorCode(), -32602);

	// Accept.
	TMap<FString, FString> StreamOnly = InSession;
	StreamOnly.Add(TEXT("Accept"), TEXT("text/event-stream"));
	Answer = SendToEndpoint(Endpoint, Post, Request(16, TEXT("ping")), StreamOnly);
	TestEqual(TEXT("an event-stream client gets one event"), Answer.Body, FString(TEXT("event: message\ndata: {\"jsonrpc\":\"2.0\",\"id\":16,\"result\":{}}\n\n")));
	TestTrue(TEXT("the event stream media type"), Answer.Header(TEXT("Content-Type")).StartsWith(TEXT("text/event-stream")));
	TMap<FString, FString> Html = InSession;
	Html.Add(TEXT("Accept"), TEXT("text/html"));
	TestEqual(TEXT("a client that accepts neither answers 406"), SendToEndpoint(Endpoint, Post, Request(17, TEXT("ping")), Html).Code, 406);

	// DELETE ends a session.
	TestEqual(TEXT("DELETE without a session answers 400"), SendToEndpoint(Endpoint, EHttpServerRequestVerbs::VERB_DELETE, FString(), {}).Code, 400);
	TestEqual(TEXT("DELETE ends the session"),
		SendToEndpoint(Endpoint, EHttpServerRequestVerbs::VERB_DELETE, FString(), {{TEXT("Mcp-Session-Id"), Session}}).Code, 200);
	Answer = SendToEndpoint(Endpoint, Post, Request(18, TEXT("ping")), InSession);
	TestTrue(TEXT("an ended session answers 404 -32001"), Answer.Code == 404 && Answer.ErrorCode() == -32001);
	TestEqual(TEXT("an ended session cannot be ended again"),
		SendToEndpoint(Endpoint, EHttpServerRequestVerbs::VERB_DELETE, FString(), {{TEXT("Mcp-Session-Id"), Session}}).Code, 404);

	// The session limit.
	FString Newest;
	for (int32 Index = 0; Index < FAssetDocumentMcpEndpoint::MaxSessions + 6; ++Index)
	{
		Newest = SendToEndpoint(Endpoint, Post, InitializeBody(TEXT("2025-11-25")), JsonHeaders()).Header(TEXT("Mcp-Session-Id"));
	}
	TestEqual(TEXT("sessions beyond the limit drop the oldest"), Endpoint.NumSessions(), FAssetDocumentMcpEndpoint::MaxSessions);
	TestEqual(TEXT("the newest session works"), SendToEndpoint(Endpoint, Post, Request(19, TEXT("ping")), JsonHeaders(Newest)).Code, 200);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMcpPendingNoticesTest,
	"UEAssetDoc.HttpServer.Mcp.PendingNotices",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMcpPendingNoticesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMcpEndpointTestsLocal;
	const EHttpServerRequestVerbs Post = EHttpServerRequestVerbs::VERB_POST;
	const TSharedRef<FAssetDocumentMcpTools> Tools = MakeShared<FAssetDocumentMcpTools>();
	FString LoadError;
	if (!TestTrue(TEXT("the tool manifest loads"), Tools->Load(FAssetDocumentMcpTools::DefaultMcpDirectory(), LoadError)))
	{
		return false;
	}
	FAssetDocumentMcpEndpoint Endpoint(Tools,
		[](const FString& Method, const FString& Route, const FString& Body, const TMap<FString, FString>& Query)
		{
			FAssetDocumentMcpRouteResult Result;
			Result.Json = AssetDocumentMcpJson::ParseObject(TEXT("{\"status\":\"ok\"}"));
			return Result;
		},
		[]() { return FAssetDocumentMcpToolPolicy(); });
	const FString Session = SendToEndpoint(Endpoint, Post, InitializeBody(TEXT("2025-11-25")), JsonHeaders()).Header(TEXT("Mcp-Session-Id"));
	const TMap<FString, FString> InSession = JsonHeaders(Session);
	const auto ToolTexts = [](const FEndpointAnswer& Answer)
	{
		TArray<FString> Texts;
		const TSharedPtr<FJsonObject> Result = Answer.Result();
		const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
		if (Result.IsValid() && Result->TryGetArrayField(TEXT("content"), Content))
		{
			for (const TSharedPtr<FJsonValue>& Block : *Content)
			{
				Texts.Add(Block->AsObject()->GetStringField(TEXT("text")));
			}
		}
		return Texts;
	};
	const auto ContextNotices = []()
	{
		int32 Status = 0;
		const TSharedPtr<FJsonObject> Context = FAssetDocumentHttpServer::CallRoute(TEXT("GET"), TEXT("/context"), FString(), {}, 0, Status);
		const TArray<TSharedPtr<FJsonValue>>* Notices = nullptr;
		TArray<FString> Files;
		if (Context.IsValid() && Context->TryGetArrayField(TEXT("pendingNotices"), Notices))
		{
			for (const TSharedPtr<FJsonValue>& Notice : *Notices)
			{
				Files.Add(Notice->AsObject()->GetStringField(TEXT("file")) + TEXT(" ") + Notice->AsObject()->GetStringField(TEXT("code")));
			}
		}
		return Files;
	};
	const FString Kind = TEXT("test_background_failure");
	const FString File = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), TEXT("NoticeTest/A.assetdoc.json")));
	ON_SCOPE_EXIT { FAssetDocumentPendingNotices::Resolve(Kind, File); };
	// Whatever an earlier test left unreported is not this test's.
	FAssetDocumentPendingNotices::TakeUnreported();

	FAssetDocumentPendingNotices::Post(Kind, File, TEXT("Test.Code"), TEXT("it failed"));
	FEndpointAnswer Answer = SendToEndpoint(Endpoint, Post, Request(2, TEXT("tools/call"), TEXT("{\"name\":\"health_check\",\"arguments\":{}}")), InSession);
	TArray<FString> Texts = ToolTexts(Answer);
	if (TestEqual(TEXT("the next tool result carries the notice as a second block"), Texts.Num(), 2))
	{
		TestEqual(TEXT("the tool's own result comes first"), Texts[0], FString(TEXT("{\"status\":\"ok\"}")));
		TestTrue(*FString::Printf(TEXT("the notice names kind, file, code and message (%s)"), *Texts[1]),
			Texts[1].Contains(Kind) && Texts[1].Contains(FPaths::GetCleanFilename(File)) && Texts[1].Contains(TEXT("[Test.Code]"))
			&& Texts[1].Contains(TEXT("it failed")));
	}
	Answer = SendToEndpoint(Endpoint, Post, Request(3, TEXT("tools/call"), TEXT("{\"name\":\"health_check\",\"arguments\":{}}")), InSession);
	TestEqual(TEXT("a notice is reported once"), ToolTexts(Answer).Num(), 1);
	TestTrue(TEXT("get_editor_context lists the open notice"), ContextNotices().Contains(FString::Printf(TEXT("%s Test.Code"), *File)));

	// The same file failing again is reported again (relative spelling of the same file).
	FAssetDocumentPendingNotices::Post(Kind, FPaths::Combine(FPaths::ProjectContentDir(), TEXT("NoticeTest/A.assetdoc.json")), TEXT("Test.Other"), TEXT("again"));
	Answer = SendToEndpoint(Endpoint, Post, Request(4, TEXT("tools/call"), TEXT("{\"name\":\"health_check\",\"arguments\":{}}")), InSession);
	Texts = ToolTexts(Answer);
	TestTrue(TEXT("a new failure of the file is reported again"), Texts.Num() == 2 && Texts[1].Contains(TEXT("[Test.Other]")));
	TestEqual(TEXT("one open notice per file"), FAssetDocumentPendingNotices::GetOpen().FilterByPredicate(
		[&Kind](const FAssetDocumentPendingNotice& Notice) { return Notice.Kind == Kind; }).Num(), 1);

	FAssetDocumentPendingNotices::Resolve(Kind, File);
	TestFalse(TEXT("a resolved notice is no longer listed"), ContextNotices().ContainsByPredicate(
		[&File](const FString& Entry) { return Entry.StartsWith(File); }));
	Answer = SendToEndpoint(Endpoint, Post, Request(5, TEXT("tools/call"), TEXT("{\"name\":\"health_check\",\"arguments\":{}}")), InSession);
	TestEqual(TEXT("nothing is appended without notices"), ToolTexts(Answer).Num(), 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
