// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentMcpEndpoint.h"

#include "AssetDocumentHttpSecurity.h"
#include "AssetDocumentMcpJson.h"
#include "AssetDocumentPendingNotices.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace AssetDocumentMcpEndpointLocal
{
const TCHAR* const SessionHeader = TEXT("Mcp-Session-Id");

enum class EResponseFormat : uint8
{
	Json,
	EventStream,
};

void Send(const FHttpResultCallback& OnComplete, int32 Status, const FString& Body, const FString& ContentType,
	const TMap<FString, FString>& Headers = {})
{
	TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(Body, ContentType);
	Response->Code = static_cast<EHttpServerResponseCodes>(Status);
	for (const TPair<FString, FString>& Header : Headers)
	{
		Response->Headers.Add(Header.Key, {Header.Value});
	}
	OnComplete(MoveTemp(Response));
}

FString JsonRpcError(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message)
{
	const TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
	Error->SetNumberField(TEXT("code"), Code);
	Error->SetStringField(TEXT("message"), Message);
	const TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Envelope->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
	Envelope->SetObjectField(TEXT("error"), Error);
	return AssetDocumentMcpJson::StringifyObject(Envelope);
}

FString JsonRpcResult(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Result)
{
	const TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Envelope->SetField(TEXT("id"), Id);
	Envelope->SetObjectField(TEXT("result"), Result);
	return AssetDocumentMcpJson::StringifyObject(Envelope);
}

void SendMessage(const FHttpResultCallback& OnComplete, int32 Status, EResponseFormat Format, const FString& Message,
	const TMap<FString, FString>& Headers = {})
{
	if (Format == EResponseFormat::EventStream)
	{
		Send(OnComplete, Status, FString::Printf(TEXT("event: message\ndata: %s\n\n"), *Message), TEXT("text/event-stream"), Headers);
	}
	else
	{
		Send(OnComplete, Status, Message, TEXT("application/json"), Headers);
	}
}

// The media types of an Accept header, lower-case, without parameters.
TArray<FString> AcceptedTypes(const FHttpServerRequest& Request)
{
	TArray<FString> Types;
	if (const TArray<FString>* Values = Request.Headers.Find(TEXT("accept")))
	{
		for (const FString& Value : *Values)
		{
			FString Type = Value;
			int32 Semicolon = INDEX_NONE;
			if (Type.FindChar(TEXT(';'), Semicolon))
			{
				Type.LeftInline(Semicolon);
			}
			Types.Add(Type.TrimStartAndEnd().ToLower());
		}
	}
	return Types;
}

FString BodyText(const FHttpServerRequest& Request)
{
	if (Request.Body.IsEmpty())
	{
		return FString();
	}
	const FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
	return FString(Converter.Length(), Converter.Get());
}
} // namespace AssetDocumentMcpEndpointLocal

FAssetDocumentMcpEndpoint::FAssetDocumentMcpEndpoint(const TSharedRef<FAssetDocumentMcpTools>& InTools, FAssetDocumentMcpRouteCaller InRouteCaller,
	TFunction<FAssetDocumentMcpToolPolicy()> InPolicy)
	: Tools(InTools)
	, RouteCaller(MoveTemp(InRouteCaller))
	, Policy(MoveTemp(InPolicy))
{
}

const TArray<FString>& FAssetDocumentMcpEndpoint::SupportedProtocolVersions()
{
	static const TArray<FString> Versions = {TEXT("2025-11-25"), TEXT("2025-06-18"), TEXT("2025-03-26"), TEXT("2024-11-05"), TEXT("2024-10-07")};
	return Versions;
}

bool FAssetDocumentMcpEndpoint::HandleRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	using namespace AssetDocumentMcpEndpointLocal;
	if (Request.Verb == EHttpServerRequestVerbs::VERB_POST)
	{
		return HandlePost(Request, OnComplete);
	}
	if (Request.Verb == EHttpServerRequestVerbs::VERB_DELETE)
	{
		return HandleDelete(Request, OnComplete);
	}
	// The server never sends requests or notifications of its own, so there is no stream to open.
	Send(OnComplete, 405, JsonRpcError(nullptr, -32000, TEXT("Method not allowed: this MCP endpoint answers POST (and DELETE to end a session).")),
		TEXT("application/json"), {{TEXT("Allow"), TEXT("POST, DELETE")}});
	return true;
}

