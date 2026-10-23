// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Golden DTO fixtures shared with the Python graph model
// (Content/Python/asset_document/tests/fixtures/graph_dto/<primitive>.<scenario>.json).
//
// GUIDs are replaced by placeholders derived from a full graph.query.v1 of the
// asset: graph i -> "A" + i, node j of graph i -> letter('B' + i) + j (1-based,
// 32 hex digits), anything else -> "F" + n. Files use Python's json.dumps(indent=2).
//
// Default run: compares live output with the checked-in files.
// Capture: set ASSETDOC_CAPTURE_GRAPH_FIXTURES=1 in the environment to (re)write them.

#include "Tests/AssetDocumentGraphTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformMisc.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "AssetDocumentPluginPaths.h"

namespace UE::AssetDocument::Graph::Tests
{
namespace
{
FString FixtureDirectory()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(GetAssetDocumentPluginBaseDir(),
		TEXT("Content/Python/asset_document/tests/fixtures/graph_dto")));
}

FString Placeholder(TCHAR Letter, int32 Index)
{
	return FString::Printf(TEXT("%c%031d"), Letter, Index);
}

class FGuidNames
{
public:
	void Learn(FSession& Session)
	{
		const FValue Query = Session.CallOrNull(TEXT("graph.query.v1"), TEXT("{}"));
		if (const FValue* Graphs = At(Query, TEXT("graphs")))
		{
			for (const FValue& Graph : Graphs->Elements)
			{
				const FString GraphGuid = Str(Graph, TEXT("graph_guid"));
				if (!GraphLetters.Contains(GraphGuid))
				{
					GraphLetters.Add(GraphGuid, TEXT('B') + GraphLetters.Num());
					Names.Add(GraphGuid, Placeholder(TEXT('A'), GraphLetters.Num()));
				}
				const TCHAR Letter = GraphLetters[GraphGuid];
				int32& Count = NodeCounts.FindOrAdd(Letter);
				for (const FValue& Node : At(Graph, TEXT("nodes"))->Elements)
				{
					const FString NodeGuid = Str(Node, TEXT("node_guid"));
					if (!Names.Contains(NodeGuid))
					{
						Names.Add(NodeGuid, Placeholder(Letter, ++Count));
					}
				}
			}
		}
	}

	FString Name(const FString& Guid)
	{
		if (const FString* Known = Names.Find(Guid))
		{
			return *Known;
		}
		return Names.Add(Guid, Placeholder(TEXT('F'), ++Others));
	}

	bool IsGuid(const FString& Text) const
	{
		FGuid Ignored;
		return Text.Len() == 32 && TryParseGuid(Text, Ignored);
	}

	FValue Normalize(const FValue& Value)
	{
		FValue Out = Value;
		if (Out.Kind == EAssetDocumentRuntimeValueKind::String && IsGuid(Out.StringValue))
		{
			Out.StringValue = Name(Out.StringValue);
		}
		for (FValue& Element : Out.Elements)
		{
			Element = Normalize(Element);
		}
		return Out;
	}

private:
	TMap<FString, FString> Names;
	TMap<FString, TCHAR> GraphLetters;
	TMap<TCHAR, int32> NodeCounts;
	int32 Others = 0;
};

FString Escape(const FString& Text)
{
	FString Out;
	for (const TCHAR C : Text)
	{
		switch (C)
		{
		case TEXT('"'): Out += TEXT("\\\""); break;
		case TEXT('\\'): Out += TEXT("\\\\"); break;
		case TEXT('\n'): Out += TEXT("\\n"); break;
		case TEXT('\r'): Out += TEXT("\\r"); break;
		case TEXT('\t'): Out += TEXT("\\t"); break;
		default:
			if (C < 0x20 || C > 0x7E)
			{
				Out += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(C));
			}
			else
			{
				Out.AppendChar(C);
			}
		}
	}
	return Out;
}

