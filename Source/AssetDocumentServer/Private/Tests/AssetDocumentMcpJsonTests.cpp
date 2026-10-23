// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// UEAssetDoc.HttpServer.Mcp.Json: the MCP endpoint writes tool results as a JavaScript MCP server
//   would (JSON.stringify of the route's JSON), so the text an MCP client reads is the same whichever path served it:
//   numbers in their shortest round-trip form (Number.prototype.toString), object keys in JavaScript property order,
//   JSON.stringify's string escapes, String() of values inside messages and encodeURIComponent for query strings.

#include "AssetDocumentMcpJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentMcpJsonTestsLocal
{
TSharedPtr<FJsonValue> Number(double Value)
{
	return MakeShared<FJsonValueNumber>(Value);
}

TSharedPtr<FJsonValue> Text(const FString& Value)
{
	return MakeShared<FJsonValueString>(Value);
}
} // namespace AssetDocumentMcpJsonTestsLocal

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMcpJsonTest,
	"UEAssetDoc.HttpServer.Mcp.Json",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMcpJsonTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMcpJsonTestsLocal;
	using namespace AssetDocumentMcpJson;

	// Number.prototype.toString().
	const TPair<double, const TCHAR*> Numbers[] = {
		{0.0, TEXT("0")},
		{-0.0, TEXT("0")},
		{1.0, TEXT("1")},
		{-1.5, TEXT("-1.5")},
		{0.1, TEXT("0.1")},
		{0.1 + 0.2, TEXT("0.30000000000000004")},
		{123.456, TEXT("123.456")},
		{3.5, TEXT("3.5")},
		{100.0, TEXT("100")},
		{1e20, TEXT("100000000000000000000")},
		{1e21, TEXT("1e+21")},
		{123456789012345680000.0, TEXT("123456789012345680000")},
		{9007199254740992.0, TEXT("9007199254740992")},
		{0.000001, TEXT("0.000001")},
		{1e-7, TEXT("1e-7")},
		{1.5e-7, TEXT("1.5e-7")},
		{-1e-10, TEXT("-1e-10")},
		{5e-324, TEXT("5e-324")},
		{1.7976931348623157e308, TEXT("1.7976931348623157e+308")},
		{0.201, TEXT("0.201")},
	};
	for (const TPair<double, const TCHAR*>& Case : Numbers)
	{
		TestEqual(*FString::Printf(TEXT("FormatNumber(%.17g)"), Case.Key), FormatNumber(Case.Key), FString(Case.Value));
	}
	TestEqual(TEXT("JSON.stringify(NaN) is null"), Stringify(Number(std::numeric_limits<double>::quiet_NaN())), FString(TEXT("null")));
	TestEqual(TEXT("JSON.stringify(Infinity) is null"), Stringify(Number(std::numeric_limits<double>::infinity())), FString(TEXT("null")));

	// Property order: array-index keys ascending, then the other keys in insertion order.
	{
		const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetField(TEXT("b"), Number(1));
		Object->SetField(TEXT("10"), Number(2));
		Object->SetField(TEXT("a"), Number(3));
		Object->SetField(TEXT("2"), Number(4));
		Object->SetField(TEXT("01"), Number(5));
		Object->SetField(TEXT("4294967295"), Number(6));
		TestEqual(TEXT("keys in JavaScript property order"), FString::Join(OrderedKeys(Object), TEXT(" ")),
			FString(TEXT("2 10 b a 01 4294967295")));
		TestEqual(TEXT("compact object"), StringifyObject(Object),
			FString(TEXT("{\"2\":4,\"10\":2,\"b\":1,\"a\":3,\"01\":5,\"4294967295\":6}")));
	}

	// Nested values, compact, null and booleans.
	{
		const TSharedPtr<FJsonObject> Parsed = ParseObject(TEXT("{ \"list\": [1, 2.50, \"x\", null, true, {\"k\": []}], \"empty\": {} }"));
		if (TestTrue(TEXT("a JSON object parses"), Parsed.IsValid()))
		{
			TestEqual(TEXT("compact nested JSON"), StringifyObject(Parsed),
				FString(TEXT("{\"list\":[1,2.5,\"x\",null,true,{\"k\":[]}],\"empty\":{}}")));
		}
		TestFalse(TEXT("an array root is not an object"), ParseObject(TEXT("[1]")).IsValid());
		TestFalse(TEXT("invalid JSON is not an object"), ParseObject(TEXT("{")).IsValid());
		TestEqual(TEXT("a null value pointer is null"), Stringify(nullptr), FString(TEXT("null")));
	}

	// JSON.stringify's escapes: quote, backslash, the short escapes, other control characters as \u00XX; U+2028 and
	// non-ASCII text stay as they are.
	{
		FString Raw = TEXT("a\"b\\c\n\t\r\b\f");
		Raw.AppendChar(static_cast<TCHAR>(0x01));
		Raw.AppendChar(static_cast<TCHAR>(0x1f));
		Raw.AppendChar(static_cast<TCHAR>(0x2028));
		Raw += TEXT("é/");
		FString Expected = TEXT("\"a\\\"b\\\\c\\n\\t\\r\\b\\f\\u0001\\u001f");
		Expected.AppendChar(static_cast<TCHAR>(0x2028));
		Expected += TEXT("é/\"");
		TestEqual(TEXT("string escapes"), Quote(Raw), Expected);
		TestEqual(TEXT("Stringify of a string value quotes it"), Stringify(Text(TEXT("x\"y"))), FString(TEXT("\"x\\\"y\"")));
	}

	// String(value) as a template literal inserts it.
	{
		const TSharedPtr<FJsonObject> Parsed = ParseObject(TEXT("{\"n\":3.5,\"b\":true,\"z\":null,\"s\":\"t\",\"a\":[1,\"a\",null,[2,3]],\"o\":{\"k\":1}}"));
		if (TestTrue(TEXT("the values parse"), Parsed.IsValid()))
		{
			TestEqual(TEXT("String(3.5)"), ToJsString(Parsed->TryGetField(TEXT("n"))), FString(TEXT("3.5")));
			TestEqual(TEXT("String(true)"), ToJsString(Parsed->TryGetField(TEXT("b"))), FString(TEXT("true")));
			TestEqual(TEXT("String(null)"), ToJsString(Parsed->TryGetField(TEXT("z"))), FString(TEXT("null")));
			TestEqual(TEXT("String of a string"), ToJsString(Parsed->TryGetField(TEXT("s"))), FString(TEXT("t")));
			TestEqual(TEXT("String of an array"), ToJsString(Parsed->TryGetField(TEXT("a"))), FString(TEXT("1,a,,2,3")));
			TestEqual(TEXT("String of an object"), ToJsString(Parsed->TryGetField(TEXT("o"))), FString(TEXT("[object Object]")));
		}
		TestEqual(TEXT("String(undefined)"), ToJsString(nullptr), FString(TEXT("undefined")));
	}

	// encodeURIComponent: spaces as %20 (never +), reserved characters escaped, UTF-8 for non-ASCII.
	const TPair<const TCHAR*, const TCHAR*> Encoded[] = {
		{TEXT("*Trigger Any*"), TEXT("*Trigger%20Any*")},
		{TEXT("A+B"), TEXT("A%2BB")},
		{TEXT("50% Mix"), TEXT("50%25%20Mix")},
		{TEXT("a&b=c"), TEXT("a%26b%3Dc")},
		{TEXT("K2:Add,Array Length|Set Length"), TEXT("K2%3AAdd%2CArray%20Length%7CSet%20Length")},
		{TEXT("-_.!~*'()"), TEXT("-_.!~*'()")},
		{TEXT("/Game/A B"), TEXT("%2FGame%2FA%20B")},
		{TEXT("é"), TEXT("%C3%A9")},
	};
	for (const TPair<const TCHAR*, const TCHAR*>& Case : Encoded)
	{
		TestEqual(*FString::Printf(TEXT("encodeURIComponent(%s)"), Case.Key), EncodeUriComponent(Case.Key), FString(Case.Value));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
