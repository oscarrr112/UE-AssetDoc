// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// UEAssetDoc.HttpServer.Security.Checks: the checks every request to the editor's HTTP server passes
//   before its route runs. A request from another machine, one whose Host header names another host (a web page whose
//   domain resolves to 127.0.0.1), one whose Origin is a non-local page, one without the bearer token when a token is
//   required, and a POST/PUT/PATCH body not declared as application/json are refused with 403, 401 or 415. Allowing
//   remote connections accepts other peers and hosts but keeps the Origin and token checks.
// UEAssetDoc.HttpServer.Security.UserOnlyFile: the file that can hold the bearer token is created, and an existing
//   file readable by others is replaced, so that only the current user can read it (Mac/Linux: mode 0600; Windows: a
//   protected DACL that allows only the current user and SYSTEM). On the Mac, an extended ACL entry inherited from the
//   folder or set on the existing file is removed too. Saving again writes a new file: a reader that opened the old
//   one does not see the new text, and no temporary file or folder is left.

#include "AssetDocumentHttpSecurity.h"
#include "AssetDocumentUserOnlyFile.h"
#include "AssetDocumentUserOnlyFileTestSupport.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "HttpServerRequest.h"
#include "IPAddress.h"
#include "Misc/AutomationTest.h"
#include "SocketSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#if PLATFORM_MAC || PLATFORM_LINUX
#include <fcntl.h>
#include <unistd.h>
#endif