FString FAssetDocumentMcpEndpoint::OpenSession(const FString& ProtocolVersion)
{
	if (Sessions.Num() >= MaxSessions)
	{
		FString Oldest;
		double OldestUse = TNumericLimits<double>::Max();
		for (const TPair<FString, FSession>& Pair : Sessions)
		{
			if (Pair.Value.LastUsed < OldestUse)
			{
				OldestUse = Pair.Value.LastUsed;
				Oldest = Pair.Key;
			}
		}
		Sessions.Remove(Oldest);
	}
	const FString Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	Sessions.Add(Id, FSession{ProtocolVersion, FPlatformTime::Seconds()});
	return Id;
}

bool FAssetDocumentMcpEndpoint::HandleDelete(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	using namespace AssetDocumentMcpEndpointLocal;
	const FString SessionId = AssetDocumentHttpSecurity::HeaderValue(Request, TEXT("mcp-session-id"));
	if (SessionId.IsEmpty())
	{
		Send(OnComplete, 400, JsonRpcError(nullptr, -32000, TEXT("Bad Request: Mcp-Session-Id header is required")), TEXT("application/json"));
		return true;
	}
	if (Sessions.Remove(SessionId) == 0)
	{
		Send(OnComplete, 404, JsonRpcError(nullptr, -32001, TEXT("Session not found")), TEXT("application/json"));
		return true;
	}
	Send(OnComplete, 200, FString(), TEXT("application/json"));
	return true;
}

