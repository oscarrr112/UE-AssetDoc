// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// UEAssetDoc.HttpServer.Mcp.Tools: the editor's MCP tools, read from the plugin's tool manifest
//   (MCP/tools.json), against a stub of the editor's routes. tools/list is the manifest (name, description, input
//   schema, in order) without the tools the settings switch off; a call checks its arguments, calls its route with the
//   query or body the manifest describes and turns the answer into the MCP result: compact JSON text, or a tool error
//   whose text names the route, the status, the message, every diagnostic (code, path, severity, details), the compile
//   status and the full JSON. delete_assets, execute_python and the screenshot have their own results; a pending play
//   operation is polled until it finishes.
// UEAssetDoc.HttpServer.Mcp.Guides: the guides the tools serve from MCP/schemas: the AssetDocument
//   guide with its section listing, sections by name, class path or short class name (the Generic section for other
//   classes), their reference parts, "all", and the level editing guide topics; each stays within its size budget.

#include "AssetDocumentMcpJson.h"
#include "AssetDocumentMcpTools.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentMcpToolsTestsLocal
{
struct FRecordedRouteCall
{
	FString Method;
	FString Endpoint;
	FString Body;
	TMap<FString, FString> Query;
};

// The editor's routes as a test sets them up: every call is recorded and answered with the next queued answer, or
// with the default one.
struct FStubRoutes
{
	TArray<FRecordedRouteCall> Calls;
	int32 Status = 200;
	FString Answer = TEXT("{\"success\":true}");
	TArray<TPair<int32, FString>> Queue;

	void Reply(int32 InStatus, const FString& InAnswer)
	{
		Status = InStatus;
		Answer = InAnswer;
	}
};

FAssetDocumentMcpRouteCaller MakeStubCaller(const TSharedRef<FStubRoutes>& Stub)
{
	return [Stub](const FString& Method, const FString& Endpoint, const FString& Body, const TMap<FString, FString>& Query)
	{
		Stub->Calls.Add({Method, Endpoint, Body, Query});
		FAssetDocumentMcpRouteResult Result;
		if (Stub->Queue.Num() > 0)
		{
			const TPair<int32, FString> Next = Stub->Queue[0];
			Stub->Queue.RemoveAt(0);
			Result.Status = Next.Key;
			Result.Json = AssetDocumentMcpJson::ParseObject(Next.Value);
		}
		else
		{
			Result.Status = Stub->Status;
			Result.Json = AssetDocumentMcpJson::ParseObject(Stub->Answer);
		}
		return Result;
	};
}

// One synchronous tools/call.
TSharedPtr<FJsonObject> CallStubTool(const FAssetDocumentMcpTools& Tools, const FString& Name, const FString& ArgumentsJson,
	const TSharedRef<FStubRoutes>& Stub, const FAssetDocumentMcpToolPolicy& Policy = FAssetDocumentMcpToolPolicy())
{
	TSharedPtr<FJsonObject> Out;
	Tools.CallTool(Name, AssetDocumentMcpJson::ParseObject(ArgumentsJson), Policy, MakeStubCaller(Stub),
		[&Out](TSharedPtr<FJsonObject> Result) { Out = Result; });
	return Out;
}

FString ResultText(const TSharedPtr<FJsonObject>& Result)
{
	TArray<FString> Texts;
	const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
	if (Result.IsValid() && Result->TryGetArrayField(TEXT("content"), Content))
	{
		for (const TSharedPtr<FJsonValue>& Entry : *Content)
		{
			const TSharedPtr<FJsonObject>* Block = nullptr;
			FString Type;
			FString Text;
			if (Entry->TryGetObject(Block) && (*Block)->TryGetStringField(TEXT("type"), Type) && Type == TEXT("text")
				&& (*Block)->TryGetStringField(TEXT("text"), Text))
			{
				Texts.Add(Text);
			}
		}
	}
	return FString::Join(Texts, TEXT("\n"));
}

bool IsToolError(const TSharedPtr<FJsonObject>& Result)
{
	bool bError = false;
	return Result.IsValid() && Result->TryGetBoolField(TEXT("isError"), bError) && bError;
}

FString McpDirectory()
{
	return FAssetDocumentMcpTools::DefaultMcpDirectory();
}

FString LoadText(const FString& Path)
{
	FString Text;
	FFileHelper::LoadFileToString(Text, *Path);
	return Text;
}

TArray<FString> MarkdownNames(const FString& Directory)
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, TEXT("*.md")), true, false);
	TArray<FString> Names;
	for (const FString& File : Files)
	{
		Names.Add(FPaths::GetBaseFilename(File));
	}
	Names.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
	return Names;
}

// The text after "<Prefix>" on the first line that starts with it.
FString LineAfter(const FString& Text, const FString& Prefix)
{
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);
	for (const FString& Line : Lines)
	{
		if (Line.StartsWith(Prefix, ESearchCase::CaseSensitive))
		{
			return Line.Mid(Prefix.Len()).TrimEnd();
		}
	}
	return FString();
}

FString LastLine(const FString& Text)
{
	TArray<FString> Lines;
	Text.TrimEnd().ParseIntoArrayLines(Lines, false);
	return Lines.Num() > 0 ? Lines.Last() : FString();
}

const TCHAR* const DefaultCatalog[] = {
	TEXT("get_asset_document_schema"), TEXT("find_assets"), TEXT("list_types"), TEXT("inspect_asset_document_target"),
	TEXT("inspect_asset_document_profile"), TEXT("create_asset_document_template"), TEXT("extract_asset_document"),
	TEXT("validate_asset_document"), TEXT("diff_asset_document"), TEXT("apply_asset_document_file"), TEXT("duplicate_asset"),
	TEXT("sample_animation_bones"), TEXT("retarget_animations"), TEXT("health_check"), TEXT("get_editor_context"), TEXT("query_level"),
	TEXT("edit_level"), TEXT("undo_editor"), TEXT("open_level"), TEXT("new_level"), TEXT("get_level_editing_guide"), TEXT("play"),
	TEXT("query_play"), TEXT("execute_python"), TEXT("get_viewport_screenshot"), TEXT("delete_assets"), TEXT("import_assets"),
	TEXT("settings"), TEXT("save_assets"), TEXT("reload_assets"), TEXT("asset_editor"), TEXT("export_assets"),
};