namespace AssetDocumentHttpSecurityTestsLocal
{
struct FSecurityProbe
{
	EHttpServerRequestVerbs Verb = EHttpServerRequestVerbs::VERB_GET;
	FString Peer = TEXT("127.0.0.1");
	TMap<FString, FString> Headers;
};

FHttpServerRequest MakeSecurityRequest(const FSecurityProbe& Probe)
{
	FHttpServerRequest Request;
	Request.Verb = Probe.Verb;
	if (!Probe.Peer.IsEmpty())
	{
		if (ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			const TSharedRef<FInternetAddr> Address = Sockets->CreateInternetAddr();
			bool bValid = false;
			Address->SetIp(*Probe.Peer, bValid);
			if (bValid)
			{
				Request.PeerAddress = Address;
			}
		}
	}
	for (const TPair<FString, FString>& Header : Probe.Headers)
	{
		// The server lower-cases header names and splits values at commas.
		TArray<FString> Values;
		Header.Value.ParseIntoArray(Values, TEXT(","), true);
		Request.Headers.Add(Header.Key.ToLower(), Values);
	}
	return Request;
}

int32 CheckStatus(const FSecurityProbe& Probe, const FAssetDocumentHttpSecurityPolicy& Policy, FString* OutMessage = nullptr)
{
	const TOptional<FAssetDocumentHttpRejection> Rejection = AssetDocumentHttpSecurity::Check(MakeSecurityRequest(Probe), Policy);
	if (OutMessage != nullptr)
	{
		*OutMessage = Rejection.IsSet() ? Rejection->Message : FString();
	}
	return Rejection.IsSet() ? Rejection->Status : 0;
}

FSecurityProbe Probe(EHttpServerRequestVerbs Verb, const TMap<FString, FString>& Headers, const FString& Peer = TEXT("127.0.0.1"))
{
	FSecurityProbe Result;
	Result.Verb = Verb;
	Result.Headers = Headers;
	Result.Peer = Peer;
	return Result;
}
} // namespace AssetDocumentHttpSecurityTestsLocal

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentHttpSecurityChecksTest,
	"UEAssetDoc.HttpServer.Security.Checks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentHttpSecurityChecksTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentHttpSecurityTestsLocal;
	const EHttpServerRequestVerbs Get = EHttpServerRequestVerbs::VERB_GET;
	const EHttpServerRequestVerbs Post = EHttpServerRequestVerbs::VERB_POST;
	const FAssetDocumentHttpSecurityPolicy Local;
	const TCHAR* const Json = TEXT("application/json");

	// Local requests pass.
	TestEqual(TEXT("a local GET passes"), CheckStatus(Probe(Get, {{TEXT("Host"), TEXT("127.0.0.1:8559")}}), Local), 0);
	TestEqual(TEXT("a request without Host passes"), CheckStatus(Probe(Get, {}), Local), 0);
	TestEqual(TEXT("an unreadable peer address passes (the listener is loopback-bound)"), CheckStatus(Probe(Get, {}, FString()), Local), 0);
	TestEqual(TEXT("a local JSON POST passes"), CheckStatus(Probe(Post, {{TEXT("Host"), TEXT("localhost:8559")}, {TEXT("Content-Type"), Json}}), Local), 0);
	TestEqual(TEXT("a JSON POST with a charset passes"), CheckStatus(Probe(Post, {{TEXT("Content-Type"), TEXT("Application/JSON; charset=utf-8")}}), Local), 0);

	// Peer and Host.
	FString Message;
	TestEqual(TEXT("another machine is refused"), CheckStatus(Probe(Get, {}, TEXT("203.0.113.7")), Local, &Message), 403);
	TestTrue(TEXT("the refusal says why"), Message.Contains(TEXT("this machine only")));
	TestEqual(TEXT("a foreign Host is refused"), CheckStatus(Probe(Get, {{TEXT("Host"), TEXT("evil.example:8559")}}), Local, &Message), 403);
	TestTrue(TEXT("the refusal names the Host"), Message.Contains(TEXT("evil.example")));
	for (const TCHAR* Host : {TEXT("localhost"), TEXT("LOCALHOST:1"), TEXT("127.0.0.2"), TEXT("[::1]:8559"), TEXT("[::1]")})
	{
		TestTrue(*FString::Printf(TEXT("Host %s is local"), Host), AssetDocumentHttpSecurity::IsLocalHost(Host));
	}
	for (const TCHAR* Host : {TEXT("localhost.evil.example"), TEXT("127.0.0.256"), TEXT("127.0.0"), TEXT("128.0.0.1"), TEXT("0.0.0.0"),
		TEXT("evil.example"), TEXT("[::2]:80")})
	{
		TestFalse(*FString::Printf(TEXT("Host %s is not local"), Host), AssetDocumentHttpSecurity::IsLocalHost(Host));
	}

	// Origin.
	for (const TCHAR* Origin : {TEXT("http://localhost:3000"), TEXT("https://127.0.0.1"), TEXT("http://[::1]:5173")})
	{
		TestEqual(*FString::Printf(TEXT("Origin %s passes"), Origin), CheckStatus(Probe(Get, {{TEXT("Origin"), Origin}}), Local), 0);
	}
	for (const TCHAR* Origin : {TEXT("http://evil.example"), TEXT("null"), TEXT("http://localhost@evil.example"), TEXT("file://"),
		TEXT("chrome-extension://abc"), TEXT("http://localhost.evil.example")})
	{
		TestEqual(*FString::Printf(TEXT("Origin %s is refused"), Origin), CheckStatus(Probe(Get, {{TEXT("Origin"), Origin}}), Local), 403);
	}

	// Content-Type of bodies.
	TestEqual(TEXT("a POST without Content-Type is refused"), CheckStatus(Probe(Post, {}), Local), 415);
	TestEqual(TEXT("a text/plain POST is refused"), CheckStatus(Probe(Post, {{TEXT("Content-Type"), TEXT("text/plain")}}), Local), 415);
	TestEqual(TEXT("a form POST is refused"), CheckStatus(Probe(Post, {{TEXT("Content-Type"), TEXT("application/x-www-form-urlencoded")}}), Local), 415);
	TestEqual(TEXT("a text/plain PUT is refused"),
		CheckStatus(Probe(EHttpServerRequestVerbs::VERB_PUT, {{TEXT("Content-Type"), TEXT("text/plain")}}), Local), 415);
	TestEqual(TEXT("a DELETE needs no Content-Type"), CheckStatus(Probe(EHttpServerRequestVerbs::VERB_DELETE, {}), Local), 0);

	// Bearer token.
	FAssetDocumentHttpSecurityPolicy Token;
	Token.Token = AssetDocumentHttpSecurity::GenerateToken();
	TestEqual(TEXT("no token is refused"), CheckStatus(Probe(Get, {}), Token, &Message), 401);
	TestTrue(TEXT("the refusal says where the token is"), Message.Contains(TEXT("service.json")));
	TestEqual(TEXT("a wrong token is refused"), CheckStatus(Probe(Get, {{TEXT("Authorization"), TEXT("Bearer 0123")}}), Token), 401);
	TestEqual(TEXT("a token prefix is refused"),
		CheckStatus(Probe(Get, {{TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Token.Token.LeftChop(1))}}), Token), 401);
	TestEqual(TEXT("a Basic credential is refused"),
		CheckStatus(Probe(Get, {{TEXT("Authorization"), FString::Printf(TEXT("Basic %s"), *Token.Token)}}), Token), 401);
	TestEqual(TEXT("the token passes"), CheckStatus(Probe(Get, {{TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Token.Token)}}), Token), 0);
	TestEqual(TEXT("the scheme is case-insensitive"),
		CheckStatus(Probe(Get, {{TEXT("Authorization"), FString::Printf(TEXT("bearer %s"), *Token.Token)}}), Token), 0);
	TestEqual(TEXT("without a token required, Authorization is ignored"), CheckStatus(Probe(Get, {{TEXT("Authorization"), TEXT("Bearer x")}}), Local), 0);

	// Remote connections: other peers and hosts pass; Origin and token checks stay.
	FAssetDocumentHttpSecurityPolicy Remote;
	Remote.bAllowRemoteConnections = true;
	Remote.Token = Token.Token;
	const FString Bearer = FString::Printf(TEXT("Bearer %s"), *Token.Token);
	TestEqual(TEXT("remote: another machine with the token passes"),
		CheckStatus(Probe(Get, {{TEXT("Host"), TEXT("198.51.100.5:8559")}, {TEXT("Authorization"), Bearer}}, TEXT("203.0.113.7")), Remote), 0);
	TestEqual(TEXT("remote: no token is refused"), CheckStatus(Probe(Get, {}, TEXT("203.0.113.7")), Remote), 401);
	TestEqual(TEXT("remote: a foreign Origin is refused"),
		CheckStatus(Probe(Get, {{TEXT("Origin"), TEXT("http://evil.example")}, {TEXT("Authorization"), Bearer}}, TEXT("203.0.113.7")), Remote), 403);

	// Tokens.
	const FString Another = AssetDocumentHttpSecurity::GenerateToken();
	TestEqual(TEXT("a token has 64 characters"), Token.Token.Len(), 64);
	bool bHex = true;
	for (const TCHAR Char : Token.Token)
	{
		bHex &= FChar::IsHexDigit(Char);
	}
	TestTrue(TEXT("a token is hexadecimal"), bHex);
	TestNotEqual(TEXT("each token is new"), Token.Token, Another);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentHttpSecurityUserOnlyFileTest,
	"UEAssetDoc.HttpServer.Security.UserOnlyFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentHttpSecurityUserOnlyFileTest::RunTest(const FString& Parameters)
{
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEAssetDocTests"),
		FString::Printf(TEXT("UserOnly-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
	IFileManager::Get().MakeDirectory(*Directory, true);

	// A new file.
	const FString Created = FPaths::Combine(Directory, TEXT("created.json"));
	FString Error;
	TestTrue(TEXT("a new file is written"), AssetDocumentUserOnlyFile::Save(TEXT("{\"authToken\": \"a\"}"), Created, Error));
	AddInfo(FString::Printf(TEXT("new file: %s"), Error.IsEmpty() ? TEXT("ok") : *Error));
	FString Why;
	TestTrue(TEXT("the new file is readable by the current user only"), AssetDocumentUserOnlyFileTests::IsReadableByCurrentUserOnly(Created, Why));
	AddInfo(FString::Printf(TEXT("new file access: %s"), Why.IsEmpty() ? TEXT("user only") : *Why));
	FString Text;
	TestTrue(TEXT("the new file holds the text"), FFileHelper::LoadFileToString(Text, *Created) && Text == TEXT("{\"authToken\": \"a\"}"));

	// An existing file written the ordinary way (others may read it) is replaced with a restricted one.
	const FString Replaced = FPaths::Combine(Directory, TEXT("replaced.json"));
	TestTrue(TEXT("an ordinary file is written first"), FFileHelper::SaveStringToFile(TEXT("{\"authToken\": \"old token, longer text\"}"), *Replaced));
	TestTrue(TEXT("the existing file is replaced"), AssetDocumentUserOnlyFile::Save(TEXT("{\"authToken\": \"b\"}"), Replaced, Error));
	Why.Reset();
	TestTrue(TEXT("the replaced file is readable by the current user only"), AssetDocumentUserOnlyFileTests::IsReadableByCurrentUserOnly(Replaced, Why));
	AddInfo(FString::Printf(TEXT("replaced file access: %s"), Why.IsEmpty() ? TEXT("user only") : *Why));
	TestTrue(TEXT("the replaced file holds only the new text"), FFileHelper::LoadFileToString(Text, *Replaced) && Text == TEXT("{\"authToken\": \"b\"}"));
	TArray<FString> Left;
	IFileManager::Get().FindFiles(Left, *FPaths::Combine(Directory, TEXT("*")), true, false);
	TestEqual(TEXT("no temporary file is left"), Left.Num(), 2);
	Left.Reset();
	IFileManager::Get().FindFiles(Left, *FPaths::Combine(Directory, TEXT("*")), false, true);
	TestEqual(TEXT("no temporary folder is left"), Left.Num(), 0);

#if PLATFORM_MAC || PLATFORM_LINUX
	// A reader that opened the file before it is saved again keeps the old text: the new text is a new file.
	const FString Reopened = FPaths::Combine(Directory, TEXT("reopened.json"));
	TestTrue(TEXT("the first text is saved"), AssetDocumentUserOnlyFile::Save(TEXT("{\"authToken\": \"first\"}"), Reopened, Error));
	const int Reader = open(TCHAR_TO_UTF8(*Reopened), O_RDONLY);
	if (TestTrue(TEXT("a reader opens the file"), Reader >= 0))
	{
		TestTrue(TEXT("the second text is saved"), AssetDocumentUserOnlyFile::Save(TEXT("{\"authToken\": \"second\"}"), Reopened, Error));
		char Buffer[128] = {};
		const ssize_t Read = pread(Reader, Buffer, sizeof(Buffer) - 1, 0);
		close(Reader);
		const FString Seen = Read > 0 ? FString(UTF8_TO_TCHAR(Buffer)) : FString();
		TestFalse(*FString::Printf(TEXT("the open reader does not see the second text (it reads '%s')"), *Seen), Seen.Contains(TEXT("second")));
		TestTrue(TEXT("the file holds the second text"), FFileHelper::LoadFileToString(Text, *Reopened) && Text == TEXT("{\"authToken\": \"second\"}"));
		Why.Reset();
		TestTrue(TEXT("the saved-again file is readable by the current user only"), AssetDocumentUserOnlyFileTests::IsReadableByCurrentUserOnly(Reopened, Why));
	}
#endif

#if PLATFORM_MAC
	// A folder whose ACL lets everyone read the files created in it, and an existing file with such an entry of its own.
	const FString AclDirectory = FPaths::Combine(Directory, TEXT("acl"));
	IFileManager::Get().MakeDirectory(*AclDirectory, true);
	TestTrue(TEXT("the folder gets an entry its new files inherit"), AssetDocumentUserOnlyFileTests::AllowEveryoneToRead(AclDirectory, true, Why));
	AddInfo(FString::Printf(TEXT("folder ACL: %s"), Why.IsEmpty() ? TEXT("set") : *Why));
	const FString Inheriting = FPaths::Combine(AclDirectory, TEXT("ordinary.json"));
	TestTrue(TEXT("an ordinary file is written in that folder"), FFileHelper::SaveStringToFile(TEXT("{}"), *Inheriting));
	TestTrue(TEXT("an ordinary file there inherits the entry"), AssetDocumentUserOnlyFileTests::HasExtendedAcl(Inheriting));

	const FString CreatedInAclFolder = FPaths::Combine(AclDirectory, TEXT("created.json"));
	TestTrue(TEXT("a new file in that folder is written"), AssetDocumentUserOnlyFile::Save(TEXT("{\"authToken\": \"c\"}"), CreatedInAclFolder, Error));
	AddInfo(FString::Printf(TEXT("new file in the ACL folder: %s"), Error.IsEmpty() ? TEXT("ok") : *Error));
	TestFalse(TEXT("the new file keeps no inherited entry"), AssetDocumentUserOnlyFileTests::HasExtendedAcl(CreatedInAclFolder));
	Why.Reset();
	TestTrue(TEXT("the new file in that folder is readable by the current user only"),
		AssetDocumentUserOnlyFileTests::IsReadableByCurrentUserOnly(CreatedInAclFolder, Why));
	AddInfo(FString::Printf(TEXT("new file in the ACL folder access: %s"), Why.IsEmpty() ? TEXT("user only") : *Why));

	const FString WithOwnEntry = FPaths::Combine(Directory, TEXT("own-entry.json"));
	TestTrue(TEXT("a file with an entry of its own is written first"), FFileHelper::SaveStringToFile(TEXT("{\"authToken\": \"old\"}"), *WithOwnEntry));
	Why.Reset();
	TestTrue(TEXT("the file gets its own entry"), AssetDocumentUserOnlyFileTests::AllowEveryoneToRead(WithOwnEntry, false, Why));
	TestTrue(TEXT("the entry is there"), AssetDocumentUserOnlyFileTests::HasExtendedAcl(WithOwnEntry));
	TestTrue(TEXT("the file with its own entry is replaced"), AssetDocumentUserOnlyFile::Save(TEXT("{\"authToken\": \"d\"}"), WithOwnEntry, Error));
	TestFalse(TEXT("the replaced file keeps no entry"), AssetDocumentUserOnlyFileTests::HasExtendedAcl(WithOwnEntry));
	TestTrue(TEXT("the replaced file holds the new text"), FFileHelper::LoadFileToString(Text, *WithOwnEntry) && Text == TEXT("{\"authToken\": \"d\"}"));
#endif

	// A file that cannot be written (its folder is missing) is reported, not left behind.
	const FString Missing = FPaths::Combine(Directory, TEXT("missing"), TEXT("service.json"));
	TestFalse(TEXT("a file in a missing folder is not written"), AssetDocumentUserOnlyFile::Save(TEXT("{}"), Missing, Error));
	TestFalse(TEXT("the failure says why"), Error.IsEmpty());
	TestFalse(TEXT("and nothing is there"), FPaths::FileExists(Missing));

	IFileManager::Get().DeleteDirectory(*Directory, false, true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