// Python json.dumps(value, indent=2) (ensure_ascii, insertion order).
void Write(const FValue& Value, int32 Indent, FString& Out)
{
	const FString Pad = FString::ChrN((Indent + 1) * 2, TEXT(' '));
	const FString Close = FString::ChrN(Indent * 2, TEXT(' '));
	switch (Value.Kind)
	{
	case EAssetDocumentRuntimeValueKind::Null: Out += TEXT("null"); return;
	case EAssetDocumentRuntimeValueKind::Boolean: Out += Value.BooleanValue ? TEXT("true") : TEXT("false"); return;
	case EAssetDocumentRuntimeValueKind::Integer: Out += FString::Printf(TEXT("%lld"), Value.IntegerValue); return;
	case EAssetDocumentRuntimeValueKind::Number:
	{
		FString Number = FString::Printf(TEXT("%.17g"), Value.NumberValue);
		if (!Number.Contains(TEXT(".")) && !Number.Contains(TEXT("e")))
		{
			Number += TEXT(".0");
		}
		Out += Number;
		return;
	}
	case EAssetDocumentRuntimeValueKind::String: Out += TEXT("\"") + Escape(Value.StringValue) + TEXT("\""); return;
	case EAssetDocumentRuntimeValueKind::Array:
		if (Value.Elements.IsEmpty())
		{
			Out += TEXT("[]");
			return;
		}
		Out += TEXT("[\n");
		for (int32 Index = 0; Index < Value.Elements.Num(); ++Index)
		{
			Out += Pad;
			Write(Value.Elements[Index], Indent + 1, Out);
			Out += Index + 1 < Value.Elements.Num() ? TEXT(",\n") : TEXT("\n");
		}
		Out += Close + TEXT("]");
		return;
	case EAssetDocumentRuntimeValueKind::Object:
		if (Value.Elements.IsEmpty())
		{
			Out += TEXT("{}");
			return;
		}
		Out += TEXT("{\n");
		for (int32 Index = 0; Index < Value.Elements.Num(); ++Index)
		{
			Out += Pad + TEXT("\"") + Escape(Value.ObjectKeys[Index]) + TEXT("\": ");
			Write(Value.Elements[Index], Indent + 1, Out);
			Out += Index + 1 < Value.Elements.Num() ? TEXT(",\n") : TEXT("\n");
		}
		Out += Close + TEXT("}");
		return;
	default:
		Out += TEXT("null");
	}
}

// A fixture's file: an engine version whose editor reports differently (a list-view node title, ...) has its own
// <name>.ue<major>.<minor>.json next to the shared one.
FString FixturePath(const FString& Name)
{
	const FString Versioned = FixtureDirectory() / FString::Printf(TEXT("%s.ue%d.%d.json"), *Name, ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION);
	return FPaths::FileExists(Versioned) ? Versioned : FixtureDirectory() / (Name + TEXT(".json"));
}

class FFixtureWriter
{
public:
	explicit FFixtureWriter(FAutomationTestBase& InTest) : Test(InTest)
	{
		bCapture = FPlatformMisc::GetEnvironmentVariable(TEXT("ASSETDOC_CAPTURE_GRAPH_FIXTURES")) == TEXT("1");
	}

