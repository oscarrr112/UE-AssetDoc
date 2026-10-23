// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentHttpSecurity.h"

#include "HttpServerRequest.h"
#include "IPAddress.h"
#include "Misc/Guid.h"

namespace AssetDocumentHttpSecurityLocal
{
bool IsLoopbackHostName(const FString& InHost)
{
	const FString Host = InHost.ToLower();
	if (Host == TEXT("localhost") || Host == TEXT("[::1]") || Host == TEXT("::1"))
	{
		return true;
	}
	TArray<FString> Octets;
	Host.ParseIntoArray(Octets, TEXT("."), false);
	if (Octets.Num() != 4 || Octets[0] != TEXT("127"))
	{
		return false;
	}
	for (const FString& Octet : Octets)
	{
		if (Octet.IsEmpty() || Octet.Len() > 3 || !Octet.IsNumeric() || FCString::Atoi(*Octet) > 255)
		{
			return false;
		}
	}
	return true;
}

// The host part of "host[:port]" or "[v6]:port".
FString StripPort(const FString& HostAndPort)
{
	if (HostAndPort.StartsWith(TEXT("[")))
	{
		int32 Close = INDEX_NONE;
		return HostAndPort.FindChar(TEXT(']'), Close) ? HostAndPort.Left(Close + 1) : HostAndPort;
	}
	int32 Colon = INDEX_NONE;
	return HostAndPort.FindLastChar(TEXT(':'), Colon) ? HostAndPort.Left(Colon) : HostAndPort;
}

bool IsLoopbackPeer(const FHttpServerRequest& Request)
{
	if (!Request.PeerAddress.IsValid())
	{
		// The listener is bound to the loopback interface; a peer address that cannot be read is not evidence of a
		// remote client.
		return true;
	}
	const FString Address = Request.PeerAddress->ToString(false).ToLower();
	return Address.StartsWith(TEXT("127.")) || Address == TEXT("::1") || Address.StartsWith(TEXT("::ffff:127."));
}

// Compares in time independent of where the strings differ.
bool TokensMatch(const FString& Given, const FString& Expected)
{
	uint32 Difference = static_cast<uint32>(Given.Len() ^ Expected.Len());
	for (int32 Index = 0; Index < Expected.Len(); ++Index)
	{
		const TCHAR GivenChar = Index < Given.Len() ? Given[Index] : TEXT('\0');
		Difference |= static_cast<uint32>(GivenChar ^ Expected[Index]);
	}
	return Difference == 0;
}
} // namespace AssetDocumentHttpSecurityLocal

namespace AssetDocumentHttpSecurity
{
FString HeaderValue(const FHttpServerRequest& Request, const TCHAR* LowerCaseName)
{
	const TArray<FString>* Values = Request.Headers.Find(LowerCaseName);
	// The server splits header values at commas; the headers checked here hold one value, so the parts are rejoined.
	return Values != nullptr ? FString::Join(*Values, TEXT(",")).TrimStartAndEnd() : FString();
}

bool IsLocalHost(const FString& HostHeader)
{
	return AssetDocumentHttpSecurityLocal::IsLoopbackHostName(AssetDocumentHttpSecurityLocal::StripPort(HostHeader.TrimStartAndEnd()));
}

bool IsLocalOrigin(const FString& InOrigin)
{
	const FString Origin = InOrigin.TrimStartAndEnd().ToLower();
	FString Rest;
	if (Origin.StartsWith(TEXT("http://")))
	{
		Rest = Origin.Mid(7);
	}
	else if (Origin.StartsWith(TEXT("https://")))
	{
		Rest = Origin.Mid(8);
	}
	else
	{
		return false;
	}
	int32 Slash = INDEX_NONE;
	if (Rest.FindChar(TEXT('/'), Slash))
	{
		Rest.LeftInline(Slash);
	}
	return !Rest.Contains(TEXT("@")) && IsLocalHost(Rest);
}

FString GenerateToken()
{
	return FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower() + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
}

TOptional<FAssetDocumentHttpRejection> Check(const FHttpServerRequest& Request, const FAssetDocumentHttpSecurityPolicy& Policy)
{
	using namespace AssetDocumentHttpSecurityLocal;
	if (!Policy.bAllowRemoteConnections)
	{
		if (!IsLoopbackPeer(Request))
		{
			return FAssetDocumentHttpRejection{403, TEXT("The UEAssetDoc HTTP server accepts requests from this machine only "
				"(Allow Remote Connections is off).")};
		}
		const FString Host = HeaderValue(Request, TEXT("host"));
		if (!Host.IsEmpty() && !IsLocalHost(Host))
		{
			return FAssetDocumentHttpRejection{403, FString::Printf(TEXT("Host '%s' is not allowed: address the UEAssetDoc HTTP "
				"server as localhost, 127.0.0.1 or [::1]."), *Host)};
		}
	}
	const FString Origin = HeaderValue(Request, TEXT("origin"));
	if (!Origin.IsEmpty() && !IsLocalOrigin(Origin))
	{
		return FAssetDocumentHttpRejection{403, FString::Printf(TEXT("Origin '%s' is not allowed: the UEAssetDoc HTTP server "
			"answers web pages only when they are served from localhost."), *Origin)};
	}
	if (!Policy.Token.IsEmpty())
	{
		const FString Authorization = HeaderValue(Request, TEXT("authorization"));
		const bool bBearer = Authorization.Len() > 7 && Authorization.Left(7).Equals(TEXT("Bearer "), ESearchCase::IgnoreCase);
		if (!bBearer || !TokensMatch(Authorization.Mid(7).TrimStartAndEnd(), Policy.Token))
		{
			return FAssetDocumentHttpRejection{401, TEXT("This editor requires a bearer token (Require Auth Token is on): send "
				"\"Authorization: Bearer <token>\" with the token from Saved/UEAssetDoc/service.json of the project.")};
		}
	}
	if (Request.Verb == EHttpServerRequestVerbs::VERB_POST || Request.Verb == EHttpServerRequestVerbs::VERB_PUT
		|| Request.Verb == EHttpServerRequestVerbs::VERB_PATCH)
	{
		FString MediaType = HeaderValue(Request, TEXT("content-type"));
		int32 Semicolon = INDEX_NONE;
		if (MediaType.FindChar(TEXT(';'), Semicolon))
		{
			MediaType.LeftInline(Semicolon);
		}
		if (!MediaType.TrimStartAndEnd().Equals(TEXT("application/json"), ESearchCase::IgnoreCase))
		{
			return FAssetDocumentHttpRejection{415, TEXT("Request bodies must be sent with \"Content-Type: application/json\".")};
		}
	}
	return {};
}
} // namespace AssetDocumentHttpSecurity
