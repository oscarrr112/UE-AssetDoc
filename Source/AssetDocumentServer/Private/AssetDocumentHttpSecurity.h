// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

struct FHttpServerRequest;

/** What the editor's HTTP server accepts. */
struct FAssetDocumentHttpSecurityPolicy
{
	/** Requests from other machines (and any Host header) are accepted; otherwise only loopback peers with a local Host. */
	bool bAllowRemoteConnections = false;

	/** When set, every request needs "Authorization: Bearer <Token>". */
	FString Token;
};

/** Why a request is refused: the status code and the error text the response carries. */
struct FAssetDocumentHttpRejection
{
	int32 Status = 403;
	FString Message;
};

/**
 * The checks every request to the editor's HTTP server passes before its route runs (the server exposes Python
 * execution and asset deletion, so a web page must not be able to reach it): the peer is the local machine, the Host
 * header names the local machine (a page whose domain resolves to 127.0.0.1 sends its own domain), an Origin header
 * names a local page, the bearer token matches when one is required, and a POST, PUT or PATCH body is declared as
 * application/json (a page can send text/plain or form bodies without a preflight, but not JSON).
 */
namespace AssetDocumentHttpSecurity
{
	TOptional<FAssetDocumentHttpRejection> Check(const FHttpServerRequest& Request, const FAssetDocumentHttpSecurityPolicy& Policy);

	/** "localhost", "127.x.x.x" or "[::1]", with or without a port. */
	bool IsLocalHost(const FString& HostHeader);

	/** An http(s) origin whose host is local; "null" and other schemes are not. */
	bool IsLocalOrigin(const FString& Origin);

	/** A new random token (64 hex characters). */
	FString GenerateToken();

	/** The first value of a request header (names are lower-case), or empty. */
	FString HeaderValue(const FHttpServerRequest& Request, const TCHAR* LowerCaseName);
}
