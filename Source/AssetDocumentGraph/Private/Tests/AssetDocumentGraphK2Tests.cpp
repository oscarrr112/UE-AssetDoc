// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Acceptance tests of the "graph" provider on K2 (Blueprint) graphs.
// Contract: docs/architecture/asset-document-graph-primitives.md.

#include "Tests/AssetDocumentGraphTestSupport.h"
#include "Adapters/AssetDocumentGraphBuiltinAdapters.h"
#include "Adapters/AssetDocumentGraphSortedKeys.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/BoxComponent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/SoftObjectPath.h"

namespace UE::AssetDocument::Graph::Tests
{
namespace
{
const TCHAR* BeginPlayKey = TEXT("K2:Event:/Script/Engine.Actor:ReceiveBeginPlay");
const TCHAR* TickKey = TEXT("K2:Event:/Script/Engine.Actor:ReceiveTick");
const TCHAR* BranchKey = TEXT("K2:IfThenElse");
const TCHAR* PrintStringKey = TEXT("K2:CallFunction:/Script/Engine.KismetSystemLibrary:PrintString");
const TCHAR* CastPawnKey = TEXT("K2:DynamicCast:/Script/Engine.Pawn");
const TCHAR* ForEachKey = TEXT("K2:MacroInstance:/Engine/EditorBlueprintResources/StandardMacros.StandardMacros:ForEachLoop");
const TCHAR* BoundOverlapKey = TEXT("K2:ComponentBoundEvent:/Script/Engine.PrimitiveComponent:OnComponentBeginOverlap@CollisionComp");
const TCHAR* RealDouble = TEXT("{\"category\":\"real\",\"sub_category\":\"double\"}");

// Spawns and returns the node guid ("" on failure).
FString Spawn(FSession& Session, FStringView GraphPath, FStringView Key, int32 X = 0, int32 Y = 0, FValue* OutResult = nullptr)
{
	FValue Result;
	const FString Args = FString::Printf(TEXT("{\"graph\":%s,\"action_key\":\"%.*s\",\"position\":{\"x\":%d,\"y\":%d}}"),
		*GraphJson(GraphPath), Key.Len(), Key.GetData(), X, Y);
	if (!Session.Call(TEXT("graph.node.spawn.v1"), Args, Result))
	{
		return FString();
	}
	Session.Test.TestEqual(FString::Printf(TEXT("spawned node reports action key %.*s"), Key.Len(), Key.GetData()),
		Str(Result, TEXT("node/action_key")), FString(Key));
	if (OutResult != nullptr)
	{
		*OutResult = Result;
	}
	return Str(Result, TEXT("node_guid"));
}

bool Connect(FSession& Session, FStringView FromNode, FStringView FromPin, FStringView ToNode, FStringView ToPin, FValue* OutResult = nullptr)
{
	FValue Result;
	const bool bOk = Session.Call(TEXT("graph.pin.connect.v1"), FString::Printf(TEXT("{\"from\":%s,\"to\":%s}"),
		*PinJson(FromNode, FromPin, TEXT("output")), *PinJson(ToNode, ToPin, TEXT("input"))), Result);
	if (OutResult != nullptr)
	{
		*OutResult = Result;
	}
	return bOk;
}

FString SetDefault(FSession& Session, FStringView Node, FStringView Pin, FStringView Value)
{
	FValue Result;
	Session.Call(TEXT("graph.pin.default.set.v1"), FString::Printf(TEXT("{\"pin\":%s,\"kind\":\"value\",\"value\":\"%.*s\"}"),
		*PinJson(Node, Pin, TEXT("input")), Value.Len(), Value.GetData()), Result);
	return Str(Result, TEXT("default_value"));
}

FValue QueryGraph(FSession& Session, FStringView GraphPath, bool bProperties = false)
{
	return Session.CallOrNull(TEXT("graph.query.v1"), FString::Printf(TEXT("{\"graphs\":[%s],\"include_properties\":%s}"),
		*GraphJson(GraphPath), bProperties ? TEXT("true") : TEXT("false")));
}

FString Finalize(FSession& Session)
{
	FValue Result;
	if (!Session.Call(TEXT("graph.finalize.v1"), TEXT("{}"), Result))
	{
		return FString();
	}
	if (const FValue* Messages = At(Result, TEXT("messages")))
	{
		for (const FValue& Message : Messages->Elements)
		{
			Session.Test.AddInfo(FString::Printf(TEXT("finalize %s: %s"), *Str(Message, TEXT("severity")), *Str(Message, TEXT("message"))));
		}
	}
	return Str(Result, TEXT("status"));
}

// First enumerated key that ends with Suffix (enumerate filtered by text).
FString FindKey(FSession& Session, FStringView GraphPath, FStringView Text, FStringView Suffix)
{
	const FValue Result = Session.CallOrNull(TEXT("graph.actions.enumerate.v1"), FString::Printf(
		TEXT("{\"graph\":%s,\"filter\":{\"text\":\"%.*s\"},\"limit\":400}"), *GraphJson(GraphPath), Text.Len(), Text.GetData()));
	TArray<FString> Seen;
	if (const FValue* Actions = At(Result, TEXT("actions")))
	{
		for (const FValue& Action : Actions->Elements)
		{
			const FString Key = Str(Action, TEXT("action_key"));
			if (Key.EndsWith(FString(Suffix)) && !IsTrue(Action, TEXT("requires_context")))
			{
				return Key;
			}
			Seen.Add(FString::Printf(TEXT("%s (%s) requires_context=%d"), *Key, *Str(Action, TEXT("display_name")),
				IsTrue(Action, TEXT("requires_context")) ? 1 : 0));
		}
	}
	Session.Test.AddInfo(FString::Printf(TEXT("no key ending with %.*s among:\n%s"), Suffix.Len(), Suffix.GetData(), *FString::Join(Seen, TEXT("\n"))));
	return FString();
}
} // namespace

// Acceptance 1: list graphs; reuse the ghost BeginPlay; Branch + PrintString;
// exec links; pin defaults; finalize compiles clean; query sees everything.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphK2EventBranchPrintTest,
	"UEAssetDoc.Graph.K2.EventBranchPrint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphK2EventBranchPrintTest::RunTest(const FString& Parameters)
{
	FTestBlueprint Blueprint(TEXT("EventBranchPrint"));
	if (!TestNotNull(TEXT("test Blueprint"), Blueprint.Get()))
	{
		return false;
	}
	FSession Session(*this, *Blueprint.Get());
	if (!Session.IsReady())
	{
		return false;
	}

	const FValue List = Session.CallOrNull(TEXT("graph.list.v1"), TEXT("{}"));
	TestEqual(TEXT("list result schema_version"), At(List, TEXT("schema_version")) != nullptr ? At(List, TEXT("schema_version"))->IntegerValue : 0, int64(1));
	TestEqual(TEXT("first graph is EventGraph"), Str(List, TEXT("graphs/0/path")), FString(TEXT("EventGraph")));
	TestEqual(TEXT("EventGraph role"), Str(List, TEXT("graphs/0/role")), FString(TEXT("ubergraph")));
	TestEqual(TEXT("EventGraph schema tag"), Str(List, TEXT("graphs/0/schema_tag")), FString(TEXT("K2")));
	TestEqual(TEXT("construction script path"), Str(List, TEXT("graphs/1/path")), FString(TEXT("UserConstructionScript")));
	TestEqual(TEXT("construction script role"), Str(List, TEXT("graphs/1/role")), FString(TEXT("function")));

	const FValue Empty = QueryGraph(Session, TEXT("EventGraph"));
	const FValue* EmptyGraph = FindGraph(Empty, TEXT("EventGraph"));
	const FValue* Ghost = EmptyGraph != nullptr ? FindNode(*EmptyGraph, TEXT("action_key"), BeginPlayKey) : nullptr;
	if (!TestNotNull(TEXT("new Actor Blueprint has a BeginPlay node"), Ghost))
	{
		return false;
	}
	TestTrue(TEXT("default BeginPlay is reported as ghost"), IsTrue(*Ghost, TEXT("extras/ghost")));
	const FString GhostGuid = Str(*Ghost, TEXT("node_guid"));
	TestEqual(TEXT("guid format is 32 upper hex"), GhostGuid, GhostGuid.ToUpper());
	TestEqual(TEXT("guid length"), GhostGuid.Len(), 32);

	FValue SpawnResult;
	const FString BeginPlay = Spawn(Session, TEXT("EventGraph"), BeginPlayKey, 0, 0, &SpawnResult);
	TestEqual(TEXT("BeginPlay spawn reuses the ghost node"), BeginPlay, GhostGuid);
	TestTrue(TEXT("spawn reports reuse"), IsTrue(SpawnResult, TEXT("reused")));
	TestFalse(TEXT("reused BeginPlay is no longer a ghost"), IsTrue(SpawnResult, TEXT("node/extras/ghost")));
	TestEqual(TEXT("reused BeginPlay is enabled"), Str(SpawnResult, TEXT("node/enabled_state")), FString(TEXT("enabled")));
	TestEqual(TEXT("mutation enlisted the whole package footprint"), Session.Transaction.Footprints.Num() > 0
		? Session.Transaction.Footprints[0].StableId : FString(), TEXT("Package:") + Blueprint.PackageName);

	const FString Branch = Spawn(Session, TEXT("EventGraph"), BranchKey, 320, 0);
	const FString Print = Spawn(Session, TEXT("EventGraph"), PrintStringKey, 640, 0);
	if (BeginPlay.IsEmpty() || Branch.IsEmpty() || Print.IsEmpty())
	{
		return false;
	}
	FValue ConnectResult;
	TestTrue(TEXT("BeginPlay.then -> Branch.execute"), Connect(Session, BeginPlay, TEXT("then"), Branch, TEXT("execute"), &ConnectResult));
	TestEqual(TEXT("connect response"), Str(ConnectResult, TEXT("response")), FString(TEXT("make")));
	TestTrue(TEXT("Branch.then -> PrintString.execute"), Connect(Session, Branch, TEXT("then"), Print, TEXT("execute")));
	TestEqual(TEXT("Condition stored canonical default"), SetDefault(Session, Branch, TEXT("Condition"), TEXT("true")), FString(TEXT("true")));
	TestEqual(TEXT("InString stored default"), SetDefault(Session, Print, TEXT("InString"), TEXT("Hi")), FString(TEXT("Hi")));
	TestEqual(TEXT("finalize compiles clean"), Finalize(Session), FString(TEXT("ok")));
	TestTrue(TEXT("Blueprint is up to date"), Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);

	const FValue Query = QueryGraph(Session, TEXT("EventGraph"));
	const FValue* Graph = FindGraph(Query, TEXT("EventGraph"));
	if (!TestNotNull(TEXT("query returns EventGraph"), Graph))
	{
		return false;
	}
	const FValue* BranchNode = FindNode(*Graph, TEXT("node_guid"), Branch);
	const FValue* PrintNode = FindNode(*Graph, TEXT("node_guid"), Print);
	const FValue* BeginPlayNode = FindNode(*Graph, TEXT("node_guid"), BeginPlay);
	if (!TestNotNull(TEXT("Branch queried"), BranchNode) || !TestNotNull(TEXT("PrintString queried"), PrintNode)
		|| !TestNotNull(TEXT("BeginPlay queried"), BeginPlayNode))
	{
		return false;
	}
	TestEqual(TEXT("Branch action key"), Str(*BranchNode, TEXT("action_key")), FString(BranchKey));
	TestEqual(TEXT("Branch class"), Str(*BranchNode, TEXT("class")), FString(TEXT("/Script/BlueprintGraph.K2Node_IfThenElse")));
	TestEqual(TEXT("PrintString action key"), Str(*PrintNode, TEXT("action_key")), FString(PrintStringKey));
	TestEqual(TEXT("BeginPlay action key"), Str(*BeginPlayNode, TEXT("action_key")), FString(BeginPlayKey));
	TestEqual(TEXT("Branch position"), At(*BranchNode, TEXT("position/x")) != nullptr ? At(*BranchNode, TEXT("position/x"))->IntegerValue : -1, int64(320));
	const FValue* Condition = FindPin(*BranchNode, TEXT("Condition"), TEXT("input"));
	TestTrue(TEXT("Condition default persisted"), Condition != nullptr && Str(*Condition, TEXT("default_value")) == TEXT("true"));
	TestTrue(TEXT("Condition type is bool"), Condition != nullptr && Str(*Condition, TEXT("type/category")) == TEXT("bool"));
	const FValue* InString = FindPin(*PrintNode, TEXT("InString"), TEXT("input"));
	TestTrue(TEXT("InString default persisted"), InString != nullptr && Str(*InString, TEXT("default_value")) == TEXT("Hi"));
	TestTrue(TEXT("link BeginPlay.then -> Branch.execute"), HasLink(*Graph, BeginPlay, TEXT("then"), Branch, TEXT("execute")));
	TestTrue(TEXT("link Branch.then -> PrintString.execute"), HasLink(*Graph, Branch, TEXT("then"), Print, TEXT("execute")));
	TestEqual(TEXT("exactly two links"), At(*Graph, TEXT("links")) != nullptr ? At(*Graph, TEXT("links"))->Elements.Num() : -1, 2);
	const FValue* ThenPin = FindPin(*BranchNode, TEXT("then"), TEXT("output"));
	TestTrue(TEXT("pin linked_to names the PrintString exec pin"), ThenPin != nullptr
		&& Str(*ThenPin, TEXT("linked_to/0/node_guid")) == Print && Str(*ThenPin, TEXT("linked_to/0/pin_name")) == TEXT("execute"));
	return true;
}

// Acceptance 2: function graph with user pins on Entry/Result, a multiply node,
// signature flags (decision 4) and a clean compile.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphK2FunctionSignatureTest,
	"UEAssetDoc.Graph.K2.FunctionSignature",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphK2FunctionSignatureTest::RunTest(const FString& Parameters)
{
	FTestBlueprint Blueprint(TEXT("FunctionSignature"));
	if (!TestNotNull(TEXT("test Blueprint"), Blueprint.Get()))
	{
		return false;
	}
	FSession Session(*this, *Blueprint.Get());
	if (!Session.IsReady())
	{
		return false;
	}
	FValue Created;
	if (!Session.Call(TEXT("graph.create.v1"), TEXT("{\"role\":\"function\",\"name\":\"Double\"}"), Created))
	{
		return false;
	}
	TestEqual(TEXT("function path has no section prefix"), Str(Created, TEXT("path")), FString(TEXT("Double")));
	TestEqual(TEXT("graph guid length"), Str(Created, TEXT("graph_guid")).Len(), 32);

	FValue Query = QueryGraph(Session, TEXT("Double"));
	const FValue* Graph = FindGraph(Query, TEXT("Double"));
	const FValue* EntryNode = Graph != nullptr ? FindNode(*Graph, TEXT("implicit_role"), TEXT("entry")) : nullptr;
	if (!TestNotNull(TEXT("function has an entry node"), EntryNode))
	{
		return false;
	}
	const FString Entry = Str(*EntryNode, TEXT("node_guid"));
	TestNotNull(TEXT("entry extras.user_pins"), At(*EntryNode, TEXT("extras/user_pins")));
	TestEqual(TEXT("entry extras.signature_flags.access"), Str(*EntryNode, TEXT("extras/signature_flags/access")), FString(TEXT("public")));
	FString Result;
	if (const FValue* ResultNode = FindNode(*Graph, TEXT("implicit_role"), TEXT("result")))
	{
		Result = Str(*ResultNode, TEXT("node_guid"));
	}
	else
	{
		Result = Spawn(Session, TEXT("Double"), TEXT("K2:FunctionResult"), 600, 0);
	}
	if (Result.IsEmpty())
	{
		return false;
	}
	FValue Pins;
	TestTrue(TEXT("entry user pins (direction = output on Entry)"), Session.Call(TEXT("graph.node.user_pins.set.v1"), FString::Printf(
		TEXT("{\"node_guid\":\"%s\",\"pins\":[{\"name\":\"In\",\"direction\":\"output\",\"type\":%s}]}"), *Entry, RealDouble), Pins));
	TestNotNull(TEXT("entry snapshot exposes the In pin"), FindPin(*At(Pins, TEXT("node")), TEXT("In"), TEXT("output")));
	TestTrue(TEXT("entry extras.user_pins"), Strings(Pins, TEXT("node/extras/user_pins")) == TArray<FString>{TEXT("In")});
	TestTrue(TEXT("result user pins (direction = input on Result)"), Session.Call(TEXT("graph.node.user_pins.set.v1"), FString::Printf(
		TEXT("{\"node_guid\":\"%s\",\"pins\":[{\"name\":\"Out\",\"direction\":\"input\",\"type\":%s}]}"), *Result, RealDouble), Pins));
	TestNotNull(TEXT("result snapshot exposes the Out pin"), FindPin(*At(Pins, TEXT("node")), TEXT("Out"), TEXT("input")));

	const FString MultiplyKey = FindKey(Session, TEXT("Double"), TEXT("Multiply"), TEXT("K2:PromotableOperator:Multiply"));
	AddInfo(FString::Printf(TEXT("multiply action key: %s"), *MultiplyKey));
	if (!TestFalse(TEXT("multiply action is enumerable"), MultiplyKey.IsEmpty()))
	{
		return false;
	}
	const FString Multiply = Spawn(Session, TEXT("Double"), MultiplyKey, 300, 100);
	if (Multiply.IsEmpty())
	{
		return false;
	}
	TestTrue(TEXT("Entry.then -> Result.execute"), Connect(Session, Entry, TEXT("then"), Result, TEXT("execute")));
	TestTrue(TEXT("Entry.In -> Multiply.A"), Connect(Session, Entry, TEXT("In"), Multiply, TEXT("A")));
	TestTrue(TEXT("Multiply.ReturnValue -> Result.Out"), Connect(Session, Multiply, TEXT("ReturnValue"), Result, TEXT("Out")));
	const FString StoredB = SetDefault(Session, Multiply, TEXT("B"), TEXT("2.0"));
	AddInfo(FString::Printf(TEXT("canonical stored B default: %s"), *StoredB));
	TestTrue(TEXT("B default stored canonically"), StoredB.StartsWith(TEXT("2.0")));

	FValue Flags;
	TestTrue(TEXT("function flags set"), Session.Call(TEXT("graph.function.flags.set.v1"), FString::Printf(
		TEXT("{\"graph\":%s,\"flags\":{\"pure\":true,\"category\":\"Math\",\"tooltip\":\"Doubles In\",\"keywords\":\"twice\",\"access\":\"protected\"}}"),
		*GraphJson(TEXT("Double"))), Flags));
	TestTrue(TEXT("flags.pure"), IsTrue(Flags, TEXT("flags/pure")));
	TestFalse(TEXT("flags.const untouched"), IsTrue(Flags, TEXT("flags/const")));
	TestEqual(TEXT("flags.category"), Str(Flags, TEXT("flags/category")), FString(TEXT("Math")));
	TestEqual(TEXT("flags.tooltip"), Str(Flags, TEXT("flags/tooltip")), FString(TEXT("Doubles In")));
	TestEqual(TEXT("flags.keywords"), Str(Flags, TEXT("flags/keywords")), FString(TEXT("twice")));
	TestEqual(TEXT("flags.access"), Str(Flags, TEXT("flags/access")), FString(TEXT("protected")));

	TestEqual(TEXT("finalize compiles clean"), Finalize(Session), FString(TEXT("ok")));
	UClass* Generated = Blueprint->GeneratedClass;
	UFunction* Function = Generated != nullptr ? Generated->FindFunctionByName(TEXT("Double")) : nullptr;
	if (TestNotNull(TEXT("compiled function exists"), Function))
	{
		TestNotNull(TEXT("compiled function has In"), FindFProperty<FProperty>(Function, TEXT("In")));
		TestNotNull(TEXT("compiled function has Out"), FindFProperty<FProperty>(Function, TEXT("Out")));
		TestTrue(TEXT("compiled function is pure"), Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
		TestTrue(TEXT("compiled function is protected"), Function->HasAnyFunctionFlags(FUNC_Protected));
	}

	Query = QueryGraph(Session, TEXT("Double"));
	Graph = FindGraph(Query, TEXT("Double"));
	if (!TestNotNull(TEXT("query Double"), Graph))
	{
		return false;
	}
	const FValue* EntryAfter = FindNode(*Graph, TEXT("node_guid"), Entry);
	const FValue* ResultAfter = FindNode(*Graph, TEXT("node_guid"), Result);
	TestTrue(TEXT("entry extras.user_pins after compile"), EntryAfter != nullptr && Strings(*EntryAfter, TEXT("extras/user_pins")) == TArray<FString>{TEXT("In")});
	TestTrue(TEXT("result extras.user_pins after compile"), ResultAfter != nullptr && Strings(*ResultAfter, TEXT("extras/user_pins")) == TArray<FString>{TEXT("Out")});
	TestTrue(TEXT("entry signature_flags.category"), EntryAfter != nullptr && Str(*EntryAfter, TEXT("extras/signature_flags/category")) == TEXT("Math"));
	TestTrue(TEXT("link Entry.In -> Multiply.A"), HasLink(*Graph, Entry, TEXT("In"), Multiply, TEXT("A")));
	TestTrue(TEXT("link Multiply.ReturnValue -> Result.Out"), HasLink(*Graph, Multiply, TEXT("ReturnValue"), Result, TEXT("Out")));

	const FValue List = Session.CallOrNull(TEXT("graph.list.v1"), TEXT("{}"));
	bool bListed = false;
	if (const FValue* Graphs = At(List, TEXT("graphs")))
	{
		for (const FValue& Entry_ : Graphs->Elements)
		{
			if (Str(Entry_, TEXT("path")) == TEXT("Double"))
			{
				bListed = Str(Entry_, TEXT("role")) == TEXT("function") && IsTrue(Entry_, TEXT("caps/rename")) && IsTrue(Entry_, TEXT("caps/remove"));
			}
		}
	}
	TestTrue(TEXT("Double listed as removable/renamable function"), bListed);
	return true;
}

// Acceptance 3: CustomEvent (+rename), DynamicCast, ForEachLoop macro,
// ComponentBoundEvent on an SCS component, dynamic pins, ghost via connect,
// include_properties shape.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphK2NodeKindsTest,
	"UEAssetDoc.Graph.K2.NodeKinds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphK2NodeKindsTest::RunTest(const FString& Parameters)
{
	FTestBlueprint Blueprint(TEXT("NodeKinds"));
	if (!TestNotNull(TEXT("test Blueprint"), Blueprint.Get()))
	{
		return false;
	}
	// Test setup through the stock SCS API (component editor equivalent).
	USCS_Node* Component = Blueprint->SimpleConstructionScript->CreateNode(UBoxComponent::StaticClass(), TEXT("CollisionComp"));
	Blueprint->SimpleConstructionScript->AddNode(Component);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint.Get());

	FSession Session(*this, *Blueprint.Get());
	if (!Session.IsReady())
	{
		return false;
	}
	const FString Custom = Spawn(Session, TEXT("EventGraph"), TEXT("K2:CustomEvent"), 0, 400);
	FValue Renamed;
	TestTrue(TEXT("rename custom event"), Session.Call(TEXT("graph.node.rename.v1"),
		FString::Printf(TEXT("{\"node_guid\":\"%s\",\"name\":\"OnPing\"}"), *Custom), Renamed));
	TestEqual(TEXT("rename returns the authored name"), Str(Renamed, TEXT("name")), FString(TEXT("OnPing")));
	const FString CastGuid = Spawn(Session, TEXT("EventGraph"), CastPawnKey, 300, 400);
	AddInfo(FString::Printf(TEXT("enumerated ForEachLoop key before load: '%s'"), *FindKey(Session, TEXT("EventGraph"), TEXT("StandardMacros.StandardMacros:ForEachLoop"), TEXT(":ForEachLoop"))));
	const FString ForEach = Spawn(Session, TEXT("EventGraph"), ForEachKey, 600, 400);
	const FString Bound = Spawn(Session, TEXT("EventGraph"), BoundOverlapKey, 0, 800);
	const FString Sequence = Spawn(Session, TEXT("EventGraph"), TEXT("K2:ExecutionSequence"), 300, 800);
	if (Custom.IsEmpty() || CastGuid.IsEmpty() || ForEach.IsEmpty() || Bound.IsEmpty() || Sequence.IsEmpty())
	{
		return false;
	}
	FValue Added;
	TestTrue(TEXT("dynamic pin add"), Session.Call(TEXT("graph.node.dynamic_pin.add.v1"),
		FString::Printf(TEXT("{\"node_guid\":\"%s\"}"), *Sequence), Added));
	TestNotNull(TEXT("dynamic pin add returns the node with then_2"), FindPin(*At(Added, TEXT("node")), TEXT("then_2"), TEXT("output")));

	FValue Query = QueryGraph(Session, TEXT("EventGraph"));
	const FValue* Graph = FindGraph(Query, TEXT("EventGraph"));
	const FValue* Tick = Graph != nullptr ? FindNode(*Graph, TEXT("action_key"), TickKey) : nullptr;
	if (!TestNotNull(TEXT("ghost Tick node"), Tick))
	{
		return false;
	}
	const FString TickGuid = Str(*Tick, TEXT("node_guid"));
	TestTrue(TEXT("Tick starts as ghost"), IsTrue(*Tick, TEXT("extras/ghost")));
	TestTrue(TEXT("Tick.then -> Sequence.execute"), Connect(Session, TickGuid, TEXT("then"), Sequence, TEXT("execute")));
	TestTrue(TEXT("Sequence.then_0 -> ForEach.Exec"), Connect(Session, Sequence, TEXT("then_0"), ForEach, TEXT("Exec")));
	TestTrue(TEXT("CustomEvent.then -> Cast.execute"), Connect(Session, Custom, TEXT("then"), CastGuid, TEXT("execute")));

	Query = QueryGraph(Session, TEXT("EventGraph"), /*bProperties=*/true);
	Graph = FindGraph(Query, TEXT("EventGraph"));
	if (!TestNotNull(TEXT("query"), Graph))
	{
		return false;
	}
	const FValue* TickAfter = FindNode(*Graph, TEXT("node_guid"), TickGuid);
	TestTrue(TEXT("connecting a ghost node enables it"), TickAfter != nullptr && !IsTrue(*TickAfter, TEXT("extras/ghost"))
		&& Str(*TickAfter, TEXT("enabled_state")) == TEXT("enabled"));
	const FValue* CustomNode = FindNode(*Graph, TEXT("node_guid"), Custom);
	TestTrue(TEXT("custom event extras.name"), CustomNode != nullptr && Str(*CustomNode, TEXT("extras/name")) == TEXT("OnPing"));
	TestTrue(TEXT("custom event action key"), CustomNode != nullptr && Str(*CustomNode, TEXT("action_key")) == TEXT("K2:CustomEvent"));
	TestTrue(TEXT("custom event extras.user_pins"), CustomNode != nullptr && At(*CustomNode, TEXT("extras/user_pins")) != nullptr);
	TestTrue(TEXT("properties.node present"), CustomNode != nullptr && At(*CustomNode, TEXT("properties/node")) != nullptr);
	TestTrue(TEXT("properties.template omitted without template"), CustomNode != nullptr && At(*CustomNode, TEXT("properties/template")) == nullptr);
	const FValue* CastNode = FindNode(*Graph, TEXT("node_guid"), CastGuid);
	TestTrue(TEXT("cast action key"), CastNode != nullptr && Str(*CastNode, TEXT("action_key")) == CastPawnKey);
	const FValue* ForEachNode = FindNode(*Graph, TEXT("node_guid"), ForEach);
	TestTrue(TEXT("macro action key"), ForEachNode != nullptr && Str(*ForEachNode, TEXT("action_key")) == ForEachKey);
	const FValue* BoundNode = FindNode(*Graph, TEXT("node_guid"), Bound);
	TestTrue(TEXT("bound event action key"), BoundNode != nullptr && Str(*BoundNode, TEXT("action_key")) == BoundOverlapKey);
	TestTrue(TEXT("Sequence can add pins"), FindNode(*Graph, TEXT("node_guid"), Sequence) != nullptr
		&& IsTrue(*FindNode(*Graph, TEXT("node_guid"), Sequence), TEXT("can_add_pin")));
	return true;
}

// Acceptance 4: schema-refused connection and unknown action key diagnostics.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphK2DiagnosticsTest,
	"UEAssetDoc.Graph.K2.Diagnostics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphK2DiagnosticsTest::RunTest(const FString& Parameters)
{
	FTestBlueprint Blueprint(TEXT("Diagnostics"));
	if (!TestNotNull(TEXT("test Blueprint"), Blueprint.Get()))
	{
		return false;
	}
	FSession Session(*this, *Blueprint.Get());
	if (!Session.IsReady())
	{
		return false;
	}
	const FString Random = Spawn(Session, TEXT("EventGraph"), TEXT("K2:CallFunction:/Script/Engine.KismetMathLibrary:RandomBool"), 0, 0);
	const FString Print = Spawn(Session, TEXT("EventGraph"), PrintStringKey, 300, 0);
	if (Random.IsEmpty() || Print.IsEmpty())
	{
		return false;
	}
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	if (Session.ExpectFailure(TEXT("graph.pin.connect.v1"), FString::Printf(TEXT("{\"from\":%s,\"to\":%s}"),
		*PinJson(Random, TEXT("ReturnValue"), TEXT("output")), *PinJson(Print, TEXT("execute"), TEXT("input"))),
		TEXT("Graph.ConnectDisallowed"), Diagnostic))
	{
		const FString Reason = DiagnosticContext(Diagnostic, TEXT("reason"));
		AddInfo(FString::Printf(TEXT("schema reason: %s"), *Reason));
		TestFalse(TEXT("disallowed connection carries the schema reason"), Reason.IsEmpty());
		TestTrue(TEXT("message quotes the reason"), Diagnostic.Message.Contains(Reason));
	}

	const FString Typo = TEXT("K2:CallFunction:/Script/Engine.KismetSystemLibrary:PrintStrin");
	if (Session.ExpectFailure(TEXT("graph.node.spawn.v1"), FString::Printf(TEXT("{\"graph\":%s,\"action_key\":\"%s\"}"),
		*GraphJson(TEXT("EventGraph")), *Typo), TEXT("Graph.ActionNotFound"), Diagnostic))
	{
		const FString Suggestions = DiagnosticContext(Diagnostic, TEXT("suggestions"));
		AddInfo(FString::Printf(TEXT("suggestions:\n%s"), *Suggestions));
		TestTrue(TEXT("suggestions include PrintString"), Suggestions.Contains(PrintStringKey));
		TestEqual(TEXT("diagnostic path"), Diagnostic.Path, FString(TEXT("/action_key")));
	}
	FValue Enumerated;
	TestTrue(TEXT("enumerate with exact keys"), Session.Call(TEXT("graph.actions.enumerate.v1"), FString::Printf(
		TEXT("{\"graph\":%s,\"filter\":{\"keys\":[\"%s\",\"%s\"]}}"), *GraphJson(TEXT("EventGraph")), PrintStringKey, *Typo), Enumerated));
	TestEqual(TEXT("known key enumerated"), Str(Enumerated, TEXT("actions/0/action_key")), FString(PrintStringKey));
	TestEqual(TEXT("unknown key unresolved"), Str(Enumerated, TEXT("unresolved/0/key")), Typo);
	TestTrue(TEXT("unresolved suggestions"), Strings(Enumerated, TEXT("unresolved/0/suggestions")).Contains(PrintStringKey));

	Session.ExpectFailure(TEXT("graph.query.v1"), TEXT("{\"graphz\":[]}"), TEXT("Graph.UnknownField"), Diagnostic);
	Session.ExpectFailure(TEXT("graph.query.v1"), TEXT("{\"graphs\":[{\"graph_path\":\"NoSuchGraph\"}]}"), TEXT("Graph.GraphNotFound"), Diagnostic);
	TestTrue(TEXT("GraphNotFound lists known paths"), Diagnostic.Message.Contains(TEXT("EventGraph")));
	Session.ExpectFailure(TEXT("graph.pin.connect.v1"), FString::Printf(TEXT("{\"from\":%s,\"to\":%s}"),
		*PinJson(Random, TEXT("NoPin"), TEXT("output")), *PinJson(Print, TEXT("execute"), TEXT("input"))), TEXT("Graph.PinNotFound"), Diagnostic);
	TestTrue(TEXT("PinNotFound lists available pins"), Diagnostic.Message.Contains(TEXT("ReturnValue")));
	Session.ExpectFailure(TEXT("graph.node.remove.v1"), TEXT("{\"node_guid\":\"0123456789ABCDEF0123456789ABCDEF\"}"), TEXT("Graph.NodeNotFound"), Diagnostic);
	Session.ExpectFailure(TEXT("graph.list.v1"), TEXT("{\"schema_version\":2}"), TEXT("Graph.UnsupportedSchemaVersion"), Diagnostic);
	// A null offset (a pager echoing the last page's next_offset) is refused at offset, not read as "unpaged".
	Session.ExpectFailure(TEXT("graph.list.v1"), TEXT("{\"offset\":null}"), TEXT("Graph.InvalidArgument"), Diagnostic);
	TestEqual(TEXT("a null offset is refused at its path"), Diagnostic.Path, FString(TEXT("/offset")));
	TestTrue(TEXT("and says so"), Diagnostic.Message.Contains(TEXT("offset is null")));
	Session.ExpectFailure(TEXT("graph.list.v1"), TEXT("{\"offset\":0,\"limit\":null}"), TEXT("Graph.InvalidArgument"), Diagnostic);
	TestEqual(TEXT("a null limit is refused at its path"), Diagnostic.Path, FString(TEXT("/limit")));
	return true;
}

// A call keyed by a subclass of the class declaring the function (NiagaraComponent's Deactivate is
// the ActorComponent's) resolves to the declaring class's action, as the palette lists it; DeactivateImmediate is a
// C++ function that is not a UFUNCTION, so no Blueprint node calls it: the refusal says so and names the callable
// relatives.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphK2InheritedCallKeyTest,
	"UEAssetDoc.Graph.K2.InheritedCallKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphK2InheritedCallKeyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!TestNotNull(TEXT("the Niagara component class is loaded"), FSoftClassPath(TEXT("/Script/Niagara.NiagaraComponent")).ResolveClass()))
	{
		return false;
	}
	FTestBlueprint Blueprint(TEXT("InheritedCallKey"));
	if (!TestNotNull(TEXT("test Blueprint"), Blueprint.Get()))
	{
		return false;
	}
	FSession Session(*this, *Blueprint.Get());
	if (!Session.IsReady())
	{
		return false;
	}
	FValue Spawned;
	TestTrue(TEXT("the subclass key places the call"), Session.Call(TEXT("graph.node.spawn.v1"), FString::Printf(
		TEXT("{\"graph\":%s,\"action_key\":\"K2:CallFunction:/Script/Niagara.NiagaraComponent:Deactivate\"}"),
		*GraphJson(TEXT("EventGraph"))), Spawned));
	TestEqual(TEXT("the node reads back under the declaring class's key"), Str(Spawned, TEXT("node/action_key")),
		FString(TEXT("K2:CallFunction:/Script/Engine.ActorComponent:Deactivate")));