TArray<FString> ListedNames(const TArray<TSharedPtr<FJsonValue>>& Listed)
{
	TArray<FString> Names;
	for (const TSharedPtr<FJsonValue>& Entry : Listed)
	{
		Names.Add(Entry->AsObject()->GetStringField(TEXT("name")));
	}
	return Names;
}

struct FPlayPollResult
{
	TSharedPtr<FJsonObject> Result;
};
} // namespace AssetDocumentMcpToolsTestsLocal

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMcpToolsTest,
	"UEAssetDoc.HttpServer.Mcp.Tools",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMcpToolsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMcpToolsTestsLocal;
	const TSharedRef<FAssetDocumentMcpTools> Tools = MakeShared<FAssetDocumentMcpTools>();
	FString LoadError;
	if (!TestTrue(*FString::Printf(TEXT("the tool manifest loads (%s)"), *LoadError), Tools->Load(McpDirectory(), LoadError)))
	{
		return false;
	}

	// tools/list is the manifest: the catalog in order, each entry {name, description, inputSchema} as the file has it.
	{
		const TArray<TSharedPtr<FJsonValue>> Listed = Tools->ListTools(FAssetDocumentMcpToolPolicy());
		TArray<FString> Expected;
		for (const TCHAR* Name : DefaultCatalog)
		{
			Expected.Add(Name);
		}
		TestEqual(TEXT("the catalog in order"), FString::Join(ListedNames(Listed), TEXT(",")), FString::Join(Expected, TEXT(",")));

		const TSharedPtr<FJsonObject> Manifest = AssetDocumentMcpJson::ParseObject(LoadText(FPaths::Combine(McpDirectory(), TEXT("tools.json"))));
		const TArray<TSharedPtr<FJsonValue>>* ManifestTools = nullptr;
		if (TestTrue(TEXT("tools.json has tools"), Manifest.IsValid() && Manifest->TryGetArrayField(TEXT("tools"), ManifestTools))
			&& TestEqual(TEXT("one listed entry per manifest tool"), Listed.Num(), ManifestTools->Num()))
		{
			for (int32 Index = 0; Index < Listed.Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Entry = Listed[Index]->AsObject();
				const TSharedPtr<FJsonObject> Source = (*ManifestTools)[Index]->AsObject();
				TestEqual(TEXT("a listed entry has name, description and inputSchema"), FString::Join(AssetDocumentMcpJson::OrderedKeys(Entry), TEXT(",")),
					FString(TEXT("name,description,inputSchema")));
				for (const TCHAR* Field : {TEXT("name"), TEXT("description"), TEXT("inputSchema")})
				{
					TestEqual(*FString::Printf(TEXT("%s %s as in tools.json"), *Entry->GetStringField(TEXT("name")), Field),
						AssetDocumentMcpJson::Stringify(Entry->TryGetField(Field)), AssetDocumentMcpJson::Stringify(Source->TryGetField(Field)));
				}
			}
		}
		const TSharedPtr<FJsonObject> ServerInfo = Tools->GetServerInfo();
		TestTrue(TEXT("the server is named ue-assetdoc"), ServerInfo.IsValid() && ServerInfo->GetStringField(TEXT("name")) == TEXT("ue-assetdoc"));
	}

	// Settings switch execute_python and delete_assets off: not listed, and a call says so without reaching the editor.
	{
		FAssetDocumentMcpToolPolicy Off;
		Off.bExecutePython = false;
		Off.bDeleteAssets = false;
		const TArray<FString> Names = ListedNames(Tools->ListTools(Off));
		TestEqual(TEXT("two tools fewer"), Names.Num(), static_cast<int32>(UE_ARRAY_COUNT(DefaultCatalog)) - 2);
		TestFalse(TEXT("execute_python is not listed"), Names.Contains(TEXT("execute_python")));
		TestFalse(TEXT("delete_assets is not listed"), Names.Contains(TEXT("delete_assets")));
		const TSharedRef<FStubRoutes> Stub = MakeShared<FStubRoutes>();
		for (const TCHAR* Name : {TEXT("execute_python"), TEXT("delete_assets")})
		{
			const TSharedPtr<FJsonObject> Result = CallStubTool(*Tools, Name, TEXT("{\"code\":\"print(1)\",\"assets\":[\"/Game/A\"]}"), Stub, Off);
			TestTrue(*FString::Printf(TEXT("%s answers a tool error"), Name), IsToolError(Result));
			TestEqual(*FString::Printf(TEXT("%s says it is off"), Name), ResultText(Result), FAssetDocumentMcpTools::DescribeDisabledTool(Name));
		}
		TestTrue(TEXT("the disabled text names the setting"), FAssetDocumentMcpTools::DescribeDisabledTool(TEXT("execute_python")).Contains(TEXT("Security")));
		TestEqual(TEXT("a disabled tool reaches no route"), Stub->Calls.Num(), 0);
		FAssetDocumentMcpToolPolicy PythonOnly;
		PythonOnly.bDeleteAssets = false;
		TestTrue(TEXT("each setting is separate"), ListedNames(Tools->ListTools(PythonOnly)).Contains(TEXT("execute_python")));
	}

	// Unknown tools and missing arguments are tool errors that reach no route.
	{
		const TSharedRef<FStubRoutes> Stub = MakeShared<FStubRoutes>();
		for (const TCHAR* Removed : {TEXT("apply_blueprint_change"), TEXT("generate_assets"), TEXT("apply_asset_document"), TEXT("no_such_tool")})
		{
			const TSharedPtr<FJsonObject> Result = CallStubTool(*Tools, Removed, TEXT("{}"), Stub);
			TestTrue(*FString::Printf(TEXT("%s is unknown"), Removed), IsToolError(Result)
				&& ResultText(Result) == FString::Printf(TEXT("Error: Error: unknown tool: %s"), Removed));
		}
		const TSharedPtr<FJsonObject> Missing = CallStubTool(*Tools, TEXT("duplicate_asset"), TEXT("{\"source\":\"/Game/A\"}"), Stub);
		TestTrue(TEXT("duplicate_asset needs target"), IsToolError(Missing) && ResultText(Missing) == TEXT("Error: Error: 'target' is required."));
		const TSharedPtr<FJsonObject> Kind = CallStubTool(*Tools, TEXT("list_types"), TEXT("{}"), Stub);
		TestTrue(TEXT("list_types needs kind"), IsToolError(Kind) && ResultText(Kind).Contains(TEXT("'kind' is required.")));
		for (const TCHAR* Tool : {TEXT("validate_asset_document"), TEXT("diff_asset_document")})
		{
			const TSharedPtr<FJsonObject> Both = CallStubTool(*Tools, Tool,
				TEXT("{\"document\":{\"SchemaVersion\":1},\"file_path\":\"/tmp/A.assetdoc.json\"}"), Stub);
			TestTrue(*FString::Printf(TEXT("%s rejects document and file_path"), Tool), IsToolError(Both)
				&& ResultText(Both).Contains(TEXT("Exactly one of 'document' or 'file_path' is required.")));
			TestTrue(*FString::Printf(TEXT("%s rejects no source"), Tool), IsToolError(CallStubTool(*Tools, Tool, TEXT("{}"), Stub)));
		}
		TestEqual(TEXT("argument errors reach no route"), Stub->Calls.Num(), 0);
	}

	// Routes, queries and bodies.
	{
		const TSharedRef<FStubRoutes> Stub = MakeShared<FStubRoutes>();
		const FString Answer = TEXT("{\"success\":true,\"message\":\"done\",\"diagnostics\":[],\"payload\":{\"Assets\":[],\"n\":2.5}}");
		Stub->Reply(200, Answer);

		// A discovery query keeps spaces, wildcards, | and + through the editor's decoding (it decodes %XX only).
		TSharedPtr<FJsonObject> Result = CallStubTool(*Tools, TEXT("find_assets"),
			TEXT("{\"name\":\"*Trigger Any*\",\"uses\":[\"K2:Add\",\"Array Length|Set Length\"],\"limit\":5,\"exact_class\":false,\"path\":\"\"}"), Stub);
		TestFalse(TEXT("find_assets succeeds"), IsToolError(Result));
		TestEqual(TEXT("a success is the route's JSON as compact text"), ResultText(Result), Answer);
		if (TestEqual(TEXT("find_assets calls one route"), Stub->Calls.Num(), 1))
		{
			const FRecordedRouteCall& Call = Stub->Calls[0];
			TestEqual(TEXT("find_assets is a GET"), Call.Method, FString(TEXT("GET")));
			TestEqual(TEXT("find_assets route"), Call.Endpoint, FString(TEXT("/assetdocument/find-assets")));
			TestEqual(TEXT("name decoded"), Call.Query.FindRef(TEXT("name")), FString(TEXT("*Trigger Any*")));
			TestEqual(TEXT("uses joined with commas"), Call.Query.FindRef(TEXT("uses")), FString(TEXT("K2:Add,Array Length|Set Length")));
			TestEqual(TEXT("numbers as text"), Call.Query.FindRef(TEXT("limit")), FString(TEXT("5")));
			TestEqual(TEXT("booleans as text"), Call.Query.FindRef(TEXT("exact_class")), FString(TEXT("false")));
			TestFalse(TEXT("an empty string is left out"), Call.Query.Contains(TEXT("path")));
			TestTrue(TEXT("a GET has no body"), Call.Body.IsEmpty());
		}

		// The encoded query is the endpoint a failure names.
		Stub->Calls.Reset();
		Stub->Reply(400, TEXT("{\"success\":false,\"message\":\"bad\"}"));
		Result = CallStubTool(*Tools, TEXT("find_assets"), TEXT("{\"name\":\"*Trigger Any*\",\"uses\":[\"A+B\"]}"), Stub);
		TestTrue(TEXT("the failure names the encoded query"), ResultText(Result).StartsWith(
			TEXT("UE AssetDocument API GET /assetdocument/find-assets?name=*Trigger%20Any*&uses=A%2BB failed with HTTP 400: bad")));

		// Query flags are sent only when true.
		Stub->Calls.Reset();
		Stub->Reply(200, Answer);
		CallStubTool(*Tools, TEXT("inspect_asset_document_target"), TEXT("{\"class_or_asset\":\"/Script/Engine.CurveFloat\",\"detail\":true}"), Stub);
		CallStubTool(*Tools, TEXT("inspect_asset_document_target"), TEXT("{\"class_or_asset\":\"/Script/Engine.CurveFloat\",\"detail\":false}"), Stub);
		if (TestEqual(TEXT("two inspect calls"), Stub->Calls.Num(), 2))
		{
			TestEqual(TEXT("detail true is sent"), Stub->Calls[0].Query.FindRef(TEXT("detail")), FString(TEXT("true")));
			TestFalse(TEXT("detail false is not sent"), Stub->Calls[1].Query.Contains(TEXT("detail")));
			TestEqual(TEXT("inspect route"), Stub->Calls[0].Endpoint, FString(TEXT("/assetdocument/inspect")));
		}

		// Bodies: named fields with their defaults, the document itself, or the arguments unchanged.
		struct FBodyCase
		{
			const TCHAR* Tool;
			const TCHAR* Arguments;
			const TCHAR* Route;
			const TCHAR* Body;
		};
		const FBodyCase BodyCases[] = {
			{TEXT("extract_asset_document"), TEXT("{\"asset_path\":\"/Game/A\"}"), TEXT("/assetdocument/extract"),
				TEXT("{\"asset_path\":\"/Game/A\",\"diff_only\":true,\"include_all_writable\":false}")},
			{TEXT("extract_asset_document"), TEXT("{\"include_all_writable\":true,\"asset_path\":\"/Game/A\",\"diff_only\":false}"),
				TEXT("/assetdocument/extract"), TEXT("{\"asset_path\":\"/Game/A\",\"diff_only\":false,\"include_all_writable\":true}")},
			{TEXT("apply_asset_document_file"), TEXT("{\"file_path\":\"/p/A.assetdoc.json\"}"), TEXT("/assetdocument/apply-file"),
				TEXT("{\"file_path\":\"/p/A.assetdoc.json\",\"save_asset\":true}")},
			{TEXT("create_asset_document_template"), TEXT("{\"target\":\"/Game/BB\",\"class\":\"/Script/AIModule.BlackboardData\"}"),
				TEXT("/assetdocument/template"), TEXT("{\"Class\":\"/Script/AIModule.BlackboardData\",\"Target\":\"/Game/BB\"}")},
			{TEXT("duplicate_asset"), TEXT("{\"source\":\"/Game/SK\",\"target\":\"/Game/SK_Copy\"}"), TEXT("/assetdocument/duplicate"),
				TEXT("{\"source\":\"/Game/SK\",\"target\":\"/Game/SK_Copy\"}")},
			{TEXT("validate_asset_document"), TEXT("{\"document\":{\"SchemaVersion\":1,\"Class\":\"/Script/Engine.CurveFloat\",\"Properties\":{}}}"),
				TEXT("/assetdocument/validate"), TEXT("{\"SchemaVersion\":1,\"Class\":\"/Script/Engine.CurveFloat\",\"Properties\":{}}")},
			{TEXT("diff_asset_document"), TEXT("{\"file_path\":\"/p/A.assetdoc.json\"}"), TEXT("/assetdocument/diff"),
				TEXT("{\"file_path\":\"/p/A.assetdoc.json\"}")},
			{TEXT("undo_editor"), TEXT("{\"expect_title\":\"AI: lamp\"}"), TEXT("/editor/undo"), TEXT("{\"expect_title\":\"AI: lamp\"}")},
			{TEXT("open_level"), TEXT("{\"path\":\"/Game/Hall\",\"save_first\":true}"), TEXT("/level/open"), TEXT("{\"path\":\"/Game/Hall\",\"save_first\":true}")},
			{TEXT("edit_level"), TEXT("{\"description\":\"AI: key light\",\"ops\":[{\"op\":\"spawn\",\"id\":\"key\",\"class\":\"/Script/Engine.PointLight\",\"properties\":{\"Intensity\":8000.0}}]}"),
				TEXT("/level/edit"), TEXT("{\"description\":\"AI: key light\",\"ops\":[{\"op\":\"spawn\",\"id\":\"key\",\"class\":\"/Script/Engine.PointLight\",\"properties\":{\"Intensity\":8000}}]}")},
			{TEXT("settings"), TEXT("{\"action\":\"write\",\"section\":\"Maps & Modes\",\"properties\":{\"GameDefaultMap\":\"/Game/Maps/Arena.Arena\"}}"),
				TEXT("/settings"), TEXT("{\"action\":\"write\",\"section\":\"Maps & Modes\",\"properties\":{\"GameDefaultMap\":\"/Game/Maps/Arena.Arena\"}}")},
			{TEXT("sample_animation_bones"), TEXT("{\"asset\":\"/Game/AS_Walk\",\"bones\":[\"hand_r\"],\"times\":[0.5]}"), TEXT("/animation/sample-bones"),
				TEXT("{\"asset\":\"/Game/AS_Walk\",\"bones\":[\"hand_r\"],\"times\":[0.5]}")},
			{TEXT("execute_python"), TEXT("{\"code\":\"print('ok')\"}"), TEXT("/execute"), TEXT("{\"Code\":\"print('ok')\"}")},
			{TEXT("execute_python"), TEXT("{\"description\":\"AI: script\",\"code\":\"x\"}"), TEXT("/execute"), TEXT("{\"Code\":\"x\",\"Description\":\"AI: script\"}")},
			{TEXT("delete_assets"), TEXT("{\"assets\":[\"/Game/A\"]}"), TEXT("/delete"), TEXT("{\"Assets\":[\"/Game/A\"]}")},
		};
		for (const FBodyCase& Case : BodyCases)
		{
			Stub->Calls.Reset();
			CallStubTool(*Tools, Case.Tool, Case.Arguments, Stub);
			if (TestEqual(*FString::Printf(TEXT("%s calls one route"), Case.Tool), Stub->Calls.Num(), 1))
			{
				TestEqual(*FString::Printf(TEXT("%s method"), Case.Tool), Stub->Calls[0].Method, FString(TEXT("POST")));
				TestEqual(*FString::Printf(TEXT("%s route"), Case.Tool), Stub->Calls[0].Endpoint, FString(Case.Route));
				TestEqual(*FString::Printf(TEXT("%s body"), Case.Tool), Stub->Calls[0].Body, FString(Case.Body));
			}
		}
		Stub->Calls.Reset();
		CallStubTool(*Tools, TEXT("health_check"), TEXT("{}"), Stub);
		TestTrue(TEXT("health_check is GET /health"), Stub->Calls.Num() == 1 && Stub->Calls[0].Method == TEXT("GET") && Stub->Calls[0].Endpoint == TEXT("/health"));
	}

	// Failures of document routes.
	{
		const TSharedRef<FStubRoutes> Stub = MakeShared<FStubRoutes>();
		const TCHAR* const Document = TEXT("{\"document\":{\"SchemaVersion\":1}}");
		auto Failure = [&](int32 Status, const FString& Answer)
		{
			Stub->Reply(Status, Answer);
			const TSharedPtr<FJsonObject> Result = CallStubTool(*Tools, TEXT("validate_asset_document"), Document, Stub);
			TestTrue(*FString::Printf(TEXT("HTTP %d %s is a tool error"), Status, *Answer.Left(60)), IsToolError(Result));
			return ResultText(Result);
		};

		const FString Body400 = TEXT("{\"success\":false,\"message\":\"AssetDocument validation failed\",\"target\":\"/Game/AI/BB_Enemy\","
			"\"asset_path\":\"/Game/AI/BB_Enemy\",\"sidecar_file_path\":\"\",\"saved_asset\":false,\"wrote_sidecar\":false,\"diagnostics\":["
			"{\"path\":\"Body.Keys[1].Name\",\"code\":\"DuplicateBlackboardKey\",\"message\":\"Key 'Target' is declared twice.\"},"
			"{\"path\":\"Body.Keys[2].KeyType\",\"code\":\"UnsupportedKeyType\",\"message\":\"Unknown key type 'Banana'.\"}]}");
		FString Text = Failure(400, Body400);
		TestTrue(TEXT("400: route, status and message"), Text.StartsWith(
			TEXT("UE AssetDocument API POST /assetdocument/validate failed with HTTP 400: AssetDocument validation failed\n")));
		TestTrue(TEXT("400: target"), Text.Contains(TEXT("\nTarget: /Game/AI/BB_Enemy\n")));
		TestTrue(TEXT("400: asset path"), Text.Contains(TEXT("\nAsset path: /Game/AI/BB_Enemy\n")));
		TestFalse(TEXT("400: an empty sidecar path is left out"), Text.Contains(TEXT("Sidecar file:")));
		TestTrue(TEXT("400: every diagnostic"), Text.Contains(TEXT("\nDiagnostics (2):\n"
			"  1. [DuplicateBlackboardKey] at Body.Keys[1].Name: Key 'Target' is declared twice.\n"
			"  2. [UnsupportedKeyType] at Body.Keys[2].KeyType: Unknown key type 'Banana'.\n")));
		TestTrue(TEXT("400: the full JSON closes the text"), Text.EndsWith(FString(TEXT("\n\nFull response JSON:\n")) + Body400));

		Text = Failure(200, TEXT("{\"success\":false,\"message\":\"Montage document rejected\",\"diagnostics\":[{\"path\":\"Body.Sections\","
			"\"code\":\"MissingSection\",\"message\":\"Sections must not be empty.\"}]}"));
		TestTrue(TEXT("success:false on 200 is a failure"), Text.Contains(TEXT("failed with HTTP 200: Montage document rejected")));
		TestTrue(TEXT("success:false diagnostics"), Text.Contains(TEXT("[MissingSection] at Body.Sections: Sections must not be empty.")));

		Text = Failure(207, TEXT("{\"success\":false,\"message\":\"Applied with errors\",\"diagnostics\":[]}"));
		TestTrue(TEXT("207 is a partial failure"), Text.Contains(TEXT("HTTP 207 (partial failure: some changes may have been applied): Applied with errors")));
		TestTrue(TEXT("no diagnostics"), Text.Contains(TEXT("\nDiagnostics: none reported.\n")));

		TestTrue(TEXT("the error field"), Failure(400, TEXT("{\"success\":false,\"error\":\"Invalid JSON body\"}")).Contains(TEXT("HTTP 400: Invalid JSON body")));
		TestTrue(TEXT("message and error"), Failure(200, TEXT("{\"success\":false,\"message\":\"Primary\",\"error\":\"Secondary\"}")).Contains(TEXT(": Primary | Secondary\n")));
		TestTrue(TEXT("a bare success:false"), Failure(200, TEXT("{\"success\":false}")).Contains(TEXT("AssetDocument request reported success=false")));
		Text = Failure(500, TEXT("{\"error\":\"backend exploded\"}"));
		TestTrue(TEXT("500 with an error"), Text.Contains(TEXT("HTTP 500: backend exploded")));
		TestTrue(TEXT("500 without a message"), Failure(500, TEXT("{}")).Contains(TEXT("failed with HTTP 500: request failed")));

		Text = Failure(400, TEXT("{\"success\":false,\"message\":\"AssetDocument apply failed\",\"diagnostics\":["
			"{\"path\":\"/Body/RowStruct\",\"code\":\"ClassReference.NonCanonicalAsset\",\"message\":\"The asset path did not resolve.\",\"severity\":\"error\","
			"\"details\":{\"primitive\":\"editor.reference.resolve_asset.v1\"}},"
			"{\"path\":\"/Body/EventGraph\",\"code\":\"Graph.FinalizeMessage\",\"message\":\"Orphaned pin.\",\"severity\":\"warning\"},"
			"{\"message\":\"only message\"},{\"code\":\"C\",\"path\":\"\",\"message\":\"m\",\"hint\":\"x\"},\"plain string\","
			"{\"code\":\"X\",\"message\":\"m\",\"severity\":\"info\"}],"
			"\"compile\":{\"Status\":\"Error\",\"Messages\":[{\"Severity\":\"Error\",\"Message\":\"Missing target\",\"Graph\":\"EventGraph\",\"Node\":\"Print String\",\"NodeGuid\":\"0\"}]}}"));
		TestTrue(TEXT("details follow an error diagnostic"), Text.Contains(TEXT(
			"  1. [ClassReference.NonCanonicalAsset] at /Body/RowStruct: The asset path did not resolve. {\"details\":{\"primitive\":\"editor.reference.resolve_asset.v1\"}}\n")));
		TestTrue(TEXT("a warning is prefixed"), Text.Contains(TEXT("  2. warning [Graph.FinalizeMessage] at /Body/EventGraph: Orphaned pin.\n")));
		TestTrue(TEXT("a diagnostic without code"), Text.Contains(TEXT("  3. [NoCode]: only message\n")));
		TestTrue(TEXT("extra fields are kept"), Text.Contains(TEXT("  4. [C]: m {\"hint\":\"x\"}\n")));
		TestTrue(TEXT("a string diagnostic"), Text.Contains(TEXT("  5. plain string\n")));
		TestTrue(TEXT("an info diagnostic"), Text.Contains(TEXT("  6. info [X]: m\n")));
		TestTrue(TEXT("the compile status"), Text.Contains(TEXT("Compile: Error (1 messages)")));
		TestTrue(TEXT("a compile message"), Text.Contains(TEXT("- Error: Missing target (Print String in EventGraph)")));
	}

	// execute_python, delete_assets and the screenshot.
	{
		const TSharedRef<FStubRoutes> Stub = MakeShared<FStubRoutes>();
		Stub->Reply(500, TEXT("{\"success\":false,\"error\":\"Traceback: ValueError: boom\",\"output\":\"\"}"));
		TSharedPtr<FJsonObject> Result = CallStubTool(*Tools, TEXT("execute_python"), TEXT("{\"code\":\"raise ValueError('boom')\"}"), Stub);
		TestTrue(TEXT("a Python error is a tool error"), IsToolError(Result));
		TestEqual(TEXT("the Python error text"), ResultText(Result),
			FString(TEXT("execute_python failed: Traceback: ValueError: boom\n{\"success\":false,\"error\":\"Traceback: ValueError: boom\",\"output\":\"\"}")));
		Stub->Reply(200, TEXT("{\"success\":true,\"output\":\"ok\",\"undo_title\":\"AI: script\"}"));
		Result = CallStubTool(*Tools, TEXT("execute_python"), TEXT("{\"code\":\"print('ok')\"}"), Stub);
		TestFalse(TEXT("a Python success is no error"), IsToolError(Result));
		TestEqual(TEXT("a Python success is compact JSON"), ResultText(Result), FString(TEXT("{\"success\":true,\"output\":\"ok\",\"undo_title\":\"AI: script\"}")));

		Stub->Reply(200, TEXT("{\"success\":false,\"total\":2,\"succeeded\":1,\"failed\":1,\"results\":[{\"asset\":\"/Game/A\",\"status\":\"Success\","
			"\"message\":\"Asset deleted successfully\"},{\"asset\":\"/Game/AM_TestA\",\"status\":\"Failed\",\"error\":\"The editor's asset deletion "
			"refused it: package /Game/BP_User references it (asset registry).\",\"referencers\":[{\"kind\":\"package\",\"referencer\":\"/Game/BP_User\"}]}]}"));
		Result = CallStubTool(*Tools, TEXT("delete_assets"), TEXT("{\"assets\":[\"/Game/A\",\"/Game/AM_TestA\"]}"), Stub);
		TestTrue(TEXT("a failed delete is a tool error"), IsToolError(Result));
		const FString Deleted = ResultText(Result);
		TestTrue(TEXT("the failure names the asset first"), Deleted.StartsWith(TEXT("/Game/AM_TestA: The editor's asset deletion refused it: package /Game/BP_User")));
		TestTrue(TEXT("the referencers are in the JSON"), Deleted.Contains(TEXT("\"referencers\":[{\"kind\":\"package\",\"referencer\":\"/Game/BP_User\"}]")));
		TestFalse(TEXT("a deleted asset is not a failure"), Deleted.Contains(TEXT("/Game/A:")));
		Stub->Reply(200, TEXT("{\"success\":true,\"total\":1,\"succeeded\":1,\"failed\":0,\"results\":[{\"asset\":\"/Game/A\",\"status\":\"Success\"}]}"));
		TestFalse(TEXT("a delete that succeeded"), IsToolError(CallStubTool(*Tools, TEXT("delete_assets"), TEXT("{\"assets\":[\"/Game/A\"]}"), Stub)));
		Stub->Reply(400, TEXT("{\"success\":false,\"error\":\"No assets specified\"}"));
		Result = CallStubTool(*Tools, TEXT("delete_assets"), TEXT("{}"), Stub);
		TestTrue(TEXT("a refused delete names the error"), IsToolError(Result) && ResultText(Result).StartsWith(TEXT("No assets specified\n")));

		Stub->Reply(200, TEXT("{\"success\":true,\"diagnostics\":[{\"code\":\"Play.Note\",\"message\":\"note\"}],\"payload\":{\"view\":\"game\",\"hud\":true,"
			"\"world_time\":1.5,\"width\":64,\"height\":36,\"quality\":80,\"mime_type\":\"image/jpeg\",\"data\":\"QUJD\",\"bytes\":2048}}"));
		Result = CallStubTool(*Tools, TEXT("get_viewport_screenshot"), TEXT("{\"view\":\"game\"}"), Stub);
		const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
		if (TestTrue(TEXT("the screenshot is an image and a caption"), Result.IsValid() && Result->TryGetArrayField(TEXT("content"), Content) && Content->Num() == 2))
		{
			const TSharedPtr<FJsonObject> Image = (*Content)[0]->AsObject();
			TestEqual(TEXT("image block"), AssetDocumentMcpJson::StringifyObject(Image), FString(TEXT("{\"type\":\"image\",\"data\":\"QUJD\",\"mimeType\":\"image/jpeg\"}")));
			TestEqual(TEXT("caption"), ResultText(Result), FString(TEXT("Game view with HUD at world time 1.5 s: 64x36 px, JPEG quality 80, 2.0 KB. Play.Note: note")));
		}
		Stub->Reply(200, TEXT("{\"success\":true,\"payload\":{\"view\":\"editor\"}}"));
		Result = CallStubTool(*Tools, TEXT("get_viewport_screenshot"), TEXT("{}"), Stub);
		TestTrue(TEXT("a screenshot without data is a tool error"), IsToolError(Result) && ResultText(Result) == TEXT("Error: screenshot failed (no data returned)"));
		Stub->Reply(200, TEXT("{\"content\":[{\"type\":\"text\",\"text\":\"as is\"}]}"));
		TestEqual(TEXT("an answer that is an MCP result passes as it is"), ResultText(CallStubTool(*Tools, TEXT("query_play"), TEXT("{}"), Stub)), FString(TEXT("as is")));
	}

	// A pending play operation is polled on the ticker until it finishes.
	{
		const TSharedRef<FStubRoutes> Stub = MakeShared<FStubRoutes>();
		Stub->Queue.Add({200, TEXT("{\"success\":true,\"payload\":{\"pending\":{\"id\":7,\"poll_ms\":0}}}")});
		Stub->Queue.Add({200, TEXT("{\"success\":true,\"payload\":{\"pending\":{\"id\":7,\"poll_ms\":30}}}")});
		Stub->Queue.Add({200, TEXT("{\"success\":true,\"message\":\"Advanced 0.20 game seconds.\",\"payload\":{\"waited_game_seconds\":0.2}}")});
		const TSharedRef<FPlayPollResult> Poll = MakeShared<FPlayPollResult>();
		Tools->CallTool(TEXT("play"), AssetDocumentMcpJson::ParseObject(TEXT("{\"action\":\"wait\",\"seconds\":0.2}")), FAssetDocumentMcpToolPolicy(),
			MakeStubCaller(Stub), [Poll](TSharedPtr<FJsonObject> Result) { Poll->Result = Result; });
		TestFalse(TEXT("a pending play operation answers later"), Poll->Result.IsValid());
		const double Deadline = FPlatformTime::Seconds() + 10.0;
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Poll, Stub, Deadline]()
		{
			if (!Poll->Result.IsValid() && FPlatformTime::Seconds() < Deadline)
			{
				return false;
			}
			TestEqual(TEXT("the finished operation is the result"), ResultText(Poll->Result),
				FString(TEXT("{\"success\":true,\"message\":\"Advanced 0.20 game seconds.\",\"payload\":{\"waited_game_seconds\":0.2}}")));
			if (TestEqual(TEXT("one start and two polls"), Stub->Calls.Num(), 3))
			{
				TestEqual(TEXT("the operation"), Stub->Calls[0].Body, FString(TEXT("{\"action\":\"wait\",\"seconds\":0.2}")));
				TestEqual(TEXT("a poll names the operation"), Stub->Calls[1].Body, FString(TEXT("{\"action\":\"poll\",\"id\":7}")));
				TestEqual(TEXT("polls go to the play route"), Stub->Calls[2].Endpoint, FString(TEXT("/play")));
			}
			return true;
		}));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMcpGuidesTest,
	"UEAssetDoc.HttpServer.Mcp.Guides",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMcpGuidesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMcpToolsTestsLocal;
	const TSharedRef<FAssetDocumentMcpTools> Tools = MakeShared<FAssetDocumentMcpTools>();
	FString LoadError;
	if (!TestTrue(TEXT("the tool manifest loads"), Tools->Load(McpDirectory(), LoadError)))
	{
		return false;
	}
	const TSharedRef<FStubRoutes> Stub = MakeShared<FStubRoutes>();
	const FString Schemas = FPaths::Combine(McpDirectory(), TEXT("schemas"));
	const FString SectionsDirectory = FPaths::Combine(Schemas, TEXT("assetdocument"));
	auto Schema = [&](const FString& Arguments) { return CallStubTool(*Tools, TEXT("get_asset_document_schema"), Arguments, Stub); };
	auto Profile = [&](const FString& Name) { return Schema(FString::Printf(TEXT("{\"profile\":%s}"), *AssetDocumentMcpJson::Quote(Name))); };

	// The core guide ends with the section listing (names only).
	const TArray<FString> SectionNames = MarkdownNames(SectionsDirectory);
	const FString Core = ResultText(Schema(TEXT("{}")));
	TestTrue(TEXT("the core guide is the guide file and the listing"), Core.StartsWith(LoadText(FPaths::Combine(Schemas, TEXT("AssetDocument.md")))));
	const FString Listing = LastLine(Core);
	TestEqual(TEXT("the listing names every section"), Listing, FString::Printf(TEXT("Sections: %s."), *FString::Join(SectionNames, TEXT(", "))));
	TestTrue(TEXT("the listing costs at most 60 chars per section"), Listing.Len() <= 60 * SectionNames.Num());
	TestFalse(TEXT("the level guide is not part of the AssetDocument guide"), Core.Contains(TEXT("# Level Editing Guide")));

	// Every class a section declares finds it, by path and by short name.
	for (const FString& Name : SectionNames)
	{
		const FString SectionText = LoadText(FPaths::Combine(SectionsDirectory, Name + TEXT(".md")));
		const FString Title = LineAfter(SectionText, TEXT("# ")).TrimStartAndEnd();
		TestTrue(*FString::Printf(TEXT("%s by name"), *Name), ResultText(Profile(Name)).StartsWith(FString::Printf(TEXT("# %s"), *Title)));
		TArray<FString> Classes;
		LineAfter(SectionText, TEXT("Classes: ")).ParseIntoArray(Classes, TEXT(","), true);
		for (FString ClassPath : Classes)
		{
			ClassPath.TrimStartAndEndInline();
			if (!ClassPath.StartsWith(TEXT("/")))
			{
				continue;
			}
			int32 Dot = INDEX_NONE;
			ClassPath.FindLastChar(TEXT('.'), Dot);
			for (const FString& Query : {ClassPath, ClassPath.Mid(Dot + 1)})
			{
				const TSharedPtr<FJsonObject> Found = Profile(Query);
				TestTrue(*FString::Printf(TEXT("%s finds %s"), *Query, *Name), !IsToolError(Found) && ResultText(Found).StartsWith(FString::Printf(TEXT("# %s"), *Title)));
			}
		}
	}

	// Other classes get the Generic section; an asset path is not a class; an unknown name lists the sections.
	for (const TCHAR* ClassPath : {TEXT("/Script/MyGame.MyDataAsset"), TEXT("/Game/Blueprints/BP_Data.BP_Data_C")})
	{
		const FString Text = ResultText(Profile(ClassPath));
		TestTrue(*FString::Printf(TEXT("%s gets the Generic section"), ClassPath), Text.Contains(TEXT("generic reflected profile"))
			&& Text.Contains(TEXT("# Generic Reflected Profile")));
	}
	for (const TCHAR* NotAClass : {TEXT("/Game/AI/BT_Enemy"), TEXT("/Game/AI/BT_Enemy.BT_Enemy")})
	{
		TestTrue(*FString::Printf(TEXT("%s is not a class path"), NotAClass), IsToolError(Profile(NotAClass)));
	}
	const TSharedPtr<FJsonObject> Unknown = Profile(TEXT("NoSuchProfile"));
	TestTrue(TEXT("an unknown profile is a tool error"), IsToolError(Unknown));
	TestTrue(TEXT("the error lists the sections with their classes"), ResultText(Unknown).Contains(TEXT("Sections:\n"))
		&& ResultText(Unknown).Contains(TEXT("- DataTable: DataTable Family Profiles (DataTable, CompositeDataTable"))
		&& ResultText(Unknown).Contains(TEXT("- Generic:")));

	// Reference parts: the section's closing line lists them; each is served as its file, by any letter case.
	const FString All = ResultText(Profile(TEXT("all")));
	int32 SectionsWithParts = 0;
	for (const FString& Name : SectionNames)
	{
		const TArray<FString> Parts = MarkdownNames(FPaths::Combine(SectionsDirectory, Name));
		if (Parts.Num() == 0)
		{
			continue;
		}
		++SectionsWithParts;
		const FString Section = ResultText(Profile(Name));
		const FString Closing = LastLine(Section);
		const FString Prefix = FString::Printf(TEXT("Reference parts (get_asset_document_schema with profile \"%s\" and part): "), *Name);
		TestTrue(*FString::Printf(TEXT("%s closes with its part list"), *Name), Closing.StartsWith(Prefix) && Closing.EndsWith(TEXT(".")));
		TestTrue(*FString::Printf(TEXT("\"all\" lists the %s parts"), *Name), All.Contains(Prefix));
		for (const FString& Part : Parts)
		{
			const FString File = LoadText(FPaths::Combine(SectionsDirectory, Name, Part + TEXT(".md")));
			const FString Title = LineAfter(File, TEXT("# ")).TrimStartAndEnd();
			TestTrue(*FString::Printf(TEXT("%s lists %s with its title"), *Name, *Part), Closing.Contains(FString::Printf(TEXT("`%s` (%s)"), *Part, *Title)));
			const FString Served = ResultText(Schema(FString::Printf(TEXT("{\"profile\":\"%s\",\"part\":\"%s\"}"), *Name, *Part)));
			TestEqual(*FString::Printf(TEXT("%s/%s is served as its file"), *Name, *Part), Served, File);
			TestEqual(*FString::Printf(TEXT("%s/%s by any letter case"), *Name, *Part),
				ResultText(Schema(FString::Printf(TEXT("{\"profile\":\"%s\",\"part\":\"%s\"}"), *Name.ToLower(), *Part.ToUpper()))), File);
			TestTrue(*FString::Printf(TEXT("\"all\" includes %s/%s"), *Name, *Part), All.Contains(FString::Printf(TEXT("# %s"), *Title)));
		}
		const TSharedPtr<FJsonObject> NoSuchPart = Schema(FString::Printf(TEXT("{\"profile\":\"%s\",\"part\":\"no_such_part\"}"), *Name));
		TestTrue(*FString::Printf(TEXT("%s: an unknown part is an error"), *Name), IsToolError(NoSuchPart)
			&& ResultText(NoSuchPart).Contains(FString::Printf(TEXT("The %s section has no part 'no_such_part'"), *Name)));
		for (const FString& Part : Parts)
		{
			TestTrue(*FString::Printf(TEXT("%s: the error lists %s"), *Name, *Part), ResultText(NoSuchPart).Contains(Part));
		}
	}
	TestTrue(TEXT("some sections have parts"), SectionsWithParts >= 4);
	const TSharedPtr<FJsonObject> NoParts = Schema(TEXT("{\"profile\":\"DataTable\",\"part\":\"layout\"}"));
	TestTrue(TEXT("a section without parts says so"), IsToolError(NoParts) && ResultText(NoParts).Contains(TEXT("no reference parts")));
	const TSharedPtr<FJsonObject> NoProfile = Schema(TEXT("{\"part\":\"layout\"}"));
	TestTrue(TEXT("a part needs its profile"), IsToolError(NoProfile) && ResultText(NoProfile).Contains(TEXT("'part' needs 'profile'")));
	for (const TCHAR* Title : {TEXT("# AssetDocument Guide"), TEXT("# BlackboardData Profile"), TEXT("# Blueprint Profile"), TEXT("# Profile Policy Metadata")})
	{
		TestTrue(*FString::Printf(TEXT("\"all\" includes %s"), Title), All.Contains(Title));
	}

	// Size budgets of the authoring guides (served text, the closing part list included): the measured size + 10%.
	const TPair<const TCHAR*, int32> Budgets[] = {
		{TEXT("UserDefinedTypes"), 5400}, {TEXT("DataTable"), 8300}, {TEXT("Material"), 8200}, {TEXT("MaterialInstance"), 4400},
		{TEXT("Niagara"), 12700}, {TEXT("MetaSound"), 9600}, {TEXT("Blueprint"), 13000}, {TEXT("WidgetBlueprint"), 4600},
	};
	TestTrue(*FString::Printf(TEXT("core guide %d chars within 11000"), Core.Len()), Core.Len() <= 11000);
	int32 Total = Core.Len();
	for (const TPair<const TCHAR*, int32>& Budget : Budgets)
	{
		const int32 Length = ResultText(Profile(Budget.Key)).Len();
		TestTrue(*FString::Printf(TEXT("%s section %d chars within %d"), Budget.Key, Length, Budget.Value), Length <= Budget.Value);
		Total += Length;
	}
	TestTrue(*FString::Printf(TEXT("the nine guides total %d chars, within 72000"), Total), Total <= 72000);

	// The level editing guide and its topics.
	auto Editor = [&](const FString& Arguments) { return ResultText(CallStubTool(*Tools, TEXT("get_level_editing_guide"), Arguments, Stub)); };
	const FString Level = Editor(TEXT("{}"));
	TestEqual(TEXT("the default topic is the level guide"), Level, LoadText(FPaths::Combine(Schemas, TEXT("LevelEditing.md"))));
	TestTrue(TEXT("the level guide names its topics"), Level.StartsWith(TEXT("# Level Editing Guide")) && Level.Contains(TEXT("import_settings")));
	TestEqual(TEXT("the playtest topic"), Editor(TEXT("{\"topic\":\"playtest\"}")), LoadText(FPaths::Combine(Schemas, TEXT("Playtest.md"))));
	TestEqual(TEXT("the import and settings topic"), Editor(TEXT("{\"topic\":\"import_settings\"}")), LoadText(FPaths::Combine(Schemas, TEXT("ImportAndSettings.md"))));
	TestTrue(TEXT("the level guide stays small"), Level.Len() <= 12000);
	TestTrue(TEXT("the playtest guide stays small"), Editor(TEXT("{\"topic\":\"playtest\"}")).Len() <= 12000);
	TestTrue(TEXT("the import and settings guide stays small"), Editor(TEXT("{\"topic\":\"import_settings\"}")).Len() <= 8000);
	TestEqual(TEXT("guides reach no route"), Stub->Calls.Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