bool FAssetDocumentMcpEndpoint::HandlePost(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	using namespace AssetDocumentMcpEndpointLocal;

	// A client that accepts only an event stream gets the one response as one event.
	const TArray<FString> Accepted = AcceptedTypes(Request);
	const bool bJson = Accepted.IsEmpty() || Accepted.Contains(TEXT("application/json")) || Accepted.Contains(TEXT("application/*"))
		|| Accepted.Contains(TEXT("*/*"));
	const bool bStream = Accepted.Contains(TEXT("text/event-stream"));
	if (!bJson && !bStream)
	{
		Send(OnComplete, 406, JsonRpcError(nullptr, -32000, TEXT("Not Acceptable: the client must accept application/json or text/event-stream")),
			TEXT("application/json"));
		return true;
	}
	const EResponseFormat Format = bJson ? EResponseFormat::Json : EResponseFormat::EventStream;

	TSharedPtr<FJsonValue> Parsed;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyText(Request));
	if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
	{
		SendMessage(OnComplete, 400, Format, JsonRpcError(nullptr, -32700, TEXT("Parse error: the body is not JSON")));
		return true;
	}
	if (Parsed->Type == EJson::Array)
	{
		SendMessage(OnComplete, 400, Format, JsonRpcError(nullptr, -32600, TEXT("Invalid Request: send one JSON-RPC message per request (no batches)")));
		return true;
	}
	const TSharedPtr<FJsonObject> Message = Parsed->Type == EJson::Object ? Parsed->AsObject() : nullptr;
	FString Method;
	const bool bHasMethod = Message.IsValid() && Message->TryGetStringField(TEXT("method"), Method);
	const TSharedPtr<FJsonValue> Id = Message.IsValid() ? Message->TryGetField(TEXT("id")) : nullptr;
	const bool bIsResponse = Message.IsValid() && !bHasMethod && Id.IsValid() && (Message->HasField(TEXT("result")) || Message->HasField(TEXT("error")));
	if (!Message.IsValid() || (!bHasMethod && !bIsResponse))
	{
		SendMessage(OnComplete, 400, Format, JsonRpcError(nullptr, -32600, TEXT("Invalid Request: not a JSON-RPC 2.0 message")));
		return true;
	}
	const bool bIsRequest = bHasMethod && Id.IsValid() && !Id->IsNull();

	const FString VersionHeader = AssetDocumentHttpSecurity::HeaderValue(Request, TEXT("mcp-protocol-version"));
	if (!VersionHeader.IsEmpty() && !SupportedProtocolVersions().Contains(VersionHeader))
	{
		SendMessage(OnComplete, 400, Format, JsonRpcError(Id, -32000, FString::Printf(TEXT("Bad Request: unsupported protocol version %s (supported: %s)"),
			*VersionHeader, *FString::Join(SupportedProtocolVersions(), TEXT(", ")))));
		return true;
	}

	if (bIsRequest && Method == TEXT("initialize"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Message->TryGetObjectField(TEXT("params"), Params))
		{
			SendMessage(OnComplete, 400, Format, JsonRpcError(Id, -32602, TEXT("Invalid params: initialize needs params")));
			return true;
		}
		FString Requested;
		(*Params)->TryGetStringField(TEXT("protocolVersion"), Requested);
		const FString Negotiated = SupportedProtocolVersions().Contains(Requested) ? Requested : SupportedProtocolVersions()[0];
		const FString SessionId = OpenSession(Negotiated);

		const TSharedPtr<FJsonObject> Capabilities = MakeShared<FJsonObject>();
		Capabilities->SetObjectField(TEXT("tools"), MakeShared<FJsonObject>());
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("protocolVersion"), Negotiated);
		Result->SetObjectField(TEXT("capabilities"), Capabilities);
		Result->SetObjectField(TEXT("serverInfo"), Tools->GetServerInfo());
		SendMessage(OnComplete, 200, Format, JsonRpcResult(Id, Result), {{SessionHeader, SessionId}});
		return true;
	}

	const FString SessionId = AssetDocumentHttpSecurity::HeaderValue(Request, TEXT("mcp-session-id"));
	if (SessionId.IsEmpty())
	{
		SendMessage(OnComplete, 400, Format, JsonRpcError(Id, -32000, TEXT("Bad Request: Mcp-Session-Id header is required (send initialize first)")));
		return true;
	}
	FSession* Session = Sessions.Find(SessionId);
	if (Session == nullptr)
	{
		SendMessage(OnComplete, 404, Format, JsonRpcError(Id, -32001, TEXT("Session not found: start a new session with initialize")));
		return true;
	}
	Session->LastUsed = FPlatformTime::Seconds();

	if (!bIsRequest)
	{
		// Notifications (initialized, cancelled) and responses need no answer.
		Send(OnComplete, 202, FString(), TEXT("application/json"));
		return true;
	}

	const TMap<FString, FString> Headers = {{SessionHeader, SessionId}};
	if (Method == TEXT("ping"))
	{
		SendMessage(OnComplete, 200, Format, JsonRpcResult(Id, MakeShared<FJsonObject>()), Headers);
		return true;
	}
	if (Method == TEXT("tools/list"))
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("tools"), Tools->ListTools(Policy()));
		SendMessage(OnComplete, 200, Format, JsonRpcResult(Id, Result), Headers);
		return true;
	}
	if (Method == TEXT("tools/call"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		FString Name;
		if (!Message->TryGetObjectField(TEXT("params"), Params) || !(*Params)->TryGetStringField(TEXT("name"), Name))
		{
			SendMessage(OnComplete, 200, Format, JsonRpcError(Id, -32602, TEXT("Invalid params: tools/call needs params.name")), Headers);
			return true;
		}
		const TSharedPtr<FJsonValue> Arguments = (*Params)->TryGetField(TEXT("arguments"));
		if (Arguments.IsValid() && Arguments->Type != EJson::Object)
		{
			SendMessage(OnComplete, 200, Format, JsonRpcError(Id, -32602, TEXT("Invalid params: tools/call arguments must be an object")), Headers);
			return true;
		}
		Tools->CallTool(Name, Arguments.IsValid() ? Arguments->AsObject() : nullptr, Policy(), RouteCaller,
			[OnComplete, Format, Id, Headers](TSharedPtr<FJsonObject> Result)
			{
				if (Result.IsValid())
				{
					FAssetDocumentPendingNotices::AppendUnreportedToToolResult(*Result);
				}
				SendMessage(OnComplete, 200, Format, JsonRpcResult(Id, Result), Headers);
			});
		return true;
	}
	SendMessage(OnComplete, 200, Format, JsonRpcError(Id, -32601, FString::Printf(TEXT("Method not found: %s"), *Method)), Headers);
	return true;
}