	FAssetDocumentRuntimeDiagnostic Diagnostic;
	if (Session.ExpectFailure(TEXT("graph.node.spawn.v1"), FString::Printf(TEXT("{\"graph\":%s,\"action_key\":\"%s\"}"),
		*GraphJson(TEXT("EventGraph")), TEXT("K2:CallFunction:/Script/Niagara.NiagaraComponent:DeactivateImmediate")),
		TEXT("Graph.ActionNotFound"), Diagnostic))
	{
		AddInfo(Diagnostic.Message);
		TestTrue(TEXT("the refusal says the function is not Blueprint-callable"), Diagnostic.Message.Contains(TEXT("UFUNCTION(BlueprintCallable)")));
		TestTrue(TEXT("the refusal names the callable Deactivate"),
			Diagnostic.Message.Contains(TEXT("K2:CallFunction:/Script/Engine.ActorComponent:Deactivate")));
	}
	return true;
}

// graph.node.pin_type.set.v1: the Select node's Index type selector (SGraphPinIndex::OnTypeChanged).
// An ambiguous key lists each candidate (menu name, owner, pins); a key the graph does not offer names the
// key of the same action it does offer (a component's Begin Play).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphK2ActionKeyDiscoveryTest,
	"UEAssetDoc.Graph.K2.ActionKeyDiscovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphK2ActionKeyDiscoveryTest::RunTest(const FString& Parameters)
{
	FTestBlueprint Blueprint(TEXT("KeyDiscovery"), TEXT("/Temp/AssetDocumentGraphTests"), UActorComponent::StaticClass());
	if (!TestNotNull(TEXT("component Blueprint"), Blueprint.Get()))
	{
		return false;
	}
	FSession Session(*this, *Blueprint.Get());
	if (!Session.IsReady())
	{
		return false;
	}
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	if (Session.ExpectFailure(TEXT("graph.node.spawn.v1"), FString::Printf(TEXT("{\"graph\":%s,\"action_key\":\"K2:GetArrayItem\"}"),
		*GraphJson(TEXT("EventGraph"))), TEXT("Graph.ActionAmbiguous"), Diagnostic))
	{
		AddInfo(Diagnostic.Message);
		TestTrue(TEXT("the message lists the suffixed keys"), Diagnostic.Message.Contains(TEXT("K2:GetArrayItem#")));
		TestTrue(TEXT("with their pins"), Diagnostic.Message.Contains(TEXT("Dimension 1")) && Diagnostic.Message.Contains(TEXT("out: ")));
		TestTrue(TEXT("and owner"), Diagnostic.Message.Contains(TEXT("owner ")));
		TestFalse(TEXT("details.described lists them"), DiagnosticContext(Diagnostic, TEXT("described")).IsEmpty());
	}
	// K2:BeginPlay in a document is this key (graphs/actions.py).
	if (Session.ExpectFailure(TEXT("graph.node.spawn.v1"), FString::Printf(
		TEXT("{\"graph\":%s,\"action_key\":\"K2:Event:/Script/Engine.Actor:ReceiveBeginPlay\"}"),
		*GraphJson(TEXT("EventGraph"))), TEXT("Graph.ActionRequiresContext"), Diagnostic))
	{
		AddInfo(Diagnostic.Message);
		const TCHAR* ComponentBeginPlay = TEXT("K2:Event:/Script/Engine.ActorComponent:ReceiveBeginPlay");
		TestTrue(TEXT("the message names the component's Begin Play key"), Diagnostic.Message.Contains(ComponentBeginPlay));
		TestTrue(TEXT("details.use names it"), DiagnosticContext(Diagnostic, TEXT("use")).Contains(ComponentBeginPlay));
		TestFalse(TEXT("the named key places the event"), Spawn(Session, TEXT("EventGraph"), ComponentBeginPlay, 0, 0).IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphK2SelectIndexTypeTest,
	"UEAssetDoc.Graph.K2.SelectIndexType",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphK2SelectIndexTypeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FTestBlueprint Blueprint(TEXT("SelectIndex"));
	if (!TestNotNull(TEXT("test Blueprint"), Blueprint.Get()))
	{
		return false;
	}
	FSession Session(*this, *Blueprint.Get());
	if (!Session.IsReady())
	{
		return false;
	}
	FValue Spawned;
	const FString Select = Spawn(Session, TEXT("EventGraph"), TEXT("K2:Select"), 0, 0, &Spawned);
	const FString Linked = Spawn(Session, TEXT("EventGraph"), TEXT("K2:Select"), 0, 300);
	const FString Random = Spawn(Session, TEXT("EventGraph"), TEXT("K2:CallFunction:/Script/Engine.KismetMathLibrary:RandomBool"), -300, 300);
	if (Select.IsEmpty() || Linked.IsEmpty() || Random.IsEmpty())
	{
		return false;
	}
	const TArray<FString> TypedPins = Strings(Spawned, TEXT("node/extras/typed_pins"));
	TestTrue(TEXT("Select reports its Index as a typed pin"), TypedPins.Num() == 1 && TypedPins[0] == TEXT("Index"));
	auto Args = [](const FString& Node, const TCHAR* Pin, const TCHAR* Category, const TCHAR* Object)
	{
		return FString::Printf(TEXT("{\"node_guid\":\"%s\",\"pin_name\":\"%s\",\"type\":{\"category\":\"%s\",\"sub_category_object\":\"%s\"}}"),
			*Node, Pin, Category, Object);
	};
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Session.ExpectFailure(TEXT("graph.node.pin_type.set.v1"), Args(Select, TEXT("Index"), TEXT("real"), TEXT("")), TEXT("Graph.InvalidArgument"), Diagnostic);
	Session.ExpectFailure(TEXT("graph.node.pin_type.set.v1"), Args(Select, TEXT("ReturnValue"), TEXT("int"), TEXT("")), TEXT("Graph.PinTypeNotEditable"), Diagnostic);
	Session.ExpectFailure(TEXT("graph.node.pin_type.set.v1"), Args(Select, TEXT("NoPin"), TEXT("int"), TEXT("")), TEXT("Graph.PinNotFound"), Diagnostic);

	FValue Typed;
	TestTrue(TEXT("pick an enum Index type"), Session.Call(TEXT("graph.node.pin_type.set.v1"),
		Args(Select, TEXT("Index"), TEXT("byte"), TEXT("/Script/Engine.ECollisionChannel")), Typed));
	const FValue* TypedNode = At(Typed, TEXT("node"));
	if (!TestNotNull(TEXT("the reconstructed node"), TypedNode))
	{
		return false;
	}
	const FValue* Index = FindPin(*TypedNode, TEXT("Index"));
	TestTrue(TEXT("the Index pin is the enum"), Index != nullptr && Str(*Index, TEXT("type/sub_category_object")) == TEXT("/Script/Engine.ECollisionChannel"));
	TestNotNull(TEXT("one option pin per enumerator"), FindPin(*TypedNode, TEXT("ECC_WorldStatic")));
	// The selector shows only while the pin is an unlinked wildcard.
	Session.ExpectFailure(TEXT("graph.node.pin_type.set.v1"), Args(Select, TEXT("Index"), TEXT("int"), TEXT("")), TEXT("Graph.PinTypeNotEditable"), Diagnostic);
	TestTrue(TEXT("link a bool to the other Select's Index"), Connect(Session, Random, TEXT("ReturnValue"), Linked, TEXT("Index")));
	Session.ExpectFailure(TEXT("graph.node.pin_type.set.v1"), Args(Linked, TEXT("Index"), TEXT("int"), TEXT("")), TEXT("Graph.PinTypeNotEditable"), Diagnostic);
	return !HasAnyErrors();
}

// The candidate list of every node spawn (the whole action database) is sorted by key with duplicate registrations
// removed: the same items and order as a stable sort plus adjacent removal, in O(n log n). The quadratic version took
// ~1 s per spawn on the PC, which put the UBlueprint group (SelectIndexType 2-4 minutes) at the 1500 s editor watchdog.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphK2CandidateOrderTest,
	"UEAssetDoc.Graph.K2.CandidateOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphK2CandidateOrderTest::RunTest(const FString& Parameters)
{
	struct FItem
	{
		FString Key;
		int32 Id = 0;
		TSet<int32> Payload; // a candidate's binding set: items are not trivially cheap to move
	};
	auto KeyOf = [](const FItem& Item) -> const FString& { return Item.Key; };
	auto Ids = [](const TArray<FItem>& Items)
	{
		TArray<FString> Out;
		for (const FItem& Item : Items)
		{
			Out.Add(FString::FromInt(Item.Id));
		}
		return FString::Join(Out, TEXT(","));
	};
	auto Make = [](int32 Count, int32 Distinct, int32 Seed)
	{
		FRandomStream Random(Seed);
		TArray<FItem> Items;
		for (int32 Id = 0; Id < Count; ++Id)
		{
			FItem& Item = Items.AddDefaulted_GetRef();
			Item.Key = FString::Printf(TEXT("K2:CallFunction:/Script/Engine.Library%d:Function%d"), Random.RandRange(0, Distinct), Random.RandRange(0, 9));
			Item.Id = Id;
			Item.Payload.Add(Id);
		}
		return Items;
	};

	// Keeps the first of equal keys, in key order (FString ordering ignores case, as the previous sort did).
	TArray<FItem> Small;
	for (const TCHAR* Key : {TEXT("c"), TEXT("a"), TEXT("b"), TEXT("A"), TEXT("c"), TEXT("a")})
	{
		Small.Add({Key, Small.Num(), {}});
	}
	SortUniqueByKey(Small, KeyOf);
	TestEqual(TEXT("first of each key, sorted"), Ids(Small), FString(TEXT("1,2,0")));

	// The same result as the stable sort and adjacent removal it replaces.
	TArray<FItem> Reference = Make(3000, 300, 7);
	TArray<FItem> Fast = Make(3000, 300, 7);
	Reference.StableSort([](const FItem& A, const FItem& B) { return A.Key < B.Key; });
	for (int32 Index = Reference.Num() - 1; Index > 0; --Index)
	{
		if (Reference[Index].Key == Reference[Index - 1].Key)
		{
			Reference.RemoveAt(Index);
		}
	}
	SortUniqueByKey(Fast, KeyOf);
	TestEqual(TEXT("same items and order as stable sort + adjacent removal"), Ids(Fast), Ids(Reference));

	// An action-database-sized list with many duplicate registrations stays fast.
	TArray<FItem> Large = Make(80000, 4000, 11);
	const double Start = FPlatformTime::Seconds();
	SortUniqueByKey(Large, KeyOf);
	const double Seconds = FPlatformTime::Seconds() - Start;
	AddInfo(FString::Printf(TEXT("80000 candidates (%d unique) sorted in %.3f s"), Large.Num(), Seconds));
	TestTrue(TEXT("80000 candidates sort in well under the per-spawn budget"), Seconds < 5.0);
	return !HasAnyErrors();
}

// Acceptance 5: action keys are deterministic and resolve on another Blueprint.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphK2ActionKeysTest,
	"UEAssetDoc.Graph.K2.ActionKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphK2ActionKeysTest::RunTest(const FString& Parameters)
{
	TArray<FString> Keys;
	{
		FTestBlueprint Blueprint(TEXT("ActionKeysA"));
		if (!TestNotNull(TEXT("test Blueprint"), Blueprint.Get()))
		{
			return false;
		}
		FSession Session(*this, *Blueprint.Get());
		if (!Session.IsReady())
		{
			return false;
		}
		auto Enumerate = [&](const FString& Filter)
		{
			const FValue Result = Session.CallOrNull(TEXT("graph.actions.enumerate.v1"), FString::Printf(
				TEXT("{\"graph\":%s,\"filter\":%s,\"limit\":400}"), *GraphJson(TEXT("EventGraph")), *Filter));
			TArray<FString> Out;
			if (const FValue* Actions = At(Result, TEXT("actions")))
			{
				for (const FValue& Action : Actions->Elements)
				{
					Out.Add(Str(Action, TEXT("action_key")));
				}
			}
			const FValue* Total = At(Result, TEXT("total"));
			Out.Add(FString::Printf(TEXT("total=%lld"), Total != nullptr ? Total->IntegerValue : -1));
			return Out;
		};
		const FString SystemLibrary = TEXT("{\"key_prefix\":\"K2:CallFunction:/Script/Engine.KismetSystemLibrary:\"}");
		const TArray<FString> First = Enumerate(SystemLibrary);
		const TArray<FString> Second = Enumerate(SystemLibrary);
		TestTrue(TEXT("enumerate twice gives identical keys"), First == Second);
		TestTrue(TEXT("system library keys include PrintString"), First.Contains(PrintStringKey));
		const TArray<FString> All = Enumerate(TEXT("{}"));
		TestTrue(TEXT("unfiltered enumerate is deterministic"), All == Enumerate(TEXT("{}")));
		TestEqual(TEXT("keys are unique"), TSet<FString>(All).Num(), All.Num());
		AddInfo(FString::Printf(TEXT("unfiltered %s, first page %d keys"), *All.Last(), All.Num() - 1));
		// Keys that needed a signature suffix are not human-writable: report them by kind.
		TMap<FString, int32> SuffixedKinds;
		int64 SuffixedTotalCount = 0;
		for (TCHAR Letter = TEXT('A'); Letter <= TEXT('Z'); ++Letter)
		{
			const FValue Suffixed = Session.CallOrNull(TEXT("graph.actions.enumerate.v1"), FString::Printf(
				TEXT("{\"graph\":%s,\"filter\":{\"text\":\"#\",\"key_prefix\":\"K2:%c\"},\"limit\":400}"), *GraphJson(TEXT("EventGraph")), Letter));
			if (const FValue* Total = At(Suffixed, TEXT("total")))
			{
				SuffixedTotalCount += Total->IntegerValue;
			}
			if (const FValue* Actions = At(Suffixed, TEXT("actions")))
			{
				for (const FValue& Action : Actions->Elements)
				{
					FString Kind = Str(Action, TEXT("action_key"));
					Kind.LeftInline(Kind.Find(TEXT("#")));
					++SuffixedKinds.FindOrAdd(Kind);
				}
			}
		}
		TArray<FString> KindLines;
		for (const TPair<FString, int32>& Pair : SuffixedKinds)
		{
			KindLines.Add(FString::Printf(TEXT("%s x%d"), *Pair.Key, Pair.Value));
		}
		AddInfo(FString::Printf(TEXT("signature-suffixed keys: total=%lld; by base key (first page per letter):\n%s"),
			SuffixedTotalCount, *FString::Join(KindLines, TEXT("\n"))));
		for (const FString& Key : All)
		{
			if (Keys.Num() >= 60)
			{
				break;
			}
			if (!Key.StartsWith(TEXT("total=")))
			{
				Keys.Add(Key);
			}
		}
	}

	FTestBlueprint Fresh(TEXT("ActionKeysB"));
	if (!TestNotNull(TEXT("second Blueprint"), Fresh.Get()))
	{
		return false;
	}
	FSession Session(*this, *Fresh.Get());
	if (!Session.IsReady())
	{
		return false;
	}
	for (const TCHAR* Key : {BranchKey, PrintStringKey, CastPawnKey, ForEachKey, TEXT("K2:CustomEvent")})
	{
		TestFalse(FString::Printf(TEXT("same key spawns on a fresh Blueprint: %s"), Key), Spawn(Session, TEXT("EventGraph"), Key).IsEmpty());
	}
	// Struct/enum nodes are keyed by their field, not by a signature suffix.
	for (const TCHAR* Key : {TEXT("K2:ForEachElementInEnum:/Script/Engine.EMovementMode"), TEXT("K2:EnumLiteral:/Script/Engine.EMovementMode")})
	{
		const FAssetDocumentPrimitiveOutcome Outcome = Session.Invoke(TEXT("graph.node.spawn.v1"),
			FString::Printf(TEXT("{\"graph\":%s,\"action_key\":\"%s\"}"), *GraphJson(TEXT("EventGraph")), Key));
		TestTrue(FString::Printf(TEXT("field key spawns: %s"), Key), Outcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		TestEqual(FString::Printf(TEXT("field key round-trips: %s"), Key), Str(Outcome.Value, TEXT("node/action_key")), FString(Key));
	}
	// Round trip sample: spawn the first enumerated keys; the spawned node's
	// derived key must equal the requested key (logged; caveats are reported).
	int32 RoundTrips = 0;
	TArray<FString> Mismatches;
	for (const FString& Key : Keys)
	{
		const FAssetDocumentPrimitiveOutcome Outcome = Session.Invoke(TEXT("graph.node.spawn.v1"),
			FString::Printf(TEXT("{\"graph\":%s,\"action_key\":\"%s\"}"), *GraphJson(TEXT("EventGraph")), *Key.ReplaceCharWithEscapedChar()));
		if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
		{
			Mismatches.Add(FString::Printf(TEXT("%s -> %s"), *Key, Outcome.Diagnostics.IsEmpty() ? TEXT("?") : *Outcome.Diagnostics[0].Code));
			continue;
		}
		const FString Derived = Str(Outcome.Value, TEXT("node/action_key"));
		if (Derived == Key)
		{
			++RoundTrips;
		}
		else
		{
			Mismatches.Add(FString::Printf(TEXT("%s -> derived '%s'"), *Key, *Derived));
		}
	}
	AddInfo(FString::Printf(TEXT("round trip %d/%d; mismatches:\n%s"), RoundTrips, Keys.Num(), *FString::Join(Mismatches, TEXT("\n"))));
	TestTrue(TEXT("most sampled keys round-trip"), RoundTrips * 10 >= Keys.Num() * 8);
	return true;
}
// Engine-content round trip: a link to a member of a struct pin splits the pin ("Split Struct Pin"),
// and a cast to another Blueprint's class has the key its node reports (the class path).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphK2SplitPinAndBlueprintCastTest,
	"UEAssetDoc.Graph.K2.SplitPinAndBlueprintCast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphK2SplitPinAndBlueprintCastTest::RunTest(const FString& Parameters)
{
	FTestBlueprint Other(TEXT("CastTarget"));
	FTestBlueprint Blueprint(TEXT("SplitPin"));
	if (!TestNotNull(TEXT("cast target Blueprint"), Other.Get()) || !TestNotNull(TEXT("test Blueprint"), Blueprint.Get())
		|| !TestNotNull(TEXT("cast target class"), Other->GeneratedClass.Get()))
	{
		return false;
	}
	FSession Session(*this, *Blueprint.Get());
	if (!Session.IsReady())
	{
		return false;
	}
	const FString Location = Spawn(Session, TEXT("EventGraph"), TEXT("K2:CallFunction:/Script/Engine.Actor:K2_GetActorLocation"), 0, 400);
	const FString Abs = Spawn(Session, TEXT("EventGraph"), TEXT("K2:CallFunction:/Script/Engine.KismetMathLibrary:Abs"), 400, 400);
	if (Location.IsEmpty() || Abs.IsEmpty())
	{
		return false;
	}
	TestTrue(TEXT("ReturnValue_X -> Abs.A splits the vector pin"), Connect(Session, Location, TEXT("ReturnValue_X"), Abs, TEXT("A")));
	const FValue Query = QueryGraph(Session, TEXT("EventGraph"));
	const FValue* Graph = FindGraph(Query, TEXT("EventGraph"));
	const FValue* Node = Graph != nullptr ? FindNode(*Graph, TEXT("node_guid"), Location) : nullptr;
	TestTrue(TEXT("the split member pins are on the node"), Node != nullptr && FindPin(*Node, TEXT("ReturnValue_Z"), TEXT("output")) != nullptr);
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Session.ExpectFailure(TEXT("graph.pin.connect.v1"), FString::Printf(TEXT("{\"from\":%s,\"to\":%s}"),
		*PinJson(Location, TEXT("ReturnValue_W"), TEXT("output")), *PinJson(Abs, TEXT("A"), TEXT("input"))), TEXT("Graph.PinNotFound"), Diagnostic);

	const FString CastKey = TEXT("K2:DynamicCast:") + Other->GeneratedClass->GetPathName();
	TestFalse(TEXT("cast to a Blueprint class spawns by its class key"), Spawn(Session, TEXT("EventGraph"), CastKey, 0, 800).IsEmpty());
	const FAssetDocumentPrimitiveOutcome Legacy = Session.Invoke(TEXT("graph.node.spawn.v1"), FString::Printf(
		TEXT("{\"graph\":%s,\"action_key\":\"K2:DynamicCast:%s\"}"), *GraphJson(TEXT("EventGraph")), *Other->GetPathName()));
	TestEqual(TEXT("the Blueprint-asset key of the cast still resolves"), Str(Legacy.Value, TEXT("node/action_key")), CastKey);
	return true;
}

// Every node spawn looks its action up in the Blueprint action list, which is kept up to date from
// what changed in the action database since the previous lookup. After each kind of Blueprint
// edit it is the list keyed from scratch, and the edited members' actions spawn.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphK2ActionListCacheTest,
	"UEAssetDoc.Graph.K2.ActionListCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphK2ActionListCacheTest::RunTest(const FString& Parameters)
{
	FTestBlueprint Blueprint(TEXT("ActionListCache"));
	if (!TestNotNull(TEXT("test Blueprint"), Blueprint.Get()))
	{
		return false;
	}
	FSession Session(*this, *Blueprint.Get());
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint.Get());
	if (!Session.IsReady() || !TestNotNull(TEXT("event graph"), EventGraph))
	{
		return false;
	}
	const auto Check = [this, &Blueprint](const TCHAR* Step, UEdGraph& Graph)
	{
		FString Difference;
		const bool bSame = K2Keys::CompareCachedActionList(*Blueprint.Get(), Graph, Difference);
		TestTrue(FString::Printf(TEXT("%s: the kept action list is the one keyed from scratch%s%s"), Step,
			bSame ? TEXT("") : TEXT(": "), *Difference), bSame);
	};
	Check(TEXT("first lookup"), *EventGraph);
	Check(TEXT("nothing changed"), *EventGraph);

	TestFalse(TEXT("Branch spawns"), Spawn(Session, TEXT("EventGraph"), BranchKey, 0, 0).IsEmpty());
	Check(TEXT("after a spawn"), *EventGraph);

	FEdGraphPinType RealType;
	RealType.PinCategory = UEdGraphSchema_K2::PC_Real;
	RealType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
	TestTrue(TEXT("variable added"), FBlueprintEditorUtils::AddMemberVariable(Blueprint.Get(), TEXT("Speed"), RealType));
	Check(TEXT("after adding a variable"), *EventGraph);
	const FString SpeedKey = TEXT("K2:VariableGet:") + K2Keys::OwnerPath(Blueprint->SkeletonGeneratedClass) + TEXT(":Speed");
	TestFalse(TEXT("the new variable's getter spawns"), Spawn(Session, TEXT("EventGraph"), SpeedKey, 0, 200).IsEmpty());
	Check(TEXT("after spawning the getter"), *EventGraph);

	USCS_Node* Component = Blueprint->SimpleConstructionScript->CreateNode(UBoxComponent::StaticClass(), TEXT("CollisionComp"));
	Blueprint->SimpleConstructionScript->AddNode(Component);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint.Get());
	Check(TEXT("after adding a component"), *EventGraph);
	TestFalse(TEXT("the component's bound event spawns"), Spawn(Session, TEXT("EventGraph"), BoundOverlapKey, 0, 400).IsEmpty());
	Check(TEXT("after spawning the bound event"), *EventGraph);

	UEdGraph* Function = FBlueprintEditorUtils::CreateNewGraph(Blueprint.Get(), TEXT("DoThing"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint.Get(), Function, /*bIsUserCreated=*/true, nullptr);
	Check(TEXT("function graph, first lookup"), *Function);
	Check(TEXT("event graph after adding a function"), *EventGraph);

	FBlueprintEditorUtils::RenameMemberVariable(Blueprint.Get(), TEXT("Speed"), TEXT("Velocity"));
	Check(TEXT("after renaming the variable"), *EventGraph);
	FBlueprintEditorUtils::RemoveMemberVariable(Blueprint.Get(), TEXT("Velocity"));
	Check(TEXT("after removing the variable"), *EventGraph);
	Check(TEXT("function graph after the edits"), *Function);
	return true;
}
} // namespace UE::AssetDocument::Graph::Tests

#endif // WITH_DEV_AUTOMATION_TESTS