	void Check(FGuidNames& Names, const FString& Name, const FValue& Value)
	{
		FString Text;
		Write(Names.Normalize(Value), 0, Text);
		Text += TEXT("\n");
		const FString Path = FixturePath(Name);
		if (bCapture)
		{
			Test.TestTrue(FString::Printf(TEXT("wrote fixture %s"), *Path), FFileHelper::SaveStringToFile(Text, *Path,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
			return;
		}
		FString Existing;
		if (!FFileHelper::LoadFileToString(Existing, *Path))
		{
			Test.AddError(FString::Printf(TEXT("fixture %s is missing; capture with ASSETDOC_CAPTURE_GRAPH_FIXTURES=1"), *Path));
			return;
		}
		Existing.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
		if (Existing != Text)
		{
			Test.AddError(FString::Printf(TEXT("fixture %s differs from live output:\n%s"), *Name, *Text));
		}
	}

private:
	FAutomationTestBase& Test;
	bool bCapture = false;
};

FValue Call(FSession& Session, const TCHAR* Primitive, const FString& Args)
{
	return Session.CallOrNull(Primitive, Args);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphFixturesTest,
	"UEAssetDoc.Graph.Fixtures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphFixturesTest::RunTest(const FString& Parameters)
{
	FFixtureWriter Fixtures(*this);

	// Empty Actor Blueprint.
	{
		FTestBlueprint Blueprint(TEXT("FixtureEmpty"));
		if (!TestNotNull(TEXT("test Blueprint"), Blueprint.Get()))
		{
			return false;
		}
		FSession Session(*this, *Blueprint.Get());
		FGuidNames Names;
		Names.Learn(Session);
		Fixtures.Check(Names, TEXT("graph.list.v1.k2_empty_actor"), Call(Session, TEXT("graph.list.v1"), TEXT("{}")));
		Fixtures.Check(Names, TEXT("graph.query.v1.k2_empty_actor"), Call(Session, TEXT("graph.query.v1"), TEXT("{}")));
	}

	// BeginPlay -> Branch -> PrintString.
	{
		FTestBlueprint Blueprint(TEXT("FixtureBranch"));
		FSession Session(*this, *Blueprint.Get());
		const FValue BeginPlay = Call(Session, TEXT("graph.node.spawn.v1"),
			TEXT("{\"graph\":{\"graph_path\":\"EventGraph\"},\"action_key\":\"K2:Event:/Script/Engine.Actor:ReceiveBeginPlay\"}"));
		const FValue Branch = Call(Session, TEXT("graph.node.spawn.v1"),
			TEXT("{\"graph\":{\"graph_path\":\"EventGraph\"},\"action_key\":\"K2:IfThenElse\",\"position\":{\"x\":320,\"y\":0}}"));
		const FValue Print = Call(Session, TEXT("graph.node.spawn.v1"),
			TEXT("{\"graph\":{\"graph_path\":\"EventGraph\"},\"action_key\":\"K2:CallFunction:/Script/Engine.KismetSystemLibrary:PrintString\",\"position\":{\"x\":640,\"y\":0}}"));
		const FString BeginPlayGuid = Str(BeginPlay, TEXT("node_guid"));
		const FString BranchGuid = Str(Branch, TEXT("node_guid"));
		const FString PrintGuid = Str(Print, TEXT("node_guid"));
		const FValue Connect = Call(Session, TEXT("graph.pin.connect.v1"), FString::Printf(TEXT("{\"from\":%s,\"to\":%s}"),
			*PinJson(BeginPlayGuid, TEXT("then"), TEXT("output")), *PinJson(BranchGuid, TEXT("execute"), TEXT("input"))));
		Call(Session, TEXT("graph.pin.connect.v1"), FString::Printf(TEXT("{\"from\":%s,\"to\":%s}"),
			*PinJson(BranchGuid, TEXT("then"), TEXT("output")), *PinJson(PrintGuid, TEXT("execute"), TEXT("input"))));
		Call(Session, TEXT("graph.pin.default.set.v1"), FString::Printf(TEXT("{\"pin\":%s,\"kind\":\"value\",\"value\":\"true\"}"),
			*PinJson(BranchGuid, TEXT("Condition"), TEXT("input"))));
		Call(Session, TEXT("graph.pin.default.set.v1"), FString::Printf(TEXT("{\"pin\":%s,\"kind\":\"value\",\"value\":\"Hello from AssetDocument\"}"),
			*PinJson(PrintGuid, TEXT("InString"), TEXT("input"))));
		FGuidNames Names;
		Names.Learn(Session);
		Fixtures.Check(Names, TEXT("graph.node.spawn.v1.k2_branch"), Branch);
		Fixtures.Check(Names, TEXT("graph.pin.connect.v1.k2_exec"), Connect);
		Fixtures.Check(Names, TEXT("graph.query.v1.k2_branch"), Call(Session, TEXT("graph.query.v1"), TEXT("{}")));
	}

	// Function AddHealth(Amount) -> NewHealth.
	{
		FTestBlueprint Blueprint(TEXT("FixtureFunction"));
		FSession Session(*this, *Blueprint.Get());
		const FValue Created = Call(Session, TEXT("graph.create.v1"), TEXT("{\"role\":\"function\",\"name\":\"AddHealth\"}"));
		const FValue Query = Call(Session, TEXT("graph.query.v1"), TEXT("{\"graphs\":[{\"graph_path\":\"AddHealth\"}]}"));
		const FValue* Graph = FindGraph(Query, TEXT("AddHealth"));
		const FValue* EntryNode = Graph != nullptr ? FindNode(*Graph, TEXT("implicit_role"), TEXT("entry")) : nullptr;
		if (!TestNotNull(TEXT("AddHealth entry"), EntryNode))
		{
			return false;
		}
		const FString Entry = Str(*EntryNode, TEXT("node_guid"));
		const FValue ResultSpawn = Call(Session, TEXT("graph.node.spawn.v1"),
			TEXT("{\"graph\":{\"graph_path\":\"AddHealth\"},\"action_key\":\"K2:FunctionResult\",\"position\":{\"x\":400,\"y\":0}}"));
		const FString Result = Str(ResultSpawn, TEXT("node_guid"));
		Call(Session, TEXT("graph.node.user_pins.set.v1"), FString::Printf(
			TEXT("{\"node_guid\":\"%s\",\"pins\":[{\"name\":\"Amount\",\"direction\":\"output\",\"type\":{\"category\":\"real\",\"sub_category\":\"double\"}}]}"), *Entry));
		Call(Session, TEXT("graph.node.user_pins.set.v1"), FString::Printf(
			TEXT("{\"node_guid\":\"%s\",\"pins\":[{\"name\":\"NewHealth\",\"direction\":\"input\",\"type\":{\"category\":\"real\",\"sub_category\":\"double\"}}]}"), *Result));
		Call(Session, TEXT("graph.pin.connect.v1"), FString::Printf(TEXT("{\"from\":%s,\"to\":%s}"),
			*PinJson(Entry, TEXT("then"), TEXT("output")), *PinJson(Result, TEXT("execute"), TEXT("input"))));
		const FValue Finalized = Call(Session, TEXT("graph.finalize.v1"), TEXT("{}"));
		FGuidNames Names;
		Names.Learn(Session);
		Fixtures.Check(Names, TEXT("graph.create.v1.k2_function"), Created);
		Fixtures.Check(Names, TEXT("graph.list.v1.k2_function"), Call(Session, TEXT("graph.list.v1"), TEXT("{}")));
		Fixtures.Check(Names, TEXT("graph.query.v1.k2_function_signature"), Call(Session, TEXT("graph.query.v1"), TEXT("{}")));
		Fixtures.Check(Names, TEXT("graph.finalize.v1.k2_ok"), Finalized);
	}
	return true;
}
} // namespace UE::AssetDocument::Graph::Tests

#endif // WITH_DEV_AUTOMATION_TESTS
